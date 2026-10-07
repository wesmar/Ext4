#Requires -Version 7.0
# SPDX-License-Identifier: GPL-2.0-only
# corrupt-vm.ps1 - damage planted on purpose (corrupt-image.sh), what the
# driver does with it in the guest (corrupt.ps1), then checked from Linux:
# the superblock records the errors, new data went around the corrupt
# group, a remount-ro volume was not written after its error.
#
#   pwsh tests\corrupt-vm.ps1 -Image <new .vhdx path>
param([string]$Ip = '', [Parameter(Mandatory = $true)][string]$Image)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$sshOptions = Get-SshOptions
$target = "$($TestEnv.User)@$Ip"
$size = 419430400          # identifies the disk inside WSL
$slot = 6
$Image = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Image)
if (Test-Path -LiteralPath $Image) { throw 'The fixture exists already: give a new path (fixtures are kept as evidence)' }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerType -eq 'SCSI' -and $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw "SCSI slot $slot of the guest is taken"
}
function Guest([string]$Code) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; " + $Code))
    $output = @(& ssh @sshOptions $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}
function StopDriver {
    Guest "`$s=Get-Service '$($TestEnv.Service)'; if (`$s.Status -ne 'Stopped') { sc.exe stop '$($TestEnv.Service)' | Out-Null; `$s.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(120)) }" | Out-Null
}
function StartDriver {
    Guest "`$s=Get-Service '$($TestEnv.Service)'; if (`$s.Status -eq 'Stopped') { sc.exe start '$($TestEnv.Service)' | Out-Null }; `$s.WaitForStatus('Running',[TimeSpan]::FromSeconds(30))" | Out-Null
}
function Linux([string]$Phase) {
    $path = '/mnt/' + $PSScriptRoot.Substring(0, 1).ToLower() + $PSScriptRoot.Substring(2).Replace('\', '/') + '/corrupt-image.sh'
    $out = @(& wsl -u root -- bash -c "sed 's/\r`$//' '$path' > /tmp/corrupt-image.sh && bash /tmp/corrupt-image.sh $Phase $size" 2>&1)
    $rc = $LASTEXITCODE
    $out | ForEach-Object { "   $_" }
    if ($rc) { throw "Linux $Phase failed ($rc)" }
}
$attached = $false
$wslMounted = $false
$wasRunning = (Guest "(Get-Service '$($TestEnv.Service)').Status.ToString()") -contains 'Running'
try {
    New-VHD -Path $Image -SizeBytes $size -Dynamic | Out-Null
    & wsl --mount --vhd $Image --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL attach failed' }
    $wslMounted = $true
    "-- Linux plants the damage"
    Linux prepare
    & wsl --unmount "\\?\$Image" | Out-Null
    $wslMounted = $false
    StopDriver
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $Image
    $attached = $true
    & scp -q @sshOptions "$PSScriptRoot/corrupt.ps1" "${target}:C:/Windows/Temp/corrupt.ps1"
    if ($LASTEXITCODE) { throw 'Guest script transfer failed' }
    StartDriver
    "-- the guest finds it"
    $out = @(& ssh @sshOptions $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\corrupt.ps1")
    $rc = $LASTEXITCODE
    $out | ForEach-Object { "   $_" }
    if ($rc -or $out -notcontains 'CORRUPT-WIN: 0 failed') { $guestFailed = $true }
    StopDriver
    Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $Image } | Remove-VMHardDiskDrive
    $attached = $false
    & wsl --mount --vhd $Image --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL read-back attach failed' }
    $wslMounted = $true
    "-- what Linux sees"
    Linux check
    if ($guestFailed) { throw 'The guest side failed' }
    'CORRUPT: planted damage found, reported and contained: ALL PASSED'
} finally {
    if ($attached) {
        StopDriver
        Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $Image } | Remove-VMHardDiskDrive
    }
    if ($wslMounted) { & wsl --unmount "\\?\$Image" | Out-Null }
    & ssh @sshOptions $target "powershell -NoProfile -Command `"Remove-Item -LiteralPath C:\Windows\Temp\corrupt.ps1 -ErrorAction SilentlyContinue`"" | Out-Null
    if ($wasRunning) { StartDriver }
}
