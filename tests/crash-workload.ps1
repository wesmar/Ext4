# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# crash-workload.ps1 - the guest half of tests/crash-vm.ps1: a fixed mix of
# namespace and data changes on the volume labelled crashlog - directories,
# files of many sizes, overwrites, appends, truncation, renames, hard links,
# deletes - with some files flushed (FlushFileBuffers) along the way. With
# -Churn, as many small files are then made, flushed, renamed and deleted,
# one commit each: enough to wrap a small journal many times over.
param([int]$Files = 120, [int]$Churn = 0)
$ErrorActionPreference = 'Stop'
$volume = $null
for ($i = 0; $i -lt 100 -and -not $volume; $i++) {
    $volume = Get-Volume | Where-Object { $_.FileSystemLabel -eq 'crashlog' -and $_.DriveLetter }
    if (-not $volume) { Start-Sleep -Milliseconds 200 }
}
if (-not $volume) { throw 'The crashlog volume has no drive letter' }
$root = "$($volume.DriveLetter):\work"
Add-Type -TypeDefinition @"
using System;
using System.IO;
public static class CW {
    public static byte[] Pattern(int seed, int count) {
        var b = new byte[count];
        for (int i = 0; i < count; i++) b[i] = (byte)((i * 31 + seed * 17 + 7) % 251);
        return b;
    }
    public static void Write(string path, int seed, int count, bool flush) {
        using (var f = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 4096)) {
            f.Write(Pattern(seed, count), 0, count);
            if (flush) f.Flush(true);
        }
    }
}
"@
[IO.Directory]::CreateDirectory("$root\a\b\c") | Out-Null
[IO.Directory]::CreateDirectory("$root\big") | Out-Null
for ($i = 0; $i -lt $Files; $i++) {
    $size = @(0, 1, 511, 4096, 4097, 65536, 200000)[$i % 7]
    [CW]::Write("$root\a\file-$i.bin", $i, $size, ($i % 5 -eq 0))
}
[CW]::Write("$root\big\seq.bin", 999, 3 * 1024 * 1024, $true)
for ($i = 0; $i -lt $Files; $i += 3) { Move-Item "$root\a\file-$i.bin" "$root\a\b\moved-$i.bin" }
for ($i = 1; $i -lt $Files; $i += 4) { Remove-Item "$root\a\file-$i.bin" -ErrorAction SilentlyContinue }
for ($i = 2; $i -lt $Files; $i += 6) {
    $f = [IO.File]::Open("$root\a\file-$i.bin", 'Open', 'ReadWrite')
    try { $f.SetLength(1234); $f.Seek(0, 'End') | Out-Null; $f.Write([CW]::Pattern($i, 9000), 0, 9000); $f.Flush($true) } finally { $f.Close() }
}
for ($i = 0; $i -lt 20; $i++) { New-Item -ItemType HardLink -Path "$root\a\b\c\link-$i" -Target "$root\big\seq.bin" | Out-Null }
for ($i = 0; $i -lt 10; $i++) { Remove-Item "$root\a\b\c\link-$i" }
[CW]::Write("$root\big\seq.bin", 1000, 1024 * 1024, $true)
Remove-Item "$root\a\b\c" -Recurse -Force
[IO.Directory]::CreateDirectory("$root\churn") | Out-Null
for ($i = 0; $i -lt $Churn; $i++) {
    [CW]::Write("$root\churn\f-$i.bin", $i, 4096 + ($i % 3) * 5000, $true)
    Move-Item "$root\churn\f-$i.bin" "$root\churn\g-$i.bin"
    if ($i -ge 10) { Remove-Item "$root\churn\g-$($i - 10).bin" }
    if ($i % 50 -eq 0) { [IO.Directory]::CreateDirectory("$root\churn\d-$i") | Out-Null }
}
'CRASH-WORKLOAD: done'
