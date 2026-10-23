#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# aging-vm.ps1 - how the block allocator lays out files on an aged volume.
#
# An empty 2 GiB ext4 volume (tests/aging-vm/image.sh) is aged through
# ext4.sys by a seeded run of creates and deletes (aging-vm/guest.ps1), then
# new files are written onto it - streams, copies, parallel streams, small
# files - and each phase reports its time and its files' fragments (runs of
# contiguous clusters; one per file is the ideal). Linux then checks the
# volume (e2fsck -fn, no recorded error) and reports how the free space lies
# (e2freefrag) and the measured files' extents (filefrag).
#
# The seed fixes every operation, so two driver images age the volume the
# same way and their numbers compare. Times under Driver Verifier are slower
# than without it; the fragments are not.
#
#   pwsh tests\aging-vm.ps1 [-Seed 1] [-Factor 3] [-Ip a.b.c.d] [-DriverImage <ext4.sys>]
#
# Exit code: the number of failures. A failed run keeps its disk as
# tests\aging-failed-<time>.vhdx.
param([int]$Seed = 1, [double]$Factor = 3, [string]$Ip = '', [string]$DriverImage = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=6')
$target = "$($TestEnv.User)@$Ip"
$service = $TestEnv.Service
$disk = Join-Path $PSScriptRoot 'aging-test.vhdx'
$slot = 7
$label = 'age4k'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory (Join-Path $PSScriptRoot 'logs') -Force | Out-Null
Start-Transcript -Path (Join-Path $PSScriptRoot "logs\aging-vm-$stamp.log") | Out-Null

if (Test-Path -LiteralPath $disk) { throw "Left over from an earlier run: $disk" }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'The fixture slot is occupied'
}
function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$agDir = WslPath "$PSScriptRoot\aging-vm"
function Image([string]$Arguments) {
    & wsl.exe -u root -- bash -c "sed 's/\r`$//' '$agDir/image.sh' | bash -s -- $Arguments" 2>&1 | ForEach-Object { "$_" }
    $script:imageRc = $LASTEXITCODE
}
function Guest([string]$Code) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; `$ProgressPreference='SilentlyContinue'; " + $Code))
    $output = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}

$attached = $false
$wslMounted = $false
$failed = 0
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    $deploy = if ($DriverImage) { @('-ImagePath', $DriverImage) } else { @() }
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" @deploy | Out-Null
    if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    $image = if ($DriverImage) { $DriverImage } else { Join-Path (Split-Path $PSScriptRoot) "bin\$($TestEnv.Driver)" }
    "== aging-vm $stamp, seed $Seed, factor $Factor, driver $((Get-FileHash -LiteralPath $image).Hash.Substring(0, 8))"
    # fixed: a dynamic disk is slower where it was never written, which
    # would time the placement's choice of region, not the driver
    New-VHD -Path $disk -SizeBytes 2214592512 -Fixed | Out-Null
    & wsl.exe --mount --vhd $disk --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL attach failed' }
    $wslMounted = $true
    Image 'make'
    if ($imageRc) { throw 'image.sh make failed' }
    & wsl.exe --unmount "\\?\$disk" | Out-Null
    $wslMounted = $false

    & scp @ssh -q "$PSScriptRoot\aging-vm\guest.ps1" "${target}:C:/Windows/Temp/aging-guest.ps1"
    if ($LASTEXITCODE) { throw 'scp failed' }
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $disk
    $attached = $true
    $letter = Guest @"
for (`$i = 0; `$i -lt 300; `$i++) {
    `$v = Get-Volume | Where-Object { `$_.FileSystemLabel -eq '$label' -and `$_.DriveLetter }
    if (`$v) { break }
    Start-Sleep -Milliseconds 200
}
`$v.DriveLetter
"@
    if ("$letter" -notmatch '^[A-Z]$') { throw "The driver did not mount $label" }

    "== the driver, on ${letter}:"
    & ssh @ssh $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\aging-guest.ps1 -Root ${letter}:\ -Seed $Seed -Factor $Factor" |
        Where-Object { $_ -notmatch 'CLIXML|<Obj' } | ForEach-Object { "   $_" }
    if ($LASTEXITCODE) { "!! the guest exited $LASTEXITCODE"; $failed++ }
    Guest "sc.exe stop '$service' | Out-Null; (Get-Service '$service').WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))" | Out-Null
    Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive
    $attached = $false
    Guest "sc.exe start '$service' | Out-Null" | Out-Null

    "== Linux"
    & wsl.exe --mount --vhd $disk --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL read-back attach failed' }
    $wslMounted = $true
    Image 'check'
    if ($imageRc) { "!! image.sh check exited $imageRc"; $failed++ }
} catch {
    "!! $_"
    $failed++
} finally {
    if ($attached) {
        try { Guest "sc.exe stop '$service' | Out-Null; (Get-Service '$service').WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))" | Out-Null } catch { }
        Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive
        try { Guest "sc.exe start '$service' | Out-Null" | Out-Null } catch { }
    }
    if ($wslMounted) { & wsl.exe --unmount "\\?\$disk" | Out-Null }
    try { Guest "Remove-Item -Force C:\Windows\Temp\aging-guest.ps1" | Out-Null } catch { }
    if ($DriverImage) { pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null }
    if (Test-Path -LiteralPath $disk) {
        if ($failed) {
            $keep = Join-Path $PSScriptRoot "aging-failed-$stamp.vhdx"
            Move-Item -LiteralPath $disk $keep
            "   the disk is kept: $keep"
        } else {
            Remove-Item -LiteralPath $disk
        }
    }
}
"AGING-VM: $failed failed ($([int]$clock.Elapsed.TotalSeconds) s)"
Stop-Transcript | Out-Null
exit $failed
