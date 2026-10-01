# openbench.ps1 - parallel open/close and create/delete throughput, run in the guest.
#
# Measures what the volume-wide create lock costs: N threads that only open,
# read and close existing files (each in its own directory, then all in one),
# and N threads that create and delete files, each in its own directory.
# Prints the elapsed milliseconds of every phase.
#
#   powershell -File openbench.ps1 [-Root E:\openbench] [-Threads 8] [-Files 400] [-Rounds 5]
param([string]$Root = 'E:\openbench', [int]$Threads = 8, [int]$Files = 400, [int]$Rounds = 5)
$ErrorActionPreference = 'Stop'
# right after a driver restart the letter may still be on its way
$drive = [IO.Path]::GetPathRoot($Root)
for ($i = 0; $i -lt 100 -and -not [IO.Directory]::Exists($drive); $i++) { Start-Sleep -Milliseconds 100 }
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading.Tasks;
public static class OB {
    public static void Prepare(string root, int threads, int files) {
        for (int t = 0; t < threads; t++) {
            string d = Path.Combine(root, "d" + t);
            Directory.CreateDirectory(d);
            for (int i = 0; i < files; i++) {
                string p = Path.Combine(d, "f" + i + ".txt");
                if (!File.Exists(p)) File.WriteAllText(p, "file " + i);
            }
        }
    }
    static long Time(Action a) { var sw = System.Diagnostics.Stopwatch.StartNew(); a(); return sw.ElapsedMilliseconds; }
    // every thread opens, reads and closes the files of directory dirOf(t)
    public static long Open(string root, int threads, int files, int rounds, bool shared) {
        return Time(() => Parallel.For(0, threads, new ParallelOptions { MaxDegreeOfParallelism = threads }, t => {
            var buf = new byte[64];
            string d = Path.Combine(root, "d" + (shared ? 0 : t));
            for (int r = 0; r < rounds; r++)
                for (int i = 0; i < files; i++)
                    using (var f = new FileStream(Path.Combine(d, "f" + i + ".txt"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                        f.Read(buf, 0, buf.Length);
        }));
    }
    // every thread creates and deletes files in its own directory
    public static long Churn(string root, int threads, int files) {
        return Time(() => Parallel.For(0, threads, new ParallelOptions { MaxDegreeOfParallelism = threads }, t => {
            string d = Path.Combine(root, "c" + t);
            Directory.CreateDirectory(d);
            for (int i = 0; i < files; i++) {
                string p = Path.Combine(d, "n" + i);
                using (var f = new FileStream(p, FileMode.CreateNew, FileAccess.Write, FileShare.None)) f.WriteByte(1);
                File.Delete(p);
            }
            Directory.Delete(d);
        }));
    }
}
"@
[OB]::Prepare($Root, $Threads, $Files)
[void][OB]::Open($Root, $Threads, $Files, 1, $false)      # warm the name cache
"open, own directories:  {0} ms" -f [OB]::Open($Root, $Threads, $Files, $Rounds, $false)
"open, one directory:    {0} ms" -f [OB]::Open($Root, $Threads, $Files, $Rounds, $true)
"create+delete, own dir: {0} ms" -f [OB]::Churn($Root, $Threads, $Files)
