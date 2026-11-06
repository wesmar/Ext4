# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# guest.ps1 - the guest half of tests/sparse-vm.ps1: what sparse files do, the
# same cases on NTFS and on the driver (Windows PowerShell 5.1).
#
#   P <name> <value>   an answer that must be NTFS's: error codes, what the
#                      ranges are, attributes. The host compares them.
#   ok / FAIL          a check made here: bytes read back, space taken
#
# Offsets are whole mebibytes wherever ranges are compared: NTFS reports sparse
# ranges by its compression units, ext4 by its blocks.
#
#   powershell -File guest.ps1 -Root E:\ [-Cases errors,basic,...] [-Keep]
#
# -Keep leaves the files and writes sums.txt for Linux; -Verify, on a later
# mount, checks that what was left is still as it was.
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Cases = 'volume,errors,access,basic,edges,grow,cached,unbuffered,locks,mapped,unsparse,overwrite,copy,links,big',
    [switch]$Keep,
    [switch]$Verify,
    [switch]$Full
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class Sg
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] input, int inputSize, byte[] output, int outputSize, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, IntPtr buffer, uint count, out uint done, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(SafeFileHandle h, IntPtr buffer, uint count, out uint done, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(SafeFileHandle h, long distance, out long position, uint method);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetEndOfFile(SafeFileHandle h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FlushFileBuffers(SafeFileHandle h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool LockFile(SafeFileHandle h, uint lowOffset, uint highOffset, uint lowCount, uint highCount);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetCompressedFileSizeW(string name, out uint high);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFileAttributesW(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetVolumeInformationW(string root, StringBuilder name, int nameSize, out uint serial, out uint maxComp, out uint flags, StringBuilder fs, int fsSize);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetDiskFreeSpaceExW(string root, out ulong available, out ulong total, out ulong free);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CopyFileW(string from, string to, bool failIfExists);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateHardLinkW(string name, string existing, IntPtr sa);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileMappingW(SafeFileHandle h, IntPtr sa, uint protect, uint sizeHigh, uint sizeLow, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr map, uint access, uint offsetHigh, uint offsetLow, UIntPtr bytes);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UnmapViewOfFile(IntPtr view);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct FindData {
        public uint Attributes;
        public long Created, Accessed, Written;
        public uint SizeHigh, SizeLow, Reserved0, Reserved1;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string Name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)] public string ShortName;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr FindFirstFileW(string name, out FindData data);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FindClose(IntPtr h);

    const uint SetSparseCode = 0x900C4, SetZeroCode = 0x980C8, QueryRangesCode = 0x940CF;
    const uint GenericRead = 0x80000000, GenericWrite = 0x40000000, ShareAll = 7;
    const uint ReadData = 1, WriteData = 2, ReadAttributes = 0x80, WriteAttributes = 0x100;
    const uint CreateAlways = 2, OpenExisting = 3, TruncateExisting = 5, OpenAlways = 4;
    const uint NoBuffering = 0x20000000, WriteThrough = 0x80000000, BackupSemantics = 0x02000000;
    const uint AttributeSparse = 0x200;
    const long M = 1L << 20;

    public static int Failed, Checks;
    public static bool Ntfs;        // the reference: what only the driver promises is not checked on it
    public static List<string> Parity = new List<string>();
    public static StringBuilder Sums = new StringBuilder();
    static string RootPath;
    static IntPtr Io;
    const int IoSize = 1 << 20;

    static void Say(string s) { Console.WriteLine(s); Console.Out.Flush(); }
    static void P(string name, object value) { Say("P " + name + " " + value); }
    static void Check(string what, bool ok, string detail) {
        Checks++;
        if (ok) Say("  ok   " + what + " " + detail);
        else { Failed++; Say("  FAIL " + what + " " + detail); }
    }
    static string PathOf(string name) { return RootPath + "\\" + name; }
    static SafeFileHandle Open(string name, uint access, uint disposition, uint flags) {
        SafeFileHandle h = CreateFileW(PathOf(name), access, ShareAll, IntPtr.Zero, disposition, flags, IntPtr.Zero);
        if (h.IsInvalid) throw new Exception("open " + name + ": error " + Marshal.GetLastWin32Error());
        return h;
    }
    static SafeFileHandle Rw(string name, uint disposition) { return Open(name, GenericRead | GenericWrite, disposition, 0); }

    static int Control(SafeFileHandle h, uint code, byte[] input, byte[] output, out int returned) {
        return DeviceIoControl(h, code, input, input == null ? 0 : input.Length, output, output == null ? 0 : output.Length, out returned, IntPtr.Zero) ?
               0 : Marshal.GetLastWin32Error();
    }
    static byte[] Pair(long a, long b) {
        byte[] r = new byte[16];
        BitConverter.GetBytes(a).CopyTo(r, 0);
        BitConverter.GetBytes(b).CopyTo(r, 8);
        return r;
    }
    static int Zero(SafeFileHandle h, long start, long end) {
        int r;
        return Control(h, SetZeroCode, Pair(start, end), null, out r);
    }
    static int Sparse(SafeFileHandle h, bool? on) {
        int r;
        return Control(h, SetSparseCode, on.HasValue ? new byte[] { on.Value ? (byte)1 : (byte)0 } : null, null, out r);
    }
    // "error" or "error:off+len,off+len", in mebibytes when they are whole
    static string Number(long v) { return v != 0 && v % M == 0 ? (v / M) + "M" : v.ToString(); }
    static string RangesRaw(SafeFileHandle h, byte[] input, int outputSize) {
        byte[] output = new byte[outputSize];
        int done, e = Control(h, QueryRangesCode, input, output, out done);
        StringBuilder s = new StringBuilder(e.ToString());
        for (int i = 0; i + 16 <= done; i += 16)
            s.Append(i == 0 ? ":" : ",").Append(Number(BitConverter.ToInt64(output, i))).Append('+').Append(Number(BitConverter.ToInt64(output, i + 8)));
        return s.ToString();
    }
    static string Ranges(SafeFileHandle h, long start, long length) { return RangesRaw(h, Pair(start, length), 4096); }

    static byte Pattern(long at) { return (byte)(at * 2654435761L >> 13 | 1); }

    static void Seek(SafeFileHandle h, long at) {
        long p;
        if (!SetFilePointerEx(h, at, out p, 0)) throw new Exception("seek: " + Marshal.GetLastWin32Error());
    }
    static void WriteAt(SafeFileHandle h, long at, long n) {
        Seek(h, at);
        while (n > 0) {
            int c = (int)Math.Min(n, IoSize);
            byte[] b = new byte[c];
            for (int i = 0; i < c; i++) b[i] = Pattern(at + i);
            Marshal.Copy(b, 0, Io, c);
            uint done;
            if (!WriteFile(h, Io, (uint)c, out done, IntPtr.Zero)) throw new Win32Failure(Marshal.GetLastWin32Error());
            at += c;
            n -= c;
        }
    }
    sealed class Win32Failure : Exception {
        public int Code;
        public Win32Failure(int code) : base("error " + code) { Code = code; }
    }
    static void SetLength(SafeFileHandle h, long size) {
        Seek(h, size);
        if (!SetEndOfFile(h)) throw new Win32Failure(Marshal.GetLastWin32Error());
    }
    // bytes of the file wrong against what it should hold: pattern, zeros in the Holes
    static string Mismatch(SafeFileHandle h, long at, long n, Func<long, bool> zero) {
        Seek(h, at);
        while (n > 0) {
            int c = (int)Math.Min(n, IoSize);
            uint done;
            if (!ReadFile(h, Io, (uint)c, out done, IntPtr.Zero)) return "read " + at + ": error " + Marshal.GetLastWin32Error();
            if (done != c) return "read " + at + ": " + done + " of " + c;
            byte[] b = new byte[c];
            Marshal.Copy(Io, b, 0, c);
            for (int i = 0; i < c; i++) {
                byte want = zero(at + i) ? (byte)0 : Pattern(at + i);
                if (b[i] != want) return "byte " + (at + i) + " is " + b[i] + ", not " + want;
            }
            at += c;
            n -= c;
        }
        return null;
    }
    static void Content(string what, SafeFileHandle h, long at, long n, Func<long, bool> zero) {
        string m = Mismatch(h, at, n, zero);
        Check(what, m == null, m ?? "");
    }
    static long OnDisk(string name) {
        uint high, low = GetCompressedFileSizeW(PathOf(name), out high);
        if (low == 0xFFFFFFFF && Marshal.GetLastWin32Error() != 0) throw new Exception("size on disk of " + name + ": " + Marshal.GetLastWin32Error());
        return (long)(((ulong)high << 32) | low);
    }
    static bool Attr(string name) { return (GetFileAttributesW(PathOf(name)) & AttributeSparse) != 0; }
    static bool Listed(string name) {
        FindData d;
        IntPtr h = FindFirstFileW(PathOf(name), out d);
        if (h == new IntPtr(-1)) throw new Exception("find " + name + ": " + Marshal.GetLastWin32Error());
        FindClose(h);
        return (d.Attributes & AttributeSparse) != 0;
    }
    static ulong Free() {
        ulong a, t, f;
        if (!GetDiskFreeSpaceExW(RootPath + "\\", out a, out t, out f)) throw new Exception("free space");
        return f;
    }
    static string Sha(string name) {
        using (FileStream s = File.OpenRead(PathOf(name)))
        using (SHA256 h = SHA256.Create())
            return BitConverter.ToString(h.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
    }
    static void Sum(string name) { Sums.Append(Sha(name)).Append("  ").Append(name.Replace('\\', '/')).Append('\n'); }

    public static void Begin(string root) {
        RootPath = root.TrimEnd('\\');
        Io = VirtualAlloc(IntPtr.Zero, (UIntPtr)(uint)IoSize, 0x3000, 4);
    }

    // ---- the cases

    public static void Volume() {
        uint serial, comp, flags;
        StringBuilder fs = new StringBuilder(64);
        GetVolumeInformationW(RootPath.Substring(0, 2) + "\\", null, 0, out serial, out comp, out flags, fs, 64);
        P("volume-sparse-flag", (flags & 0x40) != 0);
    }

    public static void Errors() {
        using (SafeFileHandle h = Rw("errors.bin", CreateAlways)) {
            WriteAt(h, 0, 2 * M);
            P("query-short-input", RangesRaw(h, new byte[8], 4096));
            P("query-negative-offset", Ranges(h, -1, 10));
            P("query-negative-length", Ranges(h, 0, -1));
            P("query-overflow", Ranges(h, long.MaxValue, 2));
            P("query-empty", Ranges(h, 0, 0));
            P("query-empty-no-output", RangesRaw(h, Pair(0, 0), 0));
            P("query-small-output", RangesRaw(h, Pair(0, M), 8));
            P("query-whole", Ranges(h, 0, 2 * M));
            P("query-past-end", Ranges(h, 0, 8 * M));
            P("query-beyond", Ranges(h, 4 * M, M));
            P("query-inside", Ranges(h, M, M));
            int r;
            P("zero-short-input", Control(h, SetZeroCode, new byte[8], null, out r));
            P("zero-negative-start", Zero(h, -1, 0));
            P("zero-negative-end", Zero(h, 0, -1));
            P("zero-reversed", Zero(h, 10, 5));
            P("zero-nothing", Zero(h, 10, 10));
            P("zero-beyond", Zero(h, 4 * M, 8 * M));
        }
        Directory.CreateDirectory(PathOf("errors.dir"));
        using (SafeFileHandle d = Open("errors.dir", GenericRead | GenericWrite, OpenExisting, BackupSemantics)) {
            P("dir-set-sparse", Sparse(d, null));
            P("dir-query", Ranges(d, 0, M));
            P("dir-zero", Zero(d, 0, M));
        }
        Directory.Delete(PathOf("errors.dir"));
        File.Delete(PathOf("errors.bin"));
    }

    public static void Access() {
        using (SafeFileHandle h = Rw("access.bin", CreateAlways)) WriteAt(h, 0, M);
        using (SafeFileHandle h = Open("access.bin", GenericRead, OpenExisting, 0)) {
            P("readonly-set-sparse", Sparse(h, true));
            P("readonly-zero", Zero(h, 0, 4096));
            P("readonly-query", Ranges(h, 0, M));
        }
        using (SafeFileHandle h = Open("access.bin", WriteData, OpenExisting, 0)) {
            P("writeonly-query", Ranges(h, 0, M));
            P("writeonly-set-sparse", Sparse(h, true));
        }
        using (SafeFileHandle h = Rw("access.bin", OpenExisting)) P("clear", Sparse(h, false));
        using (SafeFileHandle h = Open("access.bin", ReadData | WriteAttributes, OpenExisting, 0)) {
            P("attributes-set-sparse", Sparse(h, true));
            P("attributes-zero", Zero(h, 0, 4096));
        }
        P("attributes-sparse-shows", Attr("access.bin"));
        File.Delete(PathOf("access.bin"));
    }

    // data in 16 MiB, punched in the middle: bytes, ranges, attributes, space
    public static void Basic() {
        const string name = "basic.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            WriteAt(h, 0, 16 * M);
            FlushFileBuffers(h);
            long whole = OnDisk(name);
            P("before-mark", Attr(name));
            P("mark", Sparse(h, null));
            P("mark-again", Sparse(h, true));
            P("attribute", Attr(name));
            P("listed", Listed(name));
            P("ranges-marked", Ranges(h, 0, 16 * M));
            P("zero", Zero(h, 4 * M, 12 * M));
            P("ranges-punched", Ranges(h, 0, 16 * M));
            P("ranges-part", Ranges(h, 2 * M, 4 * M));
            P("ranges-in-hole", Ranges(h, 5 * M, 2 * M));
            P("ranges-from-hole", Ranges(h, 6 * M, 8 * M));
            P("ranges-past-end", Ranges(h, 0, 64 * M));
            Content("data around the hole", h, 0, 16 * M, a => a >= 4 * M && a < 12 * M);
            long punched = OnDisk(name);
            if (!Ntfs) Check("space given back", punched <= whole - 8 * M + M, whole + " -> " + punched);
            P("two-ranges-of-one", RangesRaw(h, Pair(0, 16 * M), 16 + 8));
            FlushFileBuffers(h);
        }
        P("kept-closed", Attr(name));
        using (SafeFileHandle h = Rw(name, OpenExisting))
            Content("read through a new handle", h, 0, 16 * M, a => a >= 4 * M && a < 12 * M);
    }

    // what a range does at its edges, bytes and not whole blocks
    public static void Edges() {
        const string name = "edges.bin";
        const long size = 8 * M + 1234;
        long[][] cuts = {
            new long[] { 100, 200 },                    // inside a block
            new long[] { 4000, 4200 },                  // across two blocks
            new long[] { 4096, 8192 },                  // one whole block
            new long[] { 5000, 3 * M + 77 },            // parts at both ends
            new long[] { 4 * M, 4 * M + 1 },            // one byte
            new long[] { size - 100, size },            // up to the end of file
            new long[] { size - 5000, size + 5000 },    // across it
            new long[] { size + 1, size + 4096 },       // past it
            new long[] { 6 * M + 512, 6 * M + 1024 },   // a sector
        };
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            WriteAt(h, 0, size);
            P("edges-mark", Sparse(h, null));
            foreach (long[] c in cuts) {
                P("edges-zero-" + c[0], Zero(h, c[0], c[1]));
                long[] all = Array.ConvertAll(cuts, x => x[0]);
                int upto = Array.IndexOf(all, c[0]);
                Content("after " + c[0] + ".." + c[1], h, 0, size, a => {
                    for (int i = 0; i <= upto; i++) if (a >= cuts[i][0] && a < cuts[i][1]) return true;
                    return false;
                });
            }
            SafeFileHandle u = Open(name, GenericRead, OpenExisting, 0);
            P("edges-size", new FileInfo(PathOf(name)).Length);
            FlushFileBuffers(h);
            u.Dispose();
        }
    }

    // growth: a hole of any size, nothing allocated; a write far out
    public static void Grow() {
        const string name = "grow.bin";
        ulong free = Free();
        long huge = (long)(free * 2);
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            P("grow-mark", Sparse(h, null));
            SetLength(h, huge);
            Check("grown to twice the free space", new FileInfo(PathOf(name)).Length == huge, huge.ToString());
            Check("nothing allocated", OnDisk(name) < M, OnDisk(name).ToString());
            Check("free space kept", Free() + 16 * (ulong)M >= free, free + " -> " + Free());
            P("grow-ranges-empty", Ranges(h, 0, 16 * M));
            Content("zeros at the end", h, huge - M, M, a => true);
            Content("zeros at the start", h, 0, M, a => true);
            WriteAt(h, huge - 4 * M, M);
            WriteAt(h, 2 * M, M);
            P("grow-ranges", Ranges(h, 0, 4 * M));
            Check("space of the writes", OnDisk(name) < 4 * M, OnDisk(name).ToString());
            Content("bytes written far out", h, huge - 5 * M, 5 * M, a => a < huge - 4 * M || a >= huge - 3 * M);
            SetLength(h, 8 * M);
            P("grow-cut-ranges", Ranges(h, 0, 8 * M));
        }
        // a write past the end of file: the gap stays a hole
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            P("gap-mark", Sparse(h, null));
            WriteAt(h, 0, M);
            WriteAt(h, 64 * M, M);
            P("gap-ranges", Ranges(h, 0, 65 * M));
            Check("gap takes nothing", OnDisk(name) <= 3 * M, OnDisk(name).ToString());
            Content("the gap reads as zeros", h, 0, 65 * M, a => a >= M && a < 64 * M);
        }
        File.Delete(PathOf(name));
    }

    // cached writes into a hole: the file stays whole across a flush
    public static void Cached() {
        const string name = "cached.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            P("cached-mark", Sparse(h, null));
            SetLength(h, 32 * M);
            WriteAt(h, 8 * M, M);
            P("cached-ranges", Ranges(h, 0, 32 * M));          // not flushed by the caller
            if (!Ntfs) Check("the write counts at once", OnDisk(name) >= M, OnDisk(name).ToString());
            WriteAt(h, 8 * M + 512 * 1024, M);                 // over data and hole
            FlushFileBuffers(h);
            P("cached-ranges-2", Ranges(h, 7 * M, 3 * M));
            Content("the bytes", h, 7 * M, 3 * M, a => a < 8 * M || a >= 9 * M + 512 * 1024);
        }
        using (SafeFileHandle h = Open(name, GenericRead, OpenExisting, NoBuffering))
            Content("as the disk holds them", h, 7 * M, 3 * M, a => a < 8 * M || a >= 9 * M + 512 * 1024);
        File.Delete(PathOf(name));
    }

    // an unbuffered handle, and a cached one looking on
    public static void Unbuffered() {
        const string name = "unbuffered.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            WriteAt(h, 0, 8 * M);
            P("unbuf-mark", Sparse(h, null));
            Content("cached before", h, 0, 8 * M, a => false);
            using (SafeFileHandle u = Open(name, GenericRead | GenericWrite, OpenExisting, NoBuffering)) {
                P("unbuf-zero", Zero(u, M, 3 * M));
                Content("cached sees zeros", h, 0, 8 * M, a => a >= M && a < 3 * M);
                WriteAt(u, 2 * M, M);
                Content("cached sees the write", h, 0, 8 * M, a => a >= M && a < 2 * M);
            }
            using (SafeFileHandle t = Open(name, GenericRead | GenericWrite, OpenExisting, WriteThrough)) {
                P("through-zero", Zero(t, 4 * M, 5 * M));
                Content("write through", h, 0, 8 * M, a => (a >= M && a < 2 * M) || (a >= 4 * M && a < 5 * M));
            }
            P("unbuf-ranges", Ranges(h, 0, 8 * M));
        }
        File.Delete(PathOf(name));
    }

    public static void Locks() {
        const string name = "locks.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            WriteAt(h, 0, 4 * M);
            P("locks-mark", Sparse(h, null));
            using (SafeFileHandle o = Rw(name, OpenExisting)) {
                if (!LockFile(o, (uint)M, 0, 4096, 0)) throw new Exception("lock: " + Marshal.GetLastWin32Error());
                P("locked-zero", Zero(h, 0, 2 * M));
                P("locked-owner-zero", Zero(o, M, M + 4096));
                P("beside-the-lock", Zero(h, 2 * M, 3 * M));
            }
            P("unlocked-zero", Zero(h, 0, 2 * M));
            Content("zeros", h, 0, 4 * M, a => a < 3 * M);
        }
        File.Delete(PathOf(name));
    }

    // a mapped view cannot be purged: the bytes go zero all the same, everywhere
    public static void Mapped() {
        const string name = "mapped.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            WriteAt(h, 0, 4 * M);
            P("mapped-mark", Sparse(h, null));
            IntPtr map = CreateFileMappingW(h, IntPtr.Zero, 4, 0, 0, null);
            if (map == IntPtr.Zero) throw new Exception("mapping: " + Marshal.GetLastWin32Error());
            IntPtr view = MapViewOfFile(map, 2, 0, 0, (UIntPtr)(uint)(4 * M));
            if (view == IntPtr.Zero) throw new Exception("view: " + Marshal.GetLastWin32Error());
            P("mapped-byte", Marshal.ReadByte(view, (int)M + 4096) == Pattern(M + 4096));
            P("mapped-zero", Zero(h, M, 3 * M));
            bool zero = true;
            for (long a = M; a < 3 * M; a += 4096) zero &= Marshal.ReadByte(view, (int)a) == 0;
            Check("the view shows zeros", zero, "");
            Marshal.WriteByte(view, (int)(2 * M), 0x5A);
            UnmapViewOfFile(view);
            CloseHandle(map);
            FlushFileBuffers(h);
            Seek(h, 2 * M);
            uint done;
            ReadFile(h, Io, 1, out done, IntPtr.Zero);
            Check("a write through the view stays", Marshal.ReadByte(Io) == 0x5A, Marshal.ReadByte(Io).ToString());
            P("after-the-view", Zero(h, 2 * M, 3 * M));
            Content("the rest", h, 0, 4 * M, a => a >= M && a < 3 * M);
        }
        File.Delete(PathOf(name));
    }

    // a file made ordinary again: the holes get blocks
    public static void Unsparse() {
        const string name = "unsparse.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            P("un-mark", Sparse(h, null));
            SetLength(h, 32 * M);
            WriteAt(h, 4 * M, M);
            FlushFileBuffers(h);
            long before = OnDisk(name);
            P("un-attribute", Attr(name));
            P("un-clear", Sparse(h, false));
            P("un-attribute-after", Attr(name));
            P("un-listed-after", Listed(name));
            Check("the holes hold blocks", OnDisk(name) >= 32 * M, before + " -> " + OnDisk(name));
            Content("bytes unchanged", h, 0, 32 * M, a => a < 4 * M || a >= 5 * M);
            P("un-ranges", Ranges(h, 0, 32 * M));
            long allocated = OnDisk(name);
            P("un-zero", Zero(h, 8 * M, 16 * M));
            Check("a file not sparse keeps its blocks", OnDisk(name) >= allocated, allocated + " -> " + OnDisk(name));
            Content("zeros all the same", h, 0, 32 * M, a => a < 4 * M || (a >= 5 * M && a < 8 * M) || (a >= 8 * M && a < 16 * M) || a >= 16 * M);
        }
        File.Delete(PathOf(name));
    }

    // what the mark does through an overwrite
    public static void Overwrite() {
        const string name = "overwrite.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) { WriteAt(h, 0, M); P("ow-mark", Sparse(h, null)); }
        using (SafeFileHandle h = Open(name, GenericRead | GenericWrite, TruncateExisting, 0)) P("ow-truncated", Attr(name));
        using (SafeFileHandle h = Rw(name, OpenExisting)) { P("ow-mark-2", Sparse(h, null)); }
        using (SafeFileHandle h = Rw(name, CreateAlways)) { P("ow-replaced", Attr(name)); P("ow-mark-3", Sparse(h, null)); }
        File.Delete(PathOf(name));
        // the inode goes and may come back: nothing of the mark with it
        using (SafeFileHandle h = Rw(name, CreateAlways)) P("ow-new-after-delete", Attr(name));
        File.Delete(PathOf(name));
    }

    // a copy of a sparse file
    public static void Copy() {
        const string name = "copy.bin", to = "copy-to.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            P("copy-mark", Sparse(h, null));
            SetLength(h, 64 * M);
            WriteAt(h, 8 * M, M);
            WriteAt(h, 32 * M, M);
        }
        if (!CopyFileW(PathOf(name), PathOf(to), false)) throw new Exception("copy: " + Marshal.GetLastWin32Error());
        P("copy-attribute", Attr(to));
        using (SafeFileHandle h = Rw(to, OpenExisting)) {
            P("copy-ranges", Ranges(h, 0, 64 * M));
            Content("the copy's bytes", h, 0, 64 * M, a => !((a >= 8 * M && a < 9 * M) || (a >= 32 * M && a < 33 * M)));
        }
        File.Delete(PathOf(to));
        File.Delete(PathOf(name));
    }

    // all names of a file show it
    public static void Links() {
        const string name = "links.bin", other = "links-other.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) WriteAt(h, 0, M);
        if (!CreateHardLinkW(PathOf(other), PathOf(name), IntPtr.Zero)) throw new Exception("link: " + Marshal.GetLastWin32Error());
        using (SafeFileHandle h = Rw(name, OpenExisting)) P("links-mark", Sparse(h, null));
        P("links-attribute", Attr(other));
        P("links-listed", Listed(other));
        File.Delete(PathOf(other));
        File.Delete(PathOf(name));
    }

    // a file in thousands of pieces gone in one request: on a volume of the
    // smallest journal one handle could not hold that, and the journal would
    // abort (fsd\SparseFiles.c goes in steps). The volume must stay writable.
    public static void Big() {
        const string name = "big.bin";
        const long size = 96 * M, piece = 8192;
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            WriteAt(h, 0, size);
            FlushFileBuffers(h);
            P("big-mark", Sparse(h, null));
            int errors = 0;
            for (long at = piece; at < size; at += 2 * piece) if (Zero(h, at, at + piece) != 0) errors++;
            P("big-pieces", errors);
            Content("every other piece zeros", h, 0, size, a => (a / piece) % 2 == 1);
            FlushFileBuffers(h);
            P("big-all", Zero(h, 0, size));
            P("big-ranges", Ranges(h, 0, size));
            if (!Ntfs) Check("all of it given back", OnDisk(name) < 2 * M, OnDisk(name).ToString());
            WriteAt(h, M, M);
            FlushFileBuffers(h);
            Content("writable after it", h, 0, 4 * M, a => a < M || a >= 2 * M);
        }
        File.Delete(PathOf(name));
    }

    // a file as long as ext4 lets one be, on a small volume: the limit of its
    // logical blocks, 16 TiB less a block of 4 KiB, 4 TiB of 1 KiB (the driver
    // only: NTFS has its own)
    public static void Limit(long block) {
        const string name = "limit.bin";
        long max = (0x100000000L - 1) * block;
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            P("limit-mark", Sparse(h, null));
            SetLength(h, max);
            Check("grown to the limit", new FileInfo(PathOf(name)).Length == max, max.ToString());
            WriteAt(h, max - M, M);
            WriteAt(h, max / 2, M);
            FlushFileBuffers(h);
            Check("two pieces, far apart, take no more", OnDisk(name) < 3 * M, OnDisk(name).ToString());
            Content("bytes at the very end", h, max - 2 * M, 2 * M, a => a < max - M);
            Content("bytes half way", h, max / 2 - M, 3 * M, a => a < max / 2 || a >= max / 2 + M);
            string r = Ranges(h, 0, max);
            Check("both are ranges", r.StartsWith("0:") && r.Split(',').Length == 2, r);
            Check("a whole request over the file", Zero(h, 0, max) == 0, "");
            Check("nothing left", OnDisk(name) < M, OnDisk(name).ToString());
            Check("no range left", Ranges(h, 0, max) == "0", Ranges(h, 0, max));
            SetLength(h, 0);
        }
        File.Delete(PathOf(name));
    }

    // ---- left for a later mount and for Linux (the driver only)

    public static void Leave() {
        const long size = 64 * M;
        using (SafeFileHandle h = Rw("left-holes.bin", CreateAlways)) {
            P("left-mark", Sparse(h, null));
            SetLength(h, size);
            WriteAt(h, 8 * M, M);
            WriteAt(h, 32 * M, 2 * M);
            WriteAt(h, size - M, M);
            P("left-zero", Zero(h, 32 * M + 512 * 1024, 33 * M + 512 * 1024));
        }
        using (SafeFileHandle h = Rw("left-marked-only.bin", CreateAlways)) { WriteAt(h, 0, M); P("left-mark-2", Sparse(h, null)); }
        using (SafeFileHandle h = Rw("left-cleared.bin", CreateAlways)) {
            P("left-mark-3", Sparse(h, null));
            SetLength(h, 2 * M);
            P("left-clear", Sparse(h, false));
        }
        foreach (string f in new[] { "left-holes.bin", "left-marked-only.bin", "left-cleared.bin" }) Sum(f);
    }

    // after the volume went and came back: the mark and the holes are the disk's
    public static void Revisit() {
        Check("mark of a file with holes", Attr("left-holes.bin"), "");
        Check("listed sparse", Listed("left-holes.bin"), "");
        Check("mark of a file without any", Attr("left-marked-only.bin"), "");
        Check("listed sparse without holes", Listed("left-marked-only.bin"), "");
        Check("a cleared mark stays cleared", !Attr("left-cleared.bin") && !Listed("left-cleared.bin"), "");
        using (SafeFileHandle h = Rw("left-holes.bin", OpenExisting)) {
            Say("  ranges " + Ranges(h, 0, 64 * M));
            Check("holes as they were", OnDisk("left-holes.bin") <= 5 * M, OnDisk("left-holes.bin").ToString());
        }
        using (SafeFileHandle h = Rw("left-marked-only.bin", OpenExisting)) {
            SetLength(h, 1024 * M * 4);
            Check("marked file grows by a hole after a remount", OnDisk("left-marked-only.bin") < 2 * M, OnDisk("left-marked-only.bin").ToString());
            SetLength(h, M);
        }
    }

    // a full volume: blocks come when a cached write is made, or it fails then
    public static void DiskFull() {
        const string name = "full.bin", filler = "full-filler.bin";
        using (SafeFileHandle h = Rw(name, CreateAlways)) {
            Sparse(h, null);
            SetLength(h, (long)Free() + 256 * M);
            WriteAt(h, 0, 64 * 1024);
        }
        using (SafeFileHandle f = Rw(filler, CreateAlways)) {
            long n = (long)Free();
            try { for (long at = 0; ; at += M) WriteAt(f, at, M); }
            catch (Win32Failure e) { Check("filler stopped by a full volume", e.Code == 112, e.Message); }
        }
        using (SafeFileHandle h = Rw(name, OpenExisting)) {
            // a read first: a handle that caches is the one that could go the
            // fast way, past the file system
            int error = 0;
            uint got;
            Seek(h, 0);
            ReadFile(h, Io, 4096, out got, IntPtr.Zero);
            try { WriteAt(h, 16 * M, 8 * M); } catch (Win32Failure e) { error = e.Code; }
            Check("a cached write into a hole fails when made", error == 112, "error " + error);
            FlushFileBuffers(h);
            P("full-zero", Zero(h, 0, 64 * M));
        }
        File.Delete(PathOf(filler));
        using (SafeFileHandle h = Rw(name, OpenExisting)) {
            SetLength(h, 64 * M);
            WriteAt(h, 16 * M, M);
            Content("room again", h, 16 * M, M, a => false);
        }
        File.Delete(PathOf(name));
    }
}
'@

