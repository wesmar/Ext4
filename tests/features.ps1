# features.ps1 - Linux features through ext4.sys (guest), on the disk of features-image.sh.
#
# Names are looked up folded as Linux folds them ("ŁÓDŹ.TXT" is "Łódź.txt",
# "STRASSE.TXT" is "straße.txt"), a name that folds to an existing one is
# refused, and enough new files go in to split the index blocks - which
# must hash the folded names, or Linux will not find them. A directory
# made here inherits the casefold flag. Files and directories with chattr +i
# and +a refuse what Linux refuses. features.sh checks the result from Linux.
#
#   powershell -File features.ps1
$ErrorActionPreference = 'Continue'
$fail = 0
function Check($name, $ok, $detail = '') { if ($ok) { "  ok   $name $detail" } else { "  FAIL $name $detail"; $script:fail++ } }

$vol = $null
for ($i = 0; $i -lt 150 -and -not $vol; $i++) {
    $vol = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq 'cftest' -and $_.DriveLetter }
    if (-not $vol) { Start-Sleep -Milliseconds 100 }
}
if (-not $vol) { "  FAIL features disk not mounted"; "FEATURES-WIN: 1 failed"; exit 1 }
$root = "$($vol.DriveLetter):\"
$cf = Join-Path $root 'cf'
"-- $root"

"-- what Linux wrote"
$bad = 0; $n = 0
foreach ($line in [IO.File]::ReadAllLines("$root\cf.sha256", [Text.Encoding]::UTF8)) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { continue }
    $n++
    if ((Get-FileHash -LiteralPath (Join-Path $cf $Matches[2]) -Algorithm SHA256).Hash.ToLower() -ne $Matches[1]) { $bad++ }
}
Check "$n files byte-exact" ($n -eq 300 -and $bad -eq 0) "($bad differ)"

"-- lookups folded as Linux folds them"
foreach ($pair in @(@('ŁÓDŹ.TXT', 'Łódź.txt'), @('STRASSE.TXT', 'straße.txt'), @('mixedcase.txt', 'MixedCase.TXT'),
                    @('ąćęłńóśźż ĄĆĘŁŃÓŚŹŻ.TXT', 'ĄĆĘŁŃÓŚŹŻ ąćęłńóśźż.txt'), @('ΩMEGA.TXT', 'Ωmega.txt'), @('file-ąęś-150.DAT', 'File-ĄĘŚ-150.dat'))) {
    $a = Join-Path $cf $pair[0]; $b = Join-Path $cf $pair[1]
    $same = [IO.File]::Exists($a) -and ((Get-FileHash -LiteralPath $a).Hash -eq (Get-FileHash -LiteralPath $b).Hash)
    Check "$($pair[0]) finds $($pair[1])" $same
}

"-- a name that folds to an existing one"
$dup = $true; try { $fs = [IO.File]::Open((Join-Path $cf 'ŁÓDŹ.txt'), 'CreateNew'); $fs.Close() } catch { $dup = $false }
Check 'CreateNew of ŁÓDŹ.txt refused' (-not $dup)

"-- 400 new files: index blocks split with folded hashes"
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
$lines = @()
for ($i = 1; $i -le 400; $i++) {
    $name = if ($i % 2) { "New-Żółć-$i.bin" } else { "NEW-ŻÓŁĆ-$i-X.bin" }
    $b = New-Object byte[] (1 + ($i * 37) % 5000); $rng.GetBytes($b)
    [IO.File]::WriteAllBytes((Join-Path $cf $name), $b)
    $lines += (Get-FileHash -LiteralPath (Join-Path $cf $name) -Algorithm SHA256).Hash.ToLower() + '  ' + $name
}
$miss = 0
for ($i = 1; $i -le 400; $i += 7) {
    $name = if ($i % 2) { "new-żółć-$i.BIN" } else { "new-żółć-$i-x.BIN" }
    if (-not [IO.File]::Exists((Join-Path $cf $name))) { $miss++ }
}
Check '400 written, found again in another case' ($miss -eq 0) "($miss missing)"

