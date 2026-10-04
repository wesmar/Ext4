# SPDX-License-Identifier: GPL-2.0-only
# freed-reuse.ps1 - blocks a delete freed are not given to another file before
# the delete commits (guest).
#
# File data is written in place, not through the journal. A block freed by
# the running transaction and handed to a new file at once ends up, after a
# crash before that commit, in two files: the deleted one comes back with
# the new one's data. So:
#   reuse     A is written and committed, deleted, and B is written at once:
#             B's blocks must not be A's (FSCTL_GET_RETRIEVAL_POINTERS)
#   returns   once the delete has committed, the free space is what it was
#   replace   a nearly full volume: delete a file and write one as large at
#             once - the space must be there (no ENOSPC from the deferral)
#
#   powershell -File freed-reuse.ps1 [-Drive U] [-Rounds 5]
param([string]$Drive = 'U', [int]$Rounds = 5, [int]$FileMB = 4, [int]$ReplaceMB = 48)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Extents {
    const uint FSCTL_GET_RETRIEVAL_POINTERS = 0x00090073;
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, ref long inBuf, int inLen,
                                       byte[] outBuf, int outLen, out int got, IntPtr ov);
    // the clusters of an open file as [lcn, lcn + count) ranges
    public static List<long[]> Of(FileStream f) {
        var r = new List<long[]>();
        long vcn = 0;
        byte[] buf = new byte[64 * 1024];
        int got;
        bool ok = DeviceIoControl(f.SafeFileHandle, FSCTL_GET_RETRIEVAL_POINTERS, ref vcn, 8, buf, buf.Length, out got, IntPtr.Zero);
        if (!ok && Marshal.GetLastWin32Error() != 234) throw new IOException("retrieval pointers: " + Marshal.GetLastWin32Error());
        int n = BitConverter.ToInt32(buf, 0);
        long prev = BitConverter.ToInt64(buf, 8);
        for (int i = 0; i < n; i++) {
            long next = BitConverter.ToInt64(buf, 16 + i * 16);
            long lcn = BitConverter.ToInt64(buf, 24 + i * 16);
            if (lcn >= 0) r.Add(new long[] { lcn, lcn + (next - prev) });
            prev = next;
        }
        return r;
    }
    public static long Shared(List<long[]> a, List<long[]> b) {
        long s = 0;
        foreach (var x in a) foreach (var y in b) {
            long lo = Math.Max(x[0], y[0]), hi = Math.Min(x[1], y[1]);
            if (hi > lo) s += hi - lo;
        }
        return s;
    }
    public static List<long[]> Write(string path, int mb, bool flush) {
        byte[] chunk = new byte[1 << 20];
        new Random(mb).NextBytes(chunk);
        using (var f = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None, 1 << 20)) {
            for (int i = 0; i < mb; i++) f.Write(chunk, 0, chunk.Length);
            f.Flush(flush);
            return Of(f);
        }
    }
}
"@
$root = "${Drive}:\freed-reuse"
if ([IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root, $true) }
[void][IO.Directory]::CreateDirectory($root)
function Free { (New-Object IO.DriveInfo $Drive).AvailableFreeSpace }
$failed = 0

"-- reuse: B written right after A's delete"
for ($r = 0; $r -lt $Rounds; $r++) {
    $a = [Extents]::Write("$root\a$r", $FileMB, $true)        # written and committed (fsync)
    [IO.File]::Delete("$root\a$r")
    # B's blocks are taken by its writes and flush, before the flush commits the delete
    $b = [Extents]::Write("$root\b$r", $FileMB, $true)
    $shared = [Extents]::Shared($a, $b)
    "   round ${r}: A $($a.Count) extent(s), B $($b.Count), shared clusters $shared"
    if ($shared -ne 0) { $failed++ }
    [IO.File]::Delete("$root\b$r")
}

# a commit of everything so far: fsync of a file of its own
function Commit { $h = [IO.File]::Open("$root\sync", 'Create', 'ReadWrite', 'None'); $h.Flush($true); $h.Dispose(); [IO.File]::Delete("$root\sync") }

# a nearly full volume: room for one file of ReplaceMB and a little
$room = [long]((Free) / 1MB) - $ReplaceMB - 16
$c = New-Object byte[] (1MB)
$f = New-Object IO.FileStream("$root\filler", 'CreateNew', 'Write', 'None', 1MB)
for ($i = 0; $i -lt $room; $i++) { $f.Write($c, 0, $c.Length) }
$f.Flush($true); $f.Dispose()
"   nearly full: $([long]((Free) / 1MB)) MB free"

"-- returns: after the delete commits, its blocks are reused"
$before = Free
[void][Extents]::Write("$root\old1", $ReplaceMB, $true)
[IO.File]::Delete("$root\old1")
Commit
try {
    [void][Extents]::Write("$root\new1", $ReplaceMB, $true)
    "   a file as large written after the commit"
} catch {
    "   FAIL after the commit: $($_.Exception.Message)"
    $failed++
}
[IO.File]::Delete("$root\new1")
Commit
$after = Free
"   free before $before, after $after"
if ($after -ne $before) { $failed++ }

"-- replace: delete and write as much at once"
[void][Extents]::Write("$root\old2", $ReplaceMB, $true)
[IO.File]::Delete("$root\old2")
try {
    [void][Extents]::Write("$root\new2", $ReplaceMB, $true)
    "   the new file of $ReplaceMB MB written at once"
} catch {
    "   FAIL the new file: $($_.Exception.Message)"
    $failed++
}

[IO.Directory]::Delete($root, $true)
"FREED-REUSE: $failed failed"
exit $failed
