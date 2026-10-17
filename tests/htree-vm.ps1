#Requires -Version 7.0
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# htree-vm.ps1 - indexed directories built by ext4.sys at scale, checked by
# Linux, changed by Linux, checked by the driver again.
#
# One disk of five volumes (tests/htree-vm/image.sh): 4, 2 and 1 KiB blocks,
# every hash (legacy, half_md4, tea), signed and unsigned, with and without
# metadata checksums, ext3, large_dir (a three-level index), casefold. Each
# gets thousands of names in one directory - Windows-valid UTF-8 names of
# every length (tests/htree-vm/names.c), among them groups that share a
# major hash on that volume - which the guest creates, deletes, renames,
# moves out and back, and makes again, comparing the listing and every
# lookup with what must be there after each step (htree-vm/guest.ps1). Then
# in WSL: e2fsck -fn (which checks every leaf's hash range and order), the
# kernel's listing and a lookup of every name through the index the driver
# built; the kernel changes each directory; the guest checks it and changes
# it again; Linux checks the end.
#
#   pwsh tests\htree-vm.ps1 [-Scale 1.0] [-Ip a.b.c.d]
#
# Exit code: the number of failed checks. A failed run keeps its disk as
# tests\htree-failed-<time>.vhdx.
param([double]$Scale = 1.0, [string]$Ip = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = @(Get-SshOptions) + @('-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=6')
$target = "$($TestEnv.User)@$Ip"
$service = $TestEnv.Service
$disk = Join-Path $PSScriptRoot 'htree-test.vhdx'
$slot = 5
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $PSScriptRoot 'logs\htree-vm'
New-Item -ItemType Directory $work -Force | Out-Null
Start-Transcript -Path (Join-Path $PSScriptRoot "logs\htree-vm-$stamp.log") | Out-Null

# label, names at first, bytes of a name, groups sharing a major hash
$cases = @(
    @{ Label = 'ht4k';  Count = 30000; Min = 1;   Max = 240; Pairs = 300 }
    @{ Label = 'ht1k';  Count = 20000; Min = 1;   Max = 120; Pairs = 300 }
    @{ Label = 'ht2k3'; Count = 15000; Min = 1;   Max = 200; Pairs = 200 }
    @{ Label = 'ht1kl'; Count = 60000; Min = 200; Max = 240; Pairs = 100 }
    @{ Label = 'htcf';  Count = 20000; Min = 1;   Max = 240; Pairs = 200 }
)
foreach ($c in $cases) { $c.Count = [int]($c.Count * $Scale) }

if (Test-Path -LiteralPath $disk) { throw "Left over from an earlier run: $disk" }
if (Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.ControllerNumber -eq 0 -and $_.ControllerLocation -eq $slot }) {
    throw 'The fixture slot is occupied'
}
function WslPath([string]$Path) { '/mnt/' + $Path.Substring(0, 1).ToLower() + $Path.Substring(2).Replace('\', '/') }
$htDir = WslPath "$PSScriptRoot\htree-vm"
function Image([string]$Arguments) {
    # streamed: a check prints as it goes; the exit code is the failures
    & wsl.exe -u root -- bash -c "sed 's/\r`$//' '$htDir/image.sh' | HT_DIR='$htDir' bash -s -- $Arguments" 2>&1 | ForEach-Object { "$_" }
    $script:imageRc = $LASTEXITCODE
}
function Guest([string]$Code) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; `$ProgressPreference='SilentlyContinue'; " + $Code))
    $output = @(& ssh @ssh $target "powershell -NoProfile -NonInteractive -EncodedCommand $encoded")
    if ($LASTEXITCODE) { throw "Guest failed: $LASTEXITCODE" }
    $output
}
function Attach-Wsl { & wsl.exe --mount --vhd $disk --bare | Out-Null; if ($LASTEXITCODE) { throw 'WSL attach failed' }; $script:wslMounted = $true }
function Detach-Wsl { & wsl.exe --unmount "\\?\$disk" | Out-Null; $script:wslMounted = $false }
function Attach-Vm {
    Add-VMHardDiskDrive -VMName $TestEnv.Vm -ControllerType SCSI -ControllerNumber 0 -ControllerLocation $slot -Path $disk
    $script:attached = $true
    $labels = ($cases | ForEach-Object { "'$($_.Label)'" }) -join ','
    $found = Guest @"
`$want = @($labels)
for (`$i = 0; `$i -lt 300; `$i++) {
    `$v = @(Get-Volume | Where-Object { `$want -contains `$_.FileSystemLabel -and `$_.DriveLetter })
    if (`$v.Count -eq `$want.Count) { break }
    Start-Sleep -Milliseconds 200
}
`$v | ForEach-Object { "`$(`$_.FileSystemLabel)=`$(`$_.DriveLetter)" }
"@
    $script:letters = @{}
    foreach ($f in $found) { $k, $v = "$f".Split('='); $script:letters[$k] = $v }
    if ($script:letters.Count -ne $cases.Count) { throw "Volumes the driver mounted: $($found -join ' ')" }
}
function Detach-Vm {
    Guest "sc.exe stop '$service' | Out-Null; (Get-Service '$service').WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))" | Out-Null
    Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive
    $script:attached = $false
    Guest "sc.exe start '$service' | Out-Null" | Out-Null
}
function Run-Guest([hashtable]$Case, [string]$Mode, [int]$Seed) {
    $root = "$($letters[$Case.Label]):\"
    $cf = if ($Case.Label -eq 'htcf') { '-Casefold' } else { '' }
    "-- $($Case.Label) on $root ($Mode)"
    & ssh @ssh $target "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\Windows\Temp\ht-guest.ps1 -Root $root -Names C:\Windows\Temp\ht-$($Case.Label).txt -Count $($Case.Count) -Seed $Seed -Mode $Mode $cf" |
        ForEach-Object { "   $_" }
    if ($LASTEXITCODE) { $script:failed += [Math]::Max(1, $LASTEXITCODE) }
}

$attached = $false
$wslMounted = $false
$failed = 0
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    "== htree-vm $stamp, scale $Scale"
    pwsh -NoProfile -File "$PSScriptRoot\deploy-vm.ps1" | Out-Null
    if ($LASTEXITCODE) { throw 'deploy-vm.ps1 failed' }
    New-VHD -Path $disk -SizeBytes 1610612736 -Dynamic | Out-Null
    Attach-Wsl
    "-- the volumes"
    $made = @(Image 'make')
    if ($imageRc) { throw "image.sh make failed: $($made -join ' ')" }
    $wslWork = WslPath $work
    foreach ($line in ($made -match '^ht\w+ \d ')) {
        $label, $hash, $unsigned, $seed, $cf = "$line".Split(' ')
        $c = $cases | Where-Object { $_.Label -eq $label }
        "   $label`: hash $hash$(if ($unsigned -eq '1') { ' unsigned' }), seed $seed$(if ($cf -eq '1') { ', casefold' })"
        # the guest's names: the first Count, then fresh ones for renames; Linux's own
        Image "names $wslWork $label $($c.Count * 2 + 2000) 1 $hash $unsigned $seed $($c.Min) $($c.Max) $($c.Pairs)"
        if ($imageRc) { throw 'names failed' }
        Image "names $wslWork linux-$label $([int]($c.Count / 2) + 1000) 2 $hash $unsigned $seed $($c.Min) $($c.Max) $([int]($c.Pairs / 4))"
        if ($imageRc) { throw 'names failed' }
    }
    Detach-Wsl

    & scp @ssh -q "$PSScriptRoot\htree-vm\guest.ps1" "${target}:C:/Windows/Temp/ht-guest.ps1"
    foreach ($c in $cases) { & scp @ssh -q (Join-Path $work "$($c.Label).txt") "${target}:C:/Windows/Temp/ht-$($c.Label).txt" }
    if ($LASTEXITCODE) { throw 'scp failed' }

    "== the driver builds the directories"
    Attach-Vm
    foreach ($c in $cases) { Run-Guest $c 'build' 1 }
    Detach-Vm

    "== Linux checks them, changes them, checks again"
    Attach-Wsl
    Image 'check driver'; $failed += $imageRc
    Image "linux $wslWork"; $failed += $imageRc
    Image 'check linux'; $failed += $imageRc
    Detach-Wsl

    "== the driver checks what Linux made and changes it again"
    Attach-Vm
    foreach ($c in $cases) { Run-Guest $c 'again' 2 }
    Detach-Vm

    "== Linux checks the end"
    Attach-Wsl
    Image 'check final'; $failed += $imageRc
    Detach-Wsl
} catch {
    "!! $_"
    $failed++
} finally {
    if ($attached) {
        try { Detach-Vm } catch { Get-VMHardDiskDrive -VMName $TestEnv.Vm | Where-Object { $_.Path -eq $disk } | Remove-VMHardDiskDrive }
    }
    if ($wslMounted) { Detach-Wsl }
    try { Guest "Remove-Item -Force C:\Windows\Temp\ht-*" | Out-Null } catch { }
    if (Test-Path -LiteralPath $disk) {
        if ($failed) {
            $keep = Join-Path $PSScriptRoot "htree-failed-$stamp.vhdx"
            Move-Item -LiteralPath $disk $keep
            "   the disk is kept: $keep"
        } else {
            Remove-Item -LiteralPath $disk
        }
    }
}
"HTREE-VM: $failed failed ($([int]$clock.Elapsed.TotalSeconds) s)"
Stop-Transcript | Out-Null
exit $failed