"-- rename, delete, a directory"
[IO.File]::Move((Join-Path $cf 'straße.txt'), (Join-Path $cf 'Straße-Renamed.TXT'))
Check 'rename, found folded' ([IO.File]::Exists((Join-Path $cf 'STRASSE-RENAMED.txt')))
[IO.File]::Delete((Join-Path $cf 'FILE-ąęś-7.DAT'))
Check 'delete through a folded name' (-not [IO.File]::Exists((Join-Path $cf 'File-ĄĘŚ-7.dat')))
[void][IO.Directory]::CreateDirectory((Join-Path $cf 'Subdir-Ś'))
[IO.File]::WriteAllText((Join-Path $cf 'Subdir-Ś\Inner-Ś.txt'), 'inside')
Check 'file in the new directory found folded' ([IO.File]::Exists((Join-Path $cf 'SUBDIR-ś\INNER-ś.TXT')))
$lines += (Get-FileHash -LiteralPath (Join-Path $cf 'Straße-Renamed.TXT') -Algorithm SHA256).Hash.ToLower() + '  Straße-Renamed.TXT'
[IO.File]::WriteAllText("$root\winwrite.sha256", ($lines -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))

"-- the plain directory stays case-exact on disk"
[IO.File]::WriteAllText((Join-Path $root 'plain\Abc.txt'), 'x')
Check 'plain directory works' ([IO.File]::Exists((Join-Path $root 'plain\Abc.txt')))

