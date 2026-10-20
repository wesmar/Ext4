#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# fsx-vm.ps1 - random file operations through ext4.sys, each checked against
# a model, then the volumes checked by Linux.
#
# One disk of two empty ext4 volumes, 4 KiB and 1 KiB blocks (tests/fsx-vm/
# image.sh). On each, the guest (fsx-vm/guest.ps1) runs -Seeds seeds of -Ops
# operations: writes cached, write-through and unbuffered, anywhere; reads;
# the end of file moved; the allocation size set; mapped writes and reads;
# flushes; handles reopened - every result compared with the model of what
# the files hold. A seed fixes its operations: a failure names the seed and
# the operation, and -FirstSeed repeats it. Then WSL: e2fsck -fn, no error
# the driver recorded, every kept file byte-exact as Linux reads it.
#
#   pwsh tests\fsx-vm.ps1 [-FirstSeed 1] [-Seeds 40] [-Ops 1500] [-Ip a.b.c.d] [-DriverImage <ext4.sys>]
#
# Exit code: the number of failures. A failed run keeps its disk as
# tests\fsx-failed-<time>.vhdx.
param([int]$FirstSeed = 1, [int]$Seeds = 40, [int]$Ops = 1500, [string]$Ip = '', [string]$DriverImage = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=6')
$target = "$($TestEnv.User)@$Ip"
$service = $TestEnv.Service
$disk = Join-Path $PSScriptRoot 'fsx-test.vhdx'
$slot = 6
$labels = @('fx4k', 'fx1k')
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory (Join-Path $PSScriptRoot 'logs') -Force | Out-Null
Start-Transcript -Path (Join-Path $PSScriptRoot "logs\fsx-vm-$stamp.log") | Out-Null

if (Test-Path -LiteralPath $disk) { throw "Left over from an earlier run: $disk" }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'The fixture slot is occupied'
}
function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$fxDir = WslPath "$PSScriptRoot\fsx-vm"
function Image([string]$Arguments) {
    & wsl.exe -u root -- bash -c "sed 's/\r`$//' '$fxDir/image.sh' | bash -s -- $Arguments" 2>&1 | ForEach-Object { "$_" }
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
    "== fsx-vm $stamp, seeds $FirstSeed..$($FirstSeed + $Seeds - 1), $Ops operations each"
    $deploy = if ($DriverImage) { @('-ImagePath', $DriverImage) } else { @() }
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" @deploy | Out-Null
    if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    New-VHD -Path $disk -SizeBytes 939524096 -Dynamic | Out-Null
    & wsl.exe --mount --vhd $disk --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL attach failed' }
    $wslMounted = $true
    Image 'make'
    if ($imageRc) { throw 'image.sh make failed' }
    & wsl.exe --unmount "\\?\$disk" | Out-Null
    $wslMounted = $false

    & scp @ssh -q "$PSScriptRoot\fsx-vm\guest.ps1" "${target}:C:/Windows/Temp/fsx-guest.ps1"
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
        & ssh @ssh $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\fsx-guest.ps1 -Root $($letters[$label]):\ -FirstSeed $FirstSeed -Seeds $Seeds -Ops $Ops" |
            Where-Object { $_ -notmatch 'CLIXML|<Obj' } | ForEach-Object { "   $_" }
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
    try { Guest "Remove-Item -Force C:\Windows\Temp\fsx-guest.ps1" | Out-Null } catch { }
    if ($DriverImage) { pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null }
    if (Test-Path -LiteralPath $disk) {
        if ($failed) {
            $keep = Join-Path $PSScriptRoot "fsx-failed-$stamp.vhdx"
            Move-Item -LiteralPath $disk $keep
            "   the disk is kept: $keep"
        } else {
            Remove-Item -LiteralPath $disk
        }
    }
}
"FSX-VM: $failed failed ($([int]$clock.Elapsed.TotalSeconds) s)"
Stop-Transcript | Out-Null
exit $failed
