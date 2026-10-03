# SPDX-License-Identifier: GPL-2.0-only
# Reject logical-size overflow without modifying existing file data (guest).
param([Parameter(Mandatory=$true)][string]$Root,
    [ValidateSet(1024,2048,4096)][int]$BlockSize = 4096)
$ErrorActionPreference = 'Stop'
if ((Get-Volume -DriveLetter ([IO.Path]::GetFullPath($Root).Substring(0,1))).FileSystem -notmatch '^EXT[234]$') {
    throw 'An ext test volume is required'
}
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class FileLimitProbe {
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle file, int kind,
        byte[] information, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetFilePointerEx(SafeFileHandle file, long offset, out long position, uint origin);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(SafeFileHandle file, byte[] data, uint size, out uint written, IntPtr overlapped);
    public static int SetSize(SafeFileHandle file, int kind, long size) {
        bool result = SetFileInformationByHandle(file,kind,BitConverter.GetBytes(size),8);
        return result ? 0 : Marshal.GetLastWin32Error();
    }
    public static int WriteAt(SafeFileHandle file, long offset) {
        long position; uint written;
        if (!SetFilePointerEx(file,offset,out position,0)) return Marshal.GetLastWin32Error();
        return WriteFile(file,new byte[]{1},1,out written,IntPtr.Zero) ? 0 : Marshal.GetLastWin32Error();
    }
}
'@
$path = Join-Path $Root 'logical-limit-proof.bin'
if ([IO.File]::Exists($path)) { throw 'Existing proof file; refusing overwrite' }
[IO.File]::WriteAllBytes($path,[byte[]](11,29,71,173))
$limit = [long][uint32]::MaxValue * $BlockSize
$stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
try {
    foreach ($kind in 5,6) {
        foreach ($size in ($limit+1),[long]::MaxValue) {
            $errorCode = [FileLimitProbe]::SetSize($stream.SafeFileHandle,$kind,$size)
            if ($errorCode -ne 223) { throw "Size $size kind $kind returned $errorCode, expected ERROR_FILE_TOO_LARGE" }
        }
        if ([FileLimitProbe]::SetSize($stream.SafeFileHandle,$kind,-1) -ne 87) {
            throw "Negative size kind $kind was not rejected"
        }
    }
    if ([FileLimitProbe]::WriteAt($stream.SafeFileHandle,$limit+4096) -ne 223) {
        throw 'Out-of-range native write was not rejected as ERROR_FILE_TOO_LARGE'
    }
} finally { $stream.Dispose() }
if (([IO.File]::ReadAllBytes($path) -join ',') -ne '11,29,71,173') { throw 'Proof data changed' }
[IO.File]::Delete($path)
'FILE LIMITS: oversize EOF, allocation, write and negative sizes: ALL PASSED'