"-- chattr +i and +a, honoured as on Linux"
$attr = Join-Path $root 'attr'
function Refused([scriptblock]$Action) { try { & $Action; return $false } catch { return $true } }
$imm = Join-Path $attr 'immutable.txt'
Check '+i: read works' ([IO.File]::ReadAllText($imm) -eq "immutable content`n")
Check '+i: shown read-only' (((Get-Item -LiteralPath $imm).Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0)
Check '+i: no write handle' (Refused { [IO.File]::Open($imm, 'Open', 'Write').Close() })
Check '+i: no append handle' (Refused { (New-Object IO.FileStream -ArgumentList $imm, ([IO.FileMode]::Append), ([Security.AccessControl.FileSystemRights]::AppendData), ([IO.FileShare]::Read), 4096, ([IO.FileOptions]::None)).Close() })
Check '+i: no delete' (Refused { [IO.File]::Delete($imm) })
Check '+i: no rename' (Refused { [IO.File]::Move($imm, (Join-Path $attr 'moved.txt')) })
Check '+i: no new times' (Refused { [IO.File]::SetLastWriteTime($imm, [DateTime]'2001-01-01') })
$r = cmd /c "mklink /H `"$attr\imm-link.txt`" `"$imm`"" 2>&1
Check '+i: no hard link' (-not (Test-Path -LiteralPath (Join-Path $attr 'imm-link.txt'))) "($r)"
$app = Join-Path $attr 'append.log'
Check '+a: no write handle' (Refused { [IO.File]::Open($app, 'Open', 'Write').Close() })
$ok = -not (Refused {
    $fs = New-Object IO.FileStream -ArgumentList $app, ([IO.FileMode]::Append), ([Security.AccessControl.FileSystemRights]::AppendData), ([IO.FileShare]::Read), 4096, ([IO.FileOptions]::None)
    $b = [Text.Encoding]::ASCII.GetBytes("from windows`n"); $fs.Write($b, 0, $b.Length); $fs.Close() })
Check '+a: appended through an append handle' ($ok -and [IO.File]::ReadAllText($app) -eq "first line`nfrom windows`n")
Check '+a: no delete' (Refused { [IO.File]::Delete($app) })
$idir = Join-Path $attr 'idir'
Check '+i dir: no new file' (Refused { [IO.File]::WriteAllText((Join-Path $idir 'new.txt'), 'x') })
Check '+i dir: no delete inside' (Refused { [IO.File]::Delete((Join-Path $idir 'inner.txt')) })
Check '+i dir: no rename inside' (Refused { [IO.File]::Move((Join-Path $idir 'inner.txt'), (Join-Path $idir 'x.txt')) })
[IO.File]::WriteAllText((Join-Path $idir 'inner.txt'), "changed`n")
Check '+i dir: a file in it still changes' ([IO.File]::ReadAllText((Join-Path $idir 'inner.txt')) -eq "changed`n")
$adir = Join-Path $attr 'adir'
[IO.File]::WriteAllText((Join-Path $adir 'added.txt'), "added`n")
Check '+a dir: a new file goes in' ([IO.File]::Exists((Join-Path $adir 'added.txt')))
Check '+a dir: no delete inside' (Refused { [IO.File]::Delete((Join-Path $adir 'inner.txt')) })
Check '+a dir: no rename inside' (Refused { [IO.File]::Move((Join-Path $adir 'inner.txt'), (Join-Path $adir 'x.txt')) })

"-- Linux extended attributes: SELinux labels inherited, kept on overwrite"
[IO.File]::WriteAllText((Join-Path $attr 'secdir\new.txt'), 'labelled')
[void][IO.Directory]::CreateDirectory((Join-Path $attr 'secdir\sub'))
Check 'file and directory made in a labelled directory' ([IO.File]::Exists((Join-Path $attr 'secdir\new.txt')) -and [IO.Directory]::Exists((Join-Path $attr 'secdir\sub')))
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class EaOverwrite {
    [StructLayout(LayoutKind.Sequential)] struct US { public ushort L, M; public IntPtr B; }
    [StructLayout(LayoutKind.Sequential)] struct OA { public int Len; public IntPtr Root; public IntPtr Name; public uint Attr; public IntPtr Sd, Qos; }
    [DllImport("ntdll.dll")] static extern int NtCreateFile(out IntPtr h, uint access, ref OA oa, long[] iosb, IntPtr alloc, uint attr, uint share, uint disp, uint opts, byte[] ea, uint ealen);
    [DllImport("ntdll.dll")] static extern int NtClose(IntPtr h);
    // FILE_OVERWRITE of an existing file with one EA: name, value
    public static int Run(string path, string name, string value) {
        byte[] n = Encoding.ASCII.GetBytes(name), v = Encoding.ASCII.GetBytes(value);
        byte[] ea = new byte[8 + n.Length + 1 + v.Length];
        ea[5] = (byte)n.Length; BitConverter.GetBytes((ushort)v.Length).CopyTo(ea, 6);
        n.CopyTo(ea, 8); v.CopyTo(ea, 8 + n.Length + 1);
        string nt = @"\??\" + path;
        US us = new US(); us.B = Marshal.StringToHGlobalUni(nt); us.L = (ushort)(nt.Length * 2); us.M = us.L;
        IntPtr pus = Marshal.AllocHGlobal(Marshal.SizeOf(us)); Marshal.StructureToPtr(us, pus, false);
        OA oa = new OA(); oa.Len = Marshal.SizeOf(oa); oa.Name = pus; oa.Attr = 0x40;
        IntPtr h; int st = NtCreateFile(out h, 0x40100000, ref oa, new long[2], IntPtr.Zero, 0x80, 1, 4, 0x60, ea, (uint)ea.Length);
        if (st == 0) NtClose(h);
        return st;
    }
}
"@
$st = [EaOverwrite]::Run((Join-Path $attr 'secfile'), 'shade', 'blue')
Check 'overwrite with an EA buffer' ($st -eq 0) ('(0x{0:X8})' -f $st)

"-- large_dir: three-level directory indexes, read and written"
$ld = $null
for ($i = 0; $i -lt 100 -and -not $ld; $i++) {
    $ld = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq 'ldtest' -and $_.DriveLetter }
    if (-not $ld) { Start-Sleep -Milliseconds 100 }
}
if ($ld) {
    $lr = "$($ld.DriveLetter):\"
    Add-Type -TypeDefinition @"
using System; using System.IO;
public static class LargeDir {
    public static readonly string Pad = new string('x', 240);
    public static string Name(char p, int i) { return p + i.ToString("D5") + "-" + Pad; }
    public static int Create(string dir, char p, int from, int to) {
        int n = 0; for (int i = from; i <= to; i++) { File.Create(Path.Combine(dir, Name(p, i))).Dispose(); n++; } return n; }
    public static int Delete(string dir, char p, int from, int to, int step) {
        int n = 0; for (int i = from; i <= to; i += step) { File.Delete(Path.Combine(dir, Name(p, i))); n++; } return n; }
    public static int Missing(string dir, char p, int from, int to, int step) {
        int n = 0; for (int i = from; i <= to; i += step) if (!File.Exists(Path.Combine(dir, Name(p, i)))) n++; return n; }
}
"@
    $big = "$lr\big"
    Check 'small index: 500 entries listed' (@(Get-ChildItem -LiteralPath $big -File).Count -eq 500)
    Check 'small index: lookup' ([IO.File]::ReadAllText("$big\entry-250.txt") -eq "large_dir`n")
    $b3 = "$lr\big3"
    Check 'Linux three-level index: 45000 entries listed' ([IO.Directory]::GetFiles($b3).Length -eq 45000)
    Check 'Linux three-level index: every 97th looked up' ([LargeDir]::Missing($b3, 'k', 1, 45000, 97) -eq 0)
    [void][LargeDir]::Create($b3, 'w', 1, 2000)
    [void][LargeDir]::Delete($b3, 'k', 1, 45000, 450)
    Check 'Linux three-level index: 2000 added, 100 deleted' ([LargeDir]::Missing($b3, 'w', 1, 2000, 1) -eq 0 -and [IO.Directory]::GetFiles($b3).Length -eq 46900)
    $g = "$lr\grow"
    [void][IO.Directory]::CreateDirectory($g)
    $t = [Diagnostics.Stopwatch]::StartNew()
    [void][LargeDir]::Create($g, 'g', 1, 45000)
    Check "grown to a three-level index: 45000 created ($($t.ElapsedMilliseconds) ms)" ([LargeDir]::Missing($g, 'g', 1, 45000, 1) -eq 0)
    [void][LargeDir]::Delete($g, 'g', 1, 45000, 100)
    Check 'grown index: 450 deleted, 44550 listed' ([IO.Directory]::GetFiles($g).Length -eq 44550)
} else {
    Check 'large_dir volume mounted' $false
}
"-- multi-mount protection"
function Volume($label, $seconds) {
    for ($i = 0; $i -lt $seconds * 10; $i++) {
        $v = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq $label -and $_.DriveLetter }
        if ($v) { return "$($v.DriveLetter):\" }
        Start-Sleep -Milliseconds 100
    }
    return $null
}
$mc = Volume 'mmpclean' 30
Check 'released volume mounted' ($mc -ne $null)
if ($mc) {
    [IO.File]::WriteAllText("$mc\windows.txt", "mmp`n")
    Check 'released volume: written' ([IO.File]::ReadAllText("$mc\linux.txt") -eq "mmp`n" -and [IO.File]::Exists("$mc\windows.txt"))
}
$mf = Volume 'mmpfsck' 30
Check 'volume under e2fsck mounted' ($mf -ne $null)
if ($mf) {
    Check 'volume under e2fsck: read' ([IO.File]::ReadAllText("$mf\linux.txt") -eq "mmp`n")
    Check 'volume under e2fsck: writes refused' (Refused { [IO.File]::WriteAllText("$mf\windows.txt", 'x') })
}
$ms = Volume 'mmpstale' 60
Check 'volume of a dead node mounted after the wait' ($ms -ne $null)
if ($ms) {
    [IO.File]::WriteAllText("$ms\windows.txt", "mmp`n")
    Check 'volume of a dead node: taken over, written' ([IO.File]::Exists("$ms\windows.txt"))
}

