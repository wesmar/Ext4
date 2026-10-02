# SPDX-License-Identifier: GPL-2.0-only
# luks.ps1 - LUKS volumes through ext4ctl and ext4.sys (guest).
#
# Unlocks the ext4-in-LUKS partitions of the test disk (luks-image.sh) by
# the UUIDs ext4ctl list reports, checks what Linux wrote (manifest.sha256),
# writes a set of its own (sizes around sector and page boundaries, an
# overwrite at odd offsets, unbuffered write-through, rename, delete) with a
# manifest for luks.sh to check from Linux, runs the functional suite on the
# LUKS2 volume, and locks both. Also: a wrong passphrase is refused, and the
# LVM partition's linear and thin volumes open read-only and match Linux.
# Stops at the first broken step.
#
#   powershell -File luks.ps1 [-Pass <test passphrase>]
param([string]$Pass = '<TEST_LUKS_PASSPHRASE>')
# every step is checked by hand: under 'Stop', Windows PowerShell turns a native
# program's stderr (ext4ctl refusing a passphrase, as it should) into a terminating error
$ErrorActionPreference = 'Continue'
$t = 'C:\Windows\Temp'
$ctl = "$t\ext4ctl.exe"
$fail = 0
function Check($name, $ok, $detail = '') { if ($ok) { "  ok   $name $detail" } else { "  FAIL $name $detail"; $script:fail++ } }

# the LUKS headers on the test disk, by UUID (volume numbers change); a disk
# attached a moment ago may still be on its way
for ($i = 0; $i -lt 150; $i++) {
    $list = & $ctl list
    if (@($list | Select-String 'LUKS').Count -ge 5) { break }
    Start-Sleep -Milliseconds 100
}
function UuidOf($line) { $line -replace '.*UUID ([0-9a-f-]+).*', '$1' }
$luks2 = @($list | Select-String 'LUKS2' | Where-Object { $_.Line -notmatch 'label (serpent|nodigest)' } | ForEach-Object { UuidOf $_.Line })
$serpent = @($list | Select-String 'label serpent' | ForEach-Object { UuidOf $_.Line })
$nodigest = @($list | Select-String 'label nodigest' | ForEach-Object { UuidOf $_.Line })
$luks1 = @($list | Select-String 'LUKS1' | ForEach-Object { $_.Line -replace '.*UUID ([0-9a-f-]+).*', '$1' })
if ($luks2.Count -lt 2 -or $luks1.Count -lt 1) { "  FAIL test disk not found (LUKS2: $($luks2.Count), LUKS1: $($luks1.Count))"; "LUKS-WIN: 1 failed"; exit 1 }

"-- headers ext4ctl must not open"
Check 'a cipher the driver lacks is listed as such' ([bool]($list -match 'cannot be opened here: cipher serpent-xts-plain64'))
$r = $Pass | & $ctl unlock "UUID=$($serpent[0])" --passphrase-stdin 2>&1
Check 'a serpent volume is refused, not opened into garbage' ($serpent.Count -eq 1 -and $LASTEXITCODE -ne 0 -and "$r" -match 'serpent') "($r)"
$r = $Pass.ToUpper() | & $ctl unlock "UUID=$($nodigest[0])" --passphrase-stdin 2>&1
Check 'an empty digest does not let a wrong passphrase in' ($nodigest.Count -eq 1 -and $LASTEXITCODE -ne 0) "($r)"

