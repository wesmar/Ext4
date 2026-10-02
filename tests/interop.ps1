# SPDX-License-Identifier: GPL-2.0-only
# interop.ps1 - Windows side of the metadata interoperability test (guest).
#
# Reads what interop.sh prepare built in \interop-linux (symlinks of every
# kind, hard links, read-only mode, FIFO, nanosecond times, user xattrs),
# then creates \interop-win and changes a few Linux objects the way ordinary
# programs do; interop.sh check verifies the result from Linux.
#
#   powershell -File interop.ps1 [-Drives D,E]
param([string]$Drives = 'D,E')
$ErrorActionPreference = 'Stop'
# powershell -File passes 'D,E' as one string: split it here
$DriveList = @($Drives -split '[,\s]+' | Where-Object { $_ })
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Text; using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class IO2 {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool CreateSymbolicLink(string link, string target, int flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool CreateHardLink(string link, string existing, IntPtr sa);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern SafeFileHandle CreateFile(string p, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
    [StructLayout(LayoutKind.Sequential, Pack = 4)] struct BHFI { public uint attr; public long ct, at, wt; public uint vsn, sizeHigh, sizeLow, links, idxHigh, idxLow; }
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetFileInformationByHandle(SafeFileHandle h, out BHFI i);
    [StructLayout(LayoutKind.Sequential)] struct IOSB { public IntPtr S; public IntPtr I; }
    [DllImport("ntdll.dll")] static extern int NtSetEaFile(SafeFileHandle h, out IOSB io, byte[] b, uint l);
    [DllImport("ntdll.dll")] static extern int NtQueryEaFile(SafeFileHandle h, out IOSB io, byte[] b, uint l, bool single, IntPtr list, uint ll, IntPtr idx, bool restart);
    static SafeFileHandle Open(string p, uint access) {
        var h = CreateFile(p, access, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return h;
    }
    public static uint Links(string p) { using (var h = Open(p, 0)) { BHFI i; GetFileInformationByHandle(h, out i); return i.links; } }
    public static int SetEa(string p, string name, string value) {
        byte[] n = Encoding.ASCII.GetBytes(name), v = Encoding.ASCII.GetBytes(value);
        var b = new byte[8 + n.Length + 1 + v.Length];
        b[5] = (byte)n.Length; BitConverter.GetBytes((ushort)v.Length).CopyTo(b, 6); n.CopyTo(b, 8); v.CopyTo(b, 9 + n.Length);
        using (var h = Open(p, 0x10)) { IOSB io; return NtSetEaFile(h, out io, b, (uint)b.Length); }
    }
    public static string GetEa(string p, string name) {
        var b = new byte[65536];
        using (var h = Open(p, 0x08)) { IOSB io; if (NtQueryEaFile(h, out io, b, (uint)b.Length, false, IntPtr.Zero, 0, IntPtr.Zero, true) < 0) return null; }
        for (int o = 0; ; ) {
            int next = BitConverter.ToInt32(b, o), nl = b[o + 5], vl = BitConverter.ToUInt16(b, o + 6);
            if (string.Equals(Encoding.ASCII.GetString(b, o + 8, nl), name, StringComparison.OrdinalIgnoreCase)) return Encoding.ASCII.GetString(b, o + 9 + nl, vl);
            if (next == 0) return null;
            o += next;
        }
    }
}
"@
$fail = 0
function Check($name, $expected, $actual) {
    if ("$expected" -ceq "$actual") { "  ok   $name" } else { "  FAIL $name`: expected '$expected', got '$actual'"; $script:fail++ }
}
function Try([scriptblock]$b) { try { & $b } catch { "ERR: $($_.Exception.InnerException.Message)$($_.Exception.Message)".Trim() } }

foreach ($d in $DriveList) {
    $root = "$d`:\"
    for ($i = 0; $i -lt 100 -and -not [IO.Directory]::Exists($root); $i++) { Start-Sleep -Milliseconds 100 }
    $L = "$root" + 'interop-linux'; $W = "$root" + 'interop-win'
    if (-not [IO.Directory]::Exists($L)) { "== $d`: no interop-linux, skipped"; continue }
    "== $d`: reading what Linux made"
    $expect = @{}; Get-Content "$L\expect.txt" | ForEach-Object { $f = $_ -split ' '; $expect[$f[0]] = $f[1..($f.Count - 1)] }

    Check 'relative symlink resolves'      "target data"     (Try { [IO.File]::ReadAllText("$L\rel-link").Trim() })
    Check 'deep relative symlink resolves' "deep data"       (Try { [IO.File]::ReadAllText("$L\rel-deep-link").Trim() })
    Check 'symlink chain resolves'         "target data"     (Try { [IO.File]::ReadAllText("$L\chain-link").Trim() })
    Check 'symlink with .. resolves'       "target data"     (Try { [IO.File]::ReadAllText("$L\up-link").Trim() })
    Check 'directory symlink lists'        "deep,hard3"      (Try { ([IO.Directory]::GetFileSystemEntries("$L\dir-link") | ForEach-Object { Split-Path $_ -Leaf } | Sort-Object) -join ',' })
    Check 'directory symlink is a reparse point' $true       (Try { ((Get-Item -LiteralPath "$L\dir-link" -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 })
    Check 'dangling symlink cannot be read' $true            ((Try { [IO.File]::ReadAllText("$L\dangling-link") }) -like 'ERR:*')
    Check 'hard link count 3'              "3"               (Try { [IO2]::Links("$L\hard1") })
    Check 'mode 0444 is read-only'         $true             (Try { ((Get-Item -LiteralPath "$L\m444").Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0 })
    Check 'mode 0644 is writable'          $true             (Try { ((Get-Item -LiteralPath "$L\m644").Attributes -band [IO.FileAttributes]::ReadOnly) -eq 0 })
    Check 'FIFO listed'                    $true             (Try { [IO.File]::Exists("$L\fifo") -or [IO.Directory]::Exists("$L\fifo") -or ((Get-ChildItem -LiteralPath $L -Force | Where-Object Name -eq 'fifo') -ne $null) })
    Check 'FIFO reads as an empty file'    "0"               (Try { [IO.File]::ReadAllBytes("$L\fifo").Length })
    Check 'FIFO refuses data written'      $true             ((Try { [IO.File]::AppendAllText("$L\fifo", 'x') }) -like 'ERR:*')
    $mt = [DateTime]::SpecifyKind([DateTime]'1970-01-01', 'Utc').AddSeconds([long]$expect['mtime_timed'][0]).AddTicks([Math]::Floor([long]$expect['mtime_timed'][1] / 100))
    Check 'nanosecond mtime (100 ns)'      $mt.Ticks         (Try { [IO.File]::GetLastWriteTimeUtc("$L\timed.txt").Ticks })
    Check 'Linux user xattr as EA'         "blue"            (Try { [IO2]::GetEa("$L\xattr.txt", 'color') })
    Check 'size of a Linux file'           $expect['size_target'][0] (Try { (Get-Item -LiteralPath "$L\target.txt").Length })

    "== $d`: making \interop-win and changing Linux objects"
    if ([IO.Directory]::Exists($W)) { cmd /c "rd /s /q $W" }
    [void][IO.Directory]::CreateDirectory("$W\dir")
    [IO.File]::WriteAllText("$W\file.txt", "from windows`n")
    [IO.File]::WriteAllText("$W\dir\inner.txt", "inner`n")
    [IO.File]::WriteAllText("$W\readonly.txt", "ro`n"); [IO.File]::SetAttributes("$W\readonly.txt", 'ReadOnly')
    Check 'create relative symlink'        $true             ([IO2]::CreateSymbolicLink("$W\rel-link", 'file.txt', 0))
    Check 'create directory symlink'       $true             ([IO2]::CreateSymbolicLink("$W\dir-link", 'dir', 1))
    Check 'create deep relative symlink'   $true             ([IO2]::CreateSymbolicLink("$W\deep-link", 'dir\inner.txt', 0))
    Check 'read through new symlink'       "from windows"    (Try { [IO.File]::ReadAllText("$W\rel-link").Trim() })
    Check 'create hard link'               $true             ([IO2]::CreateHardLink("$W\hard.txt", "$W\file.txt", [IntPtr]::Zero))
    [IO.File]::WriteAllText("$W\timed.txt", "t`n")
    [IO.File]::SetLastWriteTimeUtc("$W\timed.txt", [DateTime]::SpecifyKind([DateTime]'1970-01-01', 'Utc').AddSeconds(1600000000).AddTicks(1234567))
    Check 'EA on a new file'               0                 ([IO2]::SetEa("$W\file.txt", 'WINEA', 'hello'))
    [void][IO.Directory]::CreateDirectory("$L\setgid\windir")
    [IO.File]::WriteAllText("$L\setgid\winfile.txt", "in setgid`n")
    [IO.File]::WriteAllText("$L\owned.txt", "owned+windows`n")
    [IO.File]::AppendAllText("$L\m4755", "appended`n")
    Check 'EA next to a Linux xattr'       0                 ([IO2]::SetEa("$L\xattr.txt", 'WINEA', 'x'))
    [IO.File]::Move("$L\target.txt", "$L\renamed.txt")
    [IO.File]::Delete("$L\sub\hard3")
}
"INTEROP-WIN: $fail failed"
