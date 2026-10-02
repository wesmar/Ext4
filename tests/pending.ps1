# SPDX-License-Identifier: GPL-2.0-only
# pending.ps1 - delete-pending scenarios, one thread, deterministic.
#
# Each scenario ends with every name removable and nothing left open; a name
# that cannot be deleted afterwards (access denied = still delete-pending) or
# a name that should be gone but is not is a failure. These are the
# interleavings race.ps1 produces at random, taken one by one.
#
#   powershell -File pending.ps1 [-Root E:\pending]
param([string]$Root = 'E:\pending')
$ErrorActionPreference = 'Stop'
$drive = [IO.Path]::GetPathRoot($Root)
for ($i = 0; $i -lt 100 -and -not [IO.Directory]::Exists($drive); $i++) { Start-Sleep -Milliseconds 100 }
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Runtime.InteropServices;
public static class P {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.I1)] public static extern bool CreateSymbolicLink(string link, string target, int flags);
    public static FileStream Hold(string p) { return new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete); }
}
"@
$fail = 0
function Check($name, $ok) { if ($ok) { "  ok   $name" } else { "  FAIL $name"; $script:fail++ } }
function Gone($p) { -not ([IO.File]::Exists($p) -or [IO.Directory]::Exists($p)) }
function Fresh { if ([IO.Directory]::Exists($Root)) { cmd /c "rd /s /q $Root" }; [void][IO.Directory]::CreateDirectory($Root) }
function TryDelete($p) { try { [IO.File]::Delete($p); $true } catch { $false } }

Fresh; $a = "$Root\a"; [IO.File]::WriteAllText($a, 'x')
$h = [P]::Hold($a); [IO.File]::Delete($a); $h.Close()
Check '1 delete while another handle is open, then close' (Gone $a)

Fresh; $a = "$Root\a"; $l = "$Root\l"; [IO.File]::WriteAllText($a, 'x'); [void][P]::CreateSymbolicLink($l, 'a', 0)
$h = [P]::Hold($l); [IO.File]::Delete($a); $h.Close()
Check '2 target deleted while open through a symlink' (Gone $a)
Check '2 the symlink itself is still removable' (TryDelete $l)

Fresh; $a = "$Root\a"; $l = "$Root\l"; [IO.File]::WriteAllText($a, 'x'); [void][P]::CreateSymbolicLink($l, 'a', 0)
$h = [P]::Hold($l); [IO.File]::Delete($l); $h.Close()
Check '3 symlink deleted while open through it' (Gone $l)
Check '3 its target stays' (-not (Gone $a))

Fresh; $a = "$Root\a"; $b = "$Root\b"; [IO.File]::WriteAllText($a, 'x')
$h = [P]::Hold($a); [IO.File]::Move($a, $b); [IO.File]::Delete($b); $h.Close()
Check '4 renamed while open, deleted under the new name' (Gone $b)
Check '4 old name free for reuse' ((TryDelete $a) -and (Gone $a))

Fresh; $a = "$Root\a"; [IO.File]::WriteAllText($a, 'x')
$h = [P]::Hold($a); [IO.File]::Delete($a)
$re = $true; try { [IO.File]::WriteAllText($a, 'y') } catch { $re = $false }
$h.Close()
Check '5 name delete-pending: recreate refused while open' (-not $re)
Check '5 gone after the close' (Gone $a)
[IO.File]::WriteAllText($a, 'z'); Check '5 recreated afterwards' ([IO.File]::ReadAllText($a) -eq 'z')

Fresh; $a = "$Root\a"; $l = "$Root\l"; [IO.File]::WriteAllText($a, 'x'); [void][P]::CreateSymbolicLink($l, 'a', 0)
[IO.File]::Delete($a); $dang = $true; try { [P]::Hold($l).Close() } catch { $dang = $false }
[IO.File]::WriteAllText($a, 'back')
$txt = ''; try { $txt = [IO.File]::ReadAllText($l) } catch { }
Check '6 symlink resolves again once its target is recreated' ($txt -eq 'back')
Check '6 the symlink is removable' (TryDelete $l)

Fresh; $a = "$Root\a"; $b = "$Root\b"; [IO.File]::WriteAllText($a, 'x')
$h = [P]::Hold($a); [IO.File]::Delete($a); $mv = $true; try { [IO.File]::Move($a, $b) } catch { $mv = $false }; $h.Close()
Check '7 a delete-pending name cannot be renamed' (-not $mv)
Check '7 and it is gone after the close' ((Gone $a) -and (Gone $b))

# DeleteFileW reads the reparse tag of what it opened: a symlink reported
# with tag 0 was reopened through its target, and with the target gone the
# link could not be deleted (access denied) - race.ps1 hit it now and then
Fresh; $a = "$Root\a"; $l = "$Root\l"; $d = "$Root\d"; [IO.File]::WriteAllText($a, 'x')
[void][P]::CreateSymbolicLink($l, 'a', 0); [void][P]::CreateSymbolicLink($d, 'nothere', 0)
[void][IO.File]::ReadAllText($l); [IO.File]::Delete($a)
$it = Get-Item -LiteralPath $d -Force
Check '8 a dangling symlink is still listed as one' ((($it.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) -and (($it.Attributes -band [IO.FileAttributes]::Normal) -eq 0))
Check '8 and its target text is readable' ("$($it.Target)" -eq 'nothere')
Check '8 a followed symlink whose target was deleted is removable' ((TryDelete $l) -and (Gone $l))
Check '8 a dangling symlink is removable' ((TryDelete $d) -and (Gone $d))

cmd /c "rd /s /q $Root"
"pending: $fail failed"