"-- ea_inode: values in inodes of their own"
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
using Microsoft.Win32.SafeHandles;
public static class EaFile {
    [DllImport("ntdll.dll")] static extern int NtQueryEaFile(SafeFileHandle h, long[] iosb, byte[] buf, uint len, bool single, IntPtr list, uint listLen, IntPtr index, bool restart);
    [DllImport("ntdll.dll")] static extern int NtSetEaFile(SafeFileHandle h, long[] iosb, byte[] buf, uint len);
    // the bytes Linux wrote: (i * 7 + seed) % 251
    public static byte[] Pattern(int n, int seed) { byte[] b = new byte[n]; for (int i = 0; i < n; i++) b[i] = (byte)((i * 7 + seed) % 251); return b; }
    public static bool Same(byte[] a, byte[] b) { if (a == null || b == null || a.Length != b.Length) return false; for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false; return true; }
    // every EA of the file, names as stored
    public static Dictionary<string, byte[]> Get(string path) {
        var r = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            byte[] buf = new byte[256 * 1024];
            int st = NtQueryEaFile(fs.SafeFileHandle, new long[2], buf, (uint)buf.Length, false, IntPtr.Zero, 0, IntPtr.Zero, true);
            if (st == unchecked((int)0x80000012)) return r;    // STATUS_NO_MORE_EAS
            if (st != 0) throw new IOException(string.Format("NtQueryEaFile 0x{0:X8}", st));
            for (int off = 0; ; ) {
                int next = BitConverter.ToInt32(buf, off); int nl = buf[off + 5]; int vl = BitConverter.ToUInt16(buf, off + 6);
                string name = Encoding.ASCII.GetString(buf, off + 8, nl);
                byte[] v = new byte[vl]; Array.Copy(buf, off + 8 + nl + 1, v, 0, vl);
                r[name] = v;
                if (next == 0) break; off += next;
            }
        }
        return r;
    }
    // one EA set (an empty value removes it)
    public static int Set(string path, string name, byte[] value) {
        byte[] n = Encoding.ASCII.GetBytes(name);
        byte[] ea = new byte[(8 + n.Length + 1 + value.Length + 3) & ~3];
        ea[5] = (byte)n.Length; BitConverter.GetBytes((ushort)value.Length).CopyTo(ea, 6);
        n.CopyTo(ea, 8); value.CopyTo(ea, 8 + n.Length + 1);
        using (var fs = new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete))
            return NtSetEaFile(fs.SafeFileHandle, new long[2], ea, (uint)ea.Length);
    }
}
"@
$ev = Volume 'eatest' 30
Check 'ea_inode volume mounted' ($ev -ne $null)
if ($ev) {
    $e = [EaFile]::Get("$ev\big.txt")
    Check 'a 20000-byte value Linux put in an inode' ([EaFile]::Same($e['big'], [EaFile]::Pattern(20000, 1)))
    Check 'a small value beside it' ([Text.Encoding]::ASCII.GetString($e['small']) -eq 's')
    Check 'a value two files share' ([EaFile]::Same(([EaFile]::Get("$ev\shared2.txt"))['same'], [EaFile]::Pattern(9000, 2)))
    [IO.File]::WriteAllText("$ev\win-big.txt", "windows`n")
    $st = [EaFile]::Set("$ev\win-big.txt", 'winbig', [EaFile]::Pattern(40000, 4))
    Check 'a 40000-byte value written' ($st -eq 0) ('(0x{0:X8})' -f $st)
    Check '... and read back' ([EaFile]::Same(([EaFile]::Get("$ev\win-big.txt"))['winbig'], [EaFile]::Pattern(40000, 4)))
    $st = [EaFile]::Set("$ev\big.txt", 'big', [Text.Encoding]::ASCII.GetBytes('tiny'))
    Check 'a value inode replaced by a small value' ($st -eq 0 -and [Text.Encoding]::ASCII.GetString(([EaFile]::Get("$ev\big.txt"))['big']) -eq 'tiny') ('(0x{0:X8})' -f $st)
    $st = [EaFile]::Set("$ev\keep.txt", 'big', [EaFile]::Pattern(25000, 5))
    Check 'a value inode replaced by another' ($st -eq 0 -and [EaFile]::Same(([EaFile]::Get("$ev\keep.txt"))['big'], [EaFile]::Pattern(25000, 5))) ('(0x{0:X8})' -f $st)
    [IO.File]::Delete("$ev\shared1.txt")
    Check 'a file sharing its value deleted, the other keeps it' (-not [IO.File]::Exists("$ev\shared1.txt") -and [EaFile]::Same(([EaFile]::Get("$ev\shared2.txt"))['same'], [EaFile]::Pattern(9000, 2)))
    [IO.File]::WriteAllText("$ev\win-gone.txt", "gone`n")
    $st = [EaFile]::Set("$ev\win-gone.txt", 'gone', [EaFile]::Pattern(50000, 6))
    [IO.File]::Delete("$ev\win-gone.txt")
    Check 'a file with its own value inode deleted' ($st -eq 0 -and -not [IO.File]::Exists("$ev\win-gone.txt")) ('(0x{0:X8})' -f $st)
    [IO.File]::WriteAllText("$ev\win-drop.txt", "drop`n")
    $st1 = [EaFile]::Set("$ev\win-drop.txt", 'drop', [EaFile]::Pattern(30000, 7))
    $st2 = [EaFile]::Set("$ev\win-drop.txt", 'drop', [byte[]]@())
    Check 'a large value removed' ($st1 -eq 0 -and $st2 -eq 0 -and -not ([EaFile]::Get("$ev\win-drop.txt")).ContainsKey('drop')) ('(0x{0:X8} 0x{1:X8})' -f $st1, $st2)
    # an xattr block two inodes share: changing one gives it a block of its own
    $st = [EaFile]::Set("$ev\blk-a.txt", 'win', [Text.Encoding]::ASCII.GetBytes('own block'))
    $a = [EaFile]::Get("$ev\blk-a.txt"); $b = [EaFile]::Get("$ev\blk-b.txt")
    Check 'a shared xattr block: one file changed' ($st -eq 0 -and $a.Count -eq 4 -and [EaFile]::Same($a['zeta'], [EaFile]::Pattern(400, 8))) ('(0x{0:X8}, {1} EAs)' -f $st, $a.Count)
    Check 'a shared xattr block: the other untouched' ($b.Count -eq 3 -and [EaFile]::Same($b['alpha'], [EaFile]::Pattern(400, 9)) -and -not $b.ContainsKey('win'))
}

