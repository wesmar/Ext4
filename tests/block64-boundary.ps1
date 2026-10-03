# SPDX-License-Identifier: GPL-2.0-only
# Mixed-format allocation: an indirect inode at UINT32_MAX on a 64-bit volume.
param([Parameter(Mandatory=$true)][string]$Root,
    [ValidateSet(1024,2048,4096)][int]$BlockSize = 4096,
    [ValidateSet('Prepare','Verify','Finish')][string]$Phase = 'Prepare')
$boundaryPhase = $Phase
. "$PSScriptRoot/block64.ps1" -Root $Root -Phase Helpers
$path = Join-Path $rootPath 'indirect32.bin'
$eaPath = Join-Path $rootPath 'seed64.bin'
$pattern = [byte[]]::new(4096)
for ($i=0; $i -lt 4096; $i++) { $pattern[$i] = ($i*19+7) -band 255 }
$ea = [byte[]]::new([math]::Min(2000,$BlockSize-128))
for ($i=0; $i -lt $ea.Length; $i++) { $ea[$i] = ($i*23+11) -band 255 }
if ($boundaryPhase -eq 'Prepare') {
    Assert ((Get-Item -LiteralPath $path).Length -eq 4096) 'Fresh indirect fixture required'
    EqualBytes ([IO.File]::ReadAllBytes($path)) $pattern 'Last 32-bit block cached read'
    EqualBytes ([Block64Io]::Unbuffered($path,0,$null)) $pattern 'Last 32-bit block unbuffered read'
    Assert ([Block64Io]::PhysicalRuns($path)[0] -eq ([long][uint32]::MaxValue - (4096/$BlockSize - 1))) 'Boundary LCN fixture'
    $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
    try { $stream.SetLength(48*4096); $stream.Flush($true) } finally { $stream.Dispose() }
    for ($logical=1; $logical -lt 48; $logical++) {
        $block = [byte[]]::new(4096)
        for ($i=0; $i -lt 4096; $i++) { $block[$i] = ($i*19+7+$logical) -band 255 }
        [void][Block64Io]::Unbuffered($path,$logical*4096,$block)
    }
    [Block64Io]::SetEa($eaPath,$ea)
}
Assert ((Get-Item -LiteralPath $path).Length -eq 48*4096) 'Indirect growth size'
for ($logical=0; $logical -lt 48; $logical++) {
    $block = [byte[]]::new(4096)
    for ($i=0; $i -lt 4096; $i++) { $block[$i] = ($i*19+7+$logical) -band 255 }
    EqualBytes ([Block64Io]::Unbuffered($path,$logical*4096,$null)) $block 'Indirect boundary read-back'
}
$cached = [IO.File]::ReadAllBytes($path)
for ($i=0; $i -lt $cached.Length; $i++) {
    $expected = (($i%4096)*19+7+[int][math]::Floor($i/4096)) -band 255
    if ($cached[$i] -ne $expected) { throw "Cached indirect mismatch at $i" }
}
$runs = [Block64Io]::PhysicalRuns($path)
Assert (@($runs | Where-Object { $_ -gt [uint32]::MaxValue -or $_ -lt 0 }).Count -eq 0) 'Indirect pointer escaped its format'
EqualBytes ([Block64Io]::GetEa($eaPath)) $ea 'External high-address EA'
"BLOCK64-BOUNDARY: indirect data and external EA verified, phase=$boundaryPhase"
if ($boundaryPhase -eq 'Finish') {
    [IO.File]::Delete($path)
    [Block64Io]::SetEa($eaPath,[byte[]]@())
    'BLOCK64-BOUNDARY: data and EA freed'
}
