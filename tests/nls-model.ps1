# SPDX-License-Identifier: GPL-2.0-only
# Compile the before/after production converters; compare outputs exhaustively.
param(
    [Parameter(Mandatory)][string]$Reference,
    [ValidateSet('x64','x86')][string]$Architecture = 'x64',
    [string]$Output = (Join-Path $PSScriptRoot 'logs/nls-model'),
    [switch]$Benchmark
)
$ErrorActionPreference='Stop'
$Reference=(Resolve-Path -LiteralPath $Reference).Path
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
if (!$sdk -or !(Test-Path (Join-Path $toolRoot 'include/stdint.h'))) {throw 'Existing compiler/SDK is incomplete.'}
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$Output=(Resolve-Path -LiteralPath $Output).Path
$objects=[Collections.Generic.List[string]]::new()
$decl=[Collections.Generic.List[string]]::new()
$rows=[Collections.Generic.List[string]]::new()
$oldPath=$env:PATH
try {
    $env:PATH="$($compiler.DirectoryName);$oldPath"
    $options=@('/nologo','/O2','/W4','/WX','/std:clatest',"/I$PSScriptRoot/nls-model", "/I$toolRoot/include", "/I$($sdk.FullName)/ucrt", "/I$($sdk.FullName)/shared", "/I$($sdk.FullName)/um")
    foreach($file in @(Get-ChildItem -LiteralPath $Reference -Filter 'nls_*.c' |
        Where-Object Name -Match '^nls_(cp|iso8859|ascii|koi8-[ru]\.)' | Sort-Object Name)) {
        $page=$file.BaseName.Substring(4).Replace('-','_')
        foreach($side in 'before','after') {
            $source=$file.FullName
            if($side-eq'after') {$source=Join-Path "$PSScriptRoot/../src/nls" $file.Name}
            $obj=Join-Path $Output "$page-$side.obj"
            $inherited = @(if ($side -eq 'before') {'/DMODEL_INHERITED'})
            & $compiler.FullName @options @inherited /c "/DMODEL_GETTER=$($side)_$page" "/DMODEL_EXIT=exit_$($side)_$page" $source "/Fo$obj"
            if($LASTEXITCODE) {throw "Converter model compilation failed: $page/$side"}
            $objects.Add($obj)
            $decl.Add("const struct nls_table *$($side)_$page(void);")
        }
        $rows.Add("    { `"$page`", before_$page, after_$page },")
    }
    # The converters the rebuilt code pages share, and the delegating ones: koi8-ru
    # over koi8-u and euc-jp over cp932, each side over its own side's table.
    $delegating = foreach($side in 'before','after') {
        foreach($name in 'koi8-ru','euc-jp') {
            $page = $name.Replace('-','_')
            @{Source=$(if($side-eq'before'){Join-Path $Reference "nls_$name.c"}else{"$PSScriptRoot/../src/nls/nls_$name.c"}); Name="$page-$side"
              Defines=@("/DMODEL_GETTER=$($side)_$page","/DMODEL_EXIT=exit_$($side)_$page","/Dload_nls=load_nls_$side","/Dunload_nls=unload_nls_$side") + @(if($side-eq'before'){'/DMODEL_INHERITED'})}
        }
    }
    foreach($unit in @(@{Source="$PSScriptRoot/../src/nls/CharsetMap.c"; Name='charset-map'; Defines=@()}) + $delegating) {
        $obj=Join-Path $Output "$($unit.Name).obj"
        & $compiler.FullName @options @($unit.Defines) /c $unit.Source "/Fo$obj"
        if($LASTEXITCODE) {throw "Model compilation failed: $($unit.Name)"}
        $objects.Add($obj)
    }
    $header="/* Generated declarations for this model run. */`n" + ($decl-join"`n") +
        "`ntypedef struct { const char *Name; const struct nls_table *(*Before)(void); const struct nls_table *(*After)(void); } PROFILE;`n" +
        "static const PROFILE Profiles[] = {`n" + ($rows-join"`n") + "`n};`n"
    [IO.File]::WriteAllText((Join-Path $Output 'profiles.h'),$header,[Text.UTF8Encoding]::new($false))
    $directorySource = Get-Content -Raw -LiteralPath "$PSScriptRoot/../src/ext4/DirectoryOperations.c"
    $builderStart = $directorySource.IndexOf('struct dentry *Ext2BuildEntry(')
    $builderEnd = $directorySource.IndexOf("NTSTATUS`r`nExt2AddEntry", $builderStart)
    if ($builderEnd -lt 0) { $builderEnd = $directorySource.IndexOf("NTSTATUS`nExt2AddEntry", $builderStart) }
    if ($builderStart -lt 0 -or $builderEnd -lt 0) { throw 'Production dentry builder extraction failed.' }
    [IO.File]::WriteAllText((Join-Path $Output 'builder.h'),$directorySource.Substring($builderStart,$builderEnd-$builderStart),[Text.UTF8Encoding]::new($false))
    $builderObject = Join-Path $Output 'builder.obj'
    & $compiler.FullName @options /c "/I$Output" "$PSScriptRoot/nls-model/builder.c" "/Fo$builderObject"
    if($LASTEXITCODE) {throw 'Production dentry builder model compilation failed.'}
    $objects.Add($builderObject)
    $exe=Join-Path $Output 'nls-model.exe'
    & $compiler.FullName @options "/I$Output" "$PSScriptRoot/nls-model/test.c" "/Fe$exe" "/Fo$Output/test.obj" @objects /link "/LIBPATH:$toolRoot/lib/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/ucrt/$Architecture" "/LIBPATH:$kits/Lib/$($sdk.Name)/um/$Architecture"
    if($LASTEXITCODE) {throw 'NLS model link failed.'}
    if($Benchmark) {& $exe --bench} else {& $exe}
    if($LASTEXITCODE) {throw 'NLS model failed.'}
} finally {$env:PATH=$oldPath}
