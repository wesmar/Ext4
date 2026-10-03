# SPDX-License-Identifier: GPL-2.0-only
# Build/run the production run map against deterministic kernel stubs.
param([string]$Output = (Join-Path $PSScriptRoot 'logs/run-map-model'),
    [ValidateRange(1,4294967295)][uint32]$Seed = 2447445413,
    [ValidateRange(1,1000)][int]$Rounds = 1)
$ErrorActionPreference = 'Stop'
$vsRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Select-Object -Unique |
    ForEach-Object { Join-Path $_ 'Microsoft Visual Studio' } | Where-Object { Test-Path -LiteralPath $_ }
$compiler = @($vsRoots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter cl.exe -Recurse -File } |
    Where-Object FullName -match '\\MSVC\\([^\\]+)\\bin\\Hostx64\\x64\\cl\.exe$' |
    Sort-Object { [version]([regex]::Match($_.FullName, '\\MSVC\\([^\\]+)').Groups[1].Value) } -Descending)[0]
if (-not $compiler) { throw 'An existing Hostx64/x64 MSVC compiler is required.' }
$toolRoot = Split-Path (Split-Path (Split-Path (Split-Path $compiler.FullName)))
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdk = Get-ChildItem (Join-Path $kits 'Include') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
        (Test-Path (Join-Path $_.FullName 'ucrt/stdio.h')) -and
        (Test-Path (Join-Path $kits "Lib/$($_.Name)/um/x64/kernel32.lib")) -and
        (Test-Path (Join-Path $kits "Lib/$($_.Name)/ucrt/x64/ucrt.lib")) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk -or -not (Test-Path (Join-Path $toolRoot 'include/stdint.h'))) {
    throw 'The existing compiler/SDK headers or libraries are incomplete.'
}
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path -LiteralPath $Output).Path
$exe = Join-Path $Output 'run-map-model.exe'
$originalPath = $env:PATH
try {
    $env:PATH = "$($compiler.DirectoryName);$originalPath"
    & $compiler.FullName /nologo /W4 /WX /std:clatest "/I$PSScriptRoot/run-map-model" "/I$PSScriptRoot/../src/include" "/I$toolRoot/include" "/I$($sdk.FullName)/ucrt" "/I$($sdk.FullName)/shared" "/I$($sdk.FullName)/um" "$PSScriptRoot/run-map-model/test.c" "/Fe$exe" "/Fo$Output/run-map-model.obj" /link "/LIBPATH:$toolRoot/lib/x64" "/LIBPATH:$kits/Lib/$($sdk.Name)/ucrt/x64" "/LIBPATH:$kits/Lib/$($sdk.Name)/um/x64"
    if ($LASTEXITCODE) { throw "Run map model compilation failed ($LASTEXITCODE)." }
    for ($round = 0; $round -lt $Rounds; $round++) {
        $roundSeed = [uint32](([uint64]$Seed + [uint64]$round * 2654435769) % 4294967296)
        if (-not $roundSeed) { $roundSeed = 1 }
        & $exe $roundSeed
        if ($LASTEXITCODE) { throw "Run map model failed ($LASTEXITCODE), seed=$roundSeed." }
    }
} finally {
    $env:PATH = $originalPath
}
