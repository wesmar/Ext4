# ext4test.ps1 - one-shot functional test of the Ext4Fsd driver.
#
# Runs on the test machine against a mounted ext4 volume and exercises, as far as
# Win32 allows, everything a file system driver has to get right: data paths
# (cached, non-cached, write-through, mapped), sizes around block and sector
# boundaries, sparse and truncate/extend semantics, directories (deep, wide,
# odd names, patterns), rename/move/replace, hard links, symbolic links,
# attributes and timestamps, share modes, byte-range locks, delete-pending
# rules, change notification, file ids, concurrency, free-space accounting and
# a full-disk run. Every check prints "ok" or "FAIL"; the exit code is the
# number of failures.
#
# The "verify" directory and its manifest (SHA-256 per file) are left behind on
# purpose: run-ext4test.ps1 on the host checks them from Linux, independently of
# the driver's read path, and then runs e2fsck.
#
# Usage:  powershell -ExecutionPolicy Bypass -File ext4test.ps1 [-Root E:\ext4test] [-Quick]
param(
    [string]$Root = 'E:\ext4test',
    [switch]$Quick,
    [switch]$KeepAll
)

$ErrorActionPreference = 'Continue'
$script:fail = 0
$script:pass = 0
$sw = [Diagnostics.Stopwatch]::StartNew()

