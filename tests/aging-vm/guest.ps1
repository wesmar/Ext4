# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# guest.ps1 - the guest half of tests/aging-vm.ps1: age a volume the way
# use ages it, then measure how new files are laid out on it.
#
# Aging: files are created and deleted at random, the seed fixing every
# choice, until -Factor times the volume has been written, the volume kept
# between -Low and -High full. Sizes are log-uniform in three classes -
# small (4 KiB - 256 KiB, 60 %), medium (256 KiB - 8 MiB, 35 %) and large
# (8 MiB - 64 MiB, 5 %) - in sixteen directories; a third of the files have
# their size set first, as CopyFile does, the rest grow by 256 KiB writes.
# At the end the volume is brought down to -Low full.
#
# Then the measured phases, each written into a directory of its own under
# m\ and flushed: streams (files grown by 1 MiB writes, one after another),
# copies (size set first), parallel (streams written at once, one thread
# each) and small files. A file's fragments are its runs of physically
# contiguous clusters (FSCTL_GET_RETRIEVAL_POINTERS): one is the ideal.
# Last, the large files are read back around the cache, where every
# fragment costs a request to the disk.
#
#   powershell -File guest.ps1 -Root E:\ [-Seed 1] [-Factor 3]
param([Parameter(Mandatory)][string]$Root, [int]$Seed = 1, [double]$Factor = 3,
      [double]$Low = 0.55, [double]$High = 0.8)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

