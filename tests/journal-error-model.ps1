# SPDX-License-Identifier: GPL-2.0-only
# Reuse compiler discovery and extraction; the write-path checks run first.
param([string]$Output = (Join-Path $PSScriptRoot 'logs/journal-error-model'),
    [ValidateSet('x64','x86')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$journalOutput = $Output
. "$PSScriptRoot/write-path-model.ps1" -Architecture $Architecture
$Output = $journalOutput
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path -LiteralPath $Output).Path
$source = [IO.File]::ReadAllText("$PSScriptRoot/journal-error-model.c") + "`n" +
    (ProductionFunction 'journal/JournalScope.c' 'Ext2JournalDirtyBuffer' 'Ext2JournalRevokeBlocks' 'BOOLEAN') + "`n" +
    (ProductionFunction 'journal/JournalScope.c' 'Ext2JournalRevokeBlocks' '' 'BOOLEAN') + "`n"
$bufferSource = [IO.File]::ReadAllText("$PSScriptRoot/../src/linux/buffer_head.c").Replace("`r`n","`n")
$flush = [regex]::Match($bufferSource,'(?ms)^int sync_blockdev\([^}]+^}')
if (-not $flush.Success) { throw 'Cannot locate the production sync_blockdev function.' }
$source += $flush.Value
$generated = Join-Path $Output 'production-probe.c'
[IO.File]::WriteAllText($generated,$source,[Text.UTF8Encoding]::new($false))
$exe = Join-Path $Output 'journal-error-model.exe'
$oldPath = $env:PATH
try {
    $env:PATH = "$($compiler.DirectoryName);$oldPath"
    & $compiler.FullName /nologo /O2 /W4 /WX /std:clatest "/I$toolRoot/include" "/I$($sdk.FullName)/ucrt" "/I$($sdk.FullName)/shared" "/I$($sdk.FullName)/um" $generated "/Fe$exe" "/Fo$Output/model.obj" /link "/LIBPATH:$toolRoot/lib/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/ucrt/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/um/$Architecture"
    if ($LASTEXITCODE) { throw "Journal error model compilation failed ($LASTEXITCODE)." }
    & $exe
    if ($LASTEXITCODE) { throw "Journal error model failed ($LASTEXITCODE)." }
} finally { $env:PATH = $oldPath }
