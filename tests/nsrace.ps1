# SPDX-License-Identifier: GPL-2.0-only
# nsrace.ps1 - creates and deletes of one directory's names racing each other.
#
# Creates run side by side under the shared volume resource; what keeps two
# of them in one directory apart is that directory's own lock, held from
# "the name is free" to "the entry is in", and by delete. Four races on it,
# all threads released together by a barrier:
#   same-name   every thread creates the same names (CreateNew): exactly one
#               wins each name, the rest get "exists", the listing agrees
#   mkdir-fill  one thread makes directories while others create files in
#               them the moment they appear (a directory without "." yet
#               takes no names: "not found" is expected, nothing else)
#   rmdir-fill  one thread removes directories while others create files in
#               them: either the directory goes (empty) or the file stays
#               in it, never a file in a directory that is gone
#   links       a file and its hard link deleted at the same moment from
#               two directories: the inode goes once
#   parents     every thread makes a subdirectory of a directory none has
#               made yet: the parent is complete before anyone can use it
# The volume should then pass e2fsck (run-ext4test.ps1, or by hand), which is
# where a double free or an orphaned entry would show.
#
#   powershell -File nsrace.ps1 [-Root E:\nsrace] [-Threads 8] [-Rounds 300]
param([string]$Root = 'E:\nsrace', [int]$Threads = 8, [int]$Rounds = 300)
$ErrorActionPreference = 'Stop'
$drive = [IO.Path]::GetPathRoot($Root)
for ($i = 0; $i -lt 100 -and -not [IO.Directory]::Exists($drive); $i++) { Start-Sleep -Milliseconds 100 }
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading;
using System.Runtime.InteropServices;
using System.Collections.Concurrent;
public static class NsRace {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.I1)] static extern bool CreateHardLink(string link, string target, IntPtr sa);
    const int ERROR_FILE_EXISTS = 80, ERROR_ALREADY_EXISTS = 183;
    const int ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5;
    const int ERROR_DIR_NOT_EMPTY = 145, ERROR_SHARING_VIOLATION = 32;

    static int Win32(Exception e) { return Marshal.GetHRForException(e) & 0xFFFF; }

    // runs body(thread) on n threads released together
    static void Par(int n, Action<int> body, ConcurrentBag<string> bad) {
        var go = new ManualResetEvent(false);
        var ts = new Thread[n];
        for (int t = 0; t < n; t++) {
            int k = t;
            ts[t] = new Thread(() => { go.WaitOne(); try { body(k); } catch (Exception e) { bad.Add("thread " + k + ": " + e.Message); } });
            ts[t].Start();
        }
        go.Set();
        foreach (var t in ts) t.Join();
    }

    public static string SameName(string root, int n, int rounds) {
        var bad = new ConcurrentBag<string>();
        string d = Path.Combine(root, "same"); Directory.CreateDirectory(d);
        var wins = new int[rounds];
        Par(n, t => {
            for (int i = 0; i < rounds; i++) {
                try {
                    using (new FileStream(Path.Combine(d, "n" + i), FileMode.CreateNew, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete)) { }
                    Interlocked.Increment(ref wins[i]);
                } catch (IOException e) {
                    int w = Win32(e);
                    if (w != ERROR_FILE_EXISTS && w != ERROR_ALREADY_EXISTS) bad.Add("same-name n" + i + ": " + e.Message);
                }
            }
        }, bad);
        for (int i = 0; i < rounds; i++) if (wins[i] != 1) bad.Add("same-name n" + i + " created " + wins[i] + " times");
        int listed = Directory.GetFiles(d).Length;
        if (listed != rounds) bad.Add("same-name: " + listed + " names listed, " + rounds + " expected");
        return Report("same-name", bad);
    }

    public static string MkdirFill(string root, int n, int rounds) {
        var bad = new ConcurrentBag<string>();
        string d = Path.Combine(root, "mkdir"); Directory.CreateDirectory(d);
        var made = new int[rounds];
        Par(n, t => {
            for (int i = 0; i < rounds; i++) {
                string sub = Path.Combine(d, "d" + i);
                if (t == 0) { Directory.CreateDirectory(sub); continue; }
                try {
                    using (new FileStream(Path.Combine(sub, "f" + t), FileMode.CreateNew, FileAccess.Write)) { }
                    Interlocked.Increment(ref made[i]);
                } catch (IOException e) {
                    int w = Win32(e);
                    if (w != ERROR_PATH_NOT_FOUND && w != ERROR_FILE_NOT_FOUND) bad.Add("mkdir-fill d" + i + ": " + e.Message);
                }
            }
        }, bad);
        for (int i = 0; i < rounds; i++) {
            string sub = Path.Combine(d, "d" + i);
            if (!Directory.Exists(sub)) { bad.Add("mkdir-fill: d" + i + " missing"); continue; }
            int listed = Directory.GetFiles(sub).Length;
            if (listed != made[i]) bad.Add("mkdir-fill: d" + i + " lists " + listed + " files, " + made[i] + " were made");
        }
        return Report("mkdir-fill", bad);
    }

    public static string RmdirFill(string root, int n, int rounds) {
        var bad = new ConcurrentBag<string>();
        string d = Path.Combine(root, "rmdir"); Directory.CreateDirectory(d);
        for (int i = 0; i < rounds; i++) Directory.CreateDirectory(Path.Combine(d, "d" + i));
        var made = new int[rounds];
        var removed = new bool[rounds];
        Par(n, t => {
            for (int i = 0; i < rounds; i++) {
                string sub = Path.Combine(d, "d" + i);
                if (t == 0) {
                    try { Directory.Delete(sub); removed[i] = true; }
                    catch (IOException e) { if (Win32(e) != ERROR_DIR_NOT_EMPTY) bad.Add("rmdir-fill rmdir d" + i + ": " + e.Message); }
                    continue;
                }
                try {
                    using (new FileStream(Path.Combine(sub, "f" + t), FileMode.CreateNew, FileAccess.Write)) { }
                    Interlocked.Increment(ref made[i]);
                } catch (IOException e) {
                    int w = Win32(e);
                    if (w != ERROR_PATH_NOT_FOUND && w != ERROR_FILE_NOT_FOUND && w != ERROR_ACCESS_DENIED)
                        bad.Add("rmdir-fill d" + i + ": " + e.Message);
                } catch (UnauthorizedAccessException) {
                    // the directory is going: delete pending
                }
            }
        }, bad);
        for (int i = 0; i < rounds; i++) {
            string sub = Path.Combine(d, "d" + i);
            bool there = Directory.Exists(sub);
            if (removed[i] && there) bad.Add("rmdir-fill: d" + i + " removed but still there");
            if (removed[i] && made[i] > 0) bad.Add("rmdir-fill: d" + i + " removed with " + made[i] + " files made in it");
            if (!removed[i] && there && Directory.GetFiles(sub).Length != made[i])
                bad.Add("rmdir-fill: d" + i + " lists " + Directory.GetFiles(sub).Length + " files, " + made[i] + " were made");
        }
        return Report("rmdir-fill", bad);
    }

    public static string Links(string root, int n, int rounds) {
        var bad = new ConcurrentBag<string>();
        string a = Path.Combine(root, "la"), b = Path.Combine(root, "lb");
        Directory.CreateDirectory(a); Directory.CreateDirectory(b);
        byte[] data = new byte[8192]; new Random(3).NextBytes(data);
        for (int i = 0; i < rounds; i++) {
            string f = Path.Combine(a, "f" + i);
            File.WriteAllBytes(f, data);
            if (!CreateHardLink(Path.Combine(b, "l" + i), f, IntPtr.Zero)) bad.Add("links: CreateHardLink " + i + " failed " + Marshal.GetLastWin32Error());
        }
        // pairs of threads: one deletes the name in la, its partner the one in lb
        int pairs = Math.Max(1, n / 2);
        Par(pairs * 2, t => {
            string dir = (t % 2 == 0) ? a : b, prefix = (t % 2 == 0) ? "f" : "l";
            for (int i = t / 2; i < rounds; i += pairs) {
                try { File.Delete(Path.Combine(dir, prefix + i)); }
                catch (IOException e) { if (Win32(e) != ERROR_SHARING_VIOLATION) bad.Add("links " + prefix + i + ": " + e.Message); }
            }
        }, bad);
        if (Directory.GetFiles(a).Length + Directory.GetFiles(b).Length != 0) bad.Add("links: names left behind");
        return Report("links", bad);
    }

    // all threads make a subdirectory of one directory that none has made
    // yet: whoever makes the parent, everyone's subdirectory must follow
    public static string Parents(string root, int n, int rounds) {
        var bad = new ConcurrentBag<string>();
        for (int i = 0; i < rounds; i++) {
            string parent = Path.Combine(root, "p" + i);
            Par(n, t => {
                try { Directory.CreateDirectory(Path.Combine(parent, "w" + t)); }
                catch (Exception e) { bad.Add("parents p" + i + " w" + t + ": " + e.Message); }
            }, bad);
            if (Directory.GetDirectories(parent).Length != n) bad.Add("parents p" + i + ": " + Directory.GetDirectories(parent).Length + " of " + n);
        }
        return Report("parents", bad);
    }

    static string Report(string name, ConcurrentBag<string> bad) {
        var r = new System.Text.StringBuilder();
        int shown = 0;
        foreach (var s in bad) { if (shown++ < 10) r.AppendLine("  " + s); }
        r.AppendFormat("{0,-11} {1} unexpected", name, bad.Count);
        return r.ToString();
    }
}
"@
if ([IO.Directory]::Exists($Root)) { cmd /c "rd /s /q $Root" }
[void][IO.Directory]::CreateDirectory($Root)
$total = 0
foreach ($case in 'SameName', 'MkdirFill', 'RmdirFill', 'Links', 'Parents') {
    $out = [NsRace]::$case($Root, $Threads, $Rounds)
    $out
    if ($out -match '(\d+) unexpected$') { $total += [int]$Matches[1] }
}
cmd /c "rd /s /q $Root"
"nsrace: $total unexpected"
