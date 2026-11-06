#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# sparse-vm.ps1 - sparse files through ext4.sys, NTFS the reference.
#
# The guest (sparse-vm/guest.ps1) runs the same cases on an NTFS directory and
# on two ext4 volumes, 4 KiB and 1 KiB blocks (sparse-vm/image.sh): error
# codes, ranges and attributes must be the ones NTFS answers with, bytes and
# space are checked in the guest. Files are left, the volumes go and come back
# (the mark must be the disk's, not the driver's), a full volume is tried, then
# WSL: e2fsck -fn, no error the driver recorded, bytes, holes as SEEK_HOLE
# shows them, the mark, and Linux changing a file the driver punched.
#
#   pwsh tests\sparse-vm.ps1 [-Cases basic,edges] [-Ip a.b.c.d] [-DriverImage <ext4.sys>]
#
# Exit code: the number of failures. A failed run keeps its disk as
# tests\sparse-failed-<time>.vhdx.
param([string]$Cases = '', [string]$Ip = '', [string]$DriverImage = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=6')
$target = "$($TestEnv.User)@$Ip"
$service = $TestEnv.Service
$disk = Join-Path $PSScriptRoot 'sparse-test.vhdx'
$slot = 6
$labels = @('sp4k', 'sp1k')
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory (Join-Path $PSScriptRoot 'logs') -Force | Out-Null
Start-Transcript -Path (Join-Path $PSScriptRoot "logs\sparse-vm-$stamp.log") | Out-Null

if (Test-Path -LiteralPath $disk) { throw "Left over from an earlier run: $disk" }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'The fixture slot is occupied'
}
function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$spDir = WslPath "$PSScriptRoot\sparse-vm"
function Image([string]$Arguments) {
    & wsl.exe -u root -- bash -c "sed 's/\r`$//' '$spDir/image.sh' | bash -s -- $Arguments" 2>&1 | ForEach-Object { "$_" }
    $script:imageRc = $LASTEXITCODE
}
function Guest([string]$Code) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; `$ProgressPreference='SilentlyContinue'; " + $Code))
    $output = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}
# the guest on Root; its lines, and the exit code in $script:guestRc
function Run([string]$Root, [string]$Arguments) {
    $script:guestLines = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\sp-guest.ps1 -Root $Root $Arguments" 2>&1 | ForEach-Object { "$_" })
    $script:guestRc = $LASTEXITCODE
}
function Answers([string[]]$Lines) {
    $a = [ordered]@{}
    foreach ($l in $Lines) { if ($l -match '^P (\S+) (.*)$') { $a[$Matches[1]] = $Matches[2] } }
    $a
}
function Attach {
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $disk
    $script:attached = $true
}
function Letters {
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
    $l = @{}
    foreach ($f in $found) { $k, $v = "$f".Split('='); $l[$k] = $v }
    if ($l.Count -ne $labels.Count) { throw "Volumes the driver mounted: $($found -join ' ')" }
    $l
}
$stop = "sc.exe stop '$service' | Out-Null; (Get-Service '$service').WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))"

$attached = $false
$wslMounted = $false
$failed = 0
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    "== sparse-vm $stamp"
    $deploy = if ($DriverImage) { @('-ImagePath', $DriverImage) } else { @() }
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" @deploy | Out-Null
    if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    New-VHD -Path $disk -SizeBytes 872415232 -Dynamic | Out-Null
    & wsl.exe --mount --vhd $disk --bare | Out-Null
    if ($LASTEXITCODE) { throw 'WSL attach failed' }
    $wslMounted = $true
    Image 'make'
    if ($imageRc) { throw 'image.sh make failed' }
    & wsl.exe --unmount "\\?\$disk" | Out-Null
    $wslMounted = $false

    & scp @ssh -q "$PSScriptRoot\sparse-vm\guest.ps1" "${target}:C:/Windows/Temp/sp-guest.ps1"
    if ($LASTEXITCODE) { throw 'scp failed' }
    Attach
    $letters = Letters
    $caseArg = if ($Cases) { "-Cases $Cases" } else { '' }

    "== NTFS, the reference"
    Guest "New-Item -ItemType Directory -Force C:\sparse-ntfs | Out-Null" | Out-Null
    Run 'C:\sparse-ntfs' $caseArg
    Guest "Remove-Item -Recurse -Force C:\sparse-ntfs" | Out-Null
    $guestLines | Where-Object { $_ -notmatch '^(P |  ok)' } | ForEach-Object { "   $_" }
    if ($guestRc) { $failed += $guestRc }
    $reference = Answers $guestLines

    "== the driver"
    foreach ($label in $labels) {
        $letter = $letters[$label]
        "-- $label on ${letter}:"
        Run "${letter}:\" "$caseArg -Keep"
        $guestLines | Where-Object { $_ -notmatch '^(P |  ok)' } | ForEach-Object { "   $_" }
        if ($guestRc) { $failed += $guestRc }
        $mine = Answers $guestLines
        $same = 0
        foreach ($k in @($reference.Keys) + @($mine.Keys) | Sort-Object -Unique) {
            if ($k -like 'left-*') { continue }
            if ("$($reference[$k])" -ceq "$($mine[$k])") { $same++ }
            else { "   FAIL $k`: NTFS answers [$($reference[$k])], the driver [$($mine[$k])]"; $failed++ }
        }
        "   $same answers as NTFS gives them"
    }

    "== the volumes gone and back"
    Guest $stop | Out-Null
    Guest "sc.exe start '$service' | Out-Null" | Out-Null
    $letters = Letters
    foreach ($label in $labels) {
        $letter = $letters[$label]
        "-- $label on ${letter}:"
        Run "${letter}:\" '-Verify'
        $guestLines | Where-Object { $_ -notmatch '^(P )' } | ForEach-Object { "   $_" }
        if ($guestRc) { $failed += $guestRc }
    }

    "== a full volume"
    foreach ($label in $labels) {
        $letter = $letters[$label]
        "-- $label on ${letter}:"
        Run "${letter}:\" '-Cases none -Full'
        $guestLines | Where-Object { $_ -notmatch '^(P )' } | ForEach-Object { "   $_" }
        if ($guestRc) { $failed += $guestRc }
    }

    Guest $stop | Out-Null
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
        try { Guest $stop | Out-Null } catch { }
        Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive
        try { Guest "sc.exe start '$service' | Out-Null" | Out-Null } catch { }
    }
    if ($wslMounted) { & wsl.exe --unmount "\\?\$disk" | Out-Null }
    try { Guest "Remove-Item -Force C:\Windows\Temp\sp-guest.ps1" | Out-Null } catch { }
    if ($DriverImage) { pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null }
    if (Test-Path -LiteralPath $disk) {
        if ($failed) {
            $keep = Join-Path $PSScriptRoot "sparse-failed-$stamp.vhdx"
            Move-Item -LiteralPath $disk $keep
            "   the disk is kept: $keep"
        } else {
            Remove-Item -LiteralPath $disk
        }
    }
}
"SPARSE-VM: $failed failed ($([int]$clock.Elapsed.TotalSeconds) s)"
Stop-Transcript | Out-Null
exit $failed
