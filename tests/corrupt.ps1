# SPDX-License-Identifier: GPL-2.0-only
# corrupt.ps1 - the guest side of corrupt-vm.ps1: what the driver does with
# damage it finds on disk (planted by corrupt-image.sh). A corrupt inode is
# listed and refused, a directory block that fails its checksum is refused,
# new data goes around a group whose block bitmap is corrupt, a file whose
# blocks lie there can still be deleted; a volume that asks for remount-ro
# stops writing at the first error and goes on reading; one whose superblock
# fails its checksum is read and never written.
#
#   powershell -File corrupt.ps1
$ErrorActionPreference = 'Continue'
$fail = 0
function Check($name, $ok, $detail = '') { if ($ok) { "  ok   $name $detail" } else { "  FAIL $name $detail"; $script:fail++ } }
function Fails([scriptblock]$Action) { try { & $Action; return $false } catch { return $true } }
function Volume($label) {
    for ($i = 0; $i -lt 300; $i++) {
        $v = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq $label -and $_.DriveLetter }
        if ($v) { return "$($v.DriveLetter):" }
        Start-Sleep -Milliseconds 100
    }
    return $null
}
Add-Type -TypeDefinition @'
public static class Pattern {
    public static byte[] Make(int n) { byte[] b = new byte[n]; for (int i = 0; i < n; i++) b[i] = (byte)((i * 7 + 3) % 251); return b; }
    public static bool Same(byte[] a, byte[] b) { if (a.Length != b.Length) return false; for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false; return true; }
}
'@

$c = Volume 'errcont'
$r = Volume 'errro'
Check 'both volumes mounted' ($c -and $r) "($c $r)"

if ($c) {
    "-- errors=continue"
    $names = @(Get-ChildItem -LiteralPath "$c\" -Force | ForEach-Object { $_.Name })
    Check 'a file whose inode is corrupt is listed' ($names -contains 'badinode.txt') "($($names -join ','))"
    Check '... and refused when opened' (Fails { [IO.File]::ReadAllText("$c\badinode.txt") })
    Check 'a good file read' ([IO.File]::ReadAllText("$c\good.txt") -eq "good`n")
    Check 'a name in a directory whose block fails its checksum: refused' (Fails { [IO.File]::ReadAllText("$c\baddir\a.txt") })
    Check '... and nothing made there' (Fails { [IO.File]::WriteAllText("$c\baddir\new.txt", 'x') })
    $data = [Pattern]::Make(10MB)
    [IO.File]::WriteAllBytes("$c\new.bin", $data)
    Check 'a file written around the group with the corrupt bitmap' ([Pattern]::Same([IO.File]::ReadAllBytes("$c\new.bin"), $data))
    [IO.File]::Delete("$c\good.txt")
    Check 'a file with blocks in that group deleted' (-not (Test-Path -LiteralPath "$c\good.txt"))
}

if ($r) {
    "-- errors=remount-ro"
    Check 'a good file read' ([IO.File]::ReadAllText("$r\good.txt") -eq "good`n")
    Check 'the corrupt inode refused' (Fails { [IO.File]::ReadAllText("$r\badinode.txt") })
    Check '... and the volume stopped writing' (Fails { [IO.File]::WriteAllText("$r\after-error.txt", 'x') })
    Check '... while reads go on' ([IO.File]::ReadAllText("$r\good.txt") -eq "good`n")
}

$b = Volume 'badsb'
Check 'the volume whose superblock fails its checksum mounted' ($b -ne $null) "($b)"
if ($b) {
    "-- superblock checksum"
    Check 'a good file read' ([IO.File]::ReadAllText("$b\good.txt") -eq "good`n")
    Check '... and nothing written' (Fails { [IO.File]::WriteAllText("$b\new.txt", 'x') })
}

"CORRUPT-WIN: $fail failed"
exit $fail
