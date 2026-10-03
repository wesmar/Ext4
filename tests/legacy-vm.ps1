#Requires -Version 7.0
# SPDX-License-Identifier: GPL-2.0-only
# One short ext2/ext3 write/read-back run on a new dedicated Hyper-V fixture.
param([string]$Ip = '')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip=Get-TestVmIp }
$sshOptions=Get-SshOptions
$target="$($TestEnv.User)@$Ip"
$image=Join-Path $PSScriptRoot 'legacy-smoke.vhdx'
$slot=4
if (Test-Path -LiteralPath $image) { throw 'Fixture already exists; retain it or choose a new path before running' }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerType -eq 'SCSI' -and $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'Dedicated fixture slot is occupied'
}
function Guest([string]$Code) {
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; "+$Code))
    $output=@(& ssh @sshOptions $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}
function StopDriver {
    Guest @"
`$s=Get-Service '$($TestEnv.Service)'
if (`$s.Status -ne 'Stopped') {
    sc.exe stop '$($TestEnv.Service)' | Out-Null
    if (`$LASTEXITCODE) { throw 'Stop failed' }
    `$s.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(120))
}
"@ | Out-Null
}
function StartDriver {
    Guest @"
`$s=Get-Service '$($TestEnv.Service)'
if (`$s.Status -eq 'Stopped') {
    sc.exe start '$($TestEnv.Service)' | Out-Null
    if (`$LASTEXITCODE) { throw 'Start failed' }
}
`$s.WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
"@ | Out-Null
}
function Linux([string]$Phase) {
    $path='/mnt/'+$PSScriptRoot.Substring(0,1).ToLower()+$PSScriptRoot.Substring(2).Replace('\','/')+'/legacy-image.sh'
    & wsl -u root -- bash -c "sed 's/\r\$//' '$path' | bash -s -- $Phase" 2>&1
    if ($LASTEXITCODE) { throw "Linux $Phase failed" }
}
function Probe([string]$Phase) {
    $out=@(& ssh @sshOptions $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\legacy.ps1 -Phase $Phase")
    $rc=$LASTEXITCODE
    $out
    if ($rc -or $out -notcontains "LEGACY-WIN: $Phase ALL PASSED") { throw "Legacy $Phase did not pass" }
}
$attached=$false
$wslMounted=$false
$wasRunning=(Guest "(Get-Service '$($TestEnv.Service)').Status.ToString()") -contains 'Running'
try {
    New-VHD -Path $image -SizeBytes 402653184 -Dynamic | Out-Null
    & wsl --mount --vhd $image --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL attach failed' }
    $wslMounted=$true
    Linux prepare
    & wsl --unmount "\\?\$image" | Out-Null
    if ($LASTEXITCODE) { throw 'WSL detach failed' }
    $wslMounted=$false
    StopDriver
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $image
    $attached=$true
    & scp -q @sshOptions "$PSScriptRoot/legacy.ps1" "${target}:C:/Windows/Temp/legacy.ps1"
    if ($LASTEXITCODE) { throw 'Guest script transfer failed' }
    StartDriver
    Probe Write
    StopDriver
    StartDriver
    Probe Verify
    StopDriver
    Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $image } | Remove-VMHardDiskDrive
    $attached=$false
    & wsl --mount --vhd $image --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL read-back attach failed' }
    $wslMounted=$true
    Linux check
    'LEGACY: real ext2/ext3 write, resize, rename, delete, remount and Linux checks: ALL PASSED'
} finally {
    if ($attached) {
        StopDriver
        Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $image } | Remove-VMHardDiskDrive
    }
    if ($wslMounted) { & wsl --unmount "\\?\$image" | Out-Null }
    if ($wasRunning) { StartDriver }
    # Retain the fixture as evidence; no automatic deletion or repair.
}
