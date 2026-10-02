# SPDX-License-Identifier: GPL-2.0-only
# robust.ps1 - the robustness run: the corrupted images of the e2fsprogs test
# suite (robust-corpus.sh), a batch at a time as the partitions of one disk
# (robust-image.sh), attached to the running guest. The guest lists, reads,
# writes and deletes on every volume the driver mounts (robust-walk.ps1),
# then the driver is stopped. A corrupt volume may be refused, mounted
# read-only or fail any operation; the system may not crash, and nothing may
# hang - not an operation, not the stop.
#
#   pwsh tests\robust.ps1                    # the whole corpus
#   pwsh tests\robust.ps1 -Only f_dir_bad_csum,f_extents   # some images
#
# Verdicts go to tests\logs\robust-<time>.log. Crash reports name the guest's
# dump and System event; dump analysis stays inside the guest, never on the
# workstation. Best run with Driver Verifier on ext4.sys.
param(
    [string[]]$Only,
    [int]$BatchMax = 18,                 # partitions per batch (drive letters)
    [long]$BatchBytes = 280MB,           # image bytes per batch
    [int]$WalkSeconds = 60,              # one volume's walk
    [int]$StopSeconds = 60               # sc stop after a batch
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\testenv.ps1"
# a guest that crashed leaves a half-open connection: keepalives end it
$so = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3')
$vhd = $TestEnv.RobustVhd; $size = $TestEnv.RobustSize; $corpus = $TestEnv.RobustCorpus
$logDir = Join-Path $PSScriptRoot 'logs'; New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir ("robust-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))
function Say($t) { $t; Add-Content -LiteralPath $log -Value $t }
function Target { "$($TestEnv.User)@$(Get-TestVmIp)" }
function Guest($cmd) {
    $o = @(ssh @so (Target) $cmd 2>&1)
    $script:LastGuestRc = $LASTEXITCODE
    $o | Where-Object { $_ -notmatch 'CLIXML|<Obj' }
}
function Uptime { (Get-VM $TestEnv.Vm).Uptime }
function Detach { Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object Path -eq $vhd | Remove-VMHardDiskDrive }
function Invoke-WslBatch([string]$script, [string]$a) {
    $w = '/mnt/' + $PSScriptRoot.Substring(0, 1).ToLower() + $PSScriptRoot.Substring(2).Replace('\', '/')
    $o = @(wsl -u root -- bash -c "sed 's/\r`$//' '$w/$script' > /tmp/$script && bash /tmp/$script $a")
    @{ Out = $o; Rc = $LASTEXITCODE }
}
function WaitGuest {
    for ($i = 0; $i -lt 120; $i++) {
        try { if ((ssh @so -o ConnectTimeout=3 (Target) 'echo up' 2>$null) -eq 'up') { return } } catch {}
        Start-Sleep 2      # polling the guest's ssh while it boots
    }
    throw 'guest does not come back'
}
function GuestPs([string]$script) {
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
    Guest "powershell -NoProfile -EncodedCommand $enc"
}
function Reboot {
    $was = Uptime
    Guest 'shutdown /r /t 0' | Out-Null
    $deadline = (Get-Date).AddSeconds(90)
    while ((Uptime) -ge $was -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
    if ((Uptime) -ge $was) { throw 'guest shutdown did not finish; diagnose it before a forced reset' }
    WaitGuest
}
function StartDriver {
    $o = @(Guest "sc start $($TestEnv.Service) >nul 2>&1 & sc query $($TestEnv.Service) | findstr STATE")
    if ($script:LastGuestRc -ne 0 -or ($o -join ' ') -notmatch 'RUNNING') { throw "driver is not running: $($o -join ' ')" }
    $o
}
function CrashReport {
    GuestPs @'
$ProgressPreference='SilentlyContinue'
$dump=Get-ChildItem C:\Windows\Minidump -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if($dump){'guest dump: '+$dump.FullName+' ('+$dump.LastWriteTime.ToString('s')+')'}
$event=Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Microsoft-Windows-WER-SystemErrorReporting';Id=1001} -MaxEvents 1 -ErrorAction SilentlyContinue
if($event){'guest bugcheck event: '+($event.Message -replace '[\r\n]+',' ')}
'@
}

# the corpus (an absolute path: the batches are written as root) and the batches
$resolved = @(wsl -- bash -c "cd $corpus 2>/dev/null && pwd")
if ($LASTEXITCODE -ne 0 -or $resolved.Count -ne 1) { throw 'robustness corpus path cannot be resolved' }
$corpus = $resolved[0].Trim()
$list = @(wsl -- bash -c "cat $corpus/list.txt 2>/dev/null" | ForEach-Object { $s, $t = $_ -split ' '; [pscustomobject]@{ Size = [long]$s; Test = $t } })
if ($LASTEXITCODE -ne 0 -or -not $list) { throw "no corpus: wsl -- bash tests/robust-corpus.sh first" }
if ($Only) {
    $Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    $list = @($list | Where-Object { $_.Test -in $Only })
}
if (-not $list) { throw 'no robustness images match the requested selection' }
$batches = @(); $cur = @(); $bytes = 0
foreach ($e in $list) {
    $mib = [math]::Max(1, [math]::Ceiling($e.Size / 1MB)) * 1MB
    if ($cur.Count -and ($cur.Count -ge $BatchMax -or $bytes + $mib -gt $BatchBytes)) { $batches += , $cur; $cur = @(); $bytes = 0 }
    $cur += $e.Test; $bytes += $mib
}
if ($cur.Count) { $batches += , $cur }
Say "== robust $(Get-Date -Format s): $($list.Count) images in $($batches.Count) batches"

if (-not (Test-Path $vhd)) { New-VHD -Path $vhd -SizeBytes $size -Dynamic | Out-Null }
if ((Get-VHD -Path $vhd).Size -ne $size) { throw 'robustness disk has the wrong size' }
Detach
scp -q @so (Join-Path $PSScriptRoot 'robust-walk.ps1') "$(Target):C:/Windows/Temp/robust-walk.ps1"
if ($LASTEXITCODE) { throw 'robustness walker transfer failed' }
$crashes = 0; $hangs = 0; $errors = 0; $mounted = 0; $total = 0
$b = 0
foreach ($batch in $batches) {
    $b++
    # the images onto the partitions
    wsl --mount --vhd $vhd --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL did not attach the robustness disk' }
    $map = @{}
    try {
        $prepared = Invoke-WslBatch 'robust-image.sh' "$size $corpus/img $($batch -join ' ')"
        foreach ($l in $prepared.Out) { if ($l -match '^(\d+) (\S+)$') { $map[[int]$Matches[1]] = $Matches[2] } }
        if ($prepared.Rc -ne 0 -or $map.Count -ne $batch.Count) { throw "batch ${b}: disk preparation failed; this is not a test pass" }
    } finally {
        wsl --unmount "\\?\$vhd" | Out-Null
        if ($LASTEXITCODE) { throw 'WSL did not release the robustness disk' }
    }

    $up = Uptime
    StartDriver | Out-Null
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation ($TestEnv.ScsiSlot + 4) -Path $vhd
    $p = Start-Process ssh -ArgumentList (@($so) + @((Target), "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\robust-walk.ps1 -DiskSize $size -Seconds $WalkSeconds")) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$env:TEMP\robust-out.txt"
    $processHandle = $p.Handle # retain the process handle for an exit-code check
    # the walk ends, or its time is up, or the guest went down under it
    $until = (Get-Date).AddSeconds(($batch.Count * ($WalkSeconds + 5)) + 60)
    do { $done = $p.WaitForExit(5000) } while (-not $done -and (Get-Date) -lt $until -and (Uptime) -ge $up)
    $lines = @(Get-Content "$env:TEMP\robust-out.txt" -ErrorAction SilentlyContinue)
    if (-not $done) { try { $p.Kill() } catch {} }

    $crashed = (Uptime) -lt $up
    $seen = [Collections.Generic.HashSet[int]]::new()
    foreach ($l in $lines) {
        if ($l -match '^(\d+) (\S+)\s*(.*)$') {
            $n = [int]$Matches[1]
            if (-not $map.ContainsKey($n) -or -not $seen.Add($n)) { $errors++; Say "!! invalid or duplicate partition result: $l"; continue }
            $t = $map[$n]; $total++
            if ($Matches[2] -eq 'done') { $mounted++ }
            if ($Matches[2] -eq 'HANG') { $hangs++ }
            if ($Matches[2] -notin 'done','HANG','unmounted','not-ext') { $errors++ }
            Say ("{0,-36} {1} {2}" -f $t, $Matches[2], $Matches[3])
        }
    }
    if (-not $crashed -and -not $done) { Say "!! batch ${b}: the walk did not finish ($($batch -join ' '))"; $hangs++ }
    if (-not $crashed -and $done -and ($p.ExitCode -ne 0 -or $seen.Count -ne $map.Count)) {
        $errors++; Say "!! batch ${b}: walker exit $($p.ExitCode), $($seen.Count)/$($map.Count) partition results"
    }

    # the stop: every volume of the batch dismounts, the driver unloads
    if (-not $crashed) {
        $svc = $TestEnv.Service
        $st = GuestPs "sc.exe stop $svc | Out-Null; `$d = (Get-Date).AddSeconds($StopSeconds); while ((Get-Date) -lt `$d -and -not ((sc.exe query $svc) -match 'STOPPED')) { Start-Sleep -Milliseconds 200 }; (sc.exe query $svc) -match 'STATE'"
        $crashed = (Uptime) -lt $up
        if (-not $crashed -and ($script:LastGuestRc -ne 0 -or ($st -join ' ') -notmatch 'STOPPED|STOP_PENDING')) { throw "batch ${b}: stop did not report its state" }
        if (-not $crashed -and ($st -join ' ') -match 'STOP_PENDING') {
            $hangs++; Say "!! batch ${b}: sc stop hangs ($($batch -join ' ')) - rebooting the guest"
            Detach; Reboot; $up = Uptime
        }
    }
    Detach
    if ($crashed) {
        $crashes++
        WaitGuest
        Say "!! batch ${b}: CRASH ($($batch -join ' ')): $(CrashReport)"
        scp -q @so (Join-Path $PSScriptRoot 'robust-walk.ps1') "$(Target):C:/Windows/Temp/robust-walk.ps1"
        if ($LASTEXITCODE) { throw 'robustness walker transfer after crash failed' }
    }
    StartDriver | Out-Null
}
# nothing left behind: the script, and the mount manager's records of the
# volumes that came and went (drive letters, unique ids)
Guest 'del C:\Windows\Temp\robust-walk.ps1 & mountvol /R' | Out-Null
Say "== $total volumes, $mounted walked, $hangs hang(s), $crashes crash(es), $errors harness error(s); log $log"
exit ($hangs + $crashes + $errors)