[Sg]::Begin($Root)
[Sg]::Ntfs = (Split-Path -Qualifier $Root) -eq 'C:'
$dir = $Root.TrimEnd('\')
if ($Verify) {
    [Sg]::Revisit()
} else {
    foreach ($case in $Cases.Split(',')) {
        switch ($case) {
            'volume'     { [Sg]::Volume() }
            'errors'     { [Sg]::Errors() }
            'access'     { [Sg]::Access() }
            'basic'      { [Sg]::Basic() }
            'edges'      { [Sg]::Edges() }
            'grow'       { [Sg]::Grow() }
            'cached'     { [Sg]::Cached() }
            'unbuffered' { [Sg]::Unbuffered() }
            'locks'      { [Sg]::Locks() }
            'mapped'     { [Sg]::Mapped() }
            'unsparse'   { [Sg]::Unsparse() }
            'overwrite'  { [Sg]::Overwrite() }
            'copy'       { [Sg]::Copy() }
            'links'      { [Sg]::Links() }
            'big'        { [Sg]::Big() }
            'limit4k'    { [Sg]::Limit(4096) }
            'limit1k'    { [Sg]::Limit(1024) }
            'none'       { }
            default      { throw "No case $case" }
        }
    }
    if ($Keep) {
        [Sg]::Leave()
        [IO.File]::WriteAllText("$dir\sums.txt", [Sg]::Sums.ToString(), [Text.Encoding]::ASCII)
    }
    if ($Full) { [Sg]::DiskFull() }
}
"SPARSE: $([Sg]::Checks) checks, $([Sg]::Failed) failed"
exit ([Sg]::Failed)
