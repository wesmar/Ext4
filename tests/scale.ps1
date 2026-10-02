# SPDX-License-Identifier: GPL-2.0-only
# scale.ps1 - how the file system scales with threads (guest).
#
# The same work split over 1, 2, 4, 8 ... threads, on each target volume:
#   create      4 KiB files, every thread in a directory of its own
#   create-dir  4 KiB files, all threads in one shared directory
#   open-read   open, stat and read them back
#   delete      delete them
#   seq-write   a large file per thread, written sequentially
#   seq-read    and read back (uncached)
# Throughput in operations (or MB) per second, and the speed-up over one
# thread. A file system that serialises its threads stays near 1.0x.
#
#   powershell -File scale.ps1 [-Targets C,E] [-Threads 1,2,4,8] [-Files 2000] [-BigMB 128]
param([string[]]$Targets = @('C', 'E'), [int[]]$Threads = @(1, 2, 4, 8), [int]$Files = 2000, [int]$BigMB = 128)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Diagnostics;
using System.Threading;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Scale {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFile(string p, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
    const uint NO_BUFFERING = 0x20000000;

    // runs body(thread) on n threads released together; the wall time in ms
    static double Par(int n, Action<int> body) {
        var go = new ManualResetEvent(false);
        var ts = new Thread[n];
        Exception err = null;
        for (int t = 0; t < n; t++) {
            int k = t;
            ts[t] = new Thread(() => { go.WaitOne(); try { body(k); } catch (Exception e) { err = e; } });
            ts[t].Start();
        }
        var sw = Stopwatch.StartNew();
        go.Set();
        foreach (var t in ts) t.Join();
        if (err != null) throw err;
        return sw.Elapsed.TotalMilliseconds;
    }

    // one line per workload: "name threads value unit"
    public static string Run(string root, int n, int files, int bigMB) {
        var r = new System.Text.StringBuilder();
        int per = files / n;
        byte[] four = new byte[4096]; new Random(1).NextBytes(four);
        if (Directory.Exists(root)) Directory.Delete(root, true);
        Directory.CreateDirectory(root);
        for (int t = 0; t < n; t++) Directory.CreateDirectory(Path.Combine(root, "t" + t));
        string shared = Path.Combine(root, "shared"); Directory.CreateDirectory(shared);

        double ms = Par(n, t => { string d = Path.Combine(root, "t" + t); for (int i = 0; i < per; i++) File.WriteAllBytes(Path.Combine(d, "f" + i), four); });
        r.AppendFormat("create {0} {1:F0} files/s\n", n, per * n * 1000.0 / ms);
        ms = Par(n, t => { for (int i = 0; i < per; i++) File.WriteAllBytes(Path.Combine(shared, "t" + t + "-" + i), four); });
        r.AppendFormat("create-dir {0} {1:F0} files/s\n", n, per * n * 1000.0 / ms);
        ms = Par(n, t => { string d = Path.Combine(root, "t" + t); for (int i = 0; i < per; i++) { string p = Path.Combine(d, "f" + i); File.GetLastWriteTimeUtc(p); File.ReadAllBytes(p); } });
        r.AppendFormat("open-read {0} {1:F0} files/s\n", n, per * n * 1000.0 / ms);
        ms = Par(n, t => { string d = Path.Combine(root, "t" + t); for (int i = 0; i < per; i++) { File.Delete(Path.Combine(d, "f" + i)); File.Delete(Path.Combine(shared, "t" + t + "-" + i)); } });
        r.AppendFormat("delete {0} {1:F0} files/s\n", n, per * n * 2 * 1000.0 / ms);

        int mb = Math.Max(16, bigMB / n);
        byte[] chunk = new byte[1 << 20]; new Random(2).NextBytes(chunk);
        ms = Par(n, t => { using (var f = new FileStream(Path.Combine(root, "big" + t), FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20)) { for (int i = 0; i < mb; i++) f.Write(chunk, 0, chunk.Length); f.Flush(true); } });
        r.AppendFormat("seq-write {0} {1:F0} MB/s\n", n, mb * n * 1000.0 / ms);
        ms = Par(n, t => {
            using (var h = CreateFile(Path.Combine(root, "big" + t), 0x80000000, 1, IntPtr.Zero, 3, NO_BUFFERING, IntPtr.Zero))
            using (var f = new FileStream(h, FileAccess.Read, 1 << 20, false)) {
                byte[] b = new byte[1 << 20]; while (f.Read(b, 0, b.Length) > 0) { }
            }
        });
        r.AppendFormat("seq-read {0} {1:F0} MB/s\n", n, mb * n * 1000.0 / ms);
        Directory.Delete(root, true);
        return r.ToString();
    }
}
"@
$counts = $Threads
foreach ($drive in $Targets) {
    $fs = (Get-Volume -DriveLetter $drive).FileSystem
    "== ${drive}: ($fs)"
    $base = @{}
    foreach ($n in $counts) {
        foreach ($line in ([Scale]::Run("${drive}:\scale-bench", $n, $Files, $BigMB) -split "`n" | Where-Object { $_ })) {
            $name, $t, $v, $unit = $line -split ' '
            if ($n -eq $counts[0]) { $base[$name] = [double]$v }
            "{0,-11} {1,2} threads {2,8} {3,-8} {4,5:F1}x" -f $name, $t, $v, $unit, ([double]$v / $base[$name])
        }
    }
}
