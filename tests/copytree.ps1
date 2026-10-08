# SPDX-License-Identifier: GPL-2.0-only
# copytree.ps1 - whole directory trees copied onto an ext volume the way
# Windows programs copy them, compared with their sources and deleted.
#
# robocopy goes the way of Explorer and copy tools: CreateDirectory,
# CopyFileEx, attributes and times; .NET creates each directory and copies
# each file on its own. A directory of 2000 subdirectories grows past one
# block, into an index. Every directory and file must arrive, every byte
# must match, and everything must go again. ext4verify.sh then checks the
# volume from Linux, including that the driver recorded no error.
#
#   powershell -File copytree.ps1 -Root X:\copytree
param([Parameter(Mandatory)][string]$Root,
      [string[]]$Sources = @("$env:windir\System32\WindowsPowerShell",
                             "$env:ProgramFiles\WindowsPowerShell",
                             "$env:windir\Web",
                             "$env:windir\System32\en-US"))
$ErrorActionPreference = 'Continue'
$fail = 0
function Check($name, $ok, $detail = '') { if ($ok) { "  ok   $name $detail" } else { "  FAIL $name $detail"; $script:fail++ } }

# every directory and file below Path, by relative name; files with their length
function Tree([string]$Path, [string]$Prefix = '', [hashtable]$Into = @{}) {
    foreach ($d in [IO.Directory]::EnumerateDirectories($Path)) {
        $n = $Prefix + [IO.Path]::GetFileName($d)
        $Into[$n] = -1
        [void](Tree $d ($n + '\') $Into)
    }
    foreach ($f in [IO.Directory]::EnumerateFiles($Path)) {
        $Into[$Prefix + [IO.Path]::GetFileName($f)] = (New-Object IO.FileInfo $f).Length
    }
    return $Into
}

# the copy at Copy holds what Source holds: the same names, lengths and bytes
function Same([string]$Source, [string]$Copy) {
    $s = Tree $Source; $c = Tree $Copy
    $missing = @($s.Keys | Where-Object { -not $c.ContainsKey($_) })
    $extra = @($c.Keys | Where-Object { -not $s.ContainsKey($_) })
    $size = @($s.Keys | Where-Object { $c.ContainsKey($_) -and $s[$_] -ne $c[$_] })
    $bytes = @($s.Keys | Where-Object { $s[$_] -ge 0 -and $c.ContainsKey($_) -and $s[$_] -eq $c[$_] } | Where-Object {
        (Get-FileHash -LiteralPath (Join-Path $Source $_)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $Copy $_)).Hash })
    $dirs = @($s.Values | Where-Object { $_ -eq -1 }).Count
    $detail = "($dirs dirs, $($s.Count - $dirs) files; missing $($missing.Count), extra $($extra.Count), size $($size.Count), bytes $($bytes.Count))"
    @($missing + $extra + $size + $bytes) | Select-Object -First 5 | ForEach-Object { Write-Host "         $_" }
    return @{ Ok = ($missing.Count + $extra.Count + $size.Count + $bytes.Count) -eq 0 -and $s.Count -gt 0; Detail = $detail }
}

# a copy made with .NET alone: one CreateDirectory per directory, one copy per file
function NetCopy([string]$Source, [string]$Target) {
    [void][IO.Directory]::CreateDirectory($Target)
    foreach ($d in [IO.Directory]::EnumerateDirectories($Source)) {
        NetCopy $d (Join-Path $Target ([IO.Path]::GetFileName($d)))
    }
    foreach ($f in [IO.Directory]::EnumerateFiles($Source)) {
        [IO.File]::Copy($f, (Join-Path $Target ([IO.Path]::GetFileName($f))), $true)
    }
}

if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
[void][IO.Directory]::CreateDirectory($Root)
"-- trees copied to $Root"

$i = 0
foreach ($src in $Sources) {
    if (-not (Test-Path -LiteralPath $src)) { "  skip $src (not here)"; continue }
    $i++
    $name = Split-Path -Leaf $src

    $robo = Join-Path $Root "robocopy-$i-$name"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & robocopy $src $robo /E /COPY:DAT /DCOPY:DAT /R:0 /W:0 /NFL /NDL /NJH /NJS /NP | Out-Null
    $rc = $LASTEXITCODE
    $ms = $sw.ElapsedMilliseconds
    $r = Same $src $robo
    Check "robocopy $src" ($rc -lt 8 -and $r.Ok) "$($r.Detail) robocopy exit $rc, copied in $ms ms"

    # xcopy keeps the attributes too (/K), read-only folders among them
    $xc = Join-Path $Root "xcopy-$i-$name"
    $sw.Restart()
    & xcopy $src "$xc\" /E /I /H /K /Q /Y | Out-Null
    $rc = $LASTEXITCODE
    $ms = $sw.ElapsedMilliseconds
    $r = Same $src $xc
    Check "xcopy $src" ($rc -eq 0 -and $r.Ok) "$($r.Detail) xcopy exit $rc, copied in $ms ms"

    $net = Join-Path $Root "dotnet-$i-$name"
    $sw.Restart()
    try { NetCopy $src $net; $err = '' } catch { $err = $_.Exception.Message }
    $ms = $sw.ElapsedMilliseconds
    $r = Same $src $net
    Check ".NET copy $src" ($err -eq '' -and $r.Ok) "$($r.Detail) $err copied in $ms ms"
}

"-- one directory of 2000 subdirectories (an index of its own)"
$wide = Join-Path $Root 'wide'
[void][IO.Directory]::CreateDirectory($wide)
$made = 0
for ($n = 0; $n -lt 2000; $n++) {
    try { [void][IO.Directory]::CreateDirectory((Join-Path $wide ("katalog-{0:D4}-directory" -f $n))); $made++ } catch { }
}
$listed = @([IO.Directory]::EnumerateDirectories($wide)).Count
$found = 0
for ($n = 0; $n -lt 2000; $n += 37) { if ([IO.Directory]::Exists((Join-Path $wide ("KATALOG-{0:D4}-DIRECTORY" -f $n)))) { $found++ } }
Check '2000 directories made, listed and looked up' ($made -eq 2000 -and $listed -eq 2000 -and $found -eq 55) "(made $made, listed $listed, found $found of 55)"

"-- everything deleted"
Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
Check 'the copies are gone' (-not (Test-Path -LiteralPath $Root))

"COPYTREE: $fail failed"
exit $fail
