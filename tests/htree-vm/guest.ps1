# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# guest.ps1 - one indexed directory, <Root>h, driven through ext4.sys (the
# guest side of tests/htree-vm.ps1; Windows PowerShell 5.1).
#
#   build  the first Count names of the names file are created in random
#          order (one in 16 a directory), half deleted, a quarter of the
#          rest renamed, a tenth moved out to <Root>m and back, half of the
#          deleted made again, <Root>m emptied and removed
#   again  the directory as <Root>expected.txt says it is (Linux changed it
#          since): checked, then a tenth deleted, renamed, added
#
# After every step the directory is listed and compared with what it should
# hold (no name missing, none extra, none twice), every name is looked up as
# it is spelt and with a character more (not there), and in another case:
# every one in a casefolded directory (every letter), a hundred or so in a
# plain one (ASCII letters; such a lookup reads the whole directory). The
# names it ends with go to <Root>expected.txt ("f name" or "d name", UTF-8)
# for Linux to check.
#
#   powershell -File guest.ps1 -Root E:\ -Names C:\Windows\Temp\names.txt -Count 30000 [-Seed n] [-Casefold] [-Mode build|again]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Names,
    [int]$Count = 0,
    [int]$Seed = 1,
    [switch]$Casefold,
    [ValidateSet('build', 'again')][string]$Mode = 'build'
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class HtGuest
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFileAttributesW(string path);

    const uint Absent = 0xFFFFFFFF, DirectoryBit = 0x10;
    const string More = "\u00A7";       // in no generated name

    static string Dir, Aside, RootPath;
    static bool Casefold;
    static Random Rng;
    static Dictionary<string, bool> Live;   // name -> a directory
    static HashSet<string> Keys;            // the folded names in use
    static string[] Pool;                   // names not used yet
    static int PoolAt;
    static int Failed, Checks;

    static void Say(string s) { Console.WriteLine(s); Console.Out.Flush(); }

    static void Check(string what, bool ok, string detail)
    {
        Checks++;
        if (ok) {
            Say("  ok   " + what + " " + detail);
        } else {
            Failed++;
            Say("  FAIL " + what + " " + detail);
        }
    }

    static string Key(string n) { return n.ToLowerInvariant(); }

    static string Variant(string n)
    {
        if (Casefold)
            return n.ToUpperInvariant();
        char[] c = n.ToCharArray();
        for (int i = 0; i < c.Length; i++)
            if (c[i] >= 'a' && c[i] <= 'z')
                c[i] = (char)(c[i] - 32);
        return new string(c);
    }

    static string PathOf(string dir, string n) { return dir + "\\" + n; }

    static void Shuffle<T>(IList<T> list)
    {
        for (int i = list.Count - 1; i > 0; i--) {
            int j = Rng.Next(i + 1);
            T t = list[i]; list[i] = list[j]; list[j] = t;
        }
    }

    static string Fresh()
    {
        while (PoolAt < Pool.Length) {
            string n = Pool[PoolAt++];
            if (n.Length > 0 && !Keys.Contains(Key(n)))
                return n;
        }
        throw new InvalidOperationException("the names file ran out of fresh names");
    }

    static List<string> Pick(double share)
    {
        List<string> all = new List<string>(Live.Keys);
        Shuffle(all);
        return all.GetRange(0, (int)(all.Count * share));
    }

    // ---- the operations; a failure is counted and the first few shown ----

    static int Errors;
    static List<string> ErrorText = new List<string>();

    static void Error(string what, string name, Exception e)
    {
        Errors++;
        if (ErrorText.Count < 5)
            ErrorText.Add(what + " '" + name + "': " + (e == null ? "" : e.GetType().Name + " " + e.Message));
    }

    static void Done(string step, int count, Stopwatch sw)
    {
        Check(step + ": " + count + " done", Errors == 0,
              "(" + sw.ElapsedMilliseconds + " ms" + (Errors == 0 ? "" : ", " + Errors + " errors: " + string.Join(" | ", ErrorText.ToArray())) + ")");
        Errors = 0;
        ErrorText.Clear();
    }

    static bool Create(string dir, string n, bool directory)
    {
        string p = PathOf(dir, n);
        try {
            if (directory) {
                if (GetFileAttributesW(p) != Absent) {
                    Error("there before mkdir", n, null);
                    return false;
                }
                Directory.CreateDirectory(p);
            } else {
                using (new FileStream(p, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { }
            }
            return true;
        } catch (Exception e) {
            Error(directory ? "mkdir" : "create", n, e);
            return false;
        }
    }

    static bool Remove(string dir, string n, bool directory)
    {
        try {
            if (directory)
                Directory.Delete(PathOf(dir, n), false);
            else
                File.Delete(PathOf(dir, n));
            return true;
        } catch (Exception e) {
            Error("delete", n, e);
            return false;
        }
    }

    static bool Move(string fromDir, string from, string toDir, string to, bool directory)
    {
        try {
            if (directory)
                Directory.Move(PathOf(fromDir, from), PathOf(toDir, to));
            else
                File.Move(PathOf(fromDir, from), PathOf(toDir, to));
            return true;
        } catch (Exception e) {
            Error("rename", from, e);
            return false;
        }
    }

    // ---- the directory compared with what it should hold ----

    static string Some(List<string> names)
    {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < names.Count && i < 3; i++)
            b.Append(" '").Append(names[i]).Append("'");
        return b.ToString();
    }

    static void Listed(string step, string dir, Dictionary<string, bool> want)
    {
        Stopwatch sw = Stopwatch.StartNew();
        HashSet<string> seen = new HashSet<string>(StringComparer.Ordinal);
        List<string> twice = new List<string>(), missing = new List<string>(), extra = new List<string>();
        int prefix = dir.Length + 1;

        foreach (string p in Directory.EnumerateFileSystemEntries(dir)) {
            string n = p.Substring(prefix);
            if (!seen.Add(n))
                twice.Add(n);
        }
        foreach (string n in want.Keys)
            if (!seen.Contains(n))
                missing.Add(n);
        foreach (string n in seen)
            if (!want.ContainsKey(n))
                extra.Add(n);
        Check(step + ": " + want.Count + " listed", twice.Count == 0 && missing.Count == 0 && extra.Count == 0,
              "(" + sw.ElapsedMilliseconds + " ms; twice " + twice.Count + Some(twice) + ", missing " + missing.Count + Some(missing) +
              ", extra " + extra.Count + Some(extra) + ")");
    }

    static void Verify(string step)
    {
        Listed(step, Dir, Live);

        // another spelling misses the index of a plain directory, and the
        // whole directory is read for it: a hundred or so there
        Stopwatch sw = Stopwatch.StartNew();
        Random sample = new Random(Live.Count);
        int variants = 0;
        List<string> lost = new List<string>(), variant = new List<string>(), phantom = new List<string>();
        foreach (KeyValuePair<string, bool> e in Live) {
            uint a = GetFileAttributesW(PathOf(Dir, e.Key));
            if (a == Absent || ((a & DirectoryBit) != 0) != e.Value) {
                // why: the error (not only "not found"), or the wrong kind
                lost.Add(e.Key + (a == Absent ? " [error " + Marshal.GetLastWin32Error() + "]" : " [kind]"));
                continue;
            }
            string v = Variant(e.Key);
            if (v != e.Key && (Casefold || sample.Next(Live.Count) < 100)) {
                variants++;
                a = GetFileAttributesW(PathOf(Dir, v));
                if (a == Absent || ((a & DirectoryBit) != 0) != e.Value)
                    variant.Add(v);
            }
            if (GetFileAttributesW(PathOf(Dir, e.Key + More)) != Absent)
                phantom.Add(e.Key + More);
        }
        Check(step + ": " + (Live.Count * 2 + variants) + " lookups", lost.Count == 0 && variant.Count == 0 && phantom.Count == 0,
              "(" + sw.ElapsedMilliseconds + " ms; not found " + lost.Count + Some(lost) + ", " + variants + " in another case, not found " + variant.Count + Some(variant) +
              ", found but absent " + phantom.Count + Some(phantom) + ")");
    }

    static void Expected()
    {
        StringBuilder b = new StringBuilder();
        foreach (KeyValuePair<string, bool> e in Live)
            b.Append(e.Value ? "d " : "f ").Append(e.Key).Append('\n');
        File.WriteAllText(PathOf(RootPath, "expected.txt"), b.ToString(), new UTF8Encoding(false));
    }

    // ---- the runs ----

    static void Setup(string root, string names, int seed, bool casefold)
    {
        RootPath = root.TrimEnd('\\');
        Dir = PathOf(RootPath, "h");
        Aside = PathOf(RootPath, "m");
        Casefold = casefold;
        Rng = new Random(seed);
        Pool = File.ReadAllLines(names, new UTF8Encoding(false));
        Live = new Dictionary<string, bool>(StringComparer.Ordinal);
        Keys = new HashSet<string>(StringComparer.Ordinal);
        Failed = Checks = 0;
    }

    public static int Build(string root, string names, int count, int seed, bool casefold)
    {
        Setup(root, names, seed, casefold);
        Directory.CreateDirectory(Dir);         // a casefolded one is there already, made by Linux
        Directory.CreateDirectory(Aside);
        Check("the directory starts empty", !Directory.EnumerateFileSystemEntries(Dir).GetEnumerator().MoveNext(), "");

        List<string> first = new List<string>();
        for (PoolAt = 0; PoolAt < count && PoolAt < Pool.Length; PoolAt++)
            first.Add(Pool[PoolAt]);
        Shuffle(first);

        Stopwatch sw = Stopwatch.StartNew();
        int dirs = 0;
        foreach (string n in first) {
            bool d = Rng.Next(16) == 0;
            if (Create(Dir, n, d)) {
                Live[n] = d;
                Keys.Add(Key(n));
                if (d) dirs++;
            }
        }
        Done("created (" + dirs + " directories)", first.Count, sw);
        Verify("created");

        sw = Stopwatch.StartNew();
        List<string> gone = Pick(0.5);
        Dictionary<string, bool> goneKind = new Dictionary<string, bool>(StringComparer.Ordinal);
        foreach (string n in gone) {
            if (Remove(Dir, n, Live[n])) {
                goneKind[n] = Live[n];
                Live.Remove(n);
                Keys.Remove(Key(n));
            }
        }
        Done("half deleted", gone.Count, sw);
        Verify("half deleted");

        sw = Stopwatch.StartNew();
        List<string> renamed = Pick(0.25);
        foreach (string n in renamed) {
            string to = Fresh();
            bool d = Live[n];
            if (Move(Dir, n, Dir, to, d)) {
                Live.Remove(n);
                Keys.Remove(Key(n));
                Live[to] = d;
                Keys.Add(Key(to));
            }
        }
        Done("a quarter renamed", renamed.Count, sw);
        Verify("renamed");

        sw = Stopwatch.StartNew();
        Dictionary<string, bool> out_ = new Dictionary<string, bool>(StringComparer.Ordinal);
        List<string> moved = Pick(0.1);
        foreach (string n in moved) {
            bool d = Live[n];
            if (Move(Dir, n, Aside, n, d)) {
                Live.Remove(n);
                out_[n] = d;        // its key stays taken: the name comes back
            }
        }
        Done("a tenth moved out", moved.Count, sw);
        Verify("moved out");
        Listed("moved out, the other directory", Aside, out_);

        sw = Stopwatch.StartNew();
        List<string> again = new List<string>(goneKind.Keys);
        Shuffle(again);
        again = again.GetRange(0, again.Count / 2);
        int made = 0;
        foreach (string n in again) {
            if (Keys.Contains(Key(n)))
                continue;           // a renamed name took it meanwhile
            if (Create(Dir, n, goneKind[n])) {
                Live[n] = goneKind[n];
                Keys.Add(Key(n));
                made++;
            }
        }
        Done("deleted names made again", made, sw);
        Verify("made again");

        sw = Stopwatch.StartNew();
        int back = 0;
        foreach (KeyValuePair<string, bool> e in out_) {
            if (back++ % 2 == 0) {
                if (Move(Aside, e.Key, Dir, e.Key, e.Value))
                    Live[e.Key] = e.Value;
            } else if (Remove(Aside, e.Key, e.Value)) {
                Keys.Remove(Key(e.Key));
            }
        }
        Done("moved back or deleted", out_.Count, sw);
        bool refused = false;
        try { Directory.Delete(Dir, false); } catch (IOException) { refused = true; }
        Check("a directory with names is not removed", refused, "");
        bool removed = true;
        try { Directory.Delete(Aside, false); } catch (Exception e) { removed = false; Say("    " + e.Message); }
        Check("the emptied directory is removed", removed && GetFileAttributesW(Aside) == Absent, "");
        Verify("final");
        Expected();
        Say("HTREE-GUEST: " + Checks + " checks, " + Failed + " failed");
        return Failed;
    }

    public static int Again(string root, string names, int seed, bool casefold)
    {
        Setup(root, names, seed, casefold);
        foreach (string line in File.ReadAllLines(PathOf(RootPath, "expected.txt"), new UTF8Encoding(false))) {
            if (line.Length < 3)
                continue;
            Live[line.Substring(2)] = line[0] == 'd';
            Keys.Add(Key(line.Substring(2)));
        }
        PoolAt = 0;
        Verify("as Linux left it");

        Stopwatch sw = Stopwatch.StartNew();
        List<string> change = Pick(0.2);
        int half = change.Count / 2;
        for (int i = 0; i < change.Count; i++) {
            string n = change[i];
            bool d = Live[n];
            if (i < half) {
                if (Remove(Dir, n, d)) {
                    Live.Remove(n);
                    Keys.Remove(Key(n));
                }
            } else {
                string to = Fresh();
                if (Move(Dir, n, Dir, to, d)) {
                    Live.Remove(n);
                    Keys.Remove(Key(n));
                    Live[to] = d;
                    Keys.Add(Key(to));
                }
            }
        }
        int added = 0;
        for (int i = 0; i < half; i++) {
            string n = Fresh();
            bool d = Rng.Next(16) == 0;
            if (Create(Dir, n, d)) {
                Live[n] = d;
                Keys.Add(Key(n));
                added++;
            }
        }
        Done("a tenth deleted, a tenth renamed, " + added + " added", change.Count + added, sw);
        Verify("changed again");
        Expected();
        Say("HTREE-GUEST: " + Checks + " checks, " + Failed + " failed");
        return Failed;
    }
}
'@

if ($Mode -eq 'build') {
    $rc = [HtGuest]::Build($Root, $Names, $Count, $Seed, [bool]$Casefold)
} else {
    $rc = [HtGuest]::Again($Root, $Names, $Seed, [bool]$Casefold)
}
exit $rc
