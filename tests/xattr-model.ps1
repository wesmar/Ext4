# SPDX-License-Identifier: GPL-2.0-only
# Exercise the production xattr parser without mounting untrusted media.
param([string]$Output = (Join-Path $PSScriptRoot 'logs/xattr-model'),
    [ValidateSet('x64','x86')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$vsRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Select-Object -Unique |
    ForEach-Object { Join-Path $_ 'Microsoft Visual Studio' } | Where-Object { Test-Path -LiteralPath $_ }
$compiler = @($vsRoots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter cl.exe -Recurse -File } |
    Where-Object FullName -match "\\MSVC\\([^\\]+)\\bin\\Hostx64\\$Architecture\\cl\.exe$" |
    Sort-Object { [version]([regex]::Match($_.FullName, '\\MSVC\\([^\\]+)').Groups[1].Value) } -Descending)[0]
if (-not $compiler) { throw "An existing Hostx64/$Architecture MSVC compiler is required." }
$toolRoot = Split-Path (Split-Path (Split-Path (Split-Path $compiler.FullName)))
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdk = Get-ChildItem (Join-Path $kits 'Include') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
        (Test-Path (Join-Path $_.FullName 'ucrt/stdio.h')) -and
        (Test-Path (Join-Path $kits "Lib/$($_.Name)/um/$Architecture/kernel32.lib")) -and
        (Test-Path (Join-Path $kits "Lib/$($_.Name)/ucrt/$Architecture/ucrt.lib")) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk -or -not (Test-Path (Join-Path $toolRoot 'include/stdint.h'))) {
    throw 'The existing compiler/SDK headers or libraries are incomplete.'
}
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path -LiteralPath $Output).Path
$exe = Join-Path $Output 'xattr-model.exe'
$originalPath = $env:PATH
try {
    $env:PATH = "$($compiler.DirectoryName);$originalPath"
    $options = @('/nologo', '/O2', '/W4', '/WX', '/std:clatest')
    & $compiler.FullName @options "/I$PSScriptRoot/xattr-model" "/I$PSScriptRoot/../src/include" "/I$toolRoot/include" "/I$($sdk.FullName)/ucrt" "/I$($sdk.FullName)/shared" "/I$($sdk.FullName)/um" "$PSScriptRoot/xattr-model/test.c" "/Fe$exe" "/Fo$Output/xattr-model.obj" /link "/LIBPATH:$toolRoot/lib/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/ucrt/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/um/$Architecture"
    if ($LASTEXITCODE) { throw "Xattr model compilation failed ($LASTEXITCODE)." }
    & $exe
    if ($LASTEXITCODE) { throw "Xattr model failed ($LASTEXITCODE)." }
} finally {
    $env:PATH = $originalPath
}
