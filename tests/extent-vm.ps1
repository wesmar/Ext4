#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# extent-vm.ps1 - extent trees built by ext4.sys, checked by Linux.
#
# One disk of two ext4 volumes, 4 KiB and 1 KiB blocks (tests/extent-vm/
# image.sh). The guest (extent-vm/guest.ps1) writes across the holes and the
# data of a sparse file Linux made; preallocates over blocks a deleted file
# of 0xAB left and moves the end of the file into them; cuts a preallocated
# file inside its unwritten extent; writes a preallocation at random pieces;
# grows two files 4 KiB at a time in turn (thousands of extents, trees two
# and three levels deep) and cuts one back. Every result is checked in the
# guest, then in WSL: e2fsck -fn, no error the driver recorded, every file
# byte-exact as Linux reads it.
#
#   pwsh tests\extent-vm.ps1 [-Fragments 6000] [-Ip a.b.c.d] [-DriverImage <another ext4.sys>] [-Cases stale,cut]
#
# -DriverImage runs another build (an older release: the checks must catch
# what it got wrong); bin\ext4.sys is put back afterwards.
#
# Exit code: the number of failed checks. A failed run keeps its disk as
# tests\extent-failed-<time>.vhdx.
param([int]$Fragments = 6000, [string]$Ip = '', [string]$DriverImage = '', [string]$Cases = 'holes,stale,cut,pieces,fragments')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=6')
$target = "$($TestEnv.User)@$Ip"
$service = $TestEnv.Service
$disk = Join-Path $PSScriptRoot 'extent-test.vhdx'
$slot = 6
$labels = @('ex4k', 'ex1k')
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory (Join-Path $PSScriptRoot 'logs') -Force | Out-Null
Start-Transcript -Path (Join-Path $PSScriptRoot "logs\extent-vm-$stamp.log") | Out-Null

if (Test-Path -LiteralPath $disk) { throw "Left over from an earlier run: $disk" }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'The fixture slot is occupied'
}
function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$exDir = WslPath "$PSScriptRoot\extent-vm"
function Image([string]$Arguments) {
    & wsl.exe -u root -- bash -c "sed 's/\r`$//' '$exDir/image.sh' | bash -s -- $Arguments" 2>&1 | ForEach-Object { "$_" }
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
    "== extent-vm $stamp, $Fragments fragments"
    if ($DriverImage) {
        pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" -ImagePath $DriverImage | Out-Null
        if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    } else {
        pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null
        if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    }
    New-VHD -Path $disk -SizeBytes 1744830464 -Dynamic | Out-Null
    & wsl.exe --mount --vhd $disk --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL attach failed' }
    $wslMounted = $true
    Image 'make'
    if ($imageRc) { throw 'image.sh make failed' }
    & wsl.exe --unmount "\\?\$disk" | Out-Null
    $wslMounted = $false

    & scp @ssh -q "$PSScriptRoot\extent-vm\guest.ps1" "${target}:C:/Windows/Temp/ex-guest.ps1"
    if ($LASTEXITCODE) { throw 'scp failed' }
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $disk
    $attached = $true
    $want = ($labels | ForEach-Object { "'$_'" }) -join ','
    $found = Guest @"
`$want = @($want)
for (`$i = 0; `$i -lt 300; `$i++) {
    `$v = @(Get-Volume | Where-Object { `$want -contains `$_.FileSystemLabel -and `$_.DriveLetter })
    if (`$v.Count -eq `$want.Count) { break }
    Start-Sleep -Milliseconds 200
}
`$v | ForEach-Object { "`$(`$_.FileSystemLabel)=`$(`$_.DriveLetter)" }
"@
    $letters = @{}
    foreach ($f in $found) { $k, $v = "$f".Split('='); $letters[$k] = $v }
    if ($letters.Count -ne $labels.Count) { throw "Volumes the driver mounted: $($found -join ' ')" }

    "== the driver"
    foreach ($label in $labels) {
        "-- $label on $($letters[$label]):"
        & ssh @ssh $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\ex-guest.ps1 -Root $($letters[$label]):\ -Seed 1 -Fragments $Fragments -Cases $Cases" |
            ForEach-Object { "   $_" }
        if ($LASTEXITCODE) { $failed += [Math]::Max(1, $LASTEXITCODE) }
    }
    Guest "sc.exe stop '$service' | Out-Null; (Get-Service '$service').WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))" | Out-Null
    Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive
    $attached = $false
    Guest "sc.exe start '$service' | Out-Null" | Out-Null

    "== Linux"
    & wsl.exe --mount --vhd $disk --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL read-back attach failed' }
    $wslMounted = $true
    Image 'check'
    $failed += $imageRc
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
    try { Guest "Remove-Item -Force C:\Windows\Temp\ex-guest.ps1" | Out-Null } catch { }
    if ($DriverImage) { pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null }
    if (Test-Path -LiteralPath $disk) {
        if ($failed) {
            $keep = Join-Path $PSScriptRoot "extent-failed-$stamp.vhdx"
            Move-Item -LiteralPath $disk $keep
            "   the disk is kept: $keep"
        } else {
            Remove-Item -LiteralPath $disk
        }
    }
}
"EXTENT-VM: $failed failed ($([int]$clock.Elapsed.TotalSeconds) s)"
Stop-Transcript | Out-Null
exit $failed
