#Requires -Version 5.1
<#
.SYNOPSIS
    Self-configuring build of the ext4 kernel-mode file system driver.

.DESCRIPTION
    Every tool is located from evidence on disk, never from version numbers
    written into this script:

      * Visual Studio  - vswhere candidates plus a scan of the standard roots;
                         an installation is accepted only when it really holds
                         Hostx64\x64\cl.exe AND the WindowsKernelModeDriver10.0
                         platform toolset (vswhere -products * also lists
                         products without a C++ compiler, such as SQL Server
                         Management Studio).
      * MSVC toolset   - the highest VC\Tools\MSVC\<v> that contains cl.exe,
                         compared as [version].
      * Windows SDK/WDK- the highest Windows Kits\10 version that is complete
                         for a kernel build (km\wdm.h and km\x64\ntoskrnl.lib),
                         compared as [version]; it overrides the version stored
                         in the project, so old and new kits both work.

    The project compiles with /W4 /WX: a warning is a build error.

    Output:  bin\<driver>.sys            (driver name = TargetName in the project)
             bin\ext4ctl.exe             LUKS unlock and volume control (user mode, static CRT)
             symbols\<driver>.pdb        for crash dump analysis, kept out of bin\
             obj\                        intermediate files, deleted after a successful build

.PARAMETER Configuration
    Release (default) or Debug.
.PARAMETER Clean
    Delete bin\ and obj\ first.
.PARAMETER Rebuild
    Full rebuild instead of an incremental build.
.PARAMETER KeepObj
    Keep obj\ after the build (by default it is deleted, leaving only bin\).
.PARAMETER Deploy
    Copy the driver to %WINDIR%\System32\drivers afterwards (Administrator).
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$Clean,
    [switch]$Rebuild,
    [switch]$KeepObj,
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$Root     = Split-Path -Parent $MyInvocation.MyCommand.Definition
$Platform = 'x64'
$BinDir   = Join-Path $Root 'bin'
$ObjDir   = Join-Path $Root 'obj'
$SymDir   = Join-Path $Root 'symbols'
$Project  = Join-Path $Root 'src\ext4.vcxproj'
$CtlProject = Join-Path $Root 'tools\ext4ctl\ext4ctl.vcxproj'

function Write-Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Write-Info { param($m) Write-Host "    $m" -ForegroundColor Gray }
function Write-Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }
function Stop-Build { param($m) Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }

function Find-VisualStudio {
    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $candidates += & $vswhere -products '*' -prerelease -format value -property installationPath 2>$null
    }
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        $vsBase = Join-Path $base 'Microsoft Visual Studio'
        if (Test-Path $vsBase) {
            $candidates += Get-ChildItem $vsBase -Directory -ErrorAction SilentlyContinue |
                ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
                Select-Object -ExpandProperty FullName
        }
    }
    foreach ($vs in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $cl = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
              ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe' } |
              Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $cl) { continue }
        $toolset = Get-ChildItem (Join-Path $vs 'MSBuild\Microsoft\VC') -Directory -ErrorAction SilentlyContinue |
                   ForEach-Object { Join-Path $_.FullName "Platforms\$Platform\PlatformToolsets\WindowsKernelModeDriver10.0" } |
                   Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $toolset) { continue }
        $msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
        if (-not (Test-Path $msbuild)) { $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe' }
        if (-not (Test-Path $msbuild)) { continue }
        return [pscustomobject]@{ Path = $vs; MSBuild = $msbuild }
    }
    return $null
}

