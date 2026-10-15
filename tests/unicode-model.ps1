# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# Compare the production case folding (src/unicode) with the Linux trie it
# replaced, taken from git: every code point, every byte string of up to
# three bytes (four with -Full), random names, Linux's strict-mode check.
param(
    [string]$ReferenceCommit = 'c117aa7',
    [ValidateSet('x64','x86')][string]$Architecture = 'x64',
    [string]$Output = (Join-Path $PSScriptRoot 'logs/unicode-model'),
    [switch]$Full
)
$ErrorActionPreference='Stop'
$root = Split-Path $PSScriptRoot -Parent
$vsRoots=@($env:ProgramFiles,${env:ProgramFiles(x86)}) | Select-Object -Unique |
    ForEach-Object {Join-Path $_ 'Microsoft Visual Studio'} | Where-Object {Test-Path -LiteralPath $_}
$compiler=@($vsRoots | ForEach-Object {Get-ChildItem -LiteralPath $_ -Filter cl.exe -Recurse -File} |
    Where-Object FullName -match "\\MSVC\\([^\\]+)\\bin\\Hostx64\\$Architecture\\cl\.exe$" |
    Sort-Object {[version]([regex]::Match($_.FullName,'\\MSVC\\([^\\]+)').Groups[1].Value)} -Descending)[0]
if (!$compiler) {throw "An existing Hostx64/$Architecture MSVC compiler is required."}
$toolRoot=Split-Path (Split-Path (Split-Path (Split-Path $compiler.FullName)))
$kits=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdk=Get-ChildItem (Join-Path $kits 'Include') -Directory | Where-Object {
    $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
    (Test-Path (Join-Path $_.FullName 'ucrt/stdio.h')) -and
    (Test-Path (Join-Path $kits "Lib/$($_.Name)/um/$Architecture/kernel32.lib")) -and
    (Test-Path (Join-Path $kits "Lib/$($_.Name)/ucrt/$Architecture/ucrt.lib"))
} | Sort-Object {[version]$_.Name} -Descending | Select-Object -First 1
if (!$sdk) {throw 'Existing compiler/SDK is incomplete.'}

New-Item -ItemType Directory -Path $Output -Force | Out-Null
$Output=(Resolve-Path -LiteralPath $Output).Path
$reference = Join-Path $Output 'reference'
New-Item -ItemType Directory -Path $reference -Force | Out-Null
foreach ($file in 'src/unicode/utf8norm.c','src/unicode/utf8data.c','src/unicode/casefold.c',
                  'src/unicode/utf8n.h','src/include/ext4fs_unicode.h') {
    $text = (& git -C $root show "${ReferenceCommit}:$file") -join "`n"
    if ($LASTEXITCODE) {throw "git show ${ReferenceCommit}:$file failed."}
    [IO.File]::WriteAllText((Join-Path $reference (Split-Path $file -Leaf)), $text + "`n", [Text.UTF8Encoding]::new($false))
}

$oldPath=$env:PATH
try {
    $env:PATH="$($compiler.DirectoryName);$oldPath"
    $options=@('/nologo','/O2','/W4','/WX','/std:clatest', "/I$toolRoot/include", "/I$($sdk.FullName)/ucrt",
               "/I$($sdk.FullName)/shared", "/I$($sdk.FullName)/um")
    & $compiler.FullName @options /c "/I$reference" "$PSScriptRoot/unicode-model/before.c" "/Fo$Output/before.obj"
    if ($LASTEXITCODE) {throw 'Reference compilation failed.'}
    & $compiler.FullName @options /c "/I$root/src/unicode" "/I$root/src/include" "$PSScriptRoot/unicode-model/after.c" "/Fo$Output/after.obj"
    if ($LASTEXITCODE) {throw 'Production compilation failed.'}
    $exe = Join-Path $Output 'unicode-model.exe'
    & $compiler.FullName @options "$PSScriptRoot/unicode-model/test.c" "$Output/before.obj" "$Output/after.obj" "/Fe$exe" "/Fo$Output/test.obj" /link "/LIBPATH:$toolRoot/lib/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/ucrt/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/um/$Architecture"
    if ($LASTEXITCODE) {throw 'Unicode model link failed.'}
    if ($Full) {& $exe --full} else {& $exe}
    if ($LASTEXITCODE) {throw 'Unicode model failed.'}
} finally {$env:PATH=$oldPath}
