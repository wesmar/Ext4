# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# Build/run the production directory code (src/ext4/DirBlock.c, DirIndex.c,
# NameHash.c) building directories name by name, as x64 and as x86.
param([string]$Output = (Join-Path $PSScriptRoot 'logs/htree-model'),
    [ValidateRange(1,4294967295)][uint32]$Seed = 625341585,
    [ValidateRange(1,1000)][int]$Rounds = 1,
    [ValidateSet('x64','x86','both')][string]$Architecture = 'both')
$ErrorActionPreference = 'Stop'
$vsRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Select-Object -Unique |
    ForEach-Object { Join-Path $_ 'Microsoft Visual Studio' } | Where-Object { Test-Path -LiteralPath $_ }
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$architectures = if ($Architecture -eq 'both') { @('x64', 'x86') } else { @($Architecture) }
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path -LiteralPath $Output).Path
$originalPath = $env:PATH
try {
    foreach ($arch in $architectures) {
        $compiler = @($vsRoots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter cl.exe -Recurse -File } |
            Where-Object FullName -match "\\MSVC\\([^\\]+)\\bin\\Hostx64\\$arch\\cl\.exe$" |
            Sort-Object { [version]([regex]::Match($_.FullName, '\\MSVC\\([^\\]+)').Groups[1].Value) } -Descending)[0]
        if (-not $compiler) { throw "An existing Hostx64/$arch MSVC compiler is required." }
        $toolRoot = Split-Path (Split-Path (Split-Path (Split-Path $compiler.FullName)))
        $sdk = Get-ChildItem (Join-Path $kits 'Include') -Directory |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
                (Test-Path (Join-Path $_.FullName 'ucrt/stdio.h')) -and
                (Test-Path (Join-Path $kits "Lib/$($_.Name)/um/$arch/kernel32.lib")) -and
                (Test-Path (Join-Path $kits "Lib/$($_.Name)/ucrt/$arch/ucrt.lib")) } |
            Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
        if (-not $sdk -or -not (Test-Path (Join-Path $toolRoot 'include/stdint.h'))) {
            throw 'The existing compiler/SDK headers or libraries are incomplete.'
        }
        $exe = Join-Path $Output "htree-model-$arch.exe"
        $env:PATH = "$($compiler.DirectoryName);$originalPath"
        & $compiler.FullName /nologo /O2 /W4 /WX /std:clatest "/I$toolRoot/include" "/I$($sdk.FullName)/ucrt" "/I$($sdk.FullName)/shared" "/I$($sdk.FullName)/um" "$PSScriptRoot/htree-model/test.c" "/Fe$exe" "/Fo$Output/htree-model-$arch.obj" /link "/LIBPATH:$toolRoot/lib/$arch" "/LIBPATH:$kits/Lib/$($sdk.Name)/ucrt/$arch" "/LIBPATH:$kits/Lib/$($sdk.Name)/um/$arch"
        if ($LASTEXITCODE) { throw "Htree model compilation failed ($arch, $LASTEXITCODE)." }
        for ($round = 0; $round -lt $Rounds; $round++) {
            $roundSeed = [uint32](([uint64]$Seed + [uint64]$round * 2654435769) % 4294967296)
            if (-not $roundSeed) { $roundSeed = 1 }
            "== $arch"
            & $exe $roundSeed
            if ($LASTEXITCODE) { throw "Htree model failed ($arch, $LASTEXITCODE), seed=$roundSeed." }
        }
        $env:PATH = $originalPath
    }
    'HTREE-MODEL: ALL PASSED'
} finally {
    $env:PATH = $originalPath
}
