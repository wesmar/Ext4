#Requires -Version 7.0
# SPDX-License-Identifier: GPL-2.0-only
# run-ext4test.ps1 - host side of the ext4 driver test run (Hyper-V host with WSL).
#
#   1. copies the guest scripts to the VM and runs the functional test on an
#      ext volume (every file operation Win32 exposes, see ext4test.ps1);
#   2. -Stress: concurrent create/rename/mkdir/delete churn and parallel
#      write/extend/truncate/verify workers, each bounded to about a minute;
#   3. -System: service stop/start with open handles (sc stop and net stop) and
#      drive-letter checks in the guest, then a hot unplug / replug of the test
#      disk with the driver running and a handle open;
#   4. stops the driver, detaches the VHDX, runs e2fsck -fn on every ext
#      partition in WSL and verifies the SHA-256 manifest by mounting the
#      partition on Linux (data written by the driver, read by the reference
#      implementation);
#   5. re-attaches the disk, starts the driver, prints the summary.
#
# Exit code: number of failed sections (0 = everything passed).
#
#   pwsh tests\run-ext4test.ps1 [-Drive E] [-Quick] [-Stress] [-System] [-Interop] [-Luks] [-Features] [-NoFsck]
#
#   -Interop: Linux prepares modes, owners, symlinks, hard links, a FIFO,
#   nanosecond times and xattrs (interop.sh); the guest reads them and makes
#   its own (interop.ps1); Linux checks the result with the fsck stage.
param(
    [string]$Ip = '',               # default: the VM's current IPv4 from Hyper-V
    [string]$Drive = '',            # default: the largest ext volume in the guest
    [string[]]$Letters = @(),       # default: every ext volume in the guest
    [switch]$Quick,
    [switch]$Stress,
    [switch]$System,
    [switch]$Interop,
    [switch]$Luks,                  # LUKS1/LUKS2 through ext4ctl, checked from Linux (luks-image.sh builds the disk)
    [switch]$Features,              # casefold (SteamOS cards) and chattr +i/+a, on a disk features-image.sh builds afresh
    [switch]$NoFsck
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$Vm = $TestEnv.Vm
$Vhd = $TestEnv.Vhd
$Svc = $TestEnv.Service
$sshOpt = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3')
$target = "$($TestEnv.User)@$Ip"
$sp = $PSScriptRoot
$summary = [ordered]@{}
$total = [Diagnostics.Stopwatch]::StartNew()
$logDir = Join-Path $sp 'logs'
New-Item -ItemType Directory $logDir -Force | Out-Null
Start-Transcript -Path (Join-Path $logDir ("run-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))) | Out-Null

function Remote([string]$cmd) { & ssh @sshOpt $target $cmd; return $LASTEXITCODE }
function Guest-PS([string]$file, [string]$Arguments) {
    # -File keeps the guest script's exit code; -NonInteractive turns a prompt into an error instead of a hang
    $out = [Collections.Generic.List[string]]::new()
    & ssh @sshOpt $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\$file $Arguments" | ForEach-Object { $out.Add($_); Write-Host "   $_" }
    $rc = $LASTEXITCODE
    if ($rc -ne 0) { throw "$file failed (guest/ssh exit $rc)" }
    $completion = switch ($file) {
        'ext4test.ps1' { '^== RESULT: \d+ ok, \d+ failed,' }
        'resize.ps1' { '^RESIZE: \d+ cases, \d+ failed$' }
        'ext4sys.ps1'  { '^== SYS RESULT: \d+ failed' }
        'pending.ps1' { '^pending: \d+ failed' }
        'security.ps1' { '^security: \d+ failed' }
        'interop.ps1' { '^INTEROP-WIN: \d+ failed' }
        'luks.ps1' { '^LUKS-WIN: \d+ failed' }
        'features.ps1' { '^FEATURES-WIN: \d+ failed' }
        'stress.ps1' { '^ops=\d+ errors=\d+ leftover=\d+' }
        'repro.ps1' { '^done: \d+ runs, \d+ failed' }
        'race.ps1' { '^opens=\d+ changes=\d+ unexpected=\d+' }
        'nsrace.ps1' { '^nsrace: \d+ unexpected' }
        default { throw "no completion contract for $file" }
    }
    if (-not @($out | Where-Object { $_ -match $completion }).Count) {
        throw "$file did not report completion; an empty or interrupted run is not a pass"
    }
    return @{ Out = $out; Rc = $rc }
}
function Guest-Drives { (& ssh @sshOpt $target 'powershell -NoProfile -Command "(Get-PSDrive -PSProvider FileSystem).Name -join \" \""') }
function Wait-GuestDrives([bool]$present, [int]$sec) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do {
        $d = (Guest-Drives) -split ' '
        $ok = $true; foreach ($l in $Letters) { if (($d -contains $l) -ne $present) { $ok = $false } }
        if ($ok) { return $sw.ElapsedMilliseconds }
        Start-Sleep -Milliseconds 500
    } while ($sw.Elapsed.TotalSeconds -lt $sec)
    return -1
}
function Stop-GuestDriver {
    $r = & ssh @sshOpt $target "powershell -NoProfile -Command `"`$s=Get-Service $Svc; if(`$s.Status -ne \`"Stopped\`"){ sc.exe stop $Svc | Out-Null; `$s.WaitForStatus(\`"Stopped\`",[TimeSpan]::FromSeconds(120)) }; `$s.Refresh(); `$s.Status`""
    if ($r -ne 'Stopped') {
        # where the unload got to (driver\unload.c, EXT2_STEP_*), read without a debugger
        $step = & ssh @sshOpt $target "reg query HKLM\SYSTEM\CurrentControlSet\Services\$Svc /v UnloadStep & reg query HKLM\SYSTEM\CurrentControlSet\Services\$Svc /v UnloadWaitStatus" 2>&1 | Select-String 'Unload' | ForEach-Object { $_.Line.Trim() }
        throw "driver did not stop: $r ($($step -join '; '))"
    }
}
function Start-GuestDriver { [void](Remote "sc start $Svc >nul"); $t = Wait-GuestDrives $true 30; if ($t -lt 0) { throw "letters did not come back after sc start (have: $(Guest-Drives))" }; return $t }
function Detach-Disk { Remove-VMHardDiskDrive -VMName $Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $TestEnv.ScsiSlot }
function Attach-Disk { Add-VMHardDiskDrive -VMName $Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $TestEnv.ScsiSlot -Path $Vhd }

try {
"== run-ext4test $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  vm=$Vm ip=$Ip service=$Svc quick=$Quick stress=$Stress system=$System"
if ((Get-VM $Vm).State -ne 'Running') { throw "VM $Vm is not running" }
$d = Guest-Drives
if (-not $d) { throw "no ssh connection to $Ip" }
# ext volumes as the guest sees them: "letter label size" per line, largest first
$vols = & ssh @sshOpt $target 'powershell -NoProfile -Command "Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystem -match \"^EXT\" } | Sort-Object Size -Descending | ForEach-Object { \"$($_.DriveLetter) $($_.FileSystemLabel) $($_.Size) $($_.FileSystem)\" }"'
if (-not $vols) { throw "no ext volume with a drive letter in the guest (drives: $d); is the driver running?" }
$vols = @($vols | ForEach-Object { $f = $_ -split ' '; [pscustomobject]@{ Letter = $f[0]; Label = $f[1]; Size = [long]$f[2]; Fs = $f[3] } })
if (-not $Drive) { $Drive = $vols[0].Letter }
if (-not $Letters) { $Letters = @($vols.Letter) }
"   ext volumes: $(($vols | ForEach-Object { "$($_.Letter): $($_.Label) $($_.Fs) $([math]::Round($_.Size/1MB)) MB" }) -join ', ')  -> testing on $Drive`:"
if (($d -split ' ') -notcontains $Drive) { throw "drive $Drive not mounted in the guest (have: $d)" }

foreach ($f in 'ext4test.ps1', 'resize.ps1', 'ext4sys.ps1', 'ext4hold.ps1', 'stress.ps1', 'repro.ps1', 'pending.ps1', 'race.ps1', 'nsrace.ps1', 'interop.ps1', 'security.ps1', 'luks.ps1', 'features.ps1') {
    & scp -q @sshOpt "$sp\$f" "${target}:C:/Windows/Temp/$f"
    if ($LASTEXITCODE) { throw "scp $f failed" }
}
# ext4ctl for the LUKS stage, from the same build as the driver
$ctlExe = Join-Path (Split-Path $sp) 'bin\ext4ctl.exe'
if ($Luks) {
    if (-not (Test-Path $ctlExe)) { throw "-Luks needs $ctlExe (build.ps1)" }
    & scp -q @sshOpt $ctlExe "${target}:C:/Windows/Temp/ext4ctl.exe"
    if ($LASTEXITCODE) { throw 'scp ext4ctl.exe failed' }
}
# Sysinternals handle64 (if the host has it) names the processes that hold a volume when a stop is slow
$handleTool = (Get-Command handle64.exe -ErrorAction SilentlyContinue).Source
if ($handleTool) { & scp -q @sshOpt $handleTool "${target}:C:/Windows/Temp/handle64.exe" }

# ---- 0. interop: Linux builds its objects, the guest reads them and makes its own
function Invoke-WslScript([string]$name, [string]$Arguments) {
    $wdir = '/mnt/' + $sp.Substring(0, 1).ToLower() + $sp.Substring(2).Replace('\', '/')
    $wsh = "$wdir/$name"
    # the script runs from /tmp (CRLF stripped); EXT4_TESTS points at its helpers
    $o = wsl -u root -- bash -c "sed 's/\r\$//' '$wsh' > /tmp/$name && EXT4_TESTS='$wdir' bash /tmp/$name $Arguments"
    return @{ Out = $o; Rc = $LASTEXITCODE }
}
if ($Interop) {
    "-- interop: Linux prepares \interop-linux"
    Stop-GuestDriver
    Detach-Disk
    wsl --mount --vhd $Vhd --bare | Out-Null
    $r = Invoke-WslScript 'interop.sh' "prepare $($TestEnv.VhdSize)"
    if ($r.Rc -ne 0) { throw "Linux interop preparation failed (exit $($r.Rc))" }
    wsl --unmount "\\?\$Vhd" | Out-Null
    $r.Out | ForEach-Object { "   $_" }
    Attach-Disk
    $t = Start-GuestDriver
    "   driver restarted, letters back in $t ms"
    "-- interop: the guest reads, creates and changes"
    $r = Guest-PS 'interop.ps1' "-Drives $($Letters -join ',')"
    $bad = ($r.Out | Where-Object { $_ -match 'INTEROP-WIN: (\d+) failed' -and [int]$Matches[1] -gt 0 }).Count
    $summary['interop-win'] = "$bad failed"
}

# ---- 1. functional test in the guest
"-- functional test on $Drive`:\ext4test"
$sw = [Diagnostics.Stopwatch]::StartNew()
$r = Guest-PS 'ext4test.ps1' "-Root $Drive`:\ext4test $(if ($Quick) { '-Quick' })"
$summary['functional'] = "$($r.Rc) failed ($([math]::Round($sw.Elapsed.TotalSeconds)) s)"
foreach ($letter in $Letters) {
    "-- resize: cached/uncached shrink and regrow on $letter`:"
    $r = Guest-PS 'resize.ps1' "-Root $letter`:\ -Repeat 2"
    if (-not ($r.Out -match '^RESIZE: \d+ cases, 0 failed$')) { throw 'resize regression failed' }
    $summary["resize-$letter"] = '0 failed'
}
"-- delete-pending scenarios (handles, symlinks, renames)"
$sw.Restart()
$r = Guest-PS 'pending.ps1' "-Root $Drive`:\pending"
$bad = ($r.Out | Where-Object { $_ -match 'pending: (\d+) failed' -and [int]$Matches[1] -gt 0 }).Count
$summary['pending'] = "$bad failed ($([math]::Round($sw.Elapsed.TotalSeconds)) s)"
"-- security: private IOCTLs against user callers and bad buffers"
$r = Guest-PS 'security.ps1' "-Drive $Drive"
$bad = ($r.Out | Where-Object { $_ -match 'security: (\d+) failed' -and [int]$Matches[1] -gt 0 }).Count
$summary['security'] = "$bad failed"

# ---- 2. stress
if ($Stress) {
    "-- stress: 8 threads create/rename/mkdir/delete"
    $sw.Restart()
    $r = Guest-PS 'stress.ps1' "-Root $Drive`:\stress -Threads 8 -Iters 1500"
    $bad = ($r.Out | Where-Object { $_ -match 'errors=(\d+)' -and [int]$Matches[1] -gt 0 }).Count + ($r.Out | Where-Object { $_ -match 'leftover=([1-9])' }).Count
    $summary['stress'] = "$bad failed ($([math]::Round($sw.Elapsed.TotalSeconds)) s)"
    "-- stress: parallel write/extend/truncate/rename with read-back"
    $sw.Restart()
    $r = Guest-PS 'repro.ps1' "-Drive $Drive`: -Runs 3 -Files 200"
    $bad = ($r.Out | Where-Object { $_ -cmatch '\bFAIL\b' }).Count
    $summary['parallel-io'] = "$bad failed ($([math]::Round($sw.Elapsed.TotalSeconds)) s)"
    "-- stress: opens racing renames, deletes, re-creates and symlinks of the same names"
    $sw.Restart()
    $r = Guest-PS 'race.ps1' "-Root $Drive`:\race -Seconds 6"
    $bad = ($r.Out | Where-Object { $_ -match 'unexpected=(\d+)' -and [int]$Matches[1] -gt 0 }).Count
    $summary['race'] = "$bad failed ($([math]::Round($sw.Elapsed.TotalSeconds)) s)"
    "-- stress: creates and deletes of one directory's names racing each other"
    $sw.Restart()
    $r = Guest-PS 'nsrace.ps1' "-Root $Drive`:\nsrace -Threads 8 -Rounds 300"
    $bad = ($r.Out | Where-Object { $_ -match 'nsrace: (\d+) unexpected' -and [int]$Matches[1] -gt 0 }).Count
    $summary['nsrace'] = "$bad failed ($([math]::Round($sw.Elapsed.TotalSeconds)) s)"
}

# ---- 3. system tests
if ($System) {
    "-- service stop/start in the guest"
    $r = Guest-PS 'ext4sys.ps1' "-Letters $($Letters -join ',') -Service $Svc"
    $summary['service'] = "$($r.Rc) failed"

    "-- hot unplug with the driver running and a handle open"
    # a host-side job keeps an ssh session that holds a file open on the disk for 8 s
    $job = Start-Job -ScriptBlock { param($o, $t, $f) & ssh @o $t "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\ext4hold.ps1 $f" } -ArgumentList $sshOpt, $target, "$Drive`:\ext4test\manifest.sha256"
    Start-Sleep 2
    Detach-Disk
    $t = Wait-GuestDrives $false 20
    "   letters withdrawn on surprise removal: $(if ($t -ge 0) { "$t ms" } else { 'NOT within 20 s' })"
    $unplug = ($t -ge 0)
    $held = Receive-Job $job -Wait -AutoRemoveJob
    "   guest handle holder: $held"
    Attach-Disk
    $t = Wait-GuestDrives $true 40
    "   letters back after replug: $(if ($t -ge 0) { "$t ms" } else { 'NOT within 40 s' })"
    $summary['hot-plug'] = if ($unplug -and $t -ge 0) { '0 failed' } else { '1 failed' }
}

# ---- 4. fsck + Linux verification
if (-not $NoFsck) {
    "-- e2fsck and manifest verification from Linux"
    Stop-GuestDriver
    Detach-Disk
    wsl --mount --vhd $Vhd --bare | Out-Null
    Start-Sleep 3
    $wsh = '/mnt/' + $sp.Substring(0, 1).ToLower() + $sp.Substring(2).Replace('\', '/') + '/ext4verify.sh'
    $out = wsl -u root -- bash -c "sed -i 's/\r\$//' '$wsh' && bash '$wsh' $($TestEnv.VhdSize)"
    $frc = $LASTEXITCODE
    if ($Interop) {
        $ir = Invoke-WslScript 'interop.sh' "check $($TestEnv.VhdSize)"
        $out = @($out) + '-- interop: what Linux sees' + @($ir.Out)
        $summary['interop-linux'] = if ($ir.Rc -eq 0) { '0 failed' } else { "$($ir.Rc) failed" }
    }
    wsl --unmount "\\?\$Vhd" | Out-Null
    $out | ForEach-Object { "   $_" }
    Attach-Disk
    Start-Sleep 3
    $t = Start-GuestDriver
    "   driver restarted, letters back in $t ms"
    $summary['fsck+verify'] = if ($frc -eq 0) { '0 failed' } else { "FAILED (exit $frc)" }
}

# ---- 5. LUKS: ext4 inside LUKS1/LUKS2 through ext4ctl, then what Linux sees
if ($Luks) {
    $lv = $TestEnv.LuksVhd; $ls = $TestEnv.LuksSize; $lslot = $TestEnv.ScsiSlot + 1
    if (-not (Test-Path $lv)) {
        "-- LUKS: building the test disk (cryptsetup and LVM in WSL)"
        New-VHD -Path $lv -SizeBytes $ls -Dynamic | Out-Null
        wsl --mount --vhd $lv --bare | Out-Null
        $r = Invoke-WslScript 'luks-image.sh' "$ls '$($TestEnv.LuksPass)'"
        if ($r.Rc -ne 0) { throw "LUKS fixture preparation failed (exit $($r.Rc))" }
        wsl --unmount "\\?\$lv" | Out-Null
        $r.Out | Select-Object -Last 2 | ForEach-Object { "   $_" }
    }
    if (-not (Get-VMHardDiskDrive -VMName $Vm | Where-Object Path -eq $lv)) {
        Add-VMHardDiskDrive -VMName $Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $lslot -Path $lv
    }
    "-- LUKS: unlock, read, write, functional suite, lock (guest)"
    $r = Guest-PS 'luks.ps1' "-Pass $($TestEnv.LuksPass)"
    $n = $null
    $r.Out | ForEach-Object { if ($_ -match 'LUKS-WIN: (\d+) failed') { $n = $Matches[1] } }
    $summary['luks-win'] = if ($n -eq '0') { '0 failed' } else { "$(if ($n) { $n } else { 'did not finish,' }) failed" }
    "-- LUKS: what Linux sees"
    Get-VMHardDiskDrive -VMName $Vm | Where-Object Path -eq $lv | Remove-VMHardDiskDrive
    wsl --mount --vhd $lv --bare | Out-Null
    $r = Invoke-WslScript 'luks.sh' "$ls '$($TestEnv.LuksPass)'"
    wsl --unmount "\\?\$lv" | Out-Null
    $r.Out | ForEach-Object { "   $_" }
    $summary['luks-linux'] = if ($r.Rc -eq 0 -and $r.Out -match '^LUKS-LINUX: 0') { '0 failed' } else { '1 failed' }
}
# ---- 6. Linux features: a casefolded directory and chattr +i/+a, then what Linux sees
if ($Features) {
    $fv = $TestEnv.FeaturesVhd; $fs = $TestEnv.FeaturesSize; $fslot = $TestEnv.ScsiSlot + 2
    "-- Features: building the disk (mkfs -O casefold, debugfs, e2fsck -D in WSL)"
    Get-VMHardDiskDrive -VMName $Vm | Where-Object Path -eq $fv | Remove-VMHardDiskDrive
    if ((Test-Path $fv) -and (Get-VHD $fv).Size -ne $fs) { [IO.File]::Delete($fv) }   # the size identifies it in WSL
    if (-not (Test-Path $fv)) { New-VHD -Path $fv -SizeBytes $fs -Dynamic | Out-Null }
    wsl --mount --vhd $fv --bare | Out-Null
    $r = Invoke-WslScript 'features-image.sh' "$fs"
    if ($r.Rc -ne 0) { throw "Features fixture preparation failed (exit $($r.Rc))" }
    wsl --unmount "\\?\$fv" | Out-Null
    $r.Out | ForEach-Object { "   $_" }
    Add-VMHardDiskDrive -VMName $Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $fslot -Path $fv
    "-- Features: casefolded lookups and writes, chattr +i/+a (guest)"
    $r = Guest-PS 'features.ps1' ''
    $n = $null
    $r.Out | ForEach-Object { if ($_ -match 'FEATURES-WIN: (\d+) failed') { $n = $Matches[1] } }
    $summary['features-win'] = if ($n -eq '0') { '0 failed' } else { "$(if ($n) { $n } else { 'did not finish,' }) failed" }
    "-- Features: what Linux sees"
    Stop-GuestDriver
    Get-VMHardDiskDrive -VMName $Vm | Where-Object Path -eq $fv | Remove-VMHardDiskDrive
    wsl --mount --vhd $fv --bare | Out-Null
    $r = Invoke-WslScript 'features.sh' "$fs"
    wsl --unmount "\\?\$fv" | Out-Null
    $r.Out | ForEach-Object { "   $_" }
    $summary['features-linux'] = if ($r.Rc -eq 0 -and $r.Out -match '^FEATURES-LINUX: 0') { '0 failed' } else { '1 failed' }
    [void](Start-GuestDriver)
}
}
catch {
    "!! run aborted: $($_.Exception.Message)"
    $summary['aborted'] = $_.Exception.Message
}

# leave nothing behind in the guest
[void](Remote 'powershell -NoProfile -Command "foreach($f in ''ext4test.ps1'',''resize.ps1'',''ext4sys.ps1'',''ext4hold.ps1'',''stress.ps1'',''repro.ps1'',''pending.ps1'',''race.ps1'',''interop.ps1'',''security.ps1'',''luks.ps1'',''features.ps1'',''ext4ctl.exe'',''handle64.exe''){ $p=\"C:\Windows\Temp\$f\"; if([IO.File]::Exists($p)){ [IO.File]::Delete($p) } }"')

""
"== SUMMARY ($([math]::Round($total.Elapsed.TotalSeconds)) s)"
$bad = 0
foreach ($k in $summary.Keys) { "   {0,-12} {1}" -f $k, $summary[$k]; if ($summary[$k] -notmatch '^0 failed') { $bad++ } }
"== $(if ($bad -eq 0) { 'ALL PASSED' } else { "$bad SECTION(S) FAILED" })"
Stop-Transcript | Out-Null
exit $bad