"-- unlock"
$r = $Pass.ToUpper() | & $ctl unlock "UUID=$($luks1[0])" --passphrase-stdin 2>&1
Check 'wrong passphrase refused' ($LASTEXITCODE -ne 0) "($r)"
$r = $Pass | & $ctl unlock "UUID=$($luks2[0])" --letter G --passphrase-stdin 2>&1
Check 'LUKS2 (Argon2id, 4 KiB sectors) mounted as G:' ($LASTEXITCODE -eq 0 -and (Test-Path 'G:\')) "($($r -join ' '))"
$r = $Pass | & $ctl unlock "UUID=$($luks1[0])" --letter H --passphrase-stdin 2>&1
Check 'LUKS1 (PBKDF2) mounted as H:' ($LASTEXITCODE -eq 0 -and (Test-Path 'H:\')) "($($r -join ' '))"
if ($fail) { "LUKS-WIN: $fail failed"; exit 1 }

$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
function Rand([int]$n) { $b = New-Object byte[] $n; $rng.GetBytes($b); $b }

foreach ($d in 'G', 'H') {
    $root = "$($d):\"
    "-- $root read what Linux wrote"
    $bad = 0; $n = 0
    foreach ($line in Get-Content "$root\manifest.sha256") {
        if ($line -notmatch '^([0-9a-f]{64})\s+\*?(.+)$') { continue }
        $n++
        $p = Join-Path $root ($Matches[2] -replace '/', '\')
        if ((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() -ne $Matches[1]) { $bad++ }
    }
    Check "$n files byte-exact" ($n -gt 0 -and $bad -eq 0) "($bad differ)"

    "-- $root write a set for Linux"
    $dir = Join-Path $root 'winwrite'      # "G:\winwrite": FullName below is canonical, and Substring counts on it
    if ([IO.Directory]::Exists($dir)) { [IO.Directory]::Delete($dir, $true) }
    [void][IO.Directory]::CreateDirectory("$dir\sub")
    foreach ($s in 1, 511, 512, 513, 4095, 4096, 4097, 65536, 1048699, 5242880) { [IO.File]::WriteAllBytes("$dir\f$s.bin", (Rand $s)) }
    [IO.File]::WriteAllBytes("$dir\sub\deep.bin", (Rand 300000))
    $fs = [IO.File]::Open("$dir\f1048699.bin", 'Open', 'ReadWrite')
    foreach ($o in 1000, 4090, 70000, 1048000) { $fs.Position = $o; $x = Rand 777; $fs.Write($x, 0, $x.Length) }
    $fs.Close()
    $opt = [IO.FileOptions]([int][IO.FileOptions]::WriteThrough -bor 0x20000000)     # + FILE_FLAG_NO_BUFFERING
    $h = New-Object IO.FileStream -ArgumentList "$dir\direct.bin", ([IO.FileMode]::Create), ([IO.FileAccess]::Write), ([IO.FileShare]::None), 4096, $opt
    $x = Rand 65536; $h.Write($x, 0, $x.Length); $h.Close()
    [IO.File]::Move("$dir\f4097.bin", "$dir\renamed.bin")
    [IO.File]::Delete("$dir\f513.bin")
    $lines = Get-ChildItem $dir -Recurse -File | ForEach-Object {
        (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() + '  ./winwrite/' + $_.FullName.Substring($dir.Length + 1).Replace('\', '/')
    }
    [IO.File]::WriteAllText("$root\winwrite.sha256", ($lines -join "`n") + "`n")
    Check "$($lines.Count) files written" ($lines.Count -eq 11)
}

"-- functional suite on G: (LUKS2)"
$r = & powershell -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -File "$t\ext4test.ps1" -Root 'G:\ext4test' 2>&1
$functionalRc = $LASTEXITCODE
$res = ($r | Select-String 'RESULT').Line
$r | Where-Object { $_ -match 'FAIL|unexpected error|Exception' } | ForEach-Object { "  functional detail: $_" }
Check 'functional suite' ($functionalRc -eq 0 -and $res -match ', 0 failed') "(exit $functionalRc; $res)"
if (Test-Path 'G:\ext4test') { [IO.Directory]::Delete('G:\ext4test', $true) }

"-- resize regression on G: (LUKS2)"
$r = & powershell -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -File "$t\resize.ps1" -Root G:\ -Repeat 2 2>&1
$resizeRc = $LASTEXITCODE
$r | ForEach-Object { "  $_" }
Check 'cached/uncached shrink and regrow' ($resizeRc -eq 0 -and ($r -match '^RESIZE: \d+ cases, 0 failed$')) "(exit $resizeRc)"

"-- lock"
$r = & $ctl lock G: 2>&1; Check 'G: locked' ($LASTEXITCODE -eq 0) "($r)"
$r = & $ctl lock H: 2>&1; Check 'H: locked' ($LASTEXITCODE -eq 0) "($r)"

"-- LUKS2 with LVM inside: linear and thin volumes, read-only"
$r = $Pass | & $ctl unlock "UUID=$($luks2[1])" --read-only --passphrase-stdin 2>&1
Check 'LVM partition unlocked' ($LASTEXITCODE -eq 0) "($($r[-1]))"
$st = & $ctl status
$idx = (($st | Select-String $luks2[1]).Line -split '\s+')[0]
$lvs = & $ctl lv list $idx 2>&1
Check 'VG qtest found' ($LASTEXITCODE -eq 0 -and ($lvs -match '^VG qtest')) "($($lvs[0]))"
Check 'thin volume mapped' ([bool]($lvs -match '^qtest/root\s+thin .* ext2/3/4')) "($(($lvs -match 'qtest/root') -join ''))"
$r = & $ctl lv open $idx qtest/lin --letter L 2>&1
Check 'linear qtest/lin mounted as L:' ($LASTEXITCODE -eq 0 -and (Test-Path 'L:\')) "($($r -join ' '))"
$r = & $ctl lv open $idx root --letter R 2>&1
Check 'thin qtest/root mounted as R:' ($LASTEXITCODE -eq 0 -and (Test-Path 'R:\')) "($($r -join ' '))"
foreach ($d in 'L', 'R') {
    $root = "$($d):\"
    $bad = 0; $n = 0
    foreach ($line in Get-Content "$root\manifest.sha256") {
        if ($line -notmatch '^([0-9a-f]{64})\s+\*?(.+)$') { continue }
        $n++
        $p = Join-Path $root ($Matches[2] -replace '/', '\')
        if ((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() -ne $Matches[1]) { $bad++ }
    }
    Check "$($root): $n files byte-exact" ($n -gt 0 -and $bad -eq 0) "($bad differ)"
    $w = $true; try { [IO.File]::WriteAllText("$root\refused.txt", 'x') } catch { $w = $false }
    Check "$($root): writes refused" (-not $w)
}
$r = & $ctl lock $idx 2>&1
Check 'lock refused while volumes are open' ($LASTEXITCODE -ne 0) "($r)"
$r = & $ctl lv close L: 2>&1; Check 'L: closed' ($LASTEXITCODE -eq 0) "($r)"
$r = & $ctl lock $idx --force 2>&1; Check 'locked with R: still open (--force)' ($LASTEXITCODE -eq 0) "($r)"
Check 'R: gone' (-not (Test-Path 'R:\'))
Check 'nothing left unlocked' ((& $ctl status) -match 'no unlocked')

"LUKS-WIN: $fail failed"
exit $fail
