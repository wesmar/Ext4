#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# filedisk-vm.ps1 - ext4 volumes in image files, through ext4ctl and ext4.sys.
#
# Three images made by mkfs.ext4 (filedisk-vm/image.sh) go to the guest and are
# unpacked on NTFS as files with holes. The guest (filedisk-vm/guest.ps1)
# mounts two at once, fills them, trims them and watches the space go back to
# NTFS, runs fsx on them, alone and with trims running all the time, tries what
# must be refused, stops the service with an image mounted, and runs fsx on a
# volume of 1 KiB blocks in a file on an NTFS of 4 KiB sectors. The images come back to WSL: e2fsck -fn, no error the
# driver recorded, every file byte-exact as Linux reads it.
#
#   pwsh tests\filedisk-vm.ps1 [-Seeds 6] [-Ip a.b.c.d] [-DriverImage <ext4.sys>]
#
# Exit code: the number of failures.
param([int]$Seeds = 6, [string]$Ip = '', [string]$DriverImage = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=6')
$target = "$($TestEnv.User)@$Ip"
$service = $TestEnv.Service
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $PSScriptRoot "filedisk-work-$stamp"
$sectors = Join-Path $PSScriptRoot "filedisk-4k-$stamp.vhdx"
$slot = 6
$attached = $false
$dir = 'C:\fd'
$ctl = 'C:\Windows\Temp\ext4ctl.exe'
New-Item -ItemType Directory (Join-Path $PSScriptRoot 'logs') -Force | Out-Null
Start-Transcript -Path (Join-Path $PSScriptRoot "logs\filedisk-vm-$stamp.log") | Out-Null

function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$fdDir = WslPath "$PSScriptRoot\filedisk-vm"
function Image([string]$Arguments) {
    & wsl.exe -u root -- bash -c "sed 's/\r`$//' '$fdDir/image.sh' | bash -s -- $Arguments" 2>&1 | ForEach-Object { "$_" }
    $script:imageRc = $LASTEXITCODE
}
function Guest([string]$Code) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; `$ProgressPreference='SilentlyContinue'; " + $Code))
    $output = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}
function Stage([string]$Name) {
    "== $Name"
    & ssh @ssh $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\fd-guest.ps1 -Dir $dir -Ctl $ctl -Stage $Name -Fsx C:\Windows\Temp\fd-fsx.ps1 -Seeds $Seeds" 2>&1 |
        Where-Object { "$_" -notmatch 'CLIXML|<Obj' } | ForEach-Object { "$_" }
    if ($LASTEXITCODE) { $script:failed += [Math]::Max(1, $LASTEXITCODE) }
}

$failed = 0
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    "== filedisk-vm $stamp"
    $deploy = if ($DriverImage) { @('-ImagePath', $DriverImage) } else { @() }
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" @deploy | Out-Null
    if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }

    New-Item -ItemType Directory $work | Out-Null
    Image "make '$(WslPath $work)'"
    if ($imageRc) { throw 'image.sh make failed' }

    Guest "if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }; New-Item -ItemType Directory $dir | Out-Null"
    & scp @ssh -q "$PSScriptRoot\filedisk-vm\guest.ps1" "${target}:C:/Windows/Temp/fd-guest.ps1"
    & scp @ssh -q "$PSScriptRoot\fsx-vm\guest.ps1" "${target}:C:/Windows/Temp/fd-fsx.ps1"
    & scp @ssh -q (Join-Path (Split-Path $PSScriptRoot) 'bin\ext4ctl.exe') "${target}:$($ctl.Replace('\', '/'))"
    foreach ($n in 'fd4k', 'fd1k', 'fdplain') {
        & scp @ssh -q "$work\$n.img.gz" "${target}:C:/fd/$n.img.gz"
        if ($LASTEXITCODE) { throw 'scp failed' }
    }

    foreach ($s in 'unpack', 'mount', 'trim', 'fsx', 'race', 'busy', 'final', 'unload', 'pack') { Stage $s }

    if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
        throw 'The fixture slot is occupied'
    }
    New-VHD -Path $sectors -SizeBytes 2GB -Dynamic -LogicalSectorSizeBytes 4096 -PhysicalSectorSizeBytes 4096 | Out-Null
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $sectors
    $attached = $true
    Stage 'sector4k'

    foreach ($n in 'fd4k', 'fd1k', 'fd4n') {
        & scp @ssh -q "${target}:C:/fd/$n.back.gz" "$work\$n.back.gz"
    }
    "== Linux"
    Image "check '$(WslPath $work)'"
    $failed += $imageRc
} catch {
    "!! $_"
    $failed++
} finally {
    try {
        Guest "foreach ($('$')v in 'R:', 'S:', 'T:') { cmd /c $ctl unmount $('$')v --force '2>nul' | Out-Null }; Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue; Remove-Item -Force C:\Windows\Temp\fd-guest.ps1, C:\Windows\Temp\fd-fsx.ps1, $ctl -ErrorAction SilentlyContinue" | Out-Null
    } catch { }
    if ($attached) { Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $sectors } | Remove-VMHardDiskDrive }
    if (Test-Path -LiteralPath $sectors) { Remove-Item -LiteralPath $sectors }
    if ($DriverImage) { pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null }
    if (Test-Path -LiteralPath $work) { Remove-Item -Recurse -Force $work }
}
"FILEDISK-VM: $failed failed ($([int]$clock.Elapsed.TotalSeconds) s)"
Stop-Transcript | Out-Null
exit $failed
