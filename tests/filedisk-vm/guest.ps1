# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# guest.ps1 - the guest half of tests/filedisk-vm.ps1: ext4 volumes in image
# files on NTFS, through ext4ctl (Windows PowerShell 5.1).
#
#   unpack    the packed images as files with holes where they are zero
#   mount     two images at once, each with its letter, what they hold, the
#             images filling on demand
#   trim      what a volume freed goes back to NTFS, what stays is intact
#   fsx       the random operations of tests/fsx-vm on a volume in a file
#   race      the same with trims running all the time: a block a write has
#             been given must never be trimmed
#   busy      an open file keeps a volume unless forced; a file on a volume of
#             the driver is refused; a file not sparse cannot be trimmed
#   readonly  mounted read-only nothing changes
#   unload    sc stop with images mounted: clean, and the files are free
#   pack      the images packed again for Linux
#   sector4k  an image with blocks of 1 KiB on an NTFS of 4 KiB sectors: every
#             block is part of a sector of the file's volume
#
#   powershell -File guest.ps1 -Dir C:\fd -Ctl C:\Windows\Temp\ext4ctl.exe -Stage <name>
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [Parameter(Mandatory = $true)][string]$Ctl,
    [Parameter(Mandatory = $true)][string]$Stage,
    [string]$Fsx = '',
    [int]$Seeds = 6
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class Fd
{
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] input, int inputSize, byte[] output, int outputSize, out int returned, IntPtr overlapped);
    const uint SetSparseCode = 0x900C4, QueryRangesCode = 0x940CF;

    // a packed image as a sparse file, or a plain one
    public static long Unpack(string gz, string path, bool sparse) {
        byte[] b = new byte[1 << 20];
        long size = 0;
        using (var i = new GZipStream(File.OpenRead(gz), CompressionMode.Decompress))
        using (var o = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None)) {
            int r;
            if (sparse && !DeviceIoControl(o.SafeFileHandle, SetSparseCode, null, 0, null, 0, out r, IntPtr.Zero))
                throw new Exception("set sparse: " + Marshal.GetLastWin32Error());
            for (;;) {
                int n = 0, got;
                while (n < b.Length && (got = i.Read(b, n, b.Length - n)) > 0) n += got;
                if (n == 0) break;
                bool zero = true;
                for (int k = 0; k < n && zero; k++) zero = b[k] == 0;
                if (zero && sparse) { size += n; o.SetLength(size); o.Position = size; continue; }
                o.Write(b, 0, n);
                size += n;
            }
        }
        return size;
    }

    // what the image holds on NTFS, as fsutil sparse queryrange shows it
    public static long Allocated(string path) {
        using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
            byte[] input = new byte[16], output = new byte[1 << 20];
            long total = 0;
            for (long at = 0; ;) {
                BitConverter.GetBytes(at).CopyTo(input, 0);
                BitConverter.GetBytes(f.Length - at).CopyTo(input, 8);
                int done;
                bool ok = DeviceIoControl(f.SafeFileHandle, QueryRangesCode, input, 16, output, output.Length, out done, IntPtr.Zero);
                int e = ok ? 0 : Marshal.GetLastWin32Error();
                if (!ok && e != 234) throw new Exception("ranges: " + e);
                long last = at;
                for (int i = 0; i + 16 <= done; i += 16) {
                    long len = BitConverter.ToInt64(output, i + 8);
                    total += len;
                    last = BitConverter.ToInt64(output, i) + len;
                }
                if (ok || done == 0) return total;
                at = last;
            }
        }
    }

    public static void Pattern(string path, long bytes, int seed) {
        byte[] b = new byte[1 << 20];
        var r = new Random(seed);
        using (var o = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None)) {
            for (long at = 0; at < bytes; at += b.Length) { r.NextBytes(b); o.Write(b, 0, b.Length); }
            o.Flush(true);
        }
    }

    public static void Pack(string path, string gz) {
        using (var i = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var o = new GZipStream(File.Create(gz), CompressionLevel.Fastest))
            i.CopyTo(o, 1 << 20);
    }

    public static bool Free(string path) {
        try { using (new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) return true; }
        catch (IOException) { return false; }
    }
}
'@

