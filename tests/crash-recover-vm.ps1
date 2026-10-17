#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# crash-recover-vm.ps1 - the driver's own journal replay, on the crash states
# of a crash-vm.ps1 run. Each picked state (crash-replay.py --pick: around
# commit blocks, spread over the run) goes onto a fresh 128 MiB disk the guest
# then mounts with the driver as built (bin\ext4.sys): the mount replays the
# journal, the listing walks the volume, the unload writes it back. The result
# must be clean for e2fsck -fn and hold every name and every byte e2fsck's own
# replay of the same state holds.
#
#   pwsh tests\crash-recover-vm.ps1 -Run <the .vhdx of a crash-vm.ps1 run> [-Count 40]
param([Parameter(Mandatory)][string]$Run, [int]$Count = 40, [string]$Ip = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = Get-SshOptions
$target = "$($TestEnv.User)@$Ip"
$run = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Run)
$name = [IO.Path]::GetFileNameWithoutExtension($run)
$log = Join-Path ([IO.Path]::GetDirectoryName($run)) "$name.wlog"
$disk = Join-Path ([IO.Path]::GetDirectoryName($run)) "$name-recover.vhdx"
$work = "/root/crash/$name"
$slot = 4
if (-not (Test-Path -LiteralPath $log)) { throw "No write log: $log" }
if (Test-Path -LiteralPath $disk) { throw "Left over from an earlier run: $disk" }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'The fixture slot is occupied'
}
function Guest([string]$Code) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; `$ProgressPreference='SilentlyContinue'; " + $Code))
    $output = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}
function InWsl([string]$Command) {
    & wsl.exe -u root -- bash -c $Command 2>&1
    if ($LASTEXITCODE) { throw "WSL failed: $Command" }
}
function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$replay = WslPath "$PSScriptRoot\crash-replay.py"
$fixture = "sed 's/\r`$//' '$(WslPath "$PSScriptRoot\crash-image.sh")' | bash -s --"
$state, $expected, $mounted = '/dev/shm/recover-state.img', '/dev/shm/recover-expected.img', '/dev/shm/recover-mounted.img'
$service = $TestEnv.Service
$attached = $false
$wslMounted = $false
$failed = @()
try {
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null
    if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    $picked = @((InWsl "python3 '$replay' '$work/base.img' '$(WslPath $log)' --pick $Count") -split ' ' | ForEach-Object { [int]$_ })
    "$($picked.Count) states of $name"
    New-VHD -Path $disk -SizeBytes 134217728 -Dynamic | Out-Null
    foreach ($k in $picked) {
        & wsl.exe --mount --vhd $disk --bare | Out-Null
        if ($LASTEXITCODE) { throw 'WSL attach failed' }
        $wslMounted = $true
        InWsl ("python3 '$replay' '$work/base.img' '$(WslPath $log)' --state $k --out $state && " +
               "cp $state $expected && { e2fsck -fy $expected >/dev/null 2>&1 || true; } && $fixture put $state") | Out-Null
        & wsl.exe --unmount "\\?\$disk" | Out-Null
        $wslMounted = $false

        Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $disk
        $attached = $true
        # the mount replays the journal; the walk reads every directory and inode
        $seen = Guest @"
`$v = `$null
for (`$i = 0; `$i -lt 150 -and -not `$v; `$i++) {
    `$v = Get-Volume | Where-Object { `$_.FileSystemLabel -eq 'crashlog' -and `$_.DriveLetter }
    if (-not `$v) { Start-Sleep -Milliseconds 200 }
}
if (-not `$v) { 'not mounted' } else {
    @(Get-ChildItem "`$(`$v.DriveLetter):\" -Recurse -Force -ErrorAction SilentlyContinue).Count
}
sc.exe stop '$service' | Out-Null
(Get-Service '$service').WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))
"@
        Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive
        $attached = $false
        Guest "sc.exe start '$service' | Out-Null" | Out-Null

        & wsl.exe --mount --vhd $disk --bare | Out-Null
        if ($LASTEXITCODE) { throw 'WSL read-back attach failed' }
        $wslMounted = $true
        InWsl "$fixture get $mounted" | Out-Null
        & wsl.exe --unmount "\\?\$disk" | Out-Null
        $wslMounted = $false
        $verdict = @(& wsl.exe -u root -- python3 $replay --compare $expected $mounted 2>&1)
        $ok = -not $LASTEXITCODE -and "$seen" -notmatch 'not mounted'
        "state ${k}: $("$seen".Trim()) names listed; " + ($verdict -join "`n  ")
        if (-not $ok) {
            $failed += $k
            InWsl "mkdir -p $work/recover && cp $state $work/recover/state-$k.img && cp $mounted $work/recover/mounted-$k.img" | Out-Null
        }
    }
    if ($failed) { throw "States the driver recovered differently: $($failed -join ' '); images in $work/recover" }
    "RECOVER: $($picked.Count) crash states mounted by the driver: ALL PASSED"
} finally {
    if ($attached) {
        try { Guest "sc.exe stop '$service' | Out-Null" | Out-Null } catch { }
        Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive
        try { Guest "sc.exe start '$service' | Out-Null" | Out-Null } catch { }
    }
    if ($wslMounted) { & wsl.exe --unmount "\\?\$disk" | Out-Null }
    if (Test-Path -LiteralPath $disk) { Remove-Item -LiteralPath $disk }
    InWsl "rm -f $state $expected $mounted" | Out-Null
}
