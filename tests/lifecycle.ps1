# SPDX-License-Identifier: GPL-2.0-only
# Repeated unload/reload with persistent data checked after every cycle (guest).
# -Remount keeps a file open across each stop, so the stop dismounts a volume
# still in use, and opens the volumes through their device names meanwhile:
# the dismounted volume refuses them, and once it is gone every open asks each
# file system in turn to mount the device.
# -Dismount (with -Remount) first dismounts each volume by FSCTL with that
# file open, so the device carries a fresh VPB of the driver's when it stops,
# and half the openers open the devices themselves, nameless: the open RAW
# would mount if the driver let it.
param([Parameter(Mandatory=$true)][string]$Roots,
    [ValidateRange(1,1000)][int]$Cycles = 10, [string]$Service = 'ext4', [switch]$Churn,
    [switch]$Remount, [switch]$Dismount)
if ($Dismount -and -not $Remount) { throw '-Dismount needs -Remount' }
$ErrorActionPreference = 'Stop'
$paths = @($Roots -split ',' | ForEach-Object { [IO.Path]::GetFullPath($_) })
foreach ($root in $paths) {
    $volume = Get-Volume -DriveLetter $root.Substring(0,1)
    if ($volume.FileSystem -notmatch '^EXT[234]$') { throw "Not an ext test volume: $root" }
    $path = Join-Path $root 'lifecycle-proof.bin'
    if ([IO.File]::Exists($path)) { throw "Existing test file: $path" }
    [IO.File]::WriteAllBytes($path,[byte[]](1,5,13,29,61,125,253))
}
$worst = 0
if ($Churn) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;
public sealed class LifecycleChurn {
    readonly ManualResetEvent stop = new ManualResetEvent(false);
    readonly Thread[] workers;
    Exception failure;
    long reads, unavailable;
    public LifecycleChurn(string[] roots) {
        workers = new Thread[4];
        for (int i=0; i<workers.Length; i++) {
            string path = Path.Combine(roots[i % roots.Length], "lifecycle-proof.bin");
            workers[i] = new Thread(() => Run(path));
            workers[i].Start();
        }
    }
    void Run(string path) {
        byte[] expected = {1,5,13,29,61,125,253};
        while (!stop.WaitOne(1)) {
            try {
                using (var file = new FileStream(path,FileMode.Open,FileAccess.Read,
                        FileShare.ReadWrite | FileShare.Delete,1)) {
                    byte[] data = new byte[expected.Length];
                    int count=0;
                    while (count<data.Length) {
                        int got=file.Read(data,count,data.Length-count);
                        if (got==0) throw new InvalidDataException("Short lifecycle proof read");
                        count+=got;
                    }
                    for (int i=0; i<data.Length; i++)
                        if (data[i]!=expected[i]) throw new InvalidDataException("Lifecycle proof changed");
                    if (file.ReadByte()!=-1) throw new InvalidDataException("Lifecycle proof grew");
                }
                Interlocked.Increment(ref reads);
            } catch (IOException e) {
                // Withdrawal can fail an in-flight open/read, never alter its data.
                int code=e.HResult & 0xffff;
                if (code==2 || code==3 || code==6 || code==21 || code==31 ||
                    code==55 || code==995 || code==1167 || code==1006) {
                    Interlocked.Increment(ref unavailable);
                } else {
                    Interlocked.CompareExchange(ref failure,e,null);
                    stop.Set();
                }
            } catch (Exception e) {
                Interlocked.CompareExchange(ref failure,e,null);
                stop.Set();
            }
        }
    }
    public string Finish() {
        stop.Set();
        foreach (var worker in workers)
            if (!worker.Join(30000)) throw new IOException("Lifecycle worker did not drain");
        stop.Dispose();
        if (failure!=null) throw new IOException("Lifecycle churn failed",failure);
        if (reads==0) throw new IOException("Lifecycle churn performed no successful reads");
        return String.Format("LIFECYCLE CHURN: {0} verified reads, {1} withdrawn-volume responses",reads,unavailable);
    }
}
'@
    $workers = [LifecycleChurn]::new([string[]]$paths)
}
if ($Remount) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public sealed class LifecycleRemount {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern uint QueryDosDevice(string name, StringBuilder target, int max);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa,
        uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr handle, uint code, IntPtr inBuffer, uint inSize,
        IntPtr outBuffer, uint outSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CloseHandle(IntPtr handle);
    const uint GenericRead = 0x80000000, GenericWrite = 0x40000000, ShareAll = 7, OpenExisting = 3;
    const uint FsctlDismountVolume = 0x00090020;
    // the volume device itself: the drive letter and the volume GUID name
    // both go with the volume, the device stays
    public static string DevicePath(string root, string file) {
        var target = new StringBuilder(1024);
        if (QueryDosDevice(root.Substring(0,2), target, target.Capacity) == 0)
            throw new IOException("No device behind " + root);
        return @"\\?\GLOBALROOT" + target.ToString() + @"\" + file;
    }
    // FSCTL_DISMOUNT_VOLUME, whatever is open on the volume
    public static void Dismount(string root) {
        IntPtr volume = CreateFileW(@"\\.\" + root.Substring(0,2), GenericRead | GenericWrite,
            ShareAll, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        if (volume == new IntPtr(-1)) throw new IOException("Cannot open " + root, Marshal.GetLastWin32Error());
        uint returned;
        bool done = DeviceIoControl(volume, FsctlDismountVolume, IntPtr.Zero, 0, IntPtr.Zero, 0,
            out returned, IntPtr.Zero);
        int error = Marshal.GetLastWin32Error();
        CloseHandle(volume);
        if (!done) throw new IOException("Cannot dismount " + root, error);
    }
    // a path ending in a separator is the device, opened nameless (FileStream
    // refuses devices): data access, so the I/O manager mounts it
    static void Open(string path) {
        if (!path.EndsWith(@"\")) {
            using (new FileStream(path,FileMode.Open,FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete,1)) { }
            return;
        }
        IntPtr device = CreateFileW(path.TrimEnd('\\'), GenericRead, ShareAll, IntPtr.Zero,
            OpenExisting, 0, IntPtr.Zero);
        if (device == new IntPtr(-1)) {
            int error = Marshal.GetLastWin32Error();
            throw new IOException("Cannot open " + path, unchecked((int)0x80070000) | error);
        }
        CloseHandle(device);
    }
    readonly ConcurrentDictionary<int,long> codes = new ConcurrentDictionary<int,long>();
    readonly ManualResetEvent go = new ManualResetEvent(false);
    readonly Thread[] workers;
    volatile bool done;
    Exception failure;
    long opens, refused;
    public LifecycleRemount(string[] paths) {
        workers = new Thread[4];
        for (int i=0; i<workers.Length; i++) {
            string path = paths[i % paths.Length];
            workers[i] = new Thread(() => Run(path));
            workers[i].Start();
        }
    }
    public void Resume() { go.Set(); }
    public void Pause() { go.Reset(); }
    void Run(string path) {
        while (go.WaitOne() && !done) {
            try {
                Open(path);
                Interlocked.Increment(ref opens);
            } catch (IOException e) {
                // a volume on its way out (21), no file system that knows
                // it (1005), or RAW, which a nameless open (a service's, or
                // ours with -Dismount) mounts once the volume is gone and
                // which refuses any open with a name (87)
                int code=e.HResult & 0xffff;
                if (code==2 || code==3 || code==6 || code==21 || code==31 || code==55 ||
                    code==87 || code==995 || code==1005 || code==1006 || code==1167) {
                    Interlocked.Increment(ref refused);
                    codes.AddOrUpdate(code,1,(k,n) => n+1);
                } else {
                    Interlocked.CompareExchange(ref failure,e,null);
                }
            } catch (Exception e) {
                Interlocked.CompareExchange(ref failure,e,null);
            }
        }
    }
    public string Finish() {
        done = true;
        go.Set();
        foreach (var worker in workers)
            if (!worker.Join(30000)) throw new IOException("Remount worker did not drain");
        go.Dispose();
        if (failure!=null) throw new IOException(String.Format("Remount open failed: {0} {1:X8} {2}",failure.GetType().Name,failure.HResult,failure.Message),failure);
        if (refused==0) throw new IOException("No open ever met a dismounted volume");
        return String.Format("LIFECYCLE REMOUNT: {0} opens, {1} refused ({2})",opens,refused,
            String.Join(", ",codes.OrderBy(c => c.Key).Select(c => "error " + c.Key + " x" + c.Value)));
    }
}
'@
    $devicePaths = @($paths | ForEach-Object { [LifecycleRemount]::DevicePath($_, 'lifecycle-proof.bin') })
    if ($Dismount) {
        $devicePaths += @($paths | ForEach-Object { [LifecycleRemount]::DevicePath($_, '') })
    }
    $openers = [LifecycleRemount]::new([string[]]$devicePaths)
}
try {
for ($cycle=1; $cycle -le $Cycles; $cycle++) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    if ($Remount) {
        $holds = @($paths | ForEach-Object {
            [IO.File]::Open((Join-Path $_ 'lifecycle-proof.bin'),'Open','Read','ReadWrite, Delete') })
        if ($Dismount) { $paths | ForEach-Object { [LifecycleRemount]::Dismount($_) } }
        $openers.Resume()
    }
    sc.exe stop $Service | Out-Null
    if ($LASTEXITCODE) { throw "Stop failed in cycle $cycle" }
    if ($Remount) {
        # the letters go and the volumes are dismounted around the holds;
        # their close lets the last of them go
        Start-Sleep -Milliseconds 300
        $holds | ForEach-Object { $_.Dispose() }
        Start-Sleep -Milliseconds 700
        $openers.Pause()
    }
    (Get-Service $Service).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
    $worst = [math]::Max($worst,$watch.ElapsedMilliseconds)
    foreach ($root in $paths) {
        if ([IO.Directory]::Exists($root)) { throw "Drive remained accessible after stop: $root" }
    }
    sc.exe start $Service | Out-Null
    if ($LASTEXITCODE) { throw "Start failed in cycle $cycle" }
    (Get-Service $Service).WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
    foreach ($root in $paths) {
        $wait = [Diagnostics.Stopwatch]::StartNew()
        while (-not [IO.Directory]::Exists($root) -and $wait.Elapsed.TotalSeconds -lt 30) {
            Start-Sleep -Milliseconds 50
        }
        $data = [IO.File]::ReadAllBytes((Join-Path $root 'lifecycle-proof.bin'))
        if (($data -join ',') -ne '1,5,13,29,61,125,253') { throw "Data changed in cycle $cycle" }
    }
    "LIFECYCLE: cycle $cycle passed"
}
} finally {
    if ($Churn) { $workers.Finish() }
    if ($Remount) { $openers.Finish() }
}
foreach ($root in $paths) { [IO.File]::Delete((Join-Path $root 'lifecycle-proof.bin')) }
"LIFECYCLE: $Cycles cycles, 0 failed, worst stop ${worst}ms"