$script:failed = 0
function Check([string]$What, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { "  ok   $What $Detail" } else { "  FAIL $What $Detail"; $script:failed++ }
}
function Ext4Ctl([string[]]$Arguments) {
    # (what it says on stderr is no error of this script's)
    $ErrorActionPreference = 'Continue'
    $output = @(& $Ctl @Arguments 2>&1 | ForEach-Object { "$_" })
    $script:ctlRc = $LASTEXITCODE
    $output | ForEach-Object { "       $_" }
}
function Sha([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }
function WriteSums([string]$Root, [string[]]$Names) {
    $text = ($Names | ForEach-Object { "$(Sha "$Root\$_")  $($_.Replace('\', '/'))`n" }) -join ''
    [IO.File]::WriteAllText("$Root\sums.txt", $text, [Text.Encoding]::ASCII)
}
# the label: Get-Volume shows no device without a PnP node
function LabelOf([string]$Letter) { try { ([IO.DriveInfo]::new($Letter)).VolumeLabel } catch { '' } }

$img4 = "$Dir\fd4k.img"; $img1 = "$Dir\fd1k.img"; $imgPlain = "$Dir\fdplain.img"

switch ($Stage) {

'unpack' {
    "  images: $([Fd]::Unpack("$Dir\fd4k.img.gz", $img4, $true)) + $([Fd]::Unpack("$Dir\fd1k.img.gz", $img1, $true)) + $([Fd]::Unpack("$Dir\fdplain.img.gz", $imgPlain, $false)) bytes"
    Check 'a 1 GiB image is mostly holes' ([Fd]::Allocated($img4) -lt 128MB) "$([Fd]::Allocated($img4)) bytes"
}

'mount' {
    Ext4Ctl @('mount', $img4, '--letter', 'R')
    Check 'first image mounted' ($ctlRc -eq 0)
    Ext4Ctl @('mount', $img1, '--letter', 'S')
    Check 'second image mounted' ($ctlRc -eq 0)
    Ext4Ctl @('mount', $img4)
    Check 'the same image twice refused' ($ctlRc -ne 0)
    Ext4Ctl @('status')
    Check 'R: is the first image' ((LabelOf R) -eq 'fd4k') "$(LabelOf R)"
    Check 'S: is the second image' ((LabelOf S) -eq 'fd1k') "$(LabelOf S)"

    foreach ($v in 'R', 'S') {
        $image = if ($v -eq 'R') { $img4 } else { $img1 }
        $before = [Fd]::Allocated($image)
        New-Item -ItemType Directory "${v}:\tree" | Out-Null
        1..300 | ForEach-Object { [IO.File]::WriteAllBytes("${v}:\tree\f$_.bin", (New-Object byte[] (($_ * 37) % 9000 + 1))) }
        [Fd]::Pattern("${v}:\kept.bin", 16MB, 11)
        [Fd]::Pattern("${v}:\big.bin", 192MB, 12)
        $after = [Fd]::Allocated($image)
        Check "image behind ${v}: fills on demand" ($after -ge $before + 200MB) "$before -> $after"
        $sum = Sha "${v}:\big.bin"
        Set-Content "$Dir\big-$v.sum" $sum
        Set-Content "$Dir\kept-$v.sum" (Sha "${v}:\kept.bin")
    }
}

'trim' {
    foreach ($v in 'R', 'S') {
        $image = if ($v -eq 'R') { $img4 } else { $img1 }
        Check "${v}: big file read back" ((Sha "${v}:\big.bin") -eq (Get-Content "$Dir\big-$v.sum")) ''
        $before = [Fd]::Allocated($image)
        Remove-Item "${v}:\big.bin"
        Ext4Ctl @('trim', "${v}:")
        Check "${v}: trim ran" ($ctlRc -eq 0)
        $after = [Fd]::Allocated($image)
        Check "image behind ${v}: freed blocks went back to NTFS" ($after -le $before - 150MB) "$before -> $after"
        Check "${v}: what stays is intact" ((Sha "${v}:\kept.bin") -eq (Get-Content "$Dir\kept-$v.sum"))
        $tree = @(Get-ChildItem "${v}:\tree" -File)
        Check "${v}: small files stay" ($tree.Count -eq 300) "$($tree.Count)"
    }
    "  fsutil sparse queryrange:"
    & fsutil sparse queryrange $img4 | Select-Object -First 4 | ForEach-Object { "       $_" }
}

'fsx' {
    foreach ($v in 'R', 'S') {
        "  fsx on ${v}:"
        & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Fsx -Root "${v}:\" -FirstSeed 1 -Seeds $Seeds -Ops 1500 |
            Where-Object { $_ -match '^FSX|FAIL|seed' } | ForEach-Object { "       $_" }
        Check "${v}: fsx" ($LASTEXITCODE -eq 0)
    }
}

'race' {
    foreach ($v in 'R', 'S') {
        $flag = "$Dir\race-stop"
        $count = "$Dir\race-count"
        Remove-Item $flag, $count -ErrorAction SilentlyContinue
        $loop = '$n = 0; while (-not (Test-Path ''<flag>'')) { & ''<ctl>'' trim <v>: --min-bytes 4096 | Out-Null; if ($LASTEXITCODE -eq 0) { $n++ } }; Set-Content ''<count>'' $n'
        $loop = $loop.Replace('<flag>', $flag).Replace('<ctl>', $Ctl).Replace('<v>', $v).Replace('<count>', $count)
        $trims = Start-Process powershell -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand',
                 ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($loop))) -PassThru -WindowStyle Hidden
        "  fsx on ${v}: with trims running:"
        & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Fsx -Root "${v}:\" -FirstSeed 101 -Seeds $Seeds -Ops 1500 |
            Where-Object { $_ -match '^FSX|FAIL|seed' } | ForEach-Object { "       $_" }
        $rc = $LASTEXITCODE
        Set-Content $flag 'stop'
        $trims.WaitForExit()
        Check "${v}: fsx through the trims" ($rc -eq 0)
        $n = [int](Get-Content $count)
        Check "${v}: trims ran meanwhile" ($n -ge 3) "$n of them"
    }
}

