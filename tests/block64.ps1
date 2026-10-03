# SPDX-License-Identifier: GPL-2.0-only
# Validate high physical addresses through both cached and unbuffered Win32 I/O.
param([Parameter(Mandatory=$true)][string]$Root,
    [ValidateSet('All','Prepare','Verify','Finish','FinalVerify','Helpers')][string]$Phase = 'All')
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Block64Io {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(SafeFileHandle file, IntPtr data, uint length,
        out uint done, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(SafeFileHandle file, IntPtr data, uint length,
        out uint done, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetFilePointerEx(SafeFileHandle file, long offset,
        out long result, uint method);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle file, uint code,
        byte[] input, uint inputSize, byte[] output, uint outputSize,
        out uint returned, IntPtr overlapped);
    [StructLayout(LayoutKind.Sequential)] struct IOSB { public IntPtr Status, Information; }
    [DllImport("ntdll.dll")] static extern int NtSetEaFile(SafeFileHandle file,
        out IOSB status, byte[] data, uint length);
    [DllImport("ntdll.dll")] static extern int NtQueryEaFile(SafeFileHandle file,
        out IOSB status, byte[] data, uint length, bool single, IntPtr list,
        uint listLength, IntPtr index, bool restart);
    public static void SetEa(string path, byte[] value) {
        byte[] name = System.Text.Encoding.ASCII.GetBytes("BLOCK64");
        byte[] data = new byte[9+name.Length+value.Length];
        data[5] = (byte)name.Length;
        BitConverter.GetBytes((ushort)value.Length).CopyTo(data,6);
        name.CopyTo(data,8); value.CopyTo(data,9+name.Length);
        using (var file = CreateFile(path,0x10,7,IntPtr.Zero,3,0,IntPtr.Zero)) {
            if (file.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            IOSB status; int result = NtSetEaFile(file,out status,data,(uint)data.Length);
            if (result < 0) throw new IOException("NtSetEaFile: " + result.ToString("X8"));
        }
    }
    public static byte[] GetEa(string path) {
        byte[] data = new byte[65536]; uint length;
        using (var file = CreateFile(path,0x08,7,IntPtr.Zero,3,0,IntPtr.Zero)) {
            if (file.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            IOSB status; int result = NtQueryEaFile(file,out status,data,(uint)data.Length,
                false,IntPtr.Zero,0,IntPtr.Zero,true);
            if (result < 0) throw new IOException("NtQueryEaFile: " + result.ToString("X8"));
            length = (uint)status.Information.ToInt64();
        }
        for (int offset=0; offset+8<=length;) {
            int next=BitConverter.ToInt32(data,offset), nameLength=data[offset+5];
            int valueLength=BitConverter.ToUInt16(data,offset+6);
            if (offset+9+nameLength+valueLength>length) throw new IOException("Invalid EA record");
            if (System.Text.Encoding.ASCII.GetString(data,offset+8,nameLength)=="BLOCK64") {
                byte[] value = new byte[valueLength];
                Array.Copy(data,offset+9+nameLength,value,0,valueLength); return value;
            }
            if (next==0) break;
            if (next<8 || next>length-offset) throw new IOException("Invalid EA next offset");
            offset+=next;
        }
        throw new IOException("BLOCK64 EA missing");
    }
    static void Check(bool value) {
        if (!value) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public static byte[] Unbuffered(string path, long offset, byte[] write) {
        using (var file = CreateFile(path, write == null ? 0x80000000u : 0xC0000000u,
                                    3, IntPtr.Zero, 3, 0xA0000000u, IntPtr.Zero)) {
            if (file.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            IntPtr memory = Marshal.AllocHGlobal(8192);
            IntPtr aligned = new IntPtr((memory.ToInt64()+4095)&~4095L);
            try {
                long position; uint done;
                Check(SetFilePointerEx(file, offset, out position, 0));
                if (write != null) {
                    if (write.Length != 4096) throw new ArgumentException("One aligned block required");
                    Marshal.Copy(write, 0, aligned, 4096);
                    Check(WriteFile(file, aligned, 4096, out done, IntPtr.Zero));
                } else {
                    Check(ReadFile(file, aligned, 4096, out done, IntPtr.Zero));
                }
                if (done != 4096) throw new IOException("Short unbuffered transfer");
                byte[] data = new byte[4096];
                Marshal.Copy(aligned, data, 0, 4096);
                return data;
            } finally { Marshal.FreeHGlobal(memory); }
        }
    }
    public static long[] PhysicalRuns(string path) {
        using (var file = CreateFile(path, 0x80000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (file.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] output = new byte[65536]; uint returned;
            Check(DeviceIoControl(file, 0x00090073, new byte[8], 8,
                                  output, (uint)output.Length, out returned, IntPtr.Zero));
            if (returned < 16) throw new IOException("Short retrieval header");
            int count = BitConverter.ToInt32(output, 0);
            if (count < 1 || count > (returned-16)/16) throw new IOException("Invalid retrieval count");
            long[] blocks = new long[count];
            for (int i=0; i<count; ++i) blocks[i] = BitConverter.ToInt64(output, 24+i*16);
            return blocks;
        }
    }
}
'@
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function EqualBytes([byte[]]$Actual, [byte[]]$Expected, [string]$Message) {
    Assert ($Actual.Length -eq $Expected.Length) "$Message length"
    for ($i=0; $i -lt $Expected.Length; $i++) {
        if ($Actual[$i] -ne $Expected[$i]) { throw "$Message at byte $i" }
    }
}
$rootPath = [IO.Path]::GetFullPath($Root)
$ready = [Diagnostics.Stopwatch]::StartNew()
while (-not [IO.Directory]::Exists($rootPath) -and $ready.Elapsed.TotalSeconds -lt 30) {
    Start-Sleep -Milliseconds 100
}
Assert ([IO.Directory]::Exists($rootPath)) 'Test volume did not become ready'
$seed = Join-Path $rootPath 'seed64.bin'
$release = Join-Path $rootPath 'release64.bin'
if ($Phase -eq 'Helpers') { return }
if ($Phase -eq 'FinalVerify') {
    $expected = [byte[]]::new(32*4096)
    for ($i=0; $i -lt 1000; $i++) { $expected[$i] = ($i*17) -band 255 }
    EqualBytes ([IO.File]::ReadAllBytes($seed)) $expected 'Final high-address proof'
    for ($logical=0; $logical -lt 32; $logical++) {
        $part = [byte[]]::new(4096)
        [Array]::Copy($expected,$logical*4096,$part,0,4096)
        EqualBytes ([Block64Io]::Unbuffered($seed,$logical*4096,$null)) $part 'Final unbuffered proof'
    }
    Assert (@([Block64Io]::PhysicalRuns($seed) | Where-Object { $_ -gt [uint32]::MaxValue }).Count -gt 0) 'Final proof lost its high physical block'
    Assert (-not [IO.File]::Exists($release)) 'Freed fixture returned after reload'
    'BLOCK64: final persisted data, zero tail and high mapping: ALL PASSED'
    return
}
if ($Phase -in 'All','Prepare') {
Assert ((Get-Item -LiteralPath $seed).Length -eq 4096) 'Fresh dedicated fixture required'
foreach ($item in @(@($seed,0), @($release,1))) {
    $expected = [byte[]]::new(4096)
    for ($i=0; $i -lt 4096; $i++) { $expected[$i] = ($i*17 + $item[1]*31) -band 255 }
    EqualBytes ([IO.File]::ReadAllBytes($item[0])) $expected 'Cached high block read'
    EqualBytes ([Block64Io]::Unbuffered($item[0],0,$null)) $expected 'Unbuffered high block read'
    $runs = [Block64Io]::PhysicalRuns($item[0])
    Assert (@($runs | Where-Object { $_ -gt [uint32]::MaxValue }).Count -gt 0) 'No physical address above 32 bits'
}
'BLOCK64: cached and unbuffered high-address reads passed'

$stream = [IO.File]::Open($seed,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
try { $stream.SetLength(32*4096); $stream.Flush($true) } finally { $stream.Dispose() }
for ($logical=2; $logical -lt 20; $logical+=2) {
    $pattern = [byte[]]::new(4096)
    for ($i=0; $i -lt 4096; $i++) { $pattern[$i] = ($i*13 + $logical) -band 255 }
    [void][Block64Io]::Unbuffered($seed,($logical*4096),$pattern)
    EqualBytes ([Block64Io]::Unbuffered($seed,($logical*4096),$null)) $pattern 'High-address allocation read-back'
}
$runs = [Block64Io]::PhysicalRuns($seed)
Assert ($runs.Count -ge 5) 'Extent tree did not grow beyond the inode root'
Assert (@($runs | Where-Object { $_ -gt [uint32]::MaxValue }).Count -ge 5) 'High-address allocation was not exercised'
"BLOCK64: allocated high extents, retrieval runs=$($runs.Count)"
}

if ($Phase -in 'All','Prepare','Verify','Finish') {
    $cached = [IO.File]::ReadAllBytes($seed)
    Assert ($cached.Length -eq 32*4096) 'Fragmented fixture size'
    for ($logical=0; $logical -lt 32; $logical++) {
        $expected = [byte[]]::new(4096)
        for ($i=0; $i -lt 4096; $i++) {
            if ($logical -eq 0) { $expected[$i] = ($i*17) -band 255 }
            elseif ($logical -ge 2 -and $logical -lt 20 -and $logical%2 -eq 0) {
                $expected[$i] = ($i*13 + $logical) -band 255
            }
        }
        $part = [byte[]]::new(4096)
        [Array]::Copy($cached,$logical*4096,$part,0,4096)
        EqualBytes $part $expected 'Cached fragmented high-address read'
        EqualBytes ([Block64Io]::Unbuffered($seed,($logical*4096),$null)) $expected 'Unbuffered fragmented high-address read'
    }
    $runs = [Block64Io]::PhysicalRuns($seed)
    Assert ($runs.Count -ge 5) 'Fragmented tree was not preserved'
    Assert (@($runs | Where-Object { $_ -gt [uint32]::MaxValue }).Count -ge 5) 'High runs were not preserved'
    'BLOCK64: fragmented high-address tree verified'
}
if ($Phase -in 'Prepare','Verify') { return }

[IO.File]::Delete($release)
$stream = [IO.File]::Open($seed,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
try { $stream.SetLength(1000); $stream.Flush($true); $stream.SetLength(32*4096); $stream.Flush($true) }
finally { $stream.Dispose() }
$data = [IO.File]::ReadAllBytes($seed)
for ($i=0; $i -lt $data.Length; $i++) {
    $expected = if ($i -lt 1000) { ($i*17) -band 255 } else { 0 }
    if ($data[$i] -ne $expected) { throw "Truncate/regrow mismatch at $i" }
}
$zero = [byte[]]::new(4096)
for ($logical=1; $logical -lt 32; $logical++) {
    EqualBytes ([Block64Io]::Unbuffered($seed,($logical*4096),$null)) $zero 'Unbuffered zero regrowth'
}
'BLOCK64: high-address truncate, free and zero regrowth passed'
$renamed = Join-Path $rootPath 'seed64-renamed.bin'
[IO.File]::Move($seed,$renamed)
[IO.File]::Move($renamed,$seed)
$hash = (Get-FileHash -LiteralPath $seed -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $rootPath 'block64.sha256'), "$hash  seed64.bin`n", [Text.Encoding]::ASCII)
'BLOCK64: ALL PASSED'
