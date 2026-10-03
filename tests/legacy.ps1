# SPDX-License-Identifier: GPL-2.0-only
# Guest smoke test: real ext2/ext3, direct/single/double indirect data paths.
param([ValidateSet('Write','Verify')][string]$Phase = 'Write')
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class LegacyProof {
    public static byte[] Pattern(int count) {
        byte[] data = new byte[count];
        for (int i=0; i<count; i++) data[i]=(byte)((i*17+13)%251);
        return data;
    }
    public static void Check(string path, byte[] expected) {
        byte[] data=File.ReadAllBytes(path);
        if (data.Length!=expected.Length) throw new IOException("Length mismatch: "+path);
        for (int i=0; i<data.Length; i++)
            if (data[i]!=expected[i]) throw new IOException("Byte mismatch: "+path+" at "+i);
    }
    public static void Write(string path, byte[] data) {
        using (var file=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None)) {
            file.Write(data,0,data.Length);
            file.Flush(true);
        }
    }
}
'@
$wait=[Diagnostics.Stopwatch]::StartNew()
do {
    $volumes=@(Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemLabel -in 'smoke-ext2','smoke-ext3' })
    if ($volumes.Count -eq 2) { break }
    Start-Sleep -Milliseconds 100
} while ($wait.Elapsed.TotalSeconds -lt 30)
if ($volumes.Count -ne 2) { throw 'Both dedicated ext2/ext3 fixtures must mount' }
$payload=[LegacyProof]::Pattern(8*1024*1024+123)
$regrown=New-Object byte[] (1024*1024)
[Array]::Copy($payload,$regrown,4097)
foreach ($volume in $volumes) {
    $expected=if ($volume.FileSystemLabel -eq 'smoke-ext2') { 'EXT2' } else { 'EXT3' }
    if ($volume.FileSystem -ne $expected) { throw "Wrong mounted format: $($volume.FileSystem)" }
    $root="$($volume.DriveLetter):\"
    [LegacyProof]::Check((Join-Path $root 'linux-seed.bin'),[LegacyProof]::Pattern(655373))
    $proof=Join-Path $root 'legacy-proof'
    if ($Phase -eq 'Write') {
        if (Test-Path -LiteralPath $proof) { throw 'Existing proof directory; refusing overwrite' }
        [IO.Directory]::CreateDirectory($proof) | Out-Null
        $original=Join-Path $proof 'original.bin'
        [LegacyProof]::Write($original,$payload)
        [LegacyProof]::Check($original,$payload)
        [IO.File]::Move($original,(Join-Path $proof 'payload.bin'))
        $resize=Join-Path $proof 'regrown.bin'
        [LegacyProof]::Write($resize,$payload)
        $file=[IO.File]::Open($resize,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        try { $file.SetLength(4097); $file.SetLength($regrown.Length); $file.Flush($true) }
        finally { $file.Dispose() }
        [LegacyProof]::Write((Join-Path $proof 'deleted.bin'),$payload)
        [IO.File]::Delete((Join-Path $proof 'deleted.bin'))
    }
    [LegacyProof]::Check((Join-Path $proof 'payload.bin'),$payload)
    [LegacyProof]::Check((Join-Path $proof 'regrown.bin'),$regrown)
    if (Test-Path -LiteralPath (Join-Path $proof 'deleted.bin')) { throw 'Deleted file exists' }
    if (Test-Path -LiteralPath (Join-Path $proof 'original.bin')) { throw 'Old rename target exists' }
    "LEGACY-WIN: $expected $Phase passed"
}
"LEGACY-WIN: $Phase ALL PASSED"
