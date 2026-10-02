# SPDX-License-Identifier: GPL-2.0-only
# race.ps1 - concurrent opens against namespace changes of the same names.
#
# Plain opens run under the shared volume resource and resolve names from
# the name cache while other threads change those names under the exclusive
# one. Openers hammer a fixed set of names; at the same time renamers swap
# names back and forth, a recreator deletes and creates files, and a linker
# points symlinks at files and deletes the targets (dangling links, which an
# open demotes). Only "not found" and sharing errors are expected; anything
# else is reported. Bounded in time.
#
#   powershell -File race.ps1 [-Root E:\race] [-Seconds 8] [-Workers open,rename,recreate,link]
param([string]$Root = 'E:\race', [int]$Seconds = 8, [string]$Workers = 'open,rename,recreate,link')
$ErrorActionPreference = 'Stop'
# right after a driver restart the letter may still be on its way
$drive = [IO.Path]::GetPathRoot($Root)
for ($i = 0; $i -lt 100 -and -not [IO.Directory]::Exists($drive); $i++) { Start-Sleep -Milliseconds 100 }
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
using System.Collections.Concurrent;
public static class Race {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.I1)] static extern bool CreateSymbolicLink(string link, string target, int flags);
    [StructLayout(LayoutKind.Sequential)] struct IOSB { public IntPtr S; public IntPtr I; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct USTR { public ushort Len, Max; public string Buf; }
    [StructLayout(LayoutKind.Sequential)] struct OA { public int Len; public IntPtr Root; public IntPtr Name; public uint Attr; public IntPtr Sd, Qos; }
    [DllImport("ntdll.dll")] static extern int NtOpenFile(out IntPtr h, uint access, ref OA oa, out IOSB io, uint share, uint options);
    [DllImport("ntdll.dll")] static extern int NtClose(IntPtr h);
    // DELETE access on the name itself, as DeleteFileW opens it (reparse point not followed);
    // SYNCHRONIZE is required by FILE_SYNCHRONOUS_IO_NONALERT, without it every open is 0xC000000D
    static int DeleteOpen(string p) {
        string nt = @"\??\" + p;
        var u = new USTR { Len = (ushort)(nt.Length * 2), Max = (ushort)(nt.Length * 2 + 2), Buf = nt };
        IntPtr pu = Marshal.AllocHGlobal(Marshal.SizeOf(u));
        try {
            Marshal.StructureToPtr(u, pu, false);
            var oa = new OA { Len = Marshal.SizeOf(typeof(OA)), Name = pu, Attr = 0x40 };
            IntPtr h; IOSB io;
            int st = NtOpenFile(out h, 0x00110080, ref oa, out io, 7, 0x00200000 | 0x20);
            if (st >= 0) NtClose(h);
            return st;
        } finally { Marshal.DestroyStructure(pu, typeof(USTR)); Marshal.FreeHGlobal(pu); }
    }
    static string Attr(string p) { try { return File.GetAttributes(p).ToString(); } catch (Exception e) { return e.GetType().Name; } }
    const int Names = 16;
    static bool Expected(Exception e) {
        // not found, path not found, sharing violation, access denied (delete
        // pending), already exists, and reading a dangling symlink (invalid
        // function: the link is a special file then)
        var w = e as System.ComponentModel.Win32Exception;
        int h = w != null ? w.NativeErrorCode : e.HResult & 0xFFFF;
        return e is FileNotFoundException || e is DirectoryNotFoundException ||
               h == 32 || h == 5 || h == 183 || h == 80 || h == 2 || h == 3 || h == 303 || h == 1;
    }
    public static string Run(string root, int seconds, string workers) {
        if (Directory.Exists(root)) Directory.Delete(root, true);
        Directory.CreateDirectory(root);
        for (int i = 0; i < Names; i++) File.WriteAllText(Path.Combine(root, "n" + i), "x" + i);
        var bad = new ConcurrentBag<string>();
        long opens = 0, changes = 0;
        var stop = DateTime.UtcNow.AddSeconds(seconds);
        Action<string, Action<Random>> worker = (name, body) => {
            var rng = new Random(name.GetHashCode());
            while (DateTime.UtcNow < stop) {
                try { body(rng); }
                catch (Exception e) { if (!Expected(e)) bad.Add(name + ": " + e.GetType().Name + " 0x" + e.HResult.ToString("X8") + " " + e.Message); }
            }
        };
        var tasks = new System.Collections.Generic.List<Task>();
        for (int t = 0; t < (workers.Contains("open") ? 4 : 0); t++) {
            tasks.Add(Task.Run(() => worker("open", r => {
                string p = Path.Combine(root, (r.Next(3) == 0 ? "l" : "n") + r.Next(Names));
                using (var f = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) f.ReadByte();
                Interlocked.Increment(ref opens);
            })));
        }
        if (workers.Contains("rename")) tasks.Add(Task.Run(() => worker("rename", r => {
            int i = r.Next(Names);
            string a = Path.Combine(root, "n" + i), b = Path.Combine(root, "m" + i);
            File.Move(a, b); File.Move(b, a); Interlocked.Add(ref changes, 2);
        })));
        if (workers.Contains("recreate")) tasks.Add(Task.Run(() => worker("recreate", r => {
            string p = Path.Combine(root, "n" + r.Next(Names));
            File.Delete(p); File.WriteAllText(p, "y"); Interlocked.Add(ref changes, 2);
        })));
        if (workers.Contains("link")) tasks.Add(Task.Run(() => worker("link", r => {
            int i = r.Next(Names);
            string l = Path.Combine(root, "l" + i);
            if (File.Exists(l) || Directory.Exists(l)) File.Delete(l);
            if (!CreateSymbolicLink(l, "n" + i, 0)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            Interlocked.Increment(ref changes);
        })));
        Task.WaitAll(tasks.ToArray());
        // quiet now: every name must be deletable, nothing may stay pending
        foreach (var p in Directory.GetFileSystemEntries(root)) {
            try { File.Delete(p); }
            catch (Exception e) {
                // which refusal: the attributes, the NTSTATUS of a bare delete
                // open, and whether it still holds once the lazy writer ran
                string why = Attr(p) + " open=0x" + DeleteOpen(p).ToString("X8");
                Thread.Sleep(2000);
                string later;
                try { File.Delete(p); later = "deleted on retry"; } catch { later = "still refused: " + Attr(p) + " open=0x" + DeleteOpen(p).ToString("X8"); }
                bad.Add("final delete " + Path.GetFileName(p) + ": " + e.GetType().Name + " [" + why + "; " + later + "]");
            }
        }
        return "opens=" + opens + " changes=" + changes + " unexpected=" + bad.Count +
               (bad.Count > 0 ? Environment.NewLine + string.Join(Environment.NewLine, System.Linq.Enumerable.Take(System.Linq.Enumerable.Distinct(bad), 15)) : "");
    }
}
"@
try { [Race]::Run($Root, $Seconds, $Workers) } catch { $_.Exception.InnerException.ToString(); exit 1 }
if ((Get-ChildItem $Root -Force | Measure-Object).Count -eq 0) { [IO.Directory]::Delete($Root) }
