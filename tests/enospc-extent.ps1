# SPDX-License-Identifier: GPL-2.0-only
# Destructive regression test for a disposable, freshly formatted 4 KiB ext4 fixture.
# Forces a fifth extent with space for data but none for the new extent-tree node.
# Leaves the files in place for an independent e2fsck -fn inspection.
param([Parameter(Mandatory)][string]$Drive, [switch]$ReuseHole)
$ErrorActionPreference = 'Stop'
$volume = Get-Volume -DriveLetter $Drive
if ($volume.FileSystemLabel -ne 'test4k' -or $volume.Size -gt 1GB -or $volume.Size -lt 128MB) {
    throw 'Use only the disposable test4k fixture (128 MiB to 1 GiB).'
}
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class ExtentEnospc {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(SafeFileHandle file, IntPtr buffer, uint count, out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetFilePointerEx(SafeFileHandle file, long offset, out long position, uint method);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetEndOfFile(SafeFileHandle file);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetFileSizeEx(SafeFileHandle file, out long size);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool GetDiskFreeSpaceEx(string root, out ulong available, out ulong total, out ulong free);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool VirtualFree(IntPtr address, UIntPtr size, uint type);
    static SafeFileHandle Open(string name) {
        var h=CreateFile(name,0xC0000000,7,IntPtr.Zero,2,0xA0000080,IntPtr.Zero);
        if(h.IsInvalid) { h.Dispose(); throw new Win32Exception(); }
        return h;
    }
    static ulong Free(string root) {
        ulong available,total,free;
        if(!GetDiskFreeSpaceEx(root,out available,out total,out free)) throw new Win32Exception();
        return free;
    }
    static void Write(SafeFileHandle h, IntPtr buffer, uint count) {
        uint written;
        if(!WriteFile(h,buffer,count,out written,IntPtr.Zero)) throw new Win32Exception();
        if(written!=count) throw new IOException("Short write");
    }
    public static void Run(string root, bool reuseHole) {
        string dir=Path.Combine(root,"enospc-extent");
        if(Directory.Exists(dir)) throw new IOException("Fixture directory already exists; format the test disk first.");
        Directory.CreateDirectory(dir);
        IntPtr buffer=VirtualAlloc(IntPtr.Zero,(UIntPtr)(1<<20),0x3000,4);
        if(buffer==IntPtr.Zero) throw new Win32Exception();
        try {
            var pattern=new byte[1<<20];
            new Random(7).NextBytes(pattern);
            Marshal.Copy(pattern,0,buffer,pattern.Length);
            using(var target=Open(Path.Combine(dir,"target"))) {
                for(int i=0;i<4;i++) {
                    Write(target,buffer,4096);
                    using(var gap=Open(Path.Combine(dir,"gap"+i))) Write(gap,buffer,4096);
                }
                for(int i=0;i<64;i++) using(var reserve=Open(Path.Combine(dir,"reserve"+i))) {}
                using(var filler=Open(Path.Combine(dir,"filler"))) {
                    while(Free(root)>65536) {
                        ulong count=Math.Min(1UL<<20,(Free(root)-65536)&~4095UL);
                        if(count==0) break;
                        Write(filler,buffer,(uint)count);
                    }
                }
                ulong leave=reuseHole?0UL:4096UL;
                for(int i=0;Free(root)>leave && i<64;i++) {
                    using(var reserve=Open(Path.Combine(dir,"reserve"+i))) Write(reserve,buffer,4096);
                }
                if(reuseHole) {
                    if(Free(root)!=0) throw new IOException("Could not fill the fixture");
                    File.Delete(Path.Combine(dir,"gap0"));
                }
                Console.WriteLine("Free before growth: {0} bytes",Free(root));
                if(Free(root)!=4096) throw new IOException("Could not leave exactly one free block");
                long position;
                if(!SetFilePointerEx(target,20480,out position,0)) throw new Win32Exception();
                bool grew=SetEndOfFile(target);
                int error=grew?0:Marshal.GetLastWin32Error();
                long size;
                if(!GetFileSizeEx(target,out size)) throw new Win32Exception();
                ulong after=Free(root);
                Console.WriteLine("Growth success={0}, Win32 error={1}, length={2}, free after={3}",grew,error,size,after);
                if(grew || error!=112) throw new IOException("Expected ERROR_DISK_FULL while growing the extent tree");
                if(size!=16384 || after!=4096) throw new IOException("Failed growth changed the file or free-block count");
                byte[] actual;
                using(var read=new FileStream(Path.Combine(dir,"target"),FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete)) {
                    actual=new byte[read.Length];
                    int done=0;
                    while(done<actual.Length) {
                        int got=read.Read(actual,done,actual.Length-done);
                        if(got==0) throw new IOException("Short read-back");
                        done+=got;
                    }
                }
                if(actual.Length!=16384) throw new IOException("Read-back length changed");
                for(int i=0;i<actual.Length;i++) if(actual[i]!=pattern[i%4096]) throw new IOException("Read-back data changed at "+i);
                Console.WriteLine("ENOSPC-EXTENT: rollback preserved the file and free-block count; run e2fsck next.");
            }
        } finally { VirtualFree(buffer,UIntPtr.Zero,0x8000); }
    }
}
'@
[ExtentEnospc]::Run("${Drive}:\", $ReuseHole.IsPresent)
