#Requires -Version 7.0
# SPDX-License-Identifier: GPL-2.0-only
# bench-vm.ps1 - repeatable timing of the guest workloads on the deployed driver.
#
# Runs stress.ps1 (8 threads create/rename/mkdir/delete) and repro.ps1
# (parallel write/extend/truncate/rename with read-back) Runs times each and
# prints every time plus the median, so two driver builds can be compared
# on the same VM without the noise of a single run.
#
#   pwsh tests\bench-vm.ps1 [-Drive E] [-Runs 3]
param([ValidatePattern('^[A-Za-z]$')][string]$Drive = 'E',
    [ValidateRange(1,100)][int]$Runs = 3, [string]$Ip = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = Get-SshOptions
$target = "$($TestEnv.User)@$ip"
foreach ($f in 'stress.ps1', 'repro.ps1') {
    & scp -q @ssh "$PSScriptRoot\$f" "${target}:C:/Windows/Temp/$f"
    if ($LASTEXITCODE) { throw "scp failed for $f" }
}
function Median($v) {
    $s = @($v | Sort-Object)
    $middle = [int][math]::Floor($s.Count / 2)
    if ($s.Count % 2) { return $s[$middle] }
    return ($s[$middle-1] + $s[$middle]) / 2
}
$stress = @(); $io = @()
for ($i = 1; $i -le $Runs; $i++) {
    $o = & ssh @ssh $target "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\stress.ps1 -Root $Drive`:\stress -Threads 8 -Iters 1500"
    if ($LASTEXITCODE -or -not ($o -match '^ops=\d+ errors=0 leftover=0$')) {
        throw "Stress benchmark did not finish cleanly: $($o -join '; ')"
    }
    $timing = @($o | Select-String '^elapsed (\d+) ms$')
    if ($timing.Count -ne 1) { throw 'Stress benchmark timing is missing or ambiguous' }
    $stress += [int]$timing[0].Matches[0].Groups[1].Value
    $o = & ssh @ssh $target "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\repro.ps1 -Drive $Drive`: -Runs 1 -Files 200"
    if ($LASTEXITCODE -or -not ($o -match '^done: 1 runs, 0 failed$')) {
        throw "I/O benchmark did not finish cleanly: $($o -join '; ')"
    }
    $timing = @($o | Select-String '^run 1 OK (\d+) ms$')
    if ($timing.Count -ne 1) { throw 'I/O benchmark timing is missing or ambiguous' }
    $io += [int]$timing[0].Matches[0].Groups[1].Value
}
"stress ms: $($stress -join ', ')   median $(Median $stress)"
"io ms:     $($io -join ', ')   median $(Median $io)"
[void](& ssh @ssh $target 'powershell -NoProfile -Command "foreach($f in ''stress.ps1'',''repro.ps1''){ $p=\"C:\Windows\Temp\$f\"; if([IO.File]::Exists($p)){ [IO.File]::Delete($p) } }"')
if ($LASTEXITCODE) { throw 'Guest benchmark cleanup failed' }
