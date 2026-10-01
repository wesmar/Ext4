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
# Verdicts go to tests\logs\robust-<time>.log; a crash is analysed from the
# guest's minidump (!analyze -v). Best run with Driver Verifier on ext4.sys.
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
function Guest($cmd) { ssh @so (Target) $cmd 2>&1 | Where-Object { $_ -notmatch 'CLIXML|<Obj' } }
function Uptime { (Get-VM $TestEnv.Vm).Uptime }
function Detach { Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object Path -eq $vhd | Remove-VMHardDiskDrive }
function Invoke-WslBatch([string]$script, [string]$a) {
    $w = '/mnt/' + $PSScriptRoot.Substring(0, 1).ToLower() + $PSScriptRoot.Substring(2).Replace('\', '/')
    wsl -u root -- bash -c "sed 's/\r`$//' '$w/$script' > /tmp/$script && bash /tmp/$script $a"
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
    while ((Uptime) -ge $was) { Start-Sleep 1 }      # until the guest has actually gone down
    WaitGuest
}
function StartDriver { Guest "sc start $($TestEnv.Service) >nul 2>&1 & sc query $($TestEnv.Service) | findstr STATE" }
function CrashReport {
    $kd = "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\kd.exe"
    $name = (Guest 'powershell -NoProfile -Command "(Get-ChildItem C:\Windows\Minidump | Sort-Object LastWriteTime | Select-Object -Last 1).Name"' | Select-Object -Last 1).Trim()
    $local = Join-Path $env:TEMP "robust-$name"
    scp -q @so "$(Target):C:/Windows/Minidump/$name" $local
    if (-not (Test-Path $kd)) { return "minidump $name (no kd.exe to analyse it)" }
    $sym = "$(Split-Path $PSScriptRoot)\symbols;srv*$env:TEMP\symbols*https://msdl.microsoft.com/download/symbols"
    $a = & $kd -z $local -y $sym -c '!analyze -v; q' 2>&1
    [IO.File]::Delete($local)
    (($a | Select-String 'BUGCHECK_CODE|SYMBOL_NAME|FAILURE_BUCKET_ID').Line | ForEach-Object { $_.Trim() }) -join ' | '
}

# the corpus (an absolute path: the batches are written as root) and the batches
$corpus = (wsl -- bash -c "cd $corpus 2>/dev/null && pwd").Trim()
$list = @(wsl -- bash -c "cat $corpus/list.txt 2>/dev/null" | ForEach-Object { $s, $t = $_ -split ' '; [pscustomobject]@{ Size = [long]$s; Test = $t } })
if (-not $list) { throw "no corpus: wsl -- bash tests/robust-corpus.sh first" }
if ($Only) { $list = @($list | Where-Object { $_.Test -in $Only }) }
$batches = @(); $cur = @(); $bytes = 0
foreach ($e in $list) {
    $mib = [math]::Max(1, [math]::Ceiling($e.Size / 1MB)) * 1MB
    if ($cur.Count -and ($cur.Count -ge $BatchMax -or $bytes + $mib -gt $BatchBytes)) { $batches += , $cur; $cur = @(); $bytes = 0 }
    $cur += $e.Test; $bytes += $mib
}
if ($cur.Count) { $batches += , $cur }
Say "== robust $(Get-Date -Format s): $($list.Count) images in $($batches.Count) batches"

if (-not (Test-Path $vhd)) { New-VHD -Path $vhd -SizeBytes $size -Dynamic | Out-Null }
Detach
scp -q @so (Join-Path $PSScriptRoot 'robust-walk.ps1') "$(Target):C:/Windows/Temp/robust-walk.ps1"
$crashes = 0; $hangs = 0; $mounted = 0; $total = 0
$b = 0
foreach ($batch in $batches) {
    $b++
    # the images onto the partitions
    wsl --mount --vhd $vhd --bare | Out-Null
    $map = @{}
    foreach ($l in (Invoke-WslBatch 'robust-image.sh' "$size $corpus/img $($batch -join ' ')")) { if ($l -match '^(\d+) (\S+)$') { $map[[int]$Matches[1]] = $Matches[2] } }
    wsl --unmount "\\?\$vhd" | Out-Null
    if ($map.Count -ne $batch.Count) { Say "!! batch ${b}: the disk was not prepared"; continue }

    $up = Uptime
    StartDriver | Out-Null
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation ($TestEnv.ScsiSlot + 4) -Path $vhd
    $p = Start-Process ssh -ArgumentList (@($so) + @((Target), "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\robust-walk.ps1 -DiskSize $size -Seconds $WalkSeconds")) -NoNewWindow -PassThru -RedirectStandardOutput "$env:TEMP\robust-out.txt"
    # the walk ends, or its time is up, or the guest went down under it
    $until = (Get-Date).AddSeconds(($batch.Count * ($WalkSeconds + 5)) + 60)
    do { $done = $p.WaitForExit(5000) } while (-not $done -and (Get-Date) -lt $until -and (Uptime) -ge $up)
    $lines = @(Get-Content "$env:TEMP\robust-out.txt" -ErrorAction SilentlyContinue)
    if (-not $done) { try { $p.Kill() } catch {} }

    $crashed = (Uptime) -lt $up
    foreach ($l in $lines) {
        if ($l -match '^(\d+) (\S+)\s*(.*)$') {
            $t = $map[[int]$Matches[1]]; $total++
            if ($Matches[2] -eq 'done') { $mounted++ }
            if ($Matches[2] -eq 'HANG') { $hangs++ }
            Say ("{0,-36} {1} {2}" -f $t, $Matches[2], $Matches[3])
        }
    }
    if (-not $crashed -and -not $done) { Say "!! batch ${b}: the walk did not finish ($($batch -join ' '))"; $hangs++ }

    # the stop: every volume of the batch dismounts, the driver unloads
    if (-not $crashed) {
        $svc = $TestEnv.Service
        $st = GuestPs "sc.exe stop $svc | Out-Null; `$d = (Get-Date).AddSeconds($StopSeconds); while ((Get-Date) -lt `$d -and -not ((sc.exe query $svc) -match 'STOPPED')) { Start-Sleep -Milliseconds 200 }; (sc.exe query $svc) -match 'STATE'"
        $crashed = (Uptime) -lt $up
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
    }
    StartDriver | Out-Null
}
# nothing left behind: the script, and the mount manager's records of the
# volumes that came and went (drive letters, unique ids)
Guest 'del C:\Windows\Temp\robust-walk.ps1 & mountvol /R' | Out-Null
Say "== $total volumes, $mounted walked, $hangs hang(s), $crashes crash(es); log $log"
exit ($hangs + $crashes)
