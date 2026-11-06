# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# guest.ps1 - the guest half of tests/fsx-vm.ps1: random file operations,
# each one checked against a model of what the files hold.
#
# Every seed makes a directory of a few files and runs a sequence of
# operations its seed fixes, on three handles per file - cached, write-
# through and unbuffered - mixed as programs mix them: writes anywhere
# (holes, appends, across the end of file), reads, the end of file moved
# down and up, the allocation size set (preallocation, or a cut), writes
# and reads through a mapped view, flushes, handles closed and opened again,
# a file marked sparse or not, ranges zeroed (holes punched in a sparse file,
# any handle, any bytes), and what the file says it holds compared with what
# it does.
# After every operation the result is compared with the model: the bytes a
# read returns and how many, the size; every 40 operations and at the end
# the whole file is read back through two handles. The first difference
# stops the run with the seed, the operation and the operations before it,
# so the run can be repeated exactly. The newest seeds' files are kept and
# their SHA-256 written to sums.txt for Linux to check; older ones are
# deleted, so that freed blocks are found again by later seeds.
#
#   powershell -File guest.ps1 -Root E:\ [-FirstSeed 1] [-Seeds 50] [-Ops 1500] [-Files 3]
#   powershell -File guest.ps1 -Root E:\ -Minimize <history.txt>
#
# -Minimize takes the history a failure printed for its file ("everything
# done to f0.bin") and cuts it to the fewest operations that still fail the
# same way (ddmin), replaying them on one file.
param([Parameter(Mandatory)][string]$Root, [int]$FirstSeed = 1, [int]$Seeds = 50, [int]$Ops = 1500,
      [int]$Files = 3, [int]$MaxSize = 4194304, [int]$Keep = 12, [string]$Minimize = '')
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class Fsx {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, IntPtr buffer, uint count, out uint done, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(SafeFileHandle h, IntPtr buffer, uint count, out uint done, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(SafeFileHandle h, long distance, out long position, uint method);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FlushFileBuffers(SafeFileHandle h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle h, int infoClass, ref long info, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileSizeEx(SafeFileHandle h, out long size);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileMappingW(SafeFileHandle h, IntPtr sa, uint protect, uint sizeHigh, uint sizeLow, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr map, uint access, uint offsetHigh, uint offsetLow, UIntPtr bytes);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UnmapViewOfFile(IntPtr view);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FlushViewOfFile(IntPtr view, UIntPtr bytes);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("ntdll.dll")]
    static extern int RtlGetLastNtStatus();
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetDiskFreeSpaceW(string root, out uint sectorsPerCluster, out uint bytesPerSector, out uint freeClusters, out uint clusters);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] input, int inputSize, byte[] output, int outputSize, out int returned, IntPtr overlapped);

    const uint GenericRW = 0xC0000000, ShareAll = 7, OpenAlways = 4;
    const uint SetSparseCode = 0x900C4, SetZeroCode = 0x980C8, QueryRangesCode = 0x940CF;
    const uint NoBuffering = 0x20000000, WriteThrough = 0x80000000;
    const int FileAllocationInfo = 5, FileEndOfFileInfo = 6;
    const int Sector = 4096, Granularity = 65536, MaxIo = 196608;
    static readonly string[] Kinds = { "cached", "write-through", "unbuffered" };

    sealed class FileState {
        public string Path;
        public byte[] Model;
        public long Length;
        public SafeFileHandle[] H = new SafeFileHandle[3];
        public List<string> History = new List<string>();
    }

    static ulong rng;
    static ulong Next() { rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27; return rng * 2685821657736338717UL; }
    static int Below(int n) { return (int)(Next() % (ulong)n); }

    static IntPtr io;
    static List<string> recent = new List<string>();
    static List<string> report = new List<string>();
    static int op;

    static FileState current;
    static void Log(string s) {
        string line = op + ": " + (current != null ? Path.GetFileName(current.Path) + " " : "") + s;
        recent.Add(line);
        if (recent.Count > 30) recent.RemoveAt(0);
        if (current != null) current.History.Add(line);
    }
    static Exception Fail(string what) {
        report.Add("FAIL " + what);
        report.Add("     the operations before it, on every file:");
        foreach (string r in recent) report.Add("       " + r);
        if (current != null) {
            report.Add("     everything done to " + Path.GetFileName(current.Path) + ":");
            foreach (string r in current.History) report.Add("       " + r);
        }
        return new Exception(what);
    }
    static Exception Win32(string what) {
        int error = Marshal.GetLastWin32Error();
        return Fail(what + ": error " + error + " (NTSTATUS 0x" + RtlGetLastNtStatus().ToString("X8") + ")");
    }

    static void Open(FileState f) {
        uint[] flags = { 0, WriteThrough, NoBuffering };
        for (int k = 0; k < 3; k++) {
            f.H[k] = CreateFileW(f.Path, GenericRW, ShareAll, IntPtr.Zero, OpenAlways, flags[k], IntPtr.Zero);
            if (f.H[k].IsInvalid) throw Win32("open " + Kinds[k] + " " + f.Path);
        }
    }
    static void Close(FileState f) {
        for (int k = 0; k < 3; k++) if (f.H[k] != null) { f.H[k].Dispose(); f.H[k] = null; }
    }

    static void Seek(SafeFileHandle h, long at) {
        long p;
        if (!SetFilePointerEx(h, at, out p, 0)) throw Win32("seek to " + at);
    }

    static void CheckSize(FileState f, string after) {
        long size;
        if (!GetFileSizeEx(f.H[0], out size)) throw Win32("size");
        if (size != f.Length) throw Fail(f.Path + ": size " + size + ", the model says " + f.Length + " (after " + after + ")");
    }

    // what a read of [at, at + n) through handle k must return, compared
    static void Read(FileState f, int k, long at, int n) {
        Seek(f.H[k], at);
        uint done;
        if (!ReadFile(f.H[k], io, (uint)n, out done, IntPtr.Zero)) {
            if (Marshal.GetLastWin32Error() != 38 || at < f.Length) throw Win32("read " + Kinds[k] + " " + at + "+" + n);
            done = 0;   // ERROR_HANDLE_EOF: an unbuffered read at or past the end
        }
        long want = Math.Max(0, Math.Min((long)n, f.Length - at));
        if (done != want) throw Fail(f.Path + ": read " + Kinds[k] + " " + at + "+" + n + " returned " + done + " bytes, the model has " + want);
        byte[] got = new byte[done];
        Marshal.Copy(io, got, 0, (int)done);
        for (int i = 0; i < done; i++)
            if (got[i] != f.Model[at + i])
                throw Fail(f.Path + ": read " + Kinds[k] + " " + at + "+" + n + ": byte " + (at + i) + " is " + got[i] + ", the model has " + f.Model[at + i]);
    }

    static void Write(FileState f, int k, long at, int n) {
        byte[] data = new byte[n];
        for (int i = 0; i < n; i++) data[i] = (byte)Next();
        Marshal.Copy(data, 0, io, n);
        Seek(f.H[k], at);
        uint done;
        if (!WriteFile(f.H[k], io, (uint)n, out done, IntPtr.Zero)) throw Win32("write " + Kinds[k] + " " + at + "+" + n);
        if (done != n) throw Fail(f.Path + ": write " + Kinds[k] + " " + at + "+" + n + " wrote " + done);
        Buffer.BlockCopy(data, 0, f.Model, (int)at, n);
        f.Length = Math.Max(f.Length, at + n);
    }

    static void SetEnd(FileState f, long size) {
        if (!SetFileInformationByHandle(f.H[Below(2)], FileEndOfFileInfo, ref size, 8)) throw Win32("end of file " + size);
        if (size < f.Length) Array.Clear(f.Model, (int)size, (int)(f.Length - size));
        f.Length = size;
    }

    // the allocation is a whole number of clusters; one below the end of file
    // cuts the file there (MS-FSA 2.1.5.15.1, and NTFS does so)
    static void SetAllocation(FileState f, long size) {
        if (!SetFileInformationByHandle(f.H[Below(2)], FileAllocationInfo, ref size, 8)) throw Win32("allocation " + size);
        long cut = (size + cluster - 1) / cluster * cluster;
        if (cut < f.Length) {
            Array.Clear(f.Model, (int)cut, (int)(f.Length - cut));
            f.Length = cut;
        }
    }
    static long cluster;

    // a view of [at, at + n) inside the file, written or compared, then gone
    static void Mapped(FileState f, long at, int n, bool write, bool flush) {
        IntPtr map = CreateFileMappingW(f.H[0], IntPtr.Zero, write ? 4u : 2u, 0, 0, null);
        if (map == IntPtr.Zero) throw Win32("mapping");
        long start = at & ~(long)(Granularity - 1);
        int span = (int)(at + n - start);
        IntPtr view = MapViewOfFile(map, write ? 2u : 4u, (uint)(start >> 32), (uint)start, (UIntPtr)(uint)span);
        if (view == IntPtr.Zero) { CloseHandle(map); throw Win32("view " + start + "+" + span); }
        try {
            IntPtr p = new IntPtr(view.ToInt64() + (at - start));
            if (write) {
                byte[] data = new byte[n];
                for (int i = 0; i < n; i++) data[i] = (byte)Next();
                Marshal.Copy(data, 0, p, n);
                Buffer.BlockCopy(data, 0, f.Model, (int)at, n);
                if (flush && !FlushViewOfFile(view, UIntPtr.Zero)) throw Win32("flush view");
            } else {
                byte[] got = new byte[n];
                Marshal.Copy(p, got, 0, n);
                for (int i = 0; i < n; i++)
                    if (got[i] != f.Model[at + i])
                        throw Fail(f.Path + ": mapped read " + at + "+" + n + ": byte " + (at + i) + " is " + got[i] + ", the model has " + f.Model[at + i]);
            }
        } finally {
            UnmapViewOfFile(view);
            CloseHandle(map);
        }
    }

    static void Control(FileState f, int k, uint code, byte[] input, byte[] output, out int done, string what) {
        if (!DeviceIoControl(f.H[k], code, input, input.Length, output, output == null ? 0 : output.Length, out done, IntPtr.Zero))
            throw Win32(what);
    }

    static void Sparse(FileState f, int k, bool on) {
        int done;
        Control(f, k, SetSparseCode, new byte[] { on ? (byte)1 : (byte)0 }, null, out done, "sparse " + on);
    }

    // [at, at + n) zeroed: any handle, any bytes, past the end of file too;
    // the size stays
    static void Punch(FileState f, int k, long at, long n) {
        byte[] range = new byte[16];
        BitConverter.GetBytes(at).CopyTo(range, 0);
        BitConverter.GetBytes(at + n).CopyTo(range, 8);
        int done;
        Control(f, k, SetZeroCode, range, null, out done, "zero " + Kinds[k] + " " + at + "+" + n);
        long end = Math.Min(at + n, f.Length);
        if (at < end) Array.Clear(f.Model, (int)at, (int)(end - at));
    }

    // the ranges the file says hold data: in order, inside the file, and no
    // byte that is not zero outside them
    static void Ranges(FileState f, int k) {
        byte[] input = new byte[16], output = new byte[65536];
        BitConverter.GetBytes(f.Length).CopyTo(input, 8);
        int done;
        Control(f, k, QueryRangesCode, input, output, out done, "ranges");
        long at = 0;
        for (int i = 0; i + 16 <= done; i += 16) {
            long start = BitConverter.ToInt64(output, i), length = BitConverter.ToInt64(output, i + 8);
            if (start < at || length <= 0 || start + length > f.Length)
                throw Fail(f.Path + ": range " + start + "+" + length + " after " + at + ", size " + f.Length);
            for (long a = at; a < start; a++)
                if (f.Model[a] != 0) throw Fail(f.Path + ": byte " + a + " is " + f.Model[a] + ", outside every range");
            at = start + length;
        }
        for (long a = at; a < f.Length; a++)
            if (f.Model[a] != 0) throw Fail(f.Path + ": byte " + a + " is " + f.Model[a] + ", past the last range");
    }

    static void Whole(FileState f, string after) {
        CheckSize(f, after);
        foreach (int k in new int[] { 0, 2 }) {
            for (long at = 0; at < f.Length; at += MaxIo) {
                int n = (int)Math.Min(MaxIo, f.Length - at);
                if (k == 2) n = (n + Sector - 1) / Sector * Sector;
                Read(f, k, at, n);
            }
        }
    }

    // an offset: mostly near the data, sometimes anywhere; aligned for the unbuffered handle
    static long Offset(FileState f, int max, bool aligned) {
        long at = Below(4) == 0 ? Below(max) : Below((int)Math.Min(max, f.Length + 65536 + 1));
        return aligned ? at / Sector * Sector : at;
    }
    static int Length(bool aligned, long at, int max) {
        int n = Below(8) == 0 ? 1 + Below(MaxIo) : 1 + Below(16384);
        n = (int)Math.Min(n, max - at);
        if (aligned) n = Math.Max(Sector, n / Sector * Sector);
        return (int)Math.Min(n, max - at);
    }

    static void Step(FileState f, int max) {
        int what = Below(100);
        if (Below(8) == 0) {                                // sparse files
            int k = Below(3), how = Below(10);
            if (how == 0) {
                bool on = Below(4) != 0;
                Log("sparse " + Kinds[k] + " " + on + " (size " + f.Length + ")");
                Sparse(f, k, on);
            } else if (how < 4) {
                Log("ranges " + Kinds[k] + " (size " + f.Length + ")");
                Ranges(f, k);
            } else {
                long at = Offset(f, max, false);
                long n = Below(4) == 0 ? 1 + Below(max) : 1 + Below(131072);
                Log("zero " + Kinds[k] + " " + at + "+" + n + " (size " + f.Length + ")");
                Punch(f, k, at, n);
            }
            CheckSize(f, "sparse");
            return;
        }
        if (what < 34) {                                    // write
            int k = Below(10) < 5 ? 0 : Below(2) == 0 ? 1 : 2;
            long at = Offset(f, max - Sector, k == 2);
            int n = Length(k == 2, at, max);
            Log("write " + Kinds[k] + " " + at + "+" + n + " (size " + f.Length + ")");
            Write(f, k, at, n);
            CheckSize(f, "write");
        } else if (what < 56) {                             // read
            int k = Below(3) == 0 ? 2 : 0;
            long at = Offset(f, max - Sector, k == 2);
            int n = Length(k == 2, at, max);
            Log("read " + Kinds[k] + " " + at + "+" + n + " (size " + f.Length + ")");
            Read(f, k, at, n);
        } else if (what < 66) {                             // end of file
            long size = Below(3) == 0 ? Below((int)Math.Min(max, f.Length + 1)) : Below(max);
            Log("end of file " + f.Length + " -> " + size);
            SetEnd(f, size);
            CheckSize(f, "end of file");
        } else if (what < 74) {                             // allocation size
            long size = Below(4) == 0 ? Below((int)Math.Min(max, f.Length + 1)) : f.Length + Below((int)(max - f.Length) + 1);
            Log("allocation " + size + " (size " + f.Length + ")");
            SetAllocation(f, size);
            CheckSize(f, "allocation");
        } else if (what < 86) {                             // mapped
            if (f.Length < 2) return;
            long at = Below((int)f.Length - 1);
            int n = 1 + Below((int)Math.Min(MaxIo, f.Length - at));
            bool write = Below(3) != 0, flush = Below(2) == 0;
            Log("mapped " + (write ? "write" : "read") + " " + at + "+" + n + (write && flush ? " flushed" : "") + " (size " + f.Length + ")");
            Mapped(f, at, n, write, flush);
            CheckSize(f, "mapped");
        } else if (what < 93) {                             // flush
            int k = Below(3);
            Log("flush " + Kinds[k]);
            if (!FlushFileBuffers(f.H[k])) throw Win32("flush");
        } else {                                            // closed and opened again
            Log("reopen");
            Close(f);
            Open(f);
            CheckSize(f, "reopen");
        }
    }

    // ---- a history replayed, and cut down to what still fails

    // one operation as Step logs it, done again; one that no longer fits the
    // file (cut out of a shorter history) is passed over
    static void Apply(FileState f, string text, int max) {
        string[] w = text.Split(' ');
        switch (w[0]) {
        case "write":
        case "read": {
            string[] r = w[2].Split('+');
            long at = long.Parse(r[0]);
            int n = int.Parse(r[1]), k = Array.IndexOf(Kinds, w[1]);
            if (at + n > max) return;
            if (w[0] == "write") { Write(f, k, at, n); CheckSize(f, "write"); } else Read(f, k, at, n);
            return;
        }
        case "end":         // end of file <from> -> <to>
            SetEnd(f, long.Parse(w[5])); CheckSize(f, "end of file"); return;
        case "allocation":
            SetAllocation(f, long.Parse(w[1])); CheckSize(f, "allocation"); return;
        case "mapped": {
            string[] r = w[2].Split('+');
            long at = long.Parse(r[0]);
            int n = int.Parse(r[1]);
            if (at + n > f.Length) return;
            Mapped(f, at, n, w[1] == "write", w.Length > 3 && w[3] == "flushed"); CheckSize(f, "mapped");
            return;
        }
        case "flush":
            if (!FlushFileBuffers(f.H[Array.IndexOf(Kinds, w[1])])) throw Win32("flush");
            return;
        case "reopen":
            Close(f); Open(f); CheckSize(f, "reopen"); return;
        case "sparse":
            Sparse(f, Array.IndexOf(Kinds, w[1]), bool.Parse(w[2])); CheckSize(f, "sparse"); return;
        case "ranges":
            Ranges(f, Array.IndexOf(Kinds, w[1])); return;
        case "zero": {
            string[] r = w[2].Split('+');
            Punch(f, Array.IndexOf(Kinds, w[1]), long.Parse(r[0]), long.Parse(r[1])); CheckSize(f, "sparse");
            return;
        }
        case "whole":
            Whole(f, "check"); return;
        }
    }

    // the operations on one file; null when they hold, else the report and
    // the operation that failed (one past the last: the checks at the end)
    static string[] Replay(string root, List<string> ops, int max, out int failedAt) {
        rng = 0x2545F4914F6CDD1DUL;
        recent.Clear(); report.Clear(); op = 0;
        FileState f = new FileState { Path = Path.Combine(root, "fsx\\replay.bin"), Model = new byte[max] };
        current = f;
        failedAt = 0;
        try {
            if (File.Exists(f.Path)) File.Delete(f.Path);
            Open(f);
            for (op = 1; op <= ops.Count; op++) { Log(ops[op - 1]); Apply(f, ops[op - 1], max); }
            Log("whole at the end"); Whole(f, "end");
            Close(f); Open(f); Whole(f, "reopened at the end");
            return null;
        } catch (Exception e) {
            failedAt = op;
            if (report.Count == 0) report.Add("FAIL " + e.Message);
            return report.ToArray();
        } finally {
            Close(f);
        }
    }

    // what kind of failure a report shows: its first line without the numbers
    static string Kind(string[] r) {
        return System.Text.RegularExpressions.Regex.Replace(r[0], @"\d+", "#");
    }

    // ddmin (Zeller and Hildebrandt): chunks of the history removed while the
    // same kind of failure stays, halving the chunks when none can go
    public static string[] Minimize(string root, string[] history, int max) {
        if (io == IntPtr.Zero) io = VirtualAlloc(IntPtr.Zero, (UIntPtr)(uint)(MaxIo + Sector), 0x3000, 4);
        uint spc, bps, free, total;
        if (!GetDiskFreeSpaceW(Path.GetPathRoot(root), out spc, out bps, out free, out total)) throw new Exception("cluster size of " + root);
        cluster = (long)spc * bps;
        Directory.CreateDirectory(Path.Combine(root, "fsx"));
        var parse = new System.Text.RegularExpressions.Regex(@"^\s*\d+: f\d+\.bin (?<op>.*?)(?: \(size \d+\))?$");
        List<string> ops = new List<string>();
        foreach (string line in history) {
            var m = parse.Match(line);
            if (m.Success && !m.Groups["op"].Value.EndsWith("at the end")) ops.Add(m.Groups["op"].Value);
        }
        int at, replays = 1;
        string[] first = Replay(root, ops, max, out at);
        if (first == null) return new string[] { "the history of " + ops.Count + " operations holds on its own" };
        string kind = Kind(first);
        if (at <= ops.Count) ops = ops.GetRange(0, at);
        for (int chunk = Math.Max(1, ops.Count / 2); chunk >= 1; ) {
            bool cut = false;
            for (int s = 0; s < ops.Count; ) {
                List<string> trial = new List<string>(ops);
                trial.RemoveRange(s, Math.Min(chunk, ops.Count - s));
                string[] r = Replay(root, trial, max, out at);
                replays++;
                if (r != null && Kind(r) == kind) {
                    ops = at <= trial.Count ? trial.GetRange(0, at) : trial;
                    cut = true;
                } else {
                    s += chunk;
                }
            }
            if (!cut) chunk /= 2;
        }
        List<string> result = new List<string>();
        result.Add(ops.Count + " operations still fail (" + replays + " replays, from " + history.Length + " lines):");
        foreach (string o in ops) result.Add("  " + o);
        string[] last = Replay(root, ops, max, out at);
        if (last != null) foreach (string l in last) result.Add(l);
        return result.ToArray();
    }

    public static string Hash(byte[] model, long length) {
        using (SHA256 sha = SHA256.Create()) {
            byte[] h = sha.ComputeHash(model, 0, (int)length);
            StringBuilder s = new StringBuilder();
            foreach (byte b in h) s.Append(b.ToString("x2"));
            return s.ToString();
        }
    }

    // one seed: its files, its operations; null when all held, else the report
    public static string[] Run(string root, int seed, int ops, int files, int max, List<string> sums) {
        if (io == IntPtr.Zero) io = VirtualAlloc(IntPtr.Zero, (UIntPtr)(uint)(MaxIo + Sector), 0x3000, 4);
        uint spc, bps, free, total;
        if (!GetDiskFreeSpaceW(Path.GetPathRoot(root), out spc, out bps, out free, out total)) throw new Exception("cluster size of " + root);
        cluster = (long)spc * bps;
        rng = 0x9E3779B97F4A7C15UL ^ (ulong)seed * 0xD1B54A32D192ED03UL;
        recent.Clear(); report.Clear(); op = 0; current = null;
        string dir = Path.Combine(root, "fsx\\s" + seed);
        Directory.CreateDirectory(dir);
        FileState[] fs = new FileState[files];
        try {
            for (int i = 0; i < files; i++) {
                fs[i] = new FileState { Path = Path.Combine(dir, "f" + i + ".bin"), Model = new byte[max] };
                if (File.Exists(fs[i].Path)) File.Delete(fs[i].Path);
                Open(fs[i]);
            }
            for (op = 1; op <= ops; op++) {
                FileState f = fs[Below(files)];
                current = f;
                Step(f, max);
                if (op % 40 == 0) { Log("whole"); Whole(f, "check"); }
            }
            foreach (FileState f in fs) { current = f; Log("whole at the end"); Whole(f, "end"); }
            foreach (FileState f in fs) { Close(f); }
            foreach (FileState f in fs) {   // and as a fresh open sees them
                current = f;
                Log("reopened at the end");
                Open(f);
                Whole(f, "reopened at the end");
                Close(f);
                sums.Add(Hash(f.Model, f.Length) + "  fsx/s" + seed + "/" + Path.GetFileName(f.Path));
            }
            return null;
        } catch (Exception e) {
            if (report.Count == 0) report.Add("FAIL " + e.Message);
            report.Insert(0, "seed " + seed + ", operation " + op);
            return report.ToArray();
        } finally {
            foreach (FileState f in fs) if (f != null) Close(f);
        }
    }
}
'@
if ($Minimize) {
    [Fsx]::Minimize($Root, [IO.File]::ReadAllLines($Minimize), $MaxSize)
    exit 0
}
$fsx = Join-Path $Root 'fsx'
$sums = New-Object 'System.Collections.Generic.List[string]'
$kept = New-Object 'System.Collections.Generic.List[int]'
if (Test-Path $fsx) { Remove-Item -Recurse -Force $fsx }
New-Item -ItemType Directory $fsx | Out-Null
# Defender scans every file closed on a volume that is not NTFS: the run
# would measure the scanner (a scanner that is down neither scans nor stops the run)
try { Add-MpPreference -ExclusionPath $fsx } catch { "  (Defender exclusion not set: $($_.Exception.Message))" }
$failed = 0
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    for ($seed = $FirstSeed; $seed -lt $FirstSeed + $Seeds; $seed++) {
        $mine = New-Object 'System.Collections.Generic.List[string]'
        $r = [Fsx]::Run($Root, $seed, $Ops, $Files, $MaxSize, $mine)
        if ($r) { $r | ForEach-Object { "  $_" }; $failed = 1; break }
        $sums.AddRange($mine)
        $kept.Add($seed)
        if ($kept.Count -gt $Keep) {
            # an older seed goes, chosen by the seed: its blocks are found again later
            $gone = $kept[$seed % ($kept.Count - 1)]
            $kept.Remove($gone) | Out-Null
            Remove-Item -Recurse -Force (Join-Path $fsx "s$gone")
            $sums.RemoveAll([Predicate[string]] { param($l) $l.Contains("/s$gone/") }) | Out-Null
        }
    }
} finally {
    try { Remove-MpPreference -ExclusionPath $fsx } catch { }
}
[IO.File]::WriteAllText((Join-Path $Root 'sums.txt'), (($sums | ForEach-Object { "$_`n" }) -join ''), [Text.Encoding]::ASCII)
$done = if ($failed) { $seed - $FirstSeed } else { $Seeds }
"FSX: $Root seeds $FirstSeed..$($FirstSeed + $Seeds - 1), $done passed, $($Ops * $done) operations, $failed failed ($([int]$clock.Elapsed.TotalSeconds) s)"
exit $failed