function Check($name, $cond, $extra = '') {
    if ($cond) { $script:pass++; "  ok   $name $extra" } else { $script:fail++; "  FAIL $name $extra" }
}
function Note($name, $extra = '') { "  note $name $extra" }
function Try-Op([scriptblock]$b) { try { & $b; return $null } catch { return $_.Exception } }
function Inner($e) { $x = $e; while ($x.InnerException) { $x = $x.InnerException }; return $x }
function Sha($p) { (Get-FileHash -Algorithm SHA256 -Path $p).Hash }
function Same($a, $b) { return [X4]::Eq($a, $b) }
function SamePrefix($a, $b, $n) { return [X4]::EqN($a, $b, $n) }
function RandomBytes($n, $seed) { $r = New-Object System.Random($seed); $b = New-Object byte[] $n; $r.NextBytes($b); return ,$b }
function Del-Path($p) { if (Test-Path -LiteralPath $p) { if ((Get-Item -LiteralPath $p -Force) -is [IO.DirectoryInfo]) { [IO.Directory]::Delete($p, $true) } else { [IO.File]::Delete($p) } } }

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class X4 {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern SafeFileHandle CreateFile(string p, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteFile(SafeFileHandle h, byte[] b, uint n, out uint w, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadFile(SafeFileHandle h, byte[] b, uint n, out uint r, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetFilePointerEx(SafeFileHandle h, long d, out long np, uint m);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetEndOfFile(SafeFileHandle h);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FlushFileBuffers(SafeFileHandle h);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool LockFileEx(SafeFileHandle h, uint flags, uint r, uint lo, uint hi, ref System.Threading.NativeOverlapped o);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool UnlockFileEx(SafeFileHandle h, uint r, uint lo, uint hi, ref System.Threading.NativeOverlapped o);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool CreateHardLink(string n, string e, IntPtr r);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr FindFirstFileNameW(string f, uint flags, ref uint len, StringBuilder name);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool FindNextFileNameW(IntPtr h, ref uint len, StringBuilder name);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FindClose(IntPtr h);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern uint GetFinalPathNameByHandle(SafeFileHandle h, StringBuilder b, uint n, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct FILE_ID_DESCRIPTOR { public uint dwSize; public int Type; public long FileId; public long pad; }
    [DllImport("kernel32.dll", SetLastError=true)] public static extern SafeFileHandle OpenFileById(SafeFileHandle vol, ref FILE_ID_DESCRIPTOR d, uint access, uint share, IntPtr sa, uint flags);
    [StructLayout(LayoutKind.Sequential, Pack = 4)] public struct BY_HANDLE_FILE_INFORMATION { public uint attr; public long ct, at, wt; public uint vsn, sizeHigh, sizeLow, links, idxHigh, idxLow; }
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetFileInformationByHandle(SafeFileHandle h, out BY_HANDLE_FILE_INFORMATION i);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr i, int il, byte[] o, int ol, out int ret, IntPtr ov);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool DeleteFile(string p);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool GetVolumeInformation(string root, StringBuilder vol, int volLen, out uint serial, out uint maxlen, out uint flags, StringBuilder fs, int fsLen);

    [StructLayout(LayoutKind.Sequential)] public struct IO_STATUS_BLOCK { public IntPtr Status; public IntPtr Information; }
    [DllImport("ntdll.dll")] public static extern int NtSetEaFile(SafeFileHandle h, out IO_STATUS_BLOCK io, byte[] buf, uint len);
    [DllImport("ntdll.dll")] public static extern int NtQueryEaFile(SafeFileHandle h, out IO_STATUS_BLOCK io, byte[] buf, uint len, bool single, IntPtr list, uint listLen, IntPtr index, bool restart);

    public const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, FILE_SHARE_ALL = 7;
    public const uint OPEN_EXISTING = 3, CREATE_ALWAYS = 2, OPEN_ALWAYS = 4;
    public const uint FILE_FLAG_NO_BUFFERING = 0x20000000, FILE_FLAG_WRITE_THROUGH = 0x80000000, FILE_FLAG_BACKUP_SEMANTICS = 0x02000000, FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000;

    public static SafeFileHandle Open(string p, uint access, uint share, uint disp, uint flags) {
        var h = CreateFile(p, access, share, IntPtr.Zero, disp, flags, IntPtr.Zero);
        if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return h;
    }
    public static int OpenError(string p, uint access, uint share, uint disp, uint flags) {
        var h = CreateFile(p, access, share, IntPtr.Zero, disp, flags, IntPtr.Zero);
        if (h.IsInvalid) return Marshal.GetLastWin32Error();
        h.Close(); return 0;
    }
    // unbuffered write of a whole sector-aligned buffer, then unbuffered read back
    public static byte[] RawRoundTrip(string p, byte[] data, bool writeThrough) {
        uint flags = FILE_FLAG_NO_BUFFERING | (writeThrough ? FILE_FLAG_WRITE_THROUGH : 0);
        using (var h = Open(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_ALL, CREATE_ALWAYS, flags)) {
            uint w; if (!WriteFile(h, data, (uint)data.Length, out w, IntPtr.Zero) || w != data.Length) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        var back = new byte[data.Length];
        using (var h = Open(p, GENERIC_READ, FILE_SHARE_ALL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING)) {
            uint r; if (!ReadFile(h, back, (uint)back.Length, out r, IntPtr.Zero) || r != back.Length) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        return back;
    }
    public static bool Eq(byte[] a, byte[] b) { if (a == null || b == null || a.Length != b.Length) return false; for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false; return true; }
    public static bool EqN(byte[] a, byte[] b, int n) { if (a == null || b == null || a.Length < n || b.Length < n) return false; for (int i = 0; i < n; i++) if (a[i] != b[i]) return false; return true; }
    public static bool ZeroFrom(byte[] a, int from) { for (int i = from; i < a.Length; i++) if (a[i] != 0) return false; return true; }
    public static long FileSize(string p) { return new FileInfo(p).Length; }
    // one FILE_FULL_EA_INFORMATION: NextEntryOffset, Flags, NameLength, ValueLength, name, NUL, value
    public static int SetEa(string p, string name, byte[] val) {
        byte[] n = Encoding.ASCII.GetBytes(name);
        var buf = new byte[8 + n.Length + 1 + val.Length];
        buf[5] = (byte)n.Length; BitConverter.GetBytes((ushort)val.Length).CopyTo(buf, 6);
        n.CopyTo(buf, 8); val.CopyTo(buf, 8 + n.Length + 1);
        using (var h = Open(p, 0x10 /* FILE_WRITE_EA */, FILE_SHARE_ALL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)) {
            IO_STATUS_BLOCK io; return NtSetEaFile(h, out io, buf, (uint)buf.Length);
        }
    }
    // the value of one EA, looked up in the full list (names come back upper-cased)
    public static byte[] GetEa(string p, string name) {
        var buf = new byte[65536];
        using (var h = Open(p, 0x08 /* FILE_READ_EA */, FILE_SHARE_ALL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)) {
            IO_STATUS_BLOCK io; int st = NtQueryEaFile(h, out io, buf, (uint)buf.Length, false, IntPtr.Zero, 0, IntPtr.Zero, true);
            if (st < 0) return null;
        }
        for (int o = 0; ; ) {
            int next = BitConverter.ToInt32(buf, o), nl = buf[o + 5], vl = BitConverter.ToUInt16(buf, o + 6);
            if (string.Equals(Encoding.ASCII.GetString(buf, o + 8, nl), name, StringComparison.OrdinalIgnoreCase)) {
                var v = new byte[vl]; Array.Copy(buf, o + 8 + nl + 1, v, 0, vl); return v;
            }
            if (next == 0) return null;
            o += next;
        }
    }
    public static string[] HardLinks(string p) {
        var list = new System.Collections.Generic.List<string>();
        uint len = 512; var sb = new StringBuilder(512);
        IntPtr h = FindFirstFileNameW(p, 0, ref len, sb);
        if (h == new IntPtr(-1)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        try { list.Add(sb.ToString()); while (true) { len = 512; sb.Clear(); if (!FindNextFileNameW(h, ref len, sb)) break; list.Add(sb.ToString()); } }
        finally { FindClose(h); }
        return list.ToArray();
    }
    public static long FileIndex(string p) {
        using (var h = Open(p, 0, FILE_SHARE_ALL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)) {
            BY_HANDLE_FILE_INFORMATION i; if (!GetFileInformationByHandle(h, out i)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return ((long)i.idxHigh << 32) | i.idxLow;
        }
    }
    public static uint LinkCount(string p) {
        using (var h = Open(p, 0, FILE_SHARE_ALL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)) {
            BY_HANDLE_FILE_INFORMATION i; GetFileInformationByHandle(h, out i); return i.links;
        }
    }
    public static string OpenByIdName(string volume, long id) {
        using (var v = Open(volume, 0, FILE_SHARE_ALL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)) {
            var d = new FILE_ID_DESCRIPTOR(); d.dwSize = (uint)Marshal.SizeOf(typeof(FILE_ID_DESCRIPTOR)); d.Type = 0; d.FileId = id;
            var h = OpenFileById(v, ref d, GENERIC_READ, FILE_SHARE_ALL, IntPtr.Zero, FILE_FLAG_BACKUP_SEMANTICS);
            if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            using (h) { var sb = new StringBuilder(1024); uint n = GetFinalPathNameByHandle(h, sb, 1024, 0); if (n == 0) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()); return sb.ToString(); }
        }
    }
    public static uint ReparseTag(string p) {
        using (var h = Open(p, 0, FILE_SHARE_ALL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)) {
            var buf = new byte[16 * 1024]; int ret;
            if (!DeviceIoControl(h, 0x900A8 /*FSCTL_GET_REPARSE_POINT*/, IntPtr.Zero, 0, buf, buf.Length, out ret, IntPtr.Zero)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return BitConverter.ToUInt32(buf, 0);
        }
    }
    public static int LockConflict(string p) {
        // lock bytes 0..99 on one handle, try an exclusive lock on the same range from another: must fail with ERROR_LOCK_VIOLATION (33)
        using (var a = Open(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_ALL, OPEN_EXISTING, 0))
        using (var b = Open(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_ALL, OPEN_EXISTING, 0)) {
            var o = new NativeOverlapped(); var o2 = new NativeOverlapped();
            if (!LockFileEx(a, 3 /*EXCLUSIVE|FAIL_IMMEDIATELY*/, 0, 100, 0, ref o)) return -Marshal.GetLastWin32Error();
            int err = 0;
            if (!LockFileEx(b, 3, 0, 100, 0, ref o2)) err = Marshal.GetLastWin32Error();
            UnlockFileEx(a, 0, 100, 0, ref o);
            if (err == 0) return 0;
            // after unlock the second handle must succeed
            if (!LockFileEx(b, 3, 0, 100, 0, ref o2)) return -1000 - Marshal.GetLastWin32Error();
            UnlockFileEx(b, 0, 100, 0, ref o2);
            return err;
        }
    }
    public static string Parallel(string root, int workers, int files) {
        var errors = new System.Collections.Concurrent.ConcurrentBag<string>();
        System.Threading.Tasks.Parallel.For(0, workers, w => {
            var dir = Path.Combine(root, "w" + w); Directory.CreateDirectory(dir); var rng = new Random(1000 + w);
            for (int i = 0; i < files; i++) {
                string step = "create"; string p = Path.Combine(dir, "f" + i + ".bin");
                try {
                    var data = new byte[1 + (i * 4099) % 70000]; rng.NextBytes(data);
                    using (var f = new FileStream(p, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.ReadWrite)) {
                        step = "write"; f.Write(data, 0, data.Length);
                        if (i % 3 == 0) { step = "extend"; f.SetLength(data.Length + 8192); f.Position = data.Length + 4096; f.Write(data, 0, Math.Min(data.Length, 512)); }
                        if (i % 7 == 0) { step = "shrink"; f.SetLength(512); }
                        if (i % 10 == 0) { step = "flush"; f.Flush(true); }
                    }
                    step = "verify";
                    var expected = new byte[i % 7 == 0 ? 512 : data.Length + (i % 3 == 0 ? 8192 : 0)];
                    Array.Copy(data, expected, Math.Min(data.Length, expected.Length));
                    if (i % 3 == 0 && i % 7 != 0) Array.Copy(data, 0, expected, data.Length + 4096, Math.Min(data.Length, 512));
                    var actual = File.ReadAllBytes(p);
                    if (actual.Length != expected.Length) throw new Exception("length " + actual.Length + " vs " + expected.Length);
                    for (int k = 0; k < actual.Length; k++) if (actual[k] != expected[k]) throw new Exception("data mismatch at " + k);
                    step = "rename"; File.Move(p, p + ".r");
                    if (i % 4 == 0) { step = "mkdir"; Directory.CreateDirectory(p + ".d"); step = "rmdir"; Directory.Delete(p + ".d"); }
                    if (i % 2 == 0) { step = "delete"; File.Delete(p + ".r"); }
                } catch (Exception e) { errors.Add("w" + w + " f" + i + " " + step + ": " + e.Message); }
            }
        });
        return errors.Count == 0 ? "" : string.Join(" | ", errors.ToArray());
    }
    public static string Notify(string dir) {
        // events fire on thread-pool threads, no PowerShell event pump needed
        var got = new System.Collections.Concurrent.ConcurrentQueue<string>();
        using (var w = new FileSystemWatcher(dir, "notify*")) {
            w.Created += (s, e) => got.Enqueue("created:" + e.Name);
            w.Renamed += (s, e) => got.Enqueue("renamed:" + e.Name);
            w.Deleted += (s, e) => got.Enqueue("deleted:" + e.Name);
            w.Changed += (s, e) => got.Enqueue("changed:" + e.Name);
            w.EnableRaisingEvents = true;
            File.WriteAllText(Path.Combine(dir, "notify1.txt"), "n");
            File.Move(Path.Combine(dir, "notify1.txt"), Path.Combine(dir, "notify2.txt"));
            File.AppendAllText(Path.Combine(dir, "notify2.txt"), "m");
            File.Delete(Path.Combine(dir, "notify2.txt"));
            Thread.Sleep(1000);
            w.EnableRaisingEvents = false;
        }
        return string.Join(" ", got.ToArray());
    }
    public static string SharedDirChurn(string dir, int workers, int iters) {
        // everybody in ONE directory: create/rename/delete of distinct names, plus a reader listing it
        Directory.CreateDirectory(dir);
        var errors = new System.Collections.Concurrent.ConcurrentBag<string>();
        System.Threading.Tasks.Parallel.For(0, workers + 1, w => {
            try {
                if (w == workers) { for (int i = 0; i < iters / 4; i++) { Directory.GetFileSystemEntries(dir); } return; }
                for (int i = 0; i < iters; i++) {
                    string p = Path.Combine(dir, "t" + w + "_" + i);
                    File.WriteAllText(p, "x"); File.Move(p, p + ".m"); File.Delete(p + ".m");
                }
            } catch (Exception e) { errors.Add("w" + w + ": " + e.Message); }
        });
        return errors.Count == 0 ? "" : string.Join(" | ", errors.ToArray());
    }
}
"@

"== ext4test on $Root  (quick=$Quick)  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
$drive = $Root.Substring(0, 2)
$di = New-Object IO.DriveInfo($drive)
if (-not $di.IsReady) { "drive $drive not ready"; exit 99 }
[void][X4]::DeleteFile("\\?\$Root\$('n' * 255)")
if (Test-Path -LiteralPath $Root) { [IO.Directory]::Delete($Root, $true) }
New-Item -ItemType Directory -Path $Root | Out-Null
$freeStart = $di.AvailableFreeSpace

# ---------------------------------------------------------------- 1. volume
"-- volume"
$vol = New-Object Text.StringBuilder 256; $fs = New-Object Text.StringBuilder 256; [uint32]$serial = 0; [uint32]$maxlen = 0; [uint32]$flags = 0
$ok = [X4]::GetVolumeInformation("$drive\", $vol, 256, [ref]$serial, [ref]$maxlen, [ref]$flags, $fs, 256)
Check "GetVolumeInformation" $ok "(label='$($vol.ToString())' fs='$($fs.ToString())' maxlen=$maxlen flags=0x$($flags.ToString('X')))"
Check "max component length 255" ($maxlen -eq 255)
Check "free space known" ($freeStart -gt 0) "($([math]::Round($freeStart/1MB)) MB free)"
Check "lost+found is a directory" ((Get-Item "$drive\lost+found" -Force) -is [IO.DirectoryInfo])

# ---------------------------------------------------------------- 2. data paths and sizes
"-- files: sizes, cached / unbuffered / write-through"
$sizes = @(0, 1, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536, 65537, 1048576, 1048577)
if (-not $Quick) { $sizes += 8388608 }
$seed = 100
foreach ($n in $sizes) {
    $p = "$Root\size_$n.bin"; $data = RandomBytes $n ($seed++)
    [IO.File]::WriteAllBytes($p, $data)
    $back = [IO.File]::ReadAllBytes($p)
    Check "cached write/read $n" ((Same $data $back) -and ([X4]::FileSize($p) -eq $n))
}
foreach ($n in @(512, 4096, 65536, 1048576)) {
    $p = "$Root\raw_$n.bin"; $data = RandomBytes $n ($seed++)
    $e = Try-Op { $back = [X4]::RawRoundTrip($p, $data, $false); if (-not (Same $data $back)) { throw 'mismatch' } }
    Check "unbuffered write/read $n" ($e -eq $null) $(if ($e) { "($($e.Message))" })
    $e = Try-Op { $back = [X4]::RawRoundTrip("$Root\wt_$n.bin", $data, $true); if (-not (Same $data $back)) { throw 'mismatch' } }
    Check "write-through $n" ($e -eq $null) $(if ($e) { "($($e.Message))" })
    Check "unbuffered then cached read $n" (Same $data ([IO.File]::ReadAllBytes($p)))
}
# read of an unbuffered write must see the same data through the cache, and vice versa
$p = "$Root\mixed.bin"; $data = RandomBytes 8192 ($seed++)
[IO.File]::WriteAllBytes($p, $data)
$h = [X4]::Open($p, [X4]::GENERIC_READ, [X4]::FILE_SHARE_ALL, [X4]::OPEN_EXISTING, [X4]::FILE_FLAG_NO_BUFFERING)
$buf = New-Object byte[] 8192; $r = 0; [void][X4]::ReadFile($h, $buf, 8192, [ref]$r, [IntPtr]::Zero); $h.Close()
Check "cached write, unbuffered read" (($r -eq 8192) -and (Same $data $buf))

"-- files: append, overwrite, truncate, extend, sparse"
$p = "$Root\append.txt"
[IO.File]::WriteAllText($p, "hello"); [IO.File]::AppendAllText($p, " world"); [IO.File]::AppendAllText($p, "!")
Check "append" ([IO.File]::ReadAllText($p) -eq "hello world!")
[IO.File]::WriteAllText($p, "hi"); Check "overwrite shorter" ([IO.File]::ReadAllText($p) -eq "hi" -and [X4]::FileSize($p) -eq 2)
$big = RandomBytes 300000 ($seed++); [IO.File]::WriteAllBytes($p, $big); Check "overwrite longer" (Same $big ([IO.File]::ReadAllBytes($p)))
$f = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$f.SetLength(100000); $f.Close()
$back = [IO.File]::ReadAllBytes($p); Check "truncate to 100000" ($back.Length -eq 100000 -and (SamePrefix $big $back 100000))
$f = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$f.SetLength(250000); $f.Close()
$back = [IO.File]::ReadAllBytes($p)
Check "extend after truncate reads zeros" ($back.Length -eq 250000 -and ([X4]::ZeroFrom($back, 100000)) -and (SamePrefix $big $back 100000))
$f = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $f.SetLength(0); $f.Close()
Check "truncate to 0" ([X4]::FileSize($p) -eq 0)
# sparse: write at 20 MB offset on an empty file, holes must read as zeros and use little disk
$p = "$Root\sparse.bin"; $freeBefore = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
$f = [IO.File]::Open($p, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$f.Position = 20MB; $f.Write((RandomBytes 4096 ($seed++)), 0, 4096); $f.Close()
$freeAfter = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
$f = [IO.File]::OpenRead($p); $f.Position = 10MB; $b = New-Object byte[] 4096; [void]$f.Read($b, 0, 4096); $f.Close()
Check "sparse file size" ([X4]::FileSize($p) -eq 20MB + 4096)
Check "hole reads zeros" (($b | Where-Object { $_ -ne 0 }).Count -eq 0)
Note "hole allocation" "(used $([math]::Round(($freeBefore - $freeAfter)/1KB)) KB; NTFS-style zero fill is allowed, ext4-style hole is better)"
# write in the middle of a file, then past EOF with a gap
$p = "$Root\gap.bin"; [IO.File]::WriteAllBytes($p, (RandomBytes 10000 ($seed++)))
$f = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$f.Position = 5000; $f.Write([byte[]](1,2,3,4), 0, 4); $f.Position = 20000; $f.Write([byte[]](9,9), 0, 2); $f.Close()
$back = [IO.File]::ReadAllBytes($p)
Check "gap write: size, data, zeros" ($back.Length -eq 20002 -and $back[5000] -eq 1 -and $back[5003] -eq 4 -and $back[15000] -eq 0 -and $back[20001] -eq 9)

"-- memory mapping"
$p = "$Root\mapped.bin"; [IO.File]::WriteAllBytes($p, (New-Object byte[] 65536))
$mm = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile($p, [IO.FileMode]::Open, [NullString]::Value, 65536)
$acc = $mm.CreateViewAccessor(); for ($i = 0; $i -lt 65536; $i += 4096) { $acc.Write($i, [byte](($i / 4096) + 1)) }; $acc.Flush(); $acc.Dispose(); $mm.Dispose()
$back = [IO.File]::ReadAllBytes($p)
Check "mapped write visible to ReadFile" ($back[0] -eq 1 -and $back[4096] -eq 2 -and $back[61440] -eq 16)
[IO.File]::WriteAllBytes($p, (RandomBytes 65536 ($seed++)))
$mm = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile($p, [IO.FileMode]::Open, [NullString]::Value, 65536, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
$acc = $mm.CreateViewAccessor(0, 65536, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read); $mb = New-Object byte[] 65536; [void]$acc.ReadArray(0, $mb, 0, 65536); $acc.Dispose(); $mm.Dispose()
Check "WriteFile visible to mapped read" (Same $mb ([IO.File]::ReadAllBytes($p)))

# ---------------------------------------------------------------- 3. directories and names
"-- directories and names"
$p = $Root; for ($i = 1; $i -le 32; $i++) { $p = Join-Path $p ("d{0:D2}" -f $i); New-Item -ItemType Directory $p | Out-Null; Set-Content -LiteralPath (Join-Path $p "l$i.txt") -Value "level $i" -NoNewline }
Check "32 nested levels" ((Get-Content -LiteralPath (Join-Path $p 'l32.txt') -Raw) -eq 'level 32')
$long = 'n' * 255
$rc = [X4]::OpenError("\\?\$Root\$long", [X4]::GENERIC_WRITE, [X4]::FILE_SHARE_ALL, [X4]::CREATE_ALWAYS, 0)
Check "255-char name" ($rc -eq 0 -and (Get-ChildItem -LiteralPath $Root -Force | Where-Object { $_.Name.Length -eq 255 }).Count -eq 1) "(rc $rc)"
[void][X4]::DeleteFile("\\?\$Root\$long")
$rc = [X4]::OpenError("\\?\$Root\$('m' * 256)", [X4]::GENERIC_WRITE, [X4]::FILE_SHARE_ALL, [X4]::CREATE_ALWAYS, 0)
Check "256-char name refused" ($rc -ne 0) "(rc $rc)"
$names = @('zażółć gęślą jaźń.txt', 'Ärger Über.txt', '日本語ファイル.txt', 'файл.txt', 'emoji 😀.txt', 'spaces  and .dots.', 'a.b.c.d', '.hidden')
foreach ($n in $names) { [IO.File]::WriteAllText("$Root\$n", $n) }
$bad = @(); foreach ($n in $names) { if (-not (Test-Path -LiteralPath "$Root\$n") -or ([IO.File]::ReadAllText("$Root\$n") -ne $n)) { $bad += $n } }
Check "unicode / odd names round trip" ($bad.Count -eq 0) "($($bad -join ', '))"
Check "dot-prefixed name is hidden" ((Get-Item -LiteralPath "$Root\.hidden" -Force).Attributes -band [IO.FileAttributes]::Hidden)
Check "listing count" ((Get-ChildItem -LiteralPath $Root -Force).Count -ge ($names.Count + 1))
Check "case-insensitive lookup" ([IO.File]::Exists("$Root\APPEND.TXT") -and [IO.File]::Exists("$Root\Append.Txt"))
Check "exact-case name preserved" ((Get-ChildItem -LiteralPath $Root -Filter 'append.txt').Name -ceq 'append.txt')
$e = Try-Op { [IO.Directory]::Delete("$Root\d01") }
Check "rmdir of non-empty dir refused" ($e -ne $null)
$wide = "$Root\wide"; New-Item -ItemType Directory $wide | Out-Null
$N = $(if ($Quick) { 1500 } else { 6000 })
$t = [Diagnostics.Stopwatch]::StartNew(); for ($i = 0; $i -lt $N; $i++) { [IO.File]::WriteAllText("$wide\file_$i.dat", "$i") }; $tc = $t.ElapsedMilliseconds
Check "$N entries created" ((Get-ChildItem -LiteralPath $wide).Count -eq $N) "($tc ms)"
$t.Restart(); $bad = 0; for ($i = 0; $i -lt $N; $i += 7) { if ((Get-Content -LiteralPath "$wide\file_$i.dat" -Raw) -ne "$i") { $bad++ } }
Check "lookup in wide dir" ($bad -eq 0) "($($t.ElapsedMilliseconds) ms)"
$pc = (Get-ChildItem -LiteralPath $wide -Filter 'file_1??.dat').Count; Check "pattern file_1??.dat (DOS_QM rules, NTFS gives 111)" ($pc -eq 111) "($pc)"
Check "pattern file_5*.dat" ((Get-ChildItem -LiteralPath $wide -Filter 'file_5*.dat').Count -eq $(if ($N -ge 6000) { 1111 } else { 111 }))
$t.Restart(); for ($i = 0; $i -lt $N; $i += 2) { [IO.File]::Delete("$wide\file_$i.dat") }
Check "delete half of wide dir" ((Get-ChildItem -LiteralPath $wide).Count -eq $N / 2) "($($t.ElapsedMilliseconds) ms)"
$dotdot = & cmd /c "dir /a /b `"$Root\d01`""
Check ". and .. not in listing" (-not ($dotdot -contains '.' -or $dotdot -contains '..'))

"-- rename, move, replace"
$p = "$Root\ren.txt"; Set-Content -LiteralPath $p -Value 'rename me' -NoNewline
Rename-Item -LiteralPath $p 'renamed.txt'; Check "rename file" ((Test-Path -LiteralPath "$Root\renamed.txt") -and -not (Test-Path -LiteralPath $p))
[IO.File]::Move("$Root\renamed.txt", "$Root\RENAMED.txt"); Check "rename changes case only" ((Get-ChildItem -LiteralPath $Root -Filter 'renamed.txt').Name -ceq 'RENAMED.txt')
New-Item -ItemType Directory "$Root\mv" | Out-Null; Move-Item -LiteralPath "$Root\RENAMED.txt" "$Root\mv\moved.txt"
Check "move across dirs" ((Get-Content -LiteralPath "$Root\mv\moved.txt" -Raw) -eq 'rename me')
Rename-Item -LiteralPath "$Root\mv" 'mv2'; Check "rename dir" (Test-Path -LiteralPath "$Root\mv2\moved.txt")
New-Item -ItemType Directory "$Root\deep\er" -Force | Out-Null; Move-Item -LiteralPath "$Root\mv2" "$Root\deep\er\mv3"
Check "move dir into subdir" (Test-Path -LiteralPath "$Root\deep\er\mv3\moved.txt")
Set-Content -LiteralPath "$Root\target.txt" -Value 'old' -NoNewline; Set-Content -LiteralPath "$Root\source.txt" -Value 'new' -NoNewline
$e = Try-Op { [IO.File]::Move("$Root\source.txt", "$Root\target.txt") }
Check "rename onto existing refused" ($e -ne $null)
[IO.File]::Copy("$Root\source.txt", "$Root\target.txt", $true); Check "copy over existing" ((Get-Content -LiteralPath "$Root\target.txt" -Raw) -eq 'new')
[IO.File]::Replace("$Root\source.txt", "$Root\target.txt", "$Root\backup.txt")
Check "File.Replace" ((Get-Content -LiteralPath "$Root\target.txt" -Raw) -eq 'new' -and (Test-Path -LiteralPath "$Root\backup.txt") -and -not (Test-Path -LiteralPath "$Root\source.txt"))
Set-Content -LiteralPath "$Root\open.txt" -Value 'open' -NoNewline
$fo = [IO.File]::Open("$Root\open.txt", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$e = Try-Op { [IO.File]::Move("$Root\open.txt", "$Root\open2.txt") }
Check "rename of file open without FILE_SHARE_DELETE refused" ($e -ne $null); $fo.Close()
$fo = [IO.File]::Open("$Root\open.txt", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'Read,Delete')
$e = Try-Op { [IO.File]::Move("$Root\open.txt", "$Root\open2.txt") }
Check "rename of file open with FILE_SHARE_DELETE" ($e -eq $null); $fo.Close()

# ---------------------------------------------------------------- 4. attributes, times, ids
"-- attributes, timestamps, file ids"
$p = "$Root\attr.txt"; Set-Content -LiteralPath $p -Value 'a' -NoNewline
Set-ItemProperty -LiteralPath $p -Name Attributes -Value ([IO.FileAttributes]::ReadOnly)
$rc = [X4]::OpenError($p, [X4]::GENERIC_WRITE, [X4]::FILE_SHARE_ALL, [X4]::OPEN_EXISTING, 0)
Check "read-only refuses write open (ERROR_ACCESS_DENIED)" ($rc -eq 5) "(rc $rc, attributes $((Get-Item -LiteralPath $p -Force).Attributes))"
Set-ItemProperty -LiteralPath $p -Name Attributes -Value ([IO.FileAttributes]::Normal)
Set-Content -LiteralPath $p -Value 'b' -NoNewline; Check "read-only cleared" ((Get-Content -LiteralPath $p -Raw) -eq 'b')
Set-ItemProperty -LiteralPath $p -Name Attributes -Value ([IO.FileAttributes]::Hidden)
Check "hidden attribute" ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::Hidden)
$t1 = Get-Date '2021-03-04 05:06:07'; (Get-Item -LiteralPath $p -Force).LastWriteTime = $t1
Check "set mtime" (((Get-Item -LiteralPath $p -Force).LastWriteTime - $t1).TotalSeconds -lt 2)
$t2 = Get-Date '2019-01-02 03:04:05'; (Get-Item -LiteralPath $p -Force).CreationTime = $t2
Check "set creation time" (((Get-Item -LiteralPath $p -Force).CreationTime - $t2).TotalSeconds -lt 2)
$t3 = Get-Date '2020-06-07 08:09:10'; (Get-Item -LiteralPath $p -Force).LastAccessTime = $t3
Check "set access time" (((Get-Item -LiteralPath $p -Force).LastAccessTime - $t3).TotalSeconds -lt 2)
$id1 = [X4]::FileIndex($p); $id2 = [X4]::FileIndex($p)
Check "file id stable" ($id1 -eq $id2 -and $id1 -gt 0) "(id $id1)"
$e = Try-Op { $script:byid = [X4]::OpenByIdName("$drive\", $id1) }
Check "open by file id" (($e -eq $null) -and ($script:byid -like "*attr.txt")) $(if ($e) { "($($e.Message))" } else { "($script:byid)" })

# ---------------------------------------------------------------- 5. hard links
"-- hard links"
$a = "$Root\hl_a.txt"; $b = "$Root\hl_b.txt"; Set-Content -LiteralPath $a -Value 'link' -NoNewline
Check "CreateHardLink" ([X4]::CreateHardLink($b, $a, [IntPtr]::Zero))
Check "link count 2" ([X4]::LinkCount($a) -eq 2)
Check "same file id" ([X4]::FileIndex($a) -eq [X4]::FileIndex($b))
$e = Try-Op { $script:links = [X4]::HardLinks($a) }
Check "FindFirstFileName lists both" (($e -eq $null) -and ($script:links.Count -eq 2)) $(if ($e) { "($($e.Message))" } else { "($($script:links -join ', '))" })
$fb = [IO.File]::Open($b, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
[IO.File]::WriteAllText($a, 'link changed'); $rb = New-Object IO.StreamReader($fb); $seen = $rb.ReadToEnd(); $rb.Close()
Check "write via A visible through open B" ($seen -eq 'link changed')
[IO.File]::Delete($a); Check "delete A keeps B" ((Get-Content -LiteralPath $b -Raw) -eq 'link changed' -and [X4]::LinkCount($b) -eq 1)
New-Item -ItemType Directory "$Root\hlsub" | Out-Null; [void][X4]::CreateHardLink("$Root\hlsub\c.txt", $b, [IntPtr]::Zero); [IO.Directory]::Delete("$Root\hlsub", $true)
Check "file survives its subdir link" ((Get-Content -LiteralPath $b -Raw) -eq 'link changed')
[void][X4]::CreateHardLink("$Root\hl_c.txt", $b, [IntPtr]::Zero); Rename-Item -LiteralPath "$Root\hl_c.txt" 'hl_d.txt'
Check "rename a link" ((Get-Content -LiteralPath "$Root\hl_d.txt" -Raw) -eq 'link changed' -and [X4]::LinkCount($b) -eq 2)
$f1 = [IO.FileStream]::new($b, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]'ReadWrite,Delete', 4096, [IO.FileOptions]::DeleteOnClose)
$f2 = [IO.FileStream]::new("$Root\hl_d.txt", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]'ReadWrite,Delete', 4096, [IO.FileOptions]::DeleteOnClose)
$f1.Close(); $stillD = Test-Path -LiteralPath "$Root\hl_d.txt"; $f2.Close()
Check "delete-on-close on both links" ($stillD -and -not (Test-Path -LiteralPath $b) -and -not (Test-Path -LiteralPath "$Root\hl_d.txt"))
Check "hard link to a directory refused" (-not [X4]::CreateHardLink("$Root\dirlink", "$Root\d01", [IntPtr]::Zero))

# ---------------------------------------------------------------- 6. symbolic links
"-- symbolic links"
Set-Content -LiteralPath "$Root\starget.txt" -Value 'symlinked' -NoNewline
$o = & cmd /c "mklink `"$Root\sym_abs.txt`" `"$Root\starget.txt`" 2>&1"; Check "absolute file symlink" ($LASTEXITCODE -eq 0) "($o)"
Check "read through absolute symlink" ((Get-Content -LiteralPath "$Root\sym_abs.txt" -Raw) -eq 'symlinked')
$o = & cmd /c "cd /d `"$Root`" && mklink sym_rel.txt starget.txt 2>&1"; Check "relative file symlink" ($LASTEXITCODE -eq 0) "($o)"
Check "read through relative symlink" ((Get-Content -LiteralPath "$Root\sym_rel.txt" -Raw) -eq 'symlinked')
Set-Content -LiteralPath "$Root\sym_rel.txt" -Value 'via link' -NoNewline
Check "write through symlink reaches target" ((Get-Content -LiteralPath "$Root\starget.txt" -Raw) -eq 'via link')
$e = Try-Op { $script:tag = [X4]::ReparseTag("$Root\sym_abs.txt") }; Check "reparse tag is SYMLINK" (($e -eq $null) -and ($script:tag -eq 2684354572)) $(if ($e) { "($($e.Message))" } else { "(0x$($script:tag.ToString('X')))" })
Check "symlink attribute" ((Get-Item -LiteralPath "$Root\sym_abs.txt" -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)
$o = & cmd /c "mklink /D `"$Root\sym_dir`" `"$Root\d01`" 2>&1"; Check "directory symlink" ($LASTEXITCODE -eq 0) "($o)"
Check "list through directory symlink" ((Get-ChildItem -LiteralPath "$Root\sym_dir").Name -contains 'l1.txt')
Check "path through directory symlink" ((Get-Content -LiteralPath "$Root\sym_dir\l1.txt" -Raw) -eq 'level 1')
[IO.Directory]::Delete("$Root\sym_dir"); Check "delete dir symlink keeps target" ((-not (Test-Path -LiteralPath "$Root\sym_dir")) -and (Test-Path -LiteralPath "$Root\d01\l1.txt"))
$o = & cmd /c "mklink /D `"$Root\sym_dir2`" `"$Root\d01`" 2>&1"; & cmd /c "rmdir `"$Root\sym_dir2`"" | Out-Null
Check "rmdir dir symlink keeps target" ((-not (Test-Path -LiteralPath "$Root\sym_dir2")) -and (Test-Path -LiteralPath "$Root\d01\l1.txt"))
[IO.File]::Delete("$Root\sym_abs.txt"); Check "delete file symlink keeps target" ((-not (Test-Path -LiteralPath "$Root\sym_abs.txt")) -and (Test-Path -LiteralPath "$Root\starget.txt"))
$o = & cmd /c "mklink `"$Root\dangling.txt`" `"$Root\nowhere.txt`" 2>&1"
Check "dangling symlink created" ($LASTEXITCODE -eq 0 -and (Get-ChildItem -LiteralPath $Root -Force -Filter 'dangling.txt').Count -eq 1)
$e = Try-Op { [IO.File]::ReadAllText("$Root\dangling.txt") }; Check "dangling symlink read fails" ($e -ne $null)
[IO.File]::Delete("$Root\dangling.txt"); Check "dangling symlink deleted" (-not (Test-Path -LiteralPath "$Root\dangling.txt"))
$o = & cmd /c "cd /d `"$Root`" && mklink sym_rel2.txt sym_rel.txt 2>&1"
Check "symlink to symlink resolves" ((Get-Content -LiteralPath "$Root\sym_rel2.txt" -Raw) -eq 'via link')

# ---------------------------------------------------------------- 7. sharing, locks, delete pending
"-- share modes, byte-range locks, delete pending"
$p = "$Root\share.txt"; Set-Content -LiteralPath $p -Value 'share' -NoNewline
$h = [X4]::Open($p, [X4]::GENERIC_READ, 0, [X4]::OPEN_EXISTING, 0)
Check "share-none blocks second open (ERROR_SHARING_VIOLATION)" ([X4]::OpenError($p, [X4]::GENERIC_READ, [X4]::FILE_SHARE_ALL, [X4]::OPEN_EXISTING, 0) -eq 32); $h.Close()
$h = [X4]::Open($p, [X4]::GENERIC_READ, 1, [X4]::OPEN_EXISTING, 0)
Check "share-read allows read, blocks write" (([X4]::OpenError($p, [X4]::GENERIC_READ, 1, [X4]::OPEN_EXISTING, 0) -eq 0) -and ([X4]::OpenError($p, [X4]::GENERIC_WRITE, 1, [X4]::OPEN_EXISTING, 0) -eq 32)); $h.Close()
$lc = [X4]::LockConflict($p); Check "byte-range lock conflict (ERROR_LOCK_VIOLATION)" ($lc -eq 33) "(rc $lc)"
$h = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]'ReadWrite,Delete')
[IO.File]::Delete($p)
Note "delete pending: name still listed" "($(Test-Path -LiteralPath $p -ErrorAction SilentlyContinue); either is legal, POSIX-delete hides it)"
$rc = [X4]::OpenError($p, [X4]::GENERIC_READ, [X4]::FILE_SHARE_ALL, [X4]::OPEN_EXISTING, 0); Check "delete pending: new open refused (ERROR_ACCESS_DENIED)" ($rc -eq 5) "(rc $rc)"
$h.Close(); Check "deleted after last close" (-not (Test-Path -LiteralPath $p))
$e = Try-Op { [void]([IO.File]::Open("$Root\nofile.txt", [IO.FileMode]::Open)) }; Check "open missing file fails" ((Inner $e) -is [IO.FileNotFoundException])
$e = Try-Op { [void]([IO.File]::Open("$Root\nodir\nofile.txt", [IO.FileMode]::Open)) }; Check "open in missing dir fails" ((Inner $e) -is [IO.DirectoryNotFoundException])
$e = Try-Op { [IO.File]::WriteAllText("$Root\append.txt:ads", 'x') }; Check "alternate data stream refused" ($e -ne $null)

"-- change notification"
$got = [X4]::Notify($Root)
Check "create/rename/change/delete notifications" (($got -like '*created:notify1.txt*') -and ($got -like '*renamed:notify2.txt*') -and ($got -like '*changed:notify2.txt*') -and ($got -like '*deleted:notify2.txt*')) "($got)"

# ---------------------------------------------------------------- 8. concurrency
"-- concurrency"
$w = $(if ($Quick) { 4 } else { 8 }); $n = $(if ($Quick) { 60 } else { 150 })
$t.Restart(); $err = [X4]::Parallel("$Root\par", $w, $n); Check "$w workers x $n files (write/verify/rename/delete)" ($err -eq '') "($($t.ElapsedMilliseconds) ms $err)"
$t.Restart(); $err = [X4]::SharedDirChurn("$Root\churn", $w, $(if ($Quick) { 200 } else { 600 })); Check "shared directory churn with a concurrent reader" ($err -eq '') "($($t.ElapsedMilliseconds) ms $err)"

# ---------------------------------------------------------------- 9. free space accounting
"-- free space accounting"
$before = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
[IO.File]::WriteAllBytes("$Root\acct.bin", (New-Object byte[] 20MB))
$h = [X4]::Open("$Root\acct.bin", [X4]::GENERIC_READ, [X4]::FILE_SHARE_ALL, [X4]::OPEN_EXISTING, 0); [void][X4]::FlushFileBuffers($h); $h.Close()
$during = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
[IO.File]::Delete("$Root\acct.bin")
$after = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
Check "20 MB write uses ~20 MB" (($before - $during) -gt 19MB -and ($before - $during) -lt 22MB) "(used $([math]::Round(($before-$during)/1MB,1)) MB)"
Check "delete gives it back" ([math]::Abs($after - $before) -lt 1MB) "(delta $([math]::Round(($after-$before)/1KB)) KB)"

# ---------------------------------------------------------------- 10. verification set for Linux
"-- verification set (checked from Linux by the host runner)"
$vdir = "$Root\verify"; New-Item -ItemType Directory $vdir | Out-Null
$manifest = New-Object Text.StringBuilder
$vs = @(1, 100, 4096, 70000, 1000000, 1048576); if (-not $Quick) { $vs += 20000000 }
$k = 0
foreach ($n in $vs) {
    foreach ($mode in 'cached', 'unbuffered', 'extended', 'regrown', 'offset') {
        $name = "v${k}_$($mode)_$n.bin"; $k++; $p = "$vdir\$name"; $data = RandomBytes $n ($seed++)
        switch ($mode) {
            'cached'     { [IO.File]::WriteAllBytes($p, $data) }
            'unbuffered' { if ($n % 512 -eq 0) { [void][X4]::RawRoundTrip($p, $data, $false) } else { [IO.File]::WriteAllBytes($p, $data) } }
            'extended'   { $f = [IO.File]::Open($p, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $f.SetLength($n + 8192); $f.Write($data, 0, $data.Length); $f.SetLength($n); $f.Close() }
            # old bytes must not come back: data on disk, cut at an unaligned size, regrown without a write
            'regrown'    { $f = [IO.File]::Open($p, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $f.Write($data, 0, $data.Length); $f.Flush($true); $f.SetLength([math]::Floor($n / 2) + 1); $f.Close()
                           $f = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $f.SetLength($n + 5000); $f.Close() }
            # a partial write into a range that was only extended (unwritten extents)
            'offset'     { $f = [IO.File]::Open($p, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $f.SetLength($n + 8192); $f.Position = 1234; $f.Write($data, 0, $data.Length); $f.Close() }
        }
        if ($mode -in 'regrown', 'offset') {
            # what the file must contain, byte for byte: nothing but the data and zeros
            if ($mode -eq 'regrown') {
                $exp = New-Object byte[] ($n + 5000); [Array]::Copy($data, $exp, [math]::Floor($n / 2) + 1)
            } else {
                $exp = New-Object byte[] ($n + 8192); [Array]::Copy($data, 0, $exp, 1234, $n)
            }
            if (-not (Same ([IO.File]::ReadAllBytes($p)) $exp)) { Check "$mode $n bytes: data and zeros only" $false }
        }
        [void]$manifest.Append((Sha $p).ToLower()).Append('  ').Append($name).Append("`n")
    }
}
[IO.File]::WriteAllText("$Root\manifest.sha256", $manifest.ToString())
Check "verification set written" ($k -eq $vs.Count * 5) "($k files)"

# extended attributes: a small one fits in the inode, a large one needs its own
# block. The file stays in the verification set, so e2fsck checks that block
# (checksum included) from Linux afterwards.
"-- extended attributes"
$eaFile = "$vdir\ea.bin"; [IO.File]::WriteAllBytes($eaFile, [byte[]](1..16))
$eaSmall = RandomBytes 24 ($seed++); $eaLarge = RandomBytes 700 ($seed++)
Check "small EA set" ([X4]::SetEa($eaFile, 'EXT4SMALL', $eaSmall) -eq 0)
Check "large EA set (outside the inode)" ([X4]::SetEa($eaFile, 'EXT4LARGE', $eaLarge) -eq 0)
Check "small EA reads back" ([X4]::Eq(([X4]::GetEa($eaFile, 'EXT4SMALL')), $eaSmall))
Check "large EA reads back" ([X4]::Eq(([X4]::GetEa($eaFile, 'EXT4LARGE')), $eaLarge))
$eaLarge = RandomBytes 900 ($seed++)
Check "large EA replaced" (([X4]::SetEa($eaFile, 'EXT4LARGE', $eaLarge) -eq 0) -and [X4]::Eq(([X4]::GetEa($eaFile, 'EXT4LARGE')), $eaLarge))
Check "small EA kept while the large one changed" ([X4]::Eq(([X4]::GetEa($eaFile, 'EXT4SMALL')), $eaSmall))
$null = [X4]::SetEa($eaFile, 'EXT4GONE', [byte[]](1, 2, 3))
Check "EA deleted by an empty value" (([X4]::SetEa($eaFile, 'EXT4GONE', [byte[]]@()) -eq 0) -and ($null -eq [X4]::GetEa($eaFile, 'EXT4GONE')))

# ---------------------------------------------------------------- 11. full disk
if (-not $Quick) {
    "-- full disk"
    $fd = "$Root\full"; New-Item -ItemType Directory $fd | Out-Null; $freeBeforeFull = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
    $chunk = New-Object byte[] 4MB; $i = 0; $err = $null
    while ($i -lt 100000) { try { [IO.File]::WriteAllBytes("$fd\c$i.bin", $chunk); $i++ } catch { $err = Inner $_.Exception; break } }
    Check "ENOSPC reported as disk full" ($err -ne $null -and ($err.HResult -band 0xFFFF) -in @(112, 39)) "($i x 4 MB written, $($err.Message))"
    $freeFull = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
    Check "little free space left" ($freeFull -lt 8MB) "($([math]::Round($freeFull/1KB)) KB)"
    $e = Try-Op { [IO.File]::WriteAllText("$fd\small.txt", 'still works?') }
    [IO.Directory]::Delete($fd, $true)
    $freeBack = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
    Check "space back after delete" ([math]::Abs($freeBack - $freeBeforeFull) -lt 2MB) "(free $([math]::Round($freeBack/1MB)) MB, delta $([math]::Round(($freeBack-$freeBeforeFull)/1KB)) KB)"
}

# ---------------------------------------------------------------- summary
"-- cleanup"
foreach ($d in 'par', 'churn', 'wide', 'd01', 'deep') { $e = Try-Op { Del-Path "$Root\$d" }; if ($e) { "  note: cleanup of $d failed: $($e.Message)" } }
if (-not $KeepAll) { foreach ($f in Get-ChildItem -LiteralPath $Root -Force -File) { if ($f.Name -ne 'manifest.sha256') { [IO.File]::Delete($f.FullName) } } }
"== RESULT: $script:pass ok, $script:fail failed, $([math]::Round($sw.Elapsed.TotalSeconds)) s"
exit $script:fail
