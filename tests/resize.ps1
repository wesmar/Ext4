# SPDX-License-Identifier: GPL-2.0-only
# resize.ps1 - reopen, shrink and regrow files across sector/block boundaries.
param([Parameter(Mandatory)][string]$Root, [ValidateRange(1, 1000)][int]$Repeat = 10)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class ResizeProbe {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint allocation, uint protection);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool VirtualFree(IntPtr address, UIntPtr size, uint freeType);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(SafeFileHandle handle, IntPtr buffer, uint count, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle handle, int infoClass, ref long size, uint length);
    static void Resize(FileStream f, long length, bool unbuffered) {
        // FileStream.SetLength seeks first, which requires sector alignment
        // on an unbuffered handle. Set EOF directly to test arbitrary sizes.
        if (!unbuffered) { f.SetLength(length); return; }
        if (!SetFileInformationByHandle(f.SafeFileHandle, 6, ref length, 8))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public static void Run(string path, int small, int large, bool readBefore, bool shrinkUnbuffered, bool growUnbuffered) {
        string step = "write";
        try {
        byte[] data = new byte[300000];
        new Random(9143).NextBytes(data);
        File.WriteAllBytes(path, data);
        step = "shrink";
        using (FileStream f = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None, 4096,
                                           shrinkUnbuffered ? (FileOptions)0x20000000 : FileOptions.None))
            Resize(f, small, shrinkUnbuffered);
        if (readBefore) Verify(path, data, small, small);
        step = "grow";
        using (FileStream f = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None, 4096,
                                           growUnbuffered ? (FileOptions)0x20000000 : FileOptions.None))
            Resize(f, large, growUnbuffered);
        step = "verify"; Verify(path, data, small, large);
        step = "truncate zero";
        using (FileStream f = File.Open(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
            f.SetLength(0);
        if (new FileInfo(path).Length != 0) throw new IOException("truncate to zero failed");
        File.Delete(path);
        } catch (Exception e) {
            throw new IOException(step + ": " + e.ToString(), e);
        }
    }
    static void Verify(string path, byte[] source, int prefix, int length) {
        byte[] actual = File.ReadAllBytes(path);
        if (actual.Length != length) throw new IOException("unexpected length " + actual.Length);
        for (int i = 0; i < actual.Length; i++) {
            byte expected = i < prefix ? source[i] : (byte)0;
            if (actual[i] != expected) {
                byte direct = ReadDirectByte(path, i, length);
                throw new IOException("data mismatch at " + i + ": cached=" + actual[i] + " direct=" + direct + " expected=" + expected);
            }
        }
    }
    static byte ReadDirectByte(string path, int offset, int length) {
        uint count = (uint)((length + 4095) & ~4095);
        IntPtr buffer = VirtualAlloc(IntPtr.Zero, (UIntPtr)count, 0x3000, 4);
        if (buffer == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            using (SafeFileHandle h = CreateFile(path, 0x80000000, 7, IntPtr.Zero, 3, 0x20000000, IntPtr.Zero)) {
                uint read;
                if (h.IsInvalid || !ReadFile(h, buffer, count, out read, IntPtr.Zero))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                if (read != length) throw new IOException("direct read length " + read);
                return Marshal.ReadByte(buffer, offset);
            }
        } finally { VirtualFree(buffer, UIntPtr.Zero, 0x8000); }
    }
}
'@
if (-not [IO.Directory]::Exists($Root)) { throw "Root does not exist: $Root" }
$rootPath = [IO.Path]::GetFullPath($Root)
$prefix = $rootPath.TrimEnd('\') + '\'
$dir = [IO.Path]::GetFullPath((Join-Path $rootPath ('resize-' + [guid]::NewGuid().ToString('N'))))
if (-not $dir.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test directory escaped Root.' }
if ([IO.Directory]::Exists($dir)) { throw "Test directory already exists: $dir" }
[void][IO.Directory]::CreateDirectory($dir)
$fail = 0; $count = 0
try {
    for ($iteration = 0; $iteration -lt $Repeat; $iteration++) {
        foreach ($sizes in @(@(100000, 250000), @(511, 8193), @(4095, 8305), @(4097, 8201), @(65535, 250000),
                             @(511, 512), @(4095, 4096), @(4096, 8192), @(4097, 8192), @(100000, 100001))) {
            foreach ($mode in 0..7) {
                $readBefore = [bool]($mode -band 1)
                $shrinkUnbuffered = [bool]($mode -band 2)
                $growUnbuffered = [bool]($mode -band 4)
                $count++
                $path = Join-Path $dir ("case-$count.bin")
                try { [ResizeProbe]::Run($path, $sizes[0], $sizes[1], $readBefore, $shrinkUnbuffered, $growUnbuffered) }
                catch {
                    $fail++
                    "FAIL case=$count shrink=$($sizes[0]) grow=$($sizes[1]) preread=$readBefore nc-shrink=$shrinkUnbuffered nc-grow=$growUnbuffered : $($_.Exception.Message)"
                }
            }
        }
    }
} finally {
    if ([IO.Path]::GetFullPath($dir) -ne $dir -or
        -not $dir.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid cleanup target.' }
    [IO.Directory]::Delete($dir, $true)
}
"RESIZE: $count cases, $fail failed"
exit ([int]($fail -ne 0))
