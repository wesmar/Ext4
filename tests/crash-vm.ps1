#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# crash-vm.ps1 - every crash state of one run. A new 128 MiB fixture gets an
# ext4 volume labelled crashlog (crash-image.sh); a build with EXT4_WRITE_LOG
# records each write and flush the driver sends to it while the guest runs
# crash-workload.ps1; crash-replay.py then replays every prefix of that
# record onto the volume as it was and lets e2fsck judge it.
#
#   $env:CL = '/DEXT4_WRITE_LOG'; .\build.ps1 -Rebuild   (then keep that ext4.sys aside)
#   pwsh tests\crash-vm.ps1 -Image <new .vhdx> -LogDriver <that ext4.sys> [-Every N] [-Churn N]
param([Parameter(Mandatory)][string]$Image, [Parameter(Mandatory)][string]$LogDriver,
      [int]$Every = 1, [int]$Churn = 0, [string]$Ip = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = Get-SshOptions
$target = "$($TestEnv.User)@$Ip"
$image = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Image)
$logDriver = (Resolve-Path -LiteralPath $LogDriver).Path
$bin = (Resolve-Path (Join-Path $PSScriptRoot '..\bin\ext4.sys')).Path
$work = '/root/crash/' + [IO.Path]::GetFileNameWithoutExtension($image)
$slot = 4
if (Test-Path -LiteralPath $image) { throw 'The fixture exists already: give a new path' }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'The fixture slot is occupied'
}
function Guest([string]$Code) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; " + $Code))
    $output = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}
function InWsl([string]$Command) {
    & wsl.exe -u root -- bash -c $Command 2>&1
    if ($LASTEXITCODE) { throw "WSL failed: $Command" }
}
function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$saved = "$bin.crash-saved"
$attached = $false
$wslMounted = $false
try {
    New-VHD -Path $image -SizeBytes 134217728 -Dynamic | Out-Null
    & wsl.exe --mount --vhd $image --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL attach failed' }
    $wslMounted = $true
    InWsl "sed 's/\r`$//' '$(WslPath "$PSScriptRoot\crash-image.sh")' | bash -s -- prepare $work"
    & wsl.exe --unmount "\\?\$image" | Out-Null
    $wslMounted = $false

    # the recording build, for this run only
    Copy-Item -LiteralPath $bin -Destination $saved -Force
    Copy-Item -LiteralPath $logDriver -Destination $bin -Force
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $image
    $attached = $true
    & scp -q @ssh "$PSScriptRoot\crash-workload.ps1" "${target}:C:/Windows/Temp/crash-workload.ps1"
    $out = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\crash-workload.ps1 -Churn $Churn")
    $out
    if ($out -notcontains 'CRASH-WORKLOAD: done') { throw 'The workload did not finish' }
    # the record is written when the driver unloads
    Guest "sc.exe stop '$($TestEnv.Service)' | Out-Null; (Get-Service '$($TestEnv.Service)').WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))" | Out-Null
    $log = Join-Path ([IO.Path]::GetDirectoryName($image)) ([IO.Path]::GetFileNameWithoutExtension($image) + '.wlog')
    & scp -q @ssh "${target}:C:/Windows/Temp/ext4-writelog.bin" $log
    if ($LASTEXITCODE) { throw 'No write log in the guest' }
    Guest "Remove-Item C:\Windows\Temp\ext4-writelog.bin, C:\Windows\Temp\crash-workload.ps1" | Out-Null
    Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $image } | Remove-VMHardDiskDrive
    $attached = $false

    & wsl.exe --mount --vhd $image --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL read-back attach failed' }
    $wslMounted = $true
    InWsl "sed 's/\r`$//' '$(WslPath "$PSScriptRoot\crash-image.sh")' | bash -s -- final $work"
    & wsl.exe --unmount "\\?\$image" | Out-Null
    $wslMounted = $false
    & wsl.exe -u root -- python3 (WslPath "$PSScriptRoot\crash-replay.py") "$work/base.img" (WslPath $log) --final "$work/final.img" --every $Every --reorder --keep "$work/rejected"
    $replay = $LASTEXITCODE
    if ($replay) { throw "Crash states rejected; images in $work/rejected" }
    'CRASH: every crash state of the run checked: ALL PASSED'
} finally {
    if ($attached) {
        try { Guest "sc.exe stop '$($TestEnv.Service)' | Out-Null" | Out-Null } catch { }
        Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $image } | Remove-VMHardDiskDrive
    }
    if ($wslMounted) { & wsl.exe --unmount "\\?\$image" | Out-Null }
    if (Test-Path -LiteralPath $saved) {
        Move-Item -LiteralPath $saved -Destination $bin -Force
        pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null
    }
}