function Find-MsvcToolset {
    param($VsPath)
    Get-ChildItem (Join-Path $VsPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe') } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1 -ExpandProperty Name
}

function Find-KernelSdk {
    $kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $inc = Join-Path $kit 'Include'
    $lib = Join-Path $kit 'Lib'
    if (-not (Test-Path $inc)) { return $null }
    Get-ChildItem $inc -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Where-Object {
            (Test-Path (Join-Path $_.FullName 'km\wdm.h')) -and
            (Test-Path (Join-Path $lib "$($_.Name)\km\x64\ntoskrnl.lib"))
        } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1 -ExpandProperty Name
}

if (-not (Test-Path $Project)) { Stop-Build "Project not found: $Project" }
$Driver = ([xml](Get-Content $Project -Raw)).Project.PropertyGroup.TargetName | Where-Object { $_ } | Select-Object -First 1
if (-not $Driver) { Stop-Build "No TargetName in $Project" }

Write-Step "ext4 driver build - $Configuration|$Platform -> bin\$Driver.sys"

$vs = Find-VisualStudio
if (-not $vs) { Stop-Build "No Visual Studio with the C++ compiler AND the WindowsKernelModeDriver10.0 (x64) toolset was found. Install the 'Desktop development with C++' workload and the WDK." }
Write-Ok   "Visual Studio : $($vs.Path)"
Write-Info "MSBuild       : $($vs.MSBuild)"
$msvc = Find-MsvcToolset -VsPath $vs.Path
if ($msvc) { Write-Ok "MSVC toolset  : $msvc" }
$sdk = Find-KernelSdk
if (-not $sdk) { Stop-Build "No complete Windows SDK/WDK for x64 kernel builds (Include\<v>\km\wdm.h and Lib\<v>\km\x64\ntoskrnl.lib)." }
Write-Ok   "Kernel SDK/WDK: $sdk"

if ($Clean) {
    Write-Step 'Clean'
    foreach ($d in @($BinDir, $ObjDir)) {
        if (Test-Path $d) { [IO.Directory]::Delete($d, $true); Write-Info "removed $d" }
    }
}

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
Write-Step "MSBuild ($target)"
$msbuildArgs = @(
    $Project, "/t:$target",
    "/p:Configuration=$Configuration", "/p:Platform=$Platform",
    "/p:OutDir=$BinDir\", "/p:IntDir=$ObjDir\$Configuration\",
    "/p:WindowsTargetPlatformVersion=$sdk",
    '/p:SignMode=Off',
    '/nologo', '/m', '/verbosity:minimal', '/clp:Summary'
)
if ($msvc) { $msbuildArgs += "/p:VCToolsVersion=$msvc" }
& $vs.MSBuild @msbuildArgs
if ($LASTEXITCODE -ne 0) { Stop-Build "MSBuild failed with exit code $LASTEXITCODE." }

$sys = Join-Path $BinDir "$Driver.sys"
if (-not (Test-Path $sys)) { Stop-Build "Build reported success but $sys is missing." }

# ext4ctl.exe: user mode, the toolset of the same Visual Studio, the same kit
Write-Step "MSBuild ($target) ext4ctl"
$ctlArgs = @(
    $CtlProject, "/t:$target",
    "/p:Configuration=$Configuration", "/p:Platform=$Platform",
    "/p:OutDir=$BinDir\", "/p:IntDir=$ObjDir\ext4ctl\$Configuration\",
    "/p:WindowsTargetPlatformVersion=$sdk",
    '/nologo', '/m', '/verbosity:minimal', '/clp:Summary'
)
if ($msvc) { $ctlArgs += "/p:VCToolsVersion=$msvc" }
& $vs.MSBuild @ctlArgs
if ($LASTEXITCODE -ne 0) { Stop-Build "MSBuild (ext4ctl) failed with exit code $LASTEXITCODE." }
$ctl = Join-Path $BinDir 'ext4ctl.exe'
if (-not (Test-Path $ctl)) { Stop-Build "Build reported success but $ctl is missing." }

# bin\ keeps exactly the driver and the tool; symbols go to symbols\, the WDK package folder and .inf/.cat copies go
New-Item -ItemType Directory $SymDir -Force | Out-Null
foreach ($pdb in "$Driver.pdb", 'ext4ctl.pdb') {
    Move-Item (Join-Path $BinDir $pdb) (Join-Path $SymDir $pdb) -Force -ErrorAction SilentlyContinue
}
Get-ChildItem $BinDir -Directory -ErrorAction SilentlyContinue | ForEach-Object { [IO.Directory]::Delete($_.FullName, $true) }
Get-ChildItem $BinDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -ne '.sys' -and $_.Name -ne 'ext4ctl.exe' } |
    ForEach-Object { [IO.File]::Delete($_.FullName) }

if (-not $KeepObj -and (Test-Path $ObjDir)) { [IO.Directory]::Delete($ObjDir, $true) }

$info = Get-Item $sys
Write-Step 'Output'
Write-Ok ("{0}  {1:N0} bytes  ({2})" -f $sys, $info.Length, $info.LastWriteTime)
$info = Get-Item $ctl
Write-Ok ("{0}  {1:N0} bytes  ({2})" -f $ctl, $info.Length, $info.LastWriteTime)

if ($Deploy) {
    Write-Step 'Deploy to System32\drivers'
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { Stop-Build 'Deploy needs an elevated shell.' }
    $dest = Join-Path $env:WINDIR "System32\drivers\$Driver.sys"
    try {
        Copy-Item $sys $dest -Force
    } catch {
        # a loaded driver image cannot be overwritten, but it can be renamed
        $old = "$dest.$([IO.Path]::GetRandomFileName()).old"
        Move-Item $dest $old -Force
        Copy-Item $sys $dest -Force
        Write-Info "previous image moved to $(Split-Path $old -Leaf)"
    }
    Write-Ok "copied to $dest"
}
Write-Host ''