"-- inline_data: files and directories inside their inodes"
$iv = Volume 'inltest' 30
Check 'inline_data volume mounted' ($iv -ne $null)
if ($iv) {
    $alpha = -join (0..99 | ForEach-Object { [char](97 + $_ % 26) })
    Check 'a small file read' ([IO.File]::ReadAllText("$iv\small.txt") -eq "hello inline`n")
    Check 'a file past i_block (system.data) read' ([IO.File]::ReadAllText("$iv\mid.txt") -eq $alpha)
    $names = @([IO.Directory]::GetFileSystemEntries("$iv\dir") | ForEach-Object { Split-Path -Leaf $_ } | Sort-Object)
    Check 'an inline directory listed' (($names -join ',') -eq 'a.txt,b.txt') "($($names -join ','))"
    $many = @([IO.Directory]::GetFiles("$iv\many") | ForEach-Object { Split-Path -Leaf $_ } | Sort-Object)
    Check 'a directory past i_block listed' (($many -join ',') -eq 'm1.txt,m2.txt,m3.txt,m4.txt,m5.txt,m6.txt') "($($many -join ','))"
    Check 'names looked up in it' ((1..6 | Where-Object { [IO.File]::ReadAllText("$iv\many\m$_.txt") -ne "m$_`n" }).Count -eq 0)
    Check 'an empty inline directory listed empty' ([IO.Directory]::GetFileSystemEntries("$iv\deldir").Length -eq 0)
    [IO.File]::AppendAllText("$iv\grow.txt", ' from windows')
    Check 'an inline file appended to' ([IO.File]::ReadAllText("$iv\grow.txt") -eq 'grow me from windows')
    $f = [IO.File]::Open("$iv\trunc.txt", 'Open', 'ReadWrite'); $f.SetLength(0); $f.Close()
    Check 'an inline file truncated' ((Get-Item "$iv\trunc.txt").Length -eq 0)
    [IO.File]::WriteAllText("$iv\dir\new.txt", "new`n")
    Check 'a file created in an inline directory' ([IO.File]::ReadAllText("$iv\dir\new.txt") -eq "new`n" -and [IO.File]::Exists("$iv\dir\a.txt"))
    [IO.File]::Delete("$iv\dir2\x.txt")
    Check 'a file deleted from an inline directory' (-not [IO.File]::Exists("$iv\dir2\x.txt") -and [IO.File]::Exists("$iv\dir2\y.txt"))
    [IO.File]::Move("$iv\many\m1.txt", "$iv\many\m1-renamed.txt")
    Check 'a file renamed in an inline directory' ([IO.File]::Exists("$iv\many\m1-renamed.txt") -and -not [IO.File]::Exists("$iv\many\m1.txt"))
    [IO.File]::Delete("$iv\del.txt")
    Check 'an inline file deleted' (-not [IO.File]::Exists("$iv\del.txt"))
    [IO.Directory]::Delete("$iv\deldir")
    Check 'an empty inline directory removed' (-not [IO.Directory]::Exists("$iv\deldir"))
    Check 'a non-empty inline directory refused' (Refused { [IO.Directory]::Delete("$iv\nonempty") })
}

"FEATURES-WIN: $fail failed"
exit $fail
