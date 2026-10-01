#Requires -Version 7.0
# bench-vm.ps1 - repeatable timing of the guest workloads on the deployed driver.
#
# Runs stress.ps1 (8 threads create/rename/mkdir/delete) and repro.ps1
# (parallel write/extend/truncate/rename with read-back) Runs times each and
# prints every time plus the median, so two driver builds can be compared
# on the same VM without the noise of a single run.
#
#   pwsh tests\bench-vm.ps1 [-Drive E] [-Runs 3]
param([string]$Drive = 'E', [int]$Runs = 3)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\testenv.ps1"
$ip = Get-TestVmIp
$ssh = Get-SshOptions
$target = "$($TestEnv.User)@$ip"
foreach ($f in 'stress.ps1', 'repro.ps1') {
    & scp -q @ssh "$PSScriptRoot\$f" "${target}:C:/Windows/Temp/$f"
}
function Median($v) { $s = $v | Sort-Object; $s[[int][math]::Floor($s.Count / 2)] }
$stress = @(); $io = @()
for ($i = 1; $i -le $Runs; $i++) {
    $o = & ssh @ssh $target "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\stress.ps1 -Root $Drive`:\stress -Threads 8 -Iters 1500"
    $stress += [int](($o | Select-String 'elapsed (\d+)').Matches[0].Groups[1].Value)
    $o = & ssh @ssh $target "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\repro.ps1 -Drive $Drive`: -Runs 1 -Files 200"
    $io += [int](($o | Select-String 'OK (\d+) ms').Matches[0].Groups[1].Value)
}
[IO.File]::Delete('C:\Windows\Temp\stress.ps1') 2>$null
"stress ms: $($stress -join ', ')   median $(Median $stress)"
"io ms:     $($io -join ', ')   median $(Median $io)"
[void](& ssh @ssh $target 'powershell -NoProfile -Command "foreach($f in ''stress.ps1'',''repro.ps1''){ $p=\"C:\Windows\Temp\$f\"; if([IO.File]::Exists($p)){ [IO.File]::Delete($p) } }"')