public static class Aging {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, ref long input, int inputSize,
                                       byte[] output, int outputSize, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetDiskFreeSpaceExW(string root, out long available, out long total, out long free);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, IntPtr buffer, uint count, out uint done, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);

    const uint GetRetrievalPointers = 0x00090073;
    const int ErrorMoreData = 234, ErrorHandleEof = 38;
    const int Chunk = 256 * 1024, Mib = 1 << 20;
    static readonly byte[] Data = MakeData();

    static byte[] MakeData() {
        byte[] b = new byte[4 * Mib];
        new Random(4242).NextBytes(b);
        return b;
    }

    public static long FreeBytes(string root) {
        long available, total, free;
        if (!GetDiskFreeSpaceExW(root, out available, out total, out free))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return free;
    }

    // a file of Size bytes, its size set first or grown by writes of Write bytes
    static void Write(string path, long size, bool sizeFirst, int write) {
        using (FileStream f = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read, 1)) {
            if (sizeFirst)
                f.SetLength(size);
            for (long left = size; left > 0; ) {
                int n = (int)Math.Min(write, left);
                f.Write(Data, (int)((size - left) % (Data.Length - write)), n);
                left -= n;
            }
            f.Flush(true);
        }
    }

    // runs of physically contiguous clusters; holes do not count
    public static int Fragments(string path) {
        using (FileStream f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            byte[] buffer = new byte[64 * 1024];
            long vcn = 0, lastEnd = -1;
            int runs = 0;
            for (;;) {
                int returned;
                bool ok = DeviceIoControl(f.SafeFileHandle, GetRetrievalPointers, ref vcn, 8, buffer, buffer.Length, out returned, IntPtr.Zero);
                int error = ok ? 0 : Marshal.GetLastWin32Error();
                if (!ok && error == ErrorHandleEof)
                    return runs;
                if (!ok && error != ErrorMoreData)
                    throw new Win32Exception(error);
                int count = BitConverter.ToInt32(buffer, 0);
                long start = BitConverter.ToInt64(buffer, 8);
                for (int i = 0; i < count; i++) {
                    long next = BitConverter.ToInt64(buffer, 16 + i * 16);
                    long lcn = BitConverter.ToInt64(buffer, 24 + i * 16);
                    if (lcn >= 0) {
                        if (lcn != lastEnd)
                            runs++;
                        lastEnd = lcn + (next - start);
                    }
                    start = next;
                }
                vcn = start;
                if (ok)
                    return runs;
            }
        }
    }

    static long Size(Random r) {
        double u = r.NextDouble(), lo, hi;
        if (u < 0.60)      { lo = 4096;      hi = 256 * 1024; }
        else if (u < 0.95) { lo = 256 * 1024; hi = 8.0 * Mib; }
        else               { lo = 8.0 * Mib;  hi = 64.0 * Mib; }
        return (long)Math.Exp(Math.Log(lo) + r.NextDouble() * (Math.Log(hi) - Math.Log(lo)));
    }

    // what the files of a set look like: count, fragments, the worst
    static string Describe(List<string> paths, Func<string, long> sizeOf) {
        int files = 0, fragmented = 0, worst = 0, bigFiles = 0;
        long runs = 0, bigRuns = 0;
        foreach (string p in paths) {
            int n = Fragments(p);
            files++;
            runs += n;
            if (n > 1) fragmented++;
            if (n > worst) worst = n;
            if (sizeOf(p) >= Mib) { bigFiles++; bigRuns += n; }
        }
        return String.Format("files={0} fragments={1} fragmented={2} worst={3} big-files={4} big-fragments={5} big-avg={6:F2}",
                             files, runs, fragmented, worst, bigFiles, bigRuns, bigFiles == 0 ? 0.0 : (double)bigRuns / bigFiles);
    }

    public static string Age(string root, int seed, double factor, double low, double high) {
        Random r = new Random(seed);
        long capacity = FreeBytes(root);
        List<string> paths = new List<string>();
        Dictionary<string, long> sizes = new Dictionary<string, long>();
        long used = 0, written = 0;
        int created = 0, deleted = 0;
        Stopwatch clock = Stopwatch.StartNew();
        for (int d = 0; d < 16; d++)
            Directory.CreateDirectory(Path.Combine(root, "age\\d" + d.ToString("00")));

        while (written < factor * capacity) {
            bool create = paths.Count == 0 || used < low * capacity || (used <= high * capacity && r.Next(2) == 0);
            long size = Size(r);
            bool sizeFirst = r.Next(3) == 0;
            int dir = r.Next(16), victim = r.Next(Math.Max(paths.Count, 1));
            if (create && used + size > high * capacity + 64.0 * Mib)
                create = false;
            if (create) {
                string p = Path.Combine(root, "age\\d" + dir.ToString("00") + "\\f" + created + ".bin");
                Write(p, size, sizeFirst, Chunk);
                paths.Add(p);
                sizes[p] = size;
                used += size;
                written += size;
                created++;
            } else {
                string p = paths[victim];
                File.Delete(p);
                used -= sizes[p];
                sizes.Remove(p);
                paths[victim] = paths[paths.Count - 1];
                paths.RemoveAt(paths.Count - 1);
                deleted++;
            }
        }
        while (used > low * capacity) {
            int victim = r.Next(paths.Count);
            string p = paths[victim];
            File.Delete(p);
            used -= sizes[p];
            sizes.Remove(p);
            paths[victim] = paths[paths.Count - 1];
            paths.RemoveAt(paths.Count - 1);
            deleted++;
        }
        return String.Format("AGED capacity={0} written={1} created={2} deleted={3} seconds={4:F1} {5}",
                             capacity, written, created, deleted, clock.Elapsed.TotalSeconds,
                             Describe(paths, delegate(string p) { return sizes[p]; }));
    }

    // Files files of Size bytes into m\<name>: grown by Write-byte writes or
    // their size set first, one after another or each on its own thread
    public static string Phase(string root, string name, int files, long size, bool sizeFirst, bool parallel, int write) {
        string dir = Path.Combine(root, "m\\" + name);
        Directory.CreateDirectory(dir);
        List<string> paths = new List<string>();
        for (int i = 0; i < files; i++)
            paths.Add(Path.Combine(dir, "f" + i + ".bin"));
        Stopwatch clock = Stopwatch.StartNew();
        if (parallel) {
            List<Thread> threads = new List<Thread>();
            Exception failure = null;
            foreach (string p in paths) {
                string path = p;
                Thread t = new Thread(delegate() {
                    try { Write(path, size, sizeFirst, write); }
                    catch (Exception e) { failure = e; }
                });
                t.Start();
                threads.Add(t);
            }
            foreach (Thread t in threads)
                t.Join();
            if (failure != null)
                throw failure;
        } else {
            foreach (string p in paths)
                Write(p, size, sizeFirst, write);
        }
        double seconds = clock.Elapsed.TotalSeconds;
        return String.Format("PHASE {0,-8} {1} x {2} KiB: {3,7:F0} ms {4,7:F1} MB/s {5}", name, files, size / 1024,
                             seconds * 1000, files * (double)size / Mib / seconds,
                             Describe(paths, delegate(string p) { return size; }));
    }

    // every file of m\<name> read back around the cache in 8 MiB requests:
    // each fragment a request of its own to the disk
    public static string ReadBack(string root, string name) {
        const uint GenericRead = 0x80000000, ShareAll = 7, OpenExisting = 3, NoBuffering = 0x20000000;
        const int Request = 8 * Mib;
        IntPtr buffer = VirtualAlloc(IntPtr.Zero, (UIntPtr)Request, 0x3000, 4);
        long bytes = 0;
        if (buffer == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        Stopwatch clock = Stopwatch.StartNew();
        foreach (string p in Directory.GetFiles(Path.Combine(root, "m\\" + name))) {
            using (SafeFileHandle h = CreateFileW(p, GenericRead, ShareAll, IntPtr.Zero, OpenExisting, NoBuffering, IntPtr.Zero)) {
                if (h.IsInvalid)
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                for (;;) {
                    uint done;
                    if (!ReadFile(h, buffer, Request, out done, IntPtr.Zero))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (done == 0)
                        break;
                    bytes += done;
                }
            }
        }
        double seconds = clock.Elapsed.TotalSeconds;
        return String.Format("READ  {0,-8} {1} MiB unbuffered: {2,7:F0} ms {3,7:F1} MB/s", name, bytes / Mib,
                             seconds * 1000, bytes / (double)Mib / seconds);
    }
}
'@

try { Add-MpPreference -ExclusionPath $Root } catch { "   (Defender exclusion not set: $($_.Exception.Message))" }
try {
    [Aging]::Age($Root, $Seed, $Factor, $Low, $High)
    "FREE $([Aging]::FreeBytes($Root))"
    [Aging]::Phase($Root, 'stream', 4, 32MB, $false, $false, 1MB)
    [Aging]::Phase($Root, 'copy', 4, 32MB, $true, $false, 1MB)
    [Aging]::Phase($Root, 'parallel', 4, 32MB, $false, $true, 1MB)
    [Aging]::Phase($Root, 'small', 2000, 16KB, $false, $false, 16KB)
    foreach ($name in 'stream', 'copy', 'parallel', 'small') { [Aging]::ReadBack($Root, $name) }
} catch {
    "!! $($_.Exception.GetBaseException().Message)"
    exit 1
} finally {
    try { Remove-MpPreference -ExclusionPath $Root } catch { "   (Defender exclusion not removed: $($_.Exception.Message))" }
}
exit 0
