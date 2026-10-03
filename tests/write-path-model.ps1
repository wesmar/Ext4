# SPDX-License-Identifier: GPL-2.0-only
# Compile current production functions with deterministic allocation/I/O faults.
param([string]$Output = (Join-Path $PSScriptRoot 'logs/write-path-model'),
    [ValidateSet('x64','x86')][string]$Architecture = 'x64',
    [switch]$Baseline)
$ErrorActionPreference = 'Stop'
$roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Select-Object -Unique |
    ForEach-Object { Join-Path $_ 'Microsoft Visual Studio' } | Where-Object { Test-Path -LiteralPath $_ }
$compiler = @($roots | ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter cl.exe -Recurse -File } |
    Where-Object FullName -match "\\MSVC\\([^\\]+)\\bin\\Hostx64\\$Architecture\\cl\.exe$" |
    Sort-Object { [version]([regex]::Match($_.FullName, '\\MSVC\\([^\\]+)').Groups[1].Value) } -Descending)[0]
if (-not $compiler) { throw 'An existing MSVC C compiler is required.' }
$toolRoot = Split-Path (Split-Path (Split-Path (Split-Path $compiler.FullName)))
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdk = Get-ChildItem (Join-Path $kits 'Include') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
        (Test-Path (Join-Path $_.FullName 'ucrt/stdio.h')) -and
        (Test-Path (Join-Path $kits "Lib/$($_.Name)/um/$Architecture/kernel32.lib")) -and
        (Test-Path (Join-Path $kits "Lib/$($_.Name)/ucrt/$Architecture/ucrt.lib")) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'The existing Windows SDK is incomplete.' }
function ProductionFunction([string]$file,[string]$name,[string]$next,[string]$ReturnType = 'NTSTATUS') {
    $source = if ($Baseline) {
        $text = & git -C "$PSScriptRoot/.." show "HEAD:src/$file"
        if ($LASTEXITCODE) { throw "Cannot read baseline $file." }
        $text -join "`n"
    } else { [IO.File]::ReadAllText((Join-Path "$PSScriptRoot/../src" $file)) }
    $source = $source.Replace("`r`n","`n")
    $match = [regex]::Match($source, "(?m)^$ReturnType\s+$name\s*\(")
    $start = if ($match.Success) { $match.Index } else { -1 }
    $end = if ($next) { $source.IndexOf("$ReturnType`n$next", $start + 1) } else { $source.Length }
    if ($start -lt 0 -or $end -le $start) { throw "Cannot locate production function $name." }
    $source.Substring($start,$end-$start)
}
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path -LiteralPath $Output).Path
$source = [IO.File]::ReadAllText("$PSScriptRoot/write-path-model.c") + "`n" +
    (ProductionFunction 'core/BlockRuns.c' 'Ext2BuildExtents' '') + "`n" +
    (ProductionFunction 'fsd/FileWrite.c' 'Ext2WriteInode' 'Ext2WriteFile') + "`n" +
    (ProductionFunction 'fsd/ReparsePoints.c' 'Ext2WriteSymlink' 'Ext2SetReparsePoint')
$generated = Join-Path $Output 'production-probe.c'
[IO.File]::WriteAllText($generated,$source,[Text.UTF8Encoding]::new($false))
$exe = Join-Path $Output 'write-path-model.exe'
$oldPath = $env:PATH
try {
    $env:PATH = "$($compiler.DirectoryName);$oldPath"
    & $compiler.FullName /nologo /O2 /W4 /WX /std:clatest "/I$PSScriptRoot/../src/fsd" "/I$toolRoot/include" "/I$($sdk.FullName)/ucrt" "/I$($sdk.FullName)/shared" "/I$($sdk.FullName)/um" $generated "/Fe$exe" "/Fo$Output/model.obj" /link "/LIBPATH:$toolRoot/lib/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/ucrt/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/um/$Architecture"
    if ($LASTEXITCODE) { throw "Write-path model compilation failed ($LASTEXITCODE)." }
    & $exe
    if ($LASTEXITCODE) { throw "Write-path model failed ($LASTEXITCODE)." }
} finally { $env:PATH = $oldPath }
