# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# guest.ps1 - extent trees driven through ext4.sys (the guest side of
# tests/extent-vm.ps1; Windows PowerShell 5.1).
#
#   holes      a sparse file Linux made (data in two places) written across
#              its holes and its data at once; the bytes are the ones written,
#              the rest Linux's
#   stale      a file of 0xAB deleted, its blocks preallocated by a new file
#              (allocation size), the end of the file moved into them: zeros
#   cut        a preallocated file cut inside its unwritten extent: zeros
#   pieces     a preallocated file written at random 4 KiB pieces: those
#              bytes, zeros between
#   fragments  two files grown 4 KiB at a time in turn (thousands of extents),
#              read back, cut at a few points, read back
# Every file goes with its SHA-256 to <Root>sums.txt for Linux to check;
# every check is made here first. stale, cut and pieces run over blocks a
# file of 0xAB the size of the free space left (a commit between, so the
# allocator gives them out again). Their sums are of what the files must
# hold, not of what reads here: Windows reads past a file's valid data as
# zeros, whatever the disk holds - Linux reads the disk.
#
#   powershell -File guest.ps1 -Root E:\ [-Seed n] [-Fragments 6000] [-Cases holes,stale,cut,pieces,fragments]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [int]$Seed = 1,
    [int]$Fragments = 6000,
    [string]$Cases = 'holes,stale,cut,pieces,fragments'
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class ExGuest
{
    [StructLayout(LayoutKind.Sequential)]
    struct AllocationInfo { public long Size; }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle h, int cls, ref AllocationInfo info, int size);

    static int Failed, Checks;
    static Random Rng;
    static string RootPath;
    static StringBuilder Sums = new StringBuilder();

    static void Say(string s) { Console.WriteLine(s); Console.Out.Flush(); }

    static void Check(string what, bool ok, string detail)
    {
        Checks++;
        if (ok) Say("  ok   " + what + " " + detail);
        else { Failed++; Say("  FAIL " + what + " " + detail); }
    }

    static string Path(string name) { return RootPath + "\\" + name; }

    static string Sha(string name)
    {
        using (var s = File.OpenRead(Path(name)))
        using (var h = SHA256.Create())
            return BitConverter.ToString(h.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
    }

    static void Sum(string name) { Sums.Append(Sha(name)).Append("  ").Append(name).Append('\n'); }

    static byte[] Pattern(int seed, int count)
    {
        var b = new byte[count];
        new Random(seed).NextBytes(b);
        return b;
    }

    /* the first offset where the file differs from want, or -1 */
    static long Differs(string name, byte[] want)
    {
        byte[] got = File.ReadAllBytes(Path(name));
        if (got.Length != want.Length)
            return Math.Min(got.Length, want.Length);
        for (long i = 0; i < got.Length; i++)
            if (got[i] != want[i])
                return i;
        return -1;
    }

    static void Preallocate(FileStream f, long bytes)
    {
        var info = new AllocationInfo { Size = bytes };
        if (!SetFileInformationByHandle(f.SafeFileHandle, 5 /* FileAllocationInfo */, ref info, 8))
            throw new IOException("FileAllocationInfo failed: " + Marshal.GetLastWin32Error());
    }

    /* holes: Linux's sparse.bin (expected in sparse.want) written across */
    static void Holes()
    {
        byte[] want = File.ReadAllBytes(Path("sparse.want"));
        byte[] data = Pattern(Rng.Next(), 1 << 20);
        long at = 50L * 4096;

        using (var f = new FileStream(Path("sparse.bin"), FileMode.Open, FileAccess.ReadWrite)) {
            f.Position = at;
            f.Write(data, 0, data.Length);
        }
        Array.Copy(data, 0, want, at, data.Length);
        long d = Differs("sparse.bin", want);
        Check("holes: written across holes and data", d < 0, d < 0 ? "" : "(first difference at " + d + ")");
        Sum("sparse.bin");

        /* the same uncached: one request of 1 MiB maps the hole before the
           first extent and the data after it at once */
        want = File.ReadAllBytes(Path("sparse2.want"));
        var opt = (FileOptions)(0x20000000 | (int)FileOptions.WriteThrough);   /* + FILE_FLAG_NO_BUFFERING */
        using (var f = new FileStream(Path("sparse2.bin"), FileMode.Open, FileAccess.ReadWrite, FileShare.None, 4096, opt)) {
            f.Position = at;
            f.Write(data, 0, data.Length);
        }
        Array.Copy(data, 0, want, at, data.Length);
        d = Differs("sparse2.bin", want);
        Check("holes: written across holes and data uncached", d < 0, d < 0 ? "" : "(first difference at " + d + ")");
        Sum("sparse2.bin");
    }

    /* a commit, so that blocks freed are given out again (data blocks wait
       for the transaction that freed them) */
    static void Commit() { System.Threading.Thread.Sleep(7000); }

    /* the free space, all but a margin */
    static long Free()
    {
        return new DriveInfo(RootPath.Substring(0, 1)).AvailableFreeSpace - (48L << 20);
    }

    /* the size of a test file: a third of what is free, at most 200 MiB */
    static long Third()
    {
        return Math.Max(4L << 20, Math.Min(Free() / 3, 200L << 20)) & ~((1L << 20) - 1);
    }

    /* the first offset where the file is not Head and then zeros up to Size */
    static long NotZeros(string name, byte[] head, long size)
    {
        byte[] buf = new byte[1 << 20];
        long at = 0;
        using (var f = File.OpenRead(Path(name))) {
            if (f.Length != size)
                return Math.Min(f.Length, size);
            for (int n; (n = f.Read(buf, 0, buf.Length)) > 0; at += n)
                for (int i = 0; i < n; i++)
                    if (buf[i] != (at + i < head.Length ? head[at + i] : 0))
                        return at + i;
        }
        return -1;
    }

    /* the SHA-256 Head and then zeros up to Size must have: what the file
       holds as Linux reads it (no cache here hides what the disk holds) */
    static void SumOf(string name, byte[] head, long size)
    {
        byte[] zeros = new byte[1 << 20];
        using (var h = SHA256.Create()) {
            h.TransformBlock(head, 0, head.Length, null, 0);
            for (long left = size - head.Length; left > 0; left -= zeros.Length)
                h.TransformBlock(zeros, 0, (int)Math.Min(left, zeros.Length), null, 0);
            h.TransformFinalBlock(new byte[0], 0, 0);
            Sums.Append(BitConverter.ToString(h.Hash).Replace("-", "").ToLowerInvariant()).Append("  ").Append(name).Append('\n');
        }
    }

    /* the free blocks dirty: a file of 0xAB over all of them, deleted, and a
       commit, so that the allocator gives them out again */
    static void Dirty()
    {
        long size = Free() & ~((1L << 20) - 1);
        byte[] ab = new byte[1 << 20];
        for (int i = 0; i < ab.Length; i++) ab[i] = 0xAB;
        using (var f = new FileStream(Path("ab.bin"), FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, FileOptions.WriteThrough)) {
            for (long i = 0; i < size / ab.Length; i++)
                f.Write(ab, 0, ab.Length);
        }
        File.Delete(Path("ab.bin"));
        Commit();
    }

    /* stale: what a deleted file left must never show */
    static void Stale()
    {
        byte[] head = Pattern(Rng.Next(), 65536);
        long size;

        Dirty();
        size = Third();
        using (var f = new FileStream(Path("stale.bin"), FileMode.Create, FileAccess.ReadWrite)) {
            f.Write(head, 0, head.Length);
            f.Flush();
            Preallocate(f, size);           /* unwritten, past the end */
            f.SetLength(size / 2);          /* the end moved into them */
            f.SetLength(size);
        }
        long d = NotZeros("stale.bin", head, size);
        Check("stale: a preallocation the end moved into reads as zeros", d < 0, "(" + (size >> 20) + " MiB" + (d < 0 ? ")" : "; first difference at " + d + ")"));
        SumOf("stale.bin", head, size);
    }

    /* cut: inside an unwritten extent, over dirty blocks */
    static void Cut()
    {
        byte[] head = Pattern(Rng.Next(), 4096);
        long size;

        Dirty();
        size = Third();
        using (var f = new FileStream(Path("cut.bin"), FileMode.Create, FileAccess.ReadWrite)) {
            f.Write(head, 0, head.Length);
            f.Flush();
            Preallocate(f, size);
            f.SetLength(size);
            f.SetLength(size / 2 + 12345);  /* cut inside the unwritten extent */
            f.SetLength(size);              /* and grown back */
        }
        long d = NotZeros("cut.bin", head, size);
        Check("cut: an unwritten extent cut stays zeros", d < 0, "(" + (size >> 20) + " MiB" + (d < 0 ? ")" : "; first difference at " + d + ")"));
        SumOf("cut.bin", head, size);
    }
    /* pieces: written into a preallocation at random */
    static void Pieces()
    {
        int size = (int)Math.Min(64L << 20, Third());
        byte[] want = new byte[size];

        Dirty();
        using (var f = new FileStream(Path("pieces.bin"), FileMode.Create, FileAccess.ReadWrite)) {
            Preallocate(f, size);
            f.SetLength(size);
            for (int n = 0; n < 600; n++) {
                int at = Rng.Next(size / 4096) * 4096, len = 4096 * (1 + Rng.Next(Rng.Next(4) == 0 ? 40 : 3));
                if (at + len > size) len = size - at;
                byte[] piece = Pattern(Rng.Next(), len);
                f.Position = at;
                f.Write(piece, 0, len);
                Array.Copy(piece, 0, want, at, len);
            }
        }
        long d = Differs("pieces.bin", want);
        Check("pieces: written pieces, zeros between", d < 0, d < 0 ? "" : "(first difference at " + d + ")");
        Sum("pieces.bin");
    }

    /* fragments: two files grown in turn, then cut back */
    static void Fragments(int count)
    {
        byte[][] want = { new byte[count * 4096], new byte[count * 4096] };
        using (var a = new FileStream(Path("frag-a.bin"), FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
        using (var b = new FileStream(Path("frag-b.bin"), FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough)) {
            for (int i = 0; i < count; i++) {
                byte[] x = Pattern(i * 2, 4096), y = Pattern(i * 2 + 1, 4096);
                a.Write(x, 0, 4096);
                b.Write(y, 0, 4096);
                Array.Copy(x, 0, want[0], i * 4096, 4096);
                Array.Copy(y, 0, want[1], i * 4096, 4096);
            }
        }
        long d = Differs("frag-a.bin", want[0]);
        Check("fragments: " + count + " blocks in turn with another file", d < 0, d < 0 ? "" : "(first difference at " + d + ")");
        foreach (double share in new double[] { 0.9, 0.5, 0.123, 0.01 }) {
            long keep = (long)(count * 4096 * share) + 1000;
            using (var f = new FileStream(Path("frag-a.bin"), FileMode.Open, FileAccess.Write))
                f.SetLength(keep);
            byte[] w = new byte[keep];
            Array.Copy(want[0], w, keep);
            d = Differs("frag-a.bin", w);
            Check("fragments: cut to " + keep + " bytes", d < 0, d < 0 ? "" : "(first difference at " + d + ")");
        }
        Sum("frag-a.bin");
        Sum("frag-b.bin");
    }

    public static int Run(string root, int seed, int fragments, string cases)
    {
        string c = "," + cases + ",";
        RootPath = root.TrimEnd('\\');
        Rng = new Random(seed);
        if (c.Contains(",holes,")) Holes();
        if (c.Contains(",stale,")) Stale();
        if (c.Contains(",cut,")) Cut();
        if (c.Contains(",pieces,")) Pieces();
        if (c.Contains(",fragments,")) Fragments(fragments);
        File.WriteAllText(Path("sums.txt"), Sums.ToString(), new UTF8Encoding(false));
        Say("EXTENT-GUEST: " + Checks + " checks, " + Failed + " failed");
        return Failed;
    }
}
'@

exit ([ExGuest]::Run($Root, $Seed, $Fragments, $Cases))
