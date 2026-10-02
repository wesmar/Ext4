# SPDX-License-Identifier: GPL-2.0-only
# fsbench.ps1 - the same workloads on several volumes, side by side (guest).
#
# Small-file metadata (create, open, stat, enumerate, read, rename, delete,
# directories), a log-style append, random 4 KiB I/O and a large sequential
# file, each timed in milliseconds. Run it on NTFS and on ext4 in the same
# guest to see where the driver stands and what to work on next.
#
#   powershell -File fsbench.ps1 [-Targets C,E] [-Files 2000] [-BigMB 512]
param([string]$Targets = 'C,E', [ValidateRange(1,1000000)][int]$Files = 2000, [ValidateRange(1,65536)][int]$BigMB = 512)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class FB {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFile(string p, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool VirtualFree(IntPtr address, UIntPtr size, uint type);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle handle, IntPtr buffer, uint count, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(SafeFileHandle handle, long offset, out long position, uint origin);
    const uint NO_BUFFERING = 0x20000000, WRITE_THROUGH = 0x80000000;
    public static string Errors = "";
    static long T(Action a) { var sw = Stopwatch.StartNew(); try { a(); } catch (Exception e) { Errors += e.Message.Trim() + " | "; return -1; } return sw.ElapsedMilliseconds; }
    // VirtualAlloc gives sector-aligned storage; a managed byte[] does not.
    static void ReadUncached(string path, long size, Random rng) {
        uint chunk = rng == null ? 1U << 20 : 4096U;
        IntPtr buffer = VirtualAlloc(IntPtr.Zero, new UIntPtr(chunk), 0x3000, 4);
        if (buffer == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        try {
            using (var h = CreateFile(path, 0x80000000, 1, IntPtr.Zero, 3, NO_BUFFERING, IntPtr.Zero)) {
                if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                long count = rng == null ? size / chunk : 20000;
                for (long i = 0; i < count; i++) {
                    if (rng != null) {
                        long position;
                        if (!SetFilePointerEx(h, (long)rng.Next(0, (int)(size / chunk)) * chunk, out position, 0))
                            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    }
                    uint read;
                    if (!ReadFile(h, buffer, chunk, out read, IntPtr.Zero))
                        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    if (read != chunk) throw new IOException("Short unbuffered read");
                }
            }
        } finally { VirtualFree(buffer, UIntPtr.Zero, 0x8000); }
    }
    public static string Run(string root, int n, int bigMB) {
        if (Directory.Exists(root)) throw new IOException("Benchmark directory already exists: " + root);
        Directory.CreateDirectory(root);
        string d = Path.Combine(root, "small");
        Directory.CreateDirectory(d);
        byte[] four = new byte[4096]; new Random(1).NextBytes(four);
        var r = new System.Collections.Generic.List<string>();
        r.Add("create-empty " + T(() => { for (int i = 0; i < n; i++) File.Create(Path.Combine(d, "e" + i)).Dispose(); }));
        r.Add("create-4k " + T(() => { for (int i = 0; i < n; i++) File.WriteAllBytes(Path.Combine(d, "f" + i + ".dat"), four); }));
        r.Add("open-close " + T(() => { for (int k = 0; k < 3; k++) for (int i = 0; i < n; i++) using (var f = new FileStream(Path.Combine(d, "f" + i + ".dat"), FileMode.Open, FileAccess.Read, FileShare.Read, 1)) { } }));
        r.Add("stat " + T(() => { for (int k = 0; k < 3; k++) for (int i = 0; i < n; i++) File.GetLastWriteTimeUtc(Path.Combine(d, "f" + i + ".dat")); }));
        r.Add("enumerate " + T(() => { for (int k = 0; k < 10; k++) foreach (var fi in new DirectoryInfo(d).EnumerateFiles()) { var x = fi.Length > 0 ? fi.LastWriteTimeUtc : fi.CreationTimeUtc; } }));
        r.Add("read-4k " + T(() => { for (int i = 0; i < n; i++) File.ReadAllBytes(Path.Combine(d, "f" + i + ".dat")); }));
        r.Add("rename " + T(() => { for (int i = 0; i < n; i++) File.Move(Path.Combine(d, "f" + i + ".dat"), Path.Combine(d, "g" + i + ".dat")); }));
        r.Add("delete " + T(() => { for (int i = 0; i < n; i++) { File.Delete(Path.Combine(d, "g" + i + ".dat")); File.Delete(Path.Combine(d, "e" + i)); } }));
        r.Add("mkdir-rmdir " + T(() => { for (int i = 0; i < n / 4; i++) Directory.CreateDirectory(Path.Combine(d, "d" + i + "\\sub")); for (int i = 0; i < n / 4; i++) Directory.Delete(Path.Combine(d, "d" + i), true); }));
        string log = Path.Combine(root, "log.txt");
        byte[] line = System.Text.Encoding.ASCII.GetBytes("2026-10-01 00:00:00 an ordinary log line of about a hundred bytes, flushed each time ...\n");
        r.Add("append-flush " + T(() => { using (var f = new FileStream(log, FileMode.Append, FileAccess.Write, FileShare.Read, 4096)) for (int i = 0; i < 5000; i++) { f.Write(line, 0, line.Length); f.Flush(true); } }));
        string rnd = Path.Combine(root, "random.bin");
        long rsize = 256L << 20;
        using (var f = new FileStream(rnd, FileMode.Create)) { f.SetLength(rsize); var mb = new byte[1 << 20]; for (long o = 0; o < rsize; o += mb.Length) f.Write(mb, 0, mb.Length); }
        var rng = new Random(7);
        r.Add("random-4k-write " + T(() => { using (var f = new FileStream(rnd, FileMode.Open, FileAccess.ReadWrite, FileShare.None, 1)) { for (int i = 0; i < 20000; i++) { f.Position = (long)rng.Next(0, (int)(rsize / 4096)) * 4096; f.Write(four, 0, 4096); } f.Flush(true); } }));
        r.Add("random-4k-read-nocache " + T(() => ReadUncached(rnd, rsize, rng)));
        string big = Path.Combine(root, "big.bin");
        long bsize = (long)bigMB << 20;
        var buf = new byte[1 << 20]; new Random(3).NextBytes(buf);
        r.Add("seq-write+flush " + T(() => { using (var f = new FileStream(big, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20)) { for (long o = 0; o < bsize; o += buf.Length) f.Write(buf, 0, buf.Length); f.Flush(true); } }));
        r.Add("seq-read-nocache " + T(() => ReadUncached(big, bsize, null)));
        r.Add("overwrite-inplace " + T(() => { using (var f = new FileStream(big, FileMode.Open, FileAccess.Write, FileShare.None, 1 << 20)) { for (long o = 0; o < bsize; o += buf.Length) f.Write(buf, 0, buf.Length); f.Flush(true); } }));
        Directory.Delete(root, true);
        return string.Join(";", r);
    }
}
"@
$results = [ordered]@{}
$fail = 0
$runId = [guid]::NewGuid().ToString('N')
foreach ($t in @($Targets -split '[,\s]+' | Where-Object { $_ })) {
    $root = if ($t -eq 'C') { Join-Path $env:USERPROFILE "fsbench-$runId" } else { "$($t):\fsbench-$runId" }
    [FB]::Errors = ''
    $out = [FB]::Run($root, $Files, $BigMB)
    if ([FB]::Errors) { "$t errors: $([FB]::Errors)"; $fail++ }
    foreach ($kv in ($out -split ';')) {
        $k, $v = $kv -split ' '
        if (-not $results.Contains($k)) { $results[$k] = [ordered]@{} }
        $results[$k][$t] = [int]$v
    }
}
$names = @($Targets -split '[,\s]+' | Where-Object { $_ })
"{0,-24}" -f 'ms' + (($names | ForEach-Object { "{0,10}" -f "$($_):" }) -join '') + '     ratio'
foreach ($k in $results.Keys) {
    $row = $results[$k]
    $ratio = if ($names.Count -ge 2 -and $row[$names[0]] -gt 0) { '{0,8:N2}x' -f ($row[$names[1]] / $row[$names[0]]) } else { '' }
    "{0,-24}" -f $k + (($names | ForEach-Object { "{0,10}" -f $row[$_] }) -join '') + $ratio
}
"FSBENCH: $fail failed"
exit $fail