'busy' {
    $hold = [IO.File]::Open('S:\kept.bin', 'Open', 'Read', 'Read')
    Ext4Ctl @('unmount', 'S:')
    Check 'an open file keeps the volume' ($ctlRc -ne 0)
    $hold.Dispose()
    Start-Sleep -Milliseconds 500

    [IO.File]::WriteAllBytes('R:\nested.img', (New-Object byte[] 1048576))
    Ext4Ctl @('mount', 'R:\nested.img')
    Check 'a file on a volume of the driver refused' ($ctlRc -ne 0)
    Remove-Item 'R:\nested.img'

    Ext4Ctl @('mount', $imgPlain, '--letter', 'T')
    Check 'a plain file mounted' ($ctlRc -eq 0)
    Ext4Ctl @('trim', 'T:')
    Check 'a plain file takes no trim' ($ctlRc -ne 0)
    [Fd]::Pattern('T:\plain.bin', 8MB, 13)
    $sum = Sha 'T:\plain.bin'
    Ext4Ctl @('unmount', 'T:')
    Check 'a plain file unmounted' ($ctlRc -eq 0)
    Check 'and free' ([Fd]::Free($imgPlain))
    Ext4Ctl @('mount', $imgPlain, '--letter', 'T', '--read-only')
    Check 'mounted read-only' ($ctlRc -eq 0)
    Check 'the bytes are on it' ((Sha 'T:\plain.bin') -eq $sum)
    $refused = $false
    try { [IO.File]::WriteAllBytes('T:\no.bin', (New-Object byte[] 16)) } catch { $refused = $true }
    Check 'writes refused' $refused
    Ext4Ctl @('unmount', 'T:')
    Check 'read-only unmounted' ($ctlRc -eq 0)
}

'final' {
    foreach ($v in 'R', 'S') {
        $names = @('kept.bin') + @(1..300 | ForEach-Object { "tree\f$_.bin" })
        WriteSums "${v}:" $names
    }
    # a third of the journal's business: what is left must survive the flush
    Ext4Ctl @('unmount', 'R:')
    Check 'R: unmounted' ($ctlRc -eq 0)
    Check 'the first file free' ([Fd]::Free($img4))
    Check 'the letter gone' (-not (Test-Path 'R:\'))
}

'unload' {
    # S: stays mounted through the service stop
    sc.exe stop ext4 | Out-Null
    (Get-Service ext4).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120))
    Check 'sc stop with an image mounted' ((Get-Service ext4).Status -eq 'Stopped')
    Check 'the file free after it' ([Fd]::Free($img1))
    sc.exe start ext4 | Out-Null
}

'pack' {
    [Fd]::Pack($img4, "$Dir\fd4k.back.gz")
    [Fd]::Pack($img1, "$Dir\fd1k.back.gz")
    "  packed: $((Get-Item "$Dir\fd4k.back.gz").Length) + $((Get-Item "$Dir\fd1k.back.gz").Length) bytes"
}

'sector4k' {
    $disk = Get-Disk | Where-Object { $_.Size -eq 2GB -and $_.LogicalSectorSize -eq 4096 }
    Check 'a disk of 4 KiB sectors' ($null -ne $disk)
    Set-Disk -Number $disk.Number -IsOffline $false
    Set-Disk -Number $disk.Number -IsReadOnly $false
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT
    New-Partition -DiskNumber $disk.Number -UseMaximumSize -DriveLetter U | Out-Null
    Format-Volume -DriveLetter U -FileSystem NTFS -Confirm:$false | Out-Null
    $image = 'U:\fd4n.img'
    "  image: $([Fd]::Unpack("$Dir\fd1k.img.gz", $image, $true)) bytes"
    Ext4Ctl @('mount', $image, '--letter', 'V')
    Check 'mounted' ($ctlRc -eq 0)
    & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Fsx -Root 'V:\' -FirstSeed 201 -Seeds $Seeds -Ops 1500 |
        Where-Object { $_ -match '^FSX|FAIL|seed' } | ForEach-Object { "       $_" }
    Check 'fsx on blocks smaller than the sectors under them' ($LASTEXITCODE -eq 0)
    Ext4Ctl @('trim', 'V:', '--min-bytes', '4096')
    Check 'trimmed' ($ctlRc -eq 0)
    Ext4Ctl @('unmount', 'V:')
    Check 'unmounted' ($ctlRc -eq 0)
    [Fd]::Pack($image, "$Dir\fd4n.back.gz")
    Remove-Item $image
}

default { throw "No stage $Stage" }
}
exit $script:failed
