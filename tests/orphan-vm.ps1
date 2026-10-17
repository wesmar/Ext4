#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# orphan-vm.ps1 - the driver's orphan cleanup at mount, on the volumes
# tests/orphan-image.sh makes in WSL (real orphans of the kernel, a journal
# that needs recovery included, and ones made with debugfs). Each goes onto
# a fresh 128 MiB disk the guest mounts with the driver as built; then:
#   valid  writable, and e2fsck finds no orphan left; every name and byte
#          equals e2fsck's own processing of the same volume
#   bad    writable, the bad orphan recorded as an error in the superblock;
#          e2fsck leaves the rest as it leaves it on the original
#   ro     read-only (the orphan file is not sound), nothing of it changed
#
#   pwsh tests\orphan-vm.ps1 [-Generate] [-Images /root/orphans]
param([switch]$Generate, [string]$Images = '/root/orphans', [string]$Ip = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = Get-SshOptions
$target = "$($TestEnv.User)@$Ip"
$disk = Join-Path $PSScriptRoot 'orphan-mount.vhdx'
$slot = 4
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
$state, $expected, $mounted = '/dev/shm/orphan-state.img', '/dev/shm/orphan-expected.img', '/dev/shm/orphan-mounted.img'
$service = $TestEnv.Service
$attached = $false
$wslMounted = $false
$failed = @()
try {
    if ($Generate) {
        InWsl "sed 's/\r`$//' '$(WslPath "$PSScriptRoot\orphan-image.sh")' | bash -s -- $Images" | Select-Object -Last 1
    }
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null
    if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    $names = @((InWsl "ls $Images/*.img | xargs -n1 basename | sed 's/\.img`$//'") -split "`n" | Where-Object { $_ })
    "$($names.Count) orphan volumes"
    New-VHD -Path $disk -SizeBytes 134217728 -Dynamic | Out-Null
    foreach ($name in $names) {
        $kind = (InWsl "cat $Images/$name.kind").Trim()
        & wsl.exe --mount --vhd $disk --bare | Out-Null
        if ($LASTEXITCODE) { throw 'WSL attach failed' }
        $wslMounted = $true
        InWsl ("cp --sparse=always $Images/$name.img $state && cp --sparse=always $state $expected && " +
               "{ e2fsck -fy $expected >/dev/null 2>&1 || true; } && $fixture put $state") | Out-Null
        & wsl.exe --unmount "\\?\$disk" | Out-Null
        $wslMounted = $false

        Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $disk
        $attached = $true
        # the mount finishes the orphans; a probe file tells writable from not
        $seen = Guest @"
`$v = `$null
for (`$i = 0; `$i -lt 150 -and -not `$v; `$i++) {
    `$v = Get-Volume | Where-Object { `$_.FileSystemLabel -eq 'crashlog' -and `$_.DriveLetter }
    if (-not `$v) { Start-Sleep -Milliseconds 200 }
}
if (-not `$v) { 'not mounted' } else {
    `$root = "`$(`$v.DriveLetter):\"
    `$count = @(Get-ChildItem `$root -Recurse -Force -ErrorAction SilentlyContinue).Count
    `$probe = Join-Path `$root 'orphan-probe.tmp'
    try { [IO.File]::WriteAllText(`$probe, 'x'); Remove-Item -LiteralPath `$probe; `$w = 'writable' } catch { `$w = 'read-only' }
    "`$count names, `$w"
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

        $seenText = "$seen".Trim()
        $ok = $seenText -notmatch 'not mounted'
        $notes = @()
        # valid: e2fsck finds nothing more to finish; bad and ro: e2fsck may
        # repair what it repairs on the original too
        $mode = if ($kind -eq 'valid') { '--orphans-done' } else { '--lenient' }
        $verdict = @(& wsl.exe -u root -- python3 $replay --compare $expected $mounted $mode 2>&1)
        if ($LASTEXITCODE) { $ok = $false }
        $errors = (InWsl "dumpe2fs -h $mounted 2>/dev/null | grep -c 'Filesystem state:.*with errors' || true").Trim()
        $present = (InWsl "dumpe2fs -h $mounted 2>/dev/null | grep -c 'orphan_present' || true").Trim()
        switch ($kind) {
            'valid' {
                if ($seenText -notmatch 'writable' -or $errors -ne '0' -or $present -ne '0') { $ok = $false }
            }
            'bad' {
                $notes += "errors recorded: $errors"
                if ($seenText -notmatch 'writable' -or $errors -ne '1') { $ok = $false }
            }
            'ro' {
                $notes += "errors recorded: $errors, orphan_present kept: $present"
                if ($seenText -notmatch 'read-only' -or $errors -ne '1' -or $present -ne '1') { $ok = $false }
            }
        }
        "${name} [$kind]: $seenText; " + (($verdict + $notes) -join "`n  ")
        if (-not $ok) {
            $failed += $name
            InWsl "mkdir -p /root/orphan-failed && cp --sparse=always $mounted /root/orphan-failed/$name.img" | Out-Null
        }
    }
    if ($failed) { throw "Orphan volumes the driver handled wrongly: $($failed -join ' '); images in /root/orphan-failed" }
    "ORPHANS: $($names.Count) volumes mounted by the driver: ALL PASSED"
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
