# SPDX-License-Identifier: GPL-2.0-only
# Repeated unload/reload with persistent data checked after every cycle (guest).
param([Parameter(Mandatory=$true)][string]$Roots,
    [ValidateRange(1,1000)][int]$Cycles = 10, [string]$Service = 'ext4', [switch]$Churn)
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
try {
for ($cycle=1; $cycle -le $Cycles; $cycle++) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    sc.exe stop $Service | Out-Null
    if ($LASTEXITCODE) { throw "Stop failed in cycle $cycle" }
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
}
foreach ($root in $paths) { [IO.File]::Delete((Join-Path $root 'lifecycle-proof.bin')) }
"LIFECYCLE: $Cycles cycles, 0 failed, worst stop ${worst}ms"
