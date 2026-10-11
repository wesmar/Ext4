# SPDX-License-Identifier: GPL-2.0-only
# Guest integration: switch only the test volume's live code page, exercise
# representable names, then restore the exact original level-1 properties.
# Probe input comes from nls-model.exe --export-probes against the old tables.
param([string]$Drive = 'U', [Parameter(Mandatory)][string]$Probes)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class NlsVolume {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFile(string p, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint c, byte[] i, int il, byte[] o, int ol, out int r, IntPtr ov);
    public static byte[] Property(string drive, byte[] settings) {
        using (var h = CreateFile(@"\\.\" + drive + ":", 0xC0000000, 7, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] b = settings == null ? new byte[48] : (byte[])settings.Clone();
            BitConverter.GetBytes(0x4556504Du).CopyTo(b,0);
            BitConverter.GetBytes(settings == null ? 2u : 3u).CopyTo(b,8);
            int returned;
            if (!DeviceIoControl(h,0x221F40,b,b.Length,b,b.Length,out returned,IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return b;
        }
    }
}
'@
$saved = [NlsVolume]::Property($Drive, $null)
# Some retained Arabic tables map ASCII digits to Arabic-Indic digits.
# Keep the test path's scaffolding in their common alphabetic repertoire.
$token = -join ([Guid]::NewGuid().ToString('N').ToCharArray() | ForEach-Object { 'abcdefghijklmnop'[[Convert]::ToInt32([string]$_,16)] })
$root = "$Drive`:\nls-$token"
$profiles = 0
$names = 0
try {
    [void][IO.Directory]::CreateDirectory($root)
    foreach ($line in [IO.File]::ReadAllLines($Probes)) {
        if ($line -notmatch '^PROBE ([^ ]+)(.*)$') { throw "Invalid probe row: $line" }
        $page = $Matches[1]
        $characters = @($Matches[2].Trim() -split ' ' | Where-Object { $_ })
        $settings = [byte[]]$saved.Clone()
        [Array]::Clear($settings, 16, 32)
        [Text.Encoding]::ASCII.GetBytes($page).CopyTo($settings, 16)
        [void][NlsVolume]::Property($Drive, $settings)
        $actual = [Text.Encoding]::ASCII.GetString([NlsVolume]::Property($Drive, $null),16,32).Trim([char]0)
        if ($actual -cne $page) { throw "Code page did not change: $page -> $actual" }
        # U+FFFF is confirmed unrepresentable by the reference converter.
        # A failed name conversion must release its dentry exactly once.
        $invalid = Join-Path $root "invalid-$([char]0xffff).bin"
        $refused = $false
        try {
            $unexpected = [IO.File]::Open($invalid,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            $unexpected.Dispose()
        } catch {
            # Windows PowerShell may wrap a static .NET call in a
            # MethodInvocationException; only an underlying I/O error counts.
            $cause = $_.Exception
            while ($cause.InnerException) { $cause = $cause.InnerException }
            if ($cause -isnot [IO.IOException]) { throw }
            $refused = $true
        }
        if (!$refused) { throw "Unrepresentable name was accepted: $page" }
        # Test one name at a time: case-fold aliases must not collide with
        # other probes. ASCII is also covered for a code page with no extras.
        foreach ($hex in @('0041') + $characters) {
            $character = [char][Convert]::ToUInt16($hex,16)
            $name = "probe-$character-name.bin"
            $path = Join-Path $root $name
            $renamed = Join-Path $root "renamed-$character-name.bin"
            $payload = [Text.Encoding]::ASCII.GetBytes("$page/$hex")
            $file = [IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            try { $file.Write($payload,0,$payload.Length); $file.Flush($true) } finally { $file.Dispose() }
            $listed = @([IO.Directory]::GetFiles($root) | ForEach-Object { [IO.Path]::GetFileName($_) })
            if ($listed.Count -ne 1 -or $listed[0] -cne $name) { throw "Listing mismatch: $page/$hex" }
            if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)) -cne "$page/$hex") { throw "Read mismatch: $page/$hex" }
            [IO.File]::Move($path,$renamed)
            if (![IO.File]::Exists($renamed) -or [IO.File]::Exists($path)) { throw "Rename mismatch: $page/$hex" }
            [IO.File]::Delete($renamed)
            if ([IO.Directory]::GetFiles($root).Length -ne 0) { throw "Delete mismatch: $page/$hex" }
            $names++
        }
        $profiles++
        "  ok $page ($($characters.Count + 1) names)"
    }
} finally {
    # Restore even after a failed conversion; never alter the registry default.
    [void][NlsVolume]::Property($Drive,$saved)
    if ([IO.Directory]::Exists($root) -and [IO.Directory]::GetFileSystemEntries($root).Length -eq 0) {
        [IO.Directory]::Delete($root)
    }
}
$restored = [NlsVolume]::Property($Drive,$null)
if ([Convert]::ToBase64String($restored) -cne [Convert]::ToBase64String($saved)) { throw 'Volume properties were not restored exactly.' }
"NLS-VM: $profiles code pages, $names names: ALL PASSED"
