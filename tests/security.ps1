# security.ps1 - the driver's private IOCTLs against callers that must not use them (guest).
#
# As administrator: the kernel-only unload IOCTL is refused, empty or short
# buffers are refused instead of read, a legitimate query still works.
# FSCTL_GET_RETRIEVAL_POINTERS (raw user pointers): bad VCNs and an unmapped
# output buffer are refused, holes are reported as LCN -1. EAs are found by
# name in any case, a missing name comes back empty. As a
# restricted token of this thread (Administrators deny-only, as for a standard user): the control
# device cannot be opened, and volume settings cannot be changed or mount
# points created through a handle on a plain file.
#
#   powershell -File security.ps1 [-Drive E]
param([string]$Drive = 'E')
$ErrorActionPreference = 'Stop'
$root = "$Drive`:\"
for ($i = 0; $i -lt 100 -and -not [IO.Directory]::Exists($root); $i++) { Start-Sleep -Milliseconds 100 }

# access masks are written in decimal: Windows PowerShell reads 0x80000000 as a negative Int32
# IOCTL_APP_VOLUME_PROPERTY 0x221F40, IOCTL_APP_MOUNT_POINT 0x221F48,
# IOCTL_PREPARE_TO_UNLOAD 0x22A003; 'EVPM' = 0x4556504D
$cs = @'
using System; using System.Runtime.InteropServices; using Microsoft.Win32.SafeHandles;
public static class SecIo {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern SafeFileHandle CreateFile(string p, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] i, int il, byte[] o, int ol, out int r, IntPtr ov);
    public static int Open(string p, uint access) {
        var h = CreateFile(p, access, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        int e = h.IsInvalid ? Marshal.GetLastWin32Error() : 0;
        if (!h.IsInvalid) h.Close();
        return e;
    }
    public static int Ioctl(string p, uint access, uint code, int len, uint command) {
        var h = CreateFile(p, access, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        if (h.IsInvalid) return -Marshal.GetLastWin32Error();
        using (h) {
            byte[] b = len > 0 ? new byte[len] : null;
            if (b != null && len >= 12) { BitConverter.GetBytes(0x4556504Du).CopyTo(b, 0); BitConverter.GetBytes(command).CopyTo(b, 8); }
            int r;
            return DeviceIoControl(h, code, b, len, b, len, out r, IntPtr.Zero) ? 0 : Marshal.GetLastWin32Error();
        }
    }
    // --- LUKS control (IOCTL_APP_CRYPT_*): header magic 'TPRC', the rest zero
    public static int Crypt(uint code, int len) {
        var h = CreateFile(@"\\.\ext4", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) return -Marshal.GetLastWin32Error();
        using (h) {
            var b = new byte[len]; BitConverter.GetBytes(0x54505243u).CopyTo(b, 0);
            int r;
            return DeviceIoControl(h, code, b, len, b, len, out r, IntPtr.Zero) ? 0 : Marshal.GetLastWin32Error();
        }
    }
    // --- FSCTL_GET_RETRIEVAL_POINTERS (METHOD_NEITHER: the driver sees raw user pointers)
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(SafeFileHandle h, uint code, ref long vcn, int il, IntPtr o, int ol, out int r, IntPtr ov);
    // returns "error runs lcn0 next0 lastNext"
    public static string Runs(string p, long vcn, int outLen, bool badOut) {
        using (var h = CreateFile(p, 0x80000000, 7, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) return "open" + Marshal.GetLastWin32Error();
            IntPtr o = badOut ? new IntPtr(0x1000) : Marshal.AllocHGlobal(outLen);
            try {
                int r; bool ok = DeviceIoControl(h, 0x90073, ref vcn, 8, o, outLen, out r, IntPtr.Zero);
                int e = ok ? 0 : Marshal.GetLastWin32Error();
                if (badOut || (e != 0 && e != 234)) return e.ToString();
                int n = Marshal.ReadInt32(o);
                long last = n > 0 ? Marshal.ReadInt64(o, 16 + 16 * (n - 1)) : 0;
                return e + " " + n + " " + Marshal.ReadInt64(o, 24) + " " + Marshal.ReadInt64(o, 16) + " " + last;
            } finally { if (!badOut) Marshal.FreeHGlobal(o); }
        }
    }
    // --- EAs by name: FILE_GET_EA_INFORMATION list in, FILE_FULL_EA_INFORMATION out
    [StructLayout(LayoutKind.Sequential)] struct IOSB { public IntPtr S; public IntPtr I; }
    [DllImport("ntdll.dll")] static extern int NtSetEaFile(SafeFileHandle h, out IOSB io, byte[] b, uint l);
    [DllImport("ntdll.dll")] static extern int NtQueryEaFile(SafeFileHandle h, out IOSB io, byte[] b, uint l, bool single, byte[] list, uint ll, IntPtr idx, bool restart);
    public static int SetEa(string p, string name, string value) {
        byte[] n = System.Text.Encoding.ASCII.GetBytes(name), v = System.Text.Encoding.ASCII.GetBytes(value);
        var b = new byte[8 + n.Length + 1 + v.Length];
        b[5] = (byte)n.Length; BitConverter.GetBytes((ushort)v.Length).CopyTo(b, 6); n.CopyTo(b, 8); v.CopyTo(b, 9 + n.Length);
        using (var h = CreateFile(p, 0x10, 7, IntPtr.Zero, 3, 0, IntPtr.Zero)) { IOSB io; return NtSetEaFile(h, out io, b, (uint)b.Length); }
    }
    // names asked for in one list; returns "status|value1|value2..." ("<none>" for an empty value)
    public static string GetEaList(string p, bool full, params string[] names) {
        var list = new System.Collections.Generic.List<byte>();
        for (int k = 0; k < names.Length; k++) {
            byte[] n = System.Text.Encoding.ASCII.GetBytes(names[k]);
            int size = 5 + n.Length + 1, next = k + 1 < names.Length ? (size + 3) & ~3 : 0;
            list.AddRange(BitConverter.GetBytes(next)); list.Add((byte)n.Length); list.AddRange(n); list.Add(0);
            while (next != 0 && list.Count % 4 != 0) list.Add(0);
        }
        var b = new byte[4096];
        using (var h = CreateFile(p, 0x08, 7, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            IOSB io; int st = full ? NtQueryEaFile(h, out io, b, (uint)b.Length, false, null, 0, IntPtr.Zero, true)
                                   : NtQueryEaFile(h, out io, b, (uint)b.Length, false, list.ToArray(), (uint)list.Count, IntPtr.Zero, true);
            var s = new System.Text.StringBuilder(st.ToString("X"));
            if (st < 0) return s.ToString();
            for (int o = 0; ; ) {
                int next = BitConverter.ToInt32(b, o), nl = b[o + 5], vl = BitConverter.ToUInt16(b, o + 6);
                s.Append('|').Append(full ? System.Text.Encoding.ASCII.GetString(b, o + 8, nl) + "=" : "")
                 .Append(vl == 0 ? "<none>" : System.Text.Encoding.ASCII.GetString(b, o + 9 + nl, vl));
                if (next == 0) break;
                o += next;
            }
            return s.ToString();
        }
    }
    // --- a standard user's view: this thread impersonates a restricted copy of
    //     its own token (Administrators deny-only, no privileges) and runs the probes
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool CreateRestrictedToken(IntPtr t, uint flags, uint nd, SID_AND_ATTRIBUTES[] d, uint np, IntPtr p, uint nr, IntPtr r, out IntPtr n);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool ImpersonateLoggedOnUser(IntPtr t);
    [DllImport("advapi32.dll")] static extern bool RevertToSelf();
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AllocateAndInitializeSid(byte[] auth, byte n, uint s0, uint s1, uint s2, uint s3, uint s4, uint s5, uint s6, uint s7, out IntPtr sid);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.I1)] static extern bool CreateSymbolicLink(string link, string target, int flags);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] struct SID_AND_ATTRIBUTES { public IntPtr Sid; public uint Attributes; }
    public static string AsStandardUser(string file) {
        IntPtr tok, rtok, admins;
        if (!OpenProcessToken(GetCurrentProcess(), 0x000B /* ASSIGN_PRIMARY|DUPLICATE|QUERY */ | 0x0004 /* IMPERSONATE */, out tok)) return "error=token";
        AllocateAndInitializeSid(new byte[] { 0, 0, 0, 0, 0, 5 }, 2, 32, 544, 0, 0, 0, 0, 0, 0, out admins);
        var deny = new[] { new SID_AND_ATTRIBUTES { Sid = admins, Attributes = 0 } };
        if (!CreateRestrictedToken(tok, 1 /* DISABLE_MAX_PRIVILEGE */, 1, deny, 0, IntPtr.Zero, 0, IntPtr.Zero, out rtok)) return "error=restrict" + Marshal.GetLastWin32Error();
        if (!ImpersonateLoggedOnUser(rtok)) return "error=impersonate" + Marshal.GetLastWin32Error();
        try {
            return "control-open=" + Open(@"\\.\ext4", 0x80000000) +
                   " set-readonly-via-file=" + Ioctl(file, 0x80000000, 0x221F40, 64, 3) +
                   " mountpoint-via-file=" + Ioctl(file, 0x80000000, 0x221F48, 1040, 1) +
                   " symlink=" + (CreateSymbolicLink(file + ".lnk", "probe.txt", 0) ? 0 : Marshal.GetLastWin32Error());
        } finally { RevertToSelf(); CloseHandle(rtok); CloseHandle(tok); }
    }
}
'@
Add-Type -TypeDefinition $cs
$fail = 0
function Check($name, $expected, $actual) { if ("$expected" -eq "$actual") { "  ok   $name ($actual)" } else { "  FAIL $name`: expected $expected, got $actual"; $script:fail++ } }

$dir = "$root" + 'security'; $file = "$dir\probe.txt"
[void][IO.Directory]::CreateDirectory($dir)
[IO.File]::WriteAllText($file, 'probe')

"-- as administrator"
Check 'unload IOCTL from user mode refused'         5  ([SecIo]::Ioctl('\\.\ext4', 3221225472, 0x22A003, 0, 0))
Check 'volume property with no buffer refused'      87 ([SecIo]::Ioctl($file, 2147483648, 0x221F40, 0, 2))
Check 'volume property with a short buffer refused' 87 ([SecIo]::Ioctl($file, 2147483648, 0x221F40, 8, 2))
Check 'volume property query still works'           0  ([SecIo]::Ioctl("\\.\$Drive`:", 2147483648, 0x221F40, 64, 2))

"-- LUKS control (IOCTL_APP_CRYPT_*, 0x221F68..70)"
Check 'unlocked volumes can be listed'            0     ([SecIo]::Crypt(0x221F70, 8192))
Check 'unlock with a malformed request refused'   87    ([SecIo]::Crypt(0x221F68, 512))
Check 'unlock with a short buffer refused'        87    ([SecIo]::Crypt(0x221F68, 64))
Check 'lock of no such volume refused'            1168  ([SecIo]::Crypt(0x221F6C, 16))

"-- retrieval pointers (raw user pointers)"
# blocks are placed when the cache is written (delayed allocation): flush first
[IO.File]::WriteAllBytes("$dir\runs.bin", (New-Object byte[] 65536))
$f = [IO.File]::Open("$dir\runs.bin", 'Open', 'ReadWrite'); $f.Flush($true); $f.Close()
$r = ([SecIo]::Runs("$dir\runs.bin", 0, 4096, $false)) -split ' '
Check 'map of a 64 KB file'                   '0 1 16'  ("$($r[0]) $($r[1]) $($r[4])")
Check 'map starts at a real cluster'          $true     ([long]$r[2] -gt 0)
Check 'negative starting VCN refused'         87        ([SecIo]::Runs("$dir\runs.bin", -1, 4096, $false))
Check 'VCN past the end is EOF'               38        ([SecIo]::Runs("$dir\runs.bin", 4611686018427387904, 4096, $false))
Check 'unmapped output buffer refused'        1784      ([SecIo]::Runs("$dir\runs.bin", 0, 4096, $true))
$f = [IO.File]::Open("$dir\sparse.bin", 'Create'); $f.SetLength(1048576); $f.Seek(1044480, 'Begin') | Out-Null; $f.Write((New-Object byte[] 4096), 0, 4096); $f.Flush($true); $f.Close()
$r = ([SecIo]::Runs("$dir\sparse.bin", 0, 4096, $false)) -split ' '
Check 'sparse file: a hole, then the last block' '0 2 -1 256' ("$($r[0]) $($r[1]) $($r[2]) $($r[4])")

"-- EAs by name"
Check 'set Color'                             0         ([SecIo]::SetEa($file, 'Color', 'blue'))
Check 'query in another case finds it'        '0|blue'  ([SecIo]::GetEaList($file, $false, 'COLOR'))
Check 'missing name comes back empty'         '0|blue|<none>' ([SecIo]::GetEaList($file, $false, 'color', 'NOSUCH'))
Check 'set COLOR replaces Color'              0         ([SecIo]::SetEa($file, 'COLOR', 'red'))
Check 'one attribute left'                    '0|COLOR=red' ([SecIo]::GetEaList($file, $true))

"-- as a standard user (restricted token: Administrators deny-only)"
try {
    $res = @{}
    ([SecIo]::AsStandardUser($file)) -split ' ' | ForEach-Object { $k, $v = $_ -split '='; $res[$k] = $v }
    if ($res['error']) { "  (restricted token: $($res['error']))" }
    Check 'control device closed to users'          5  $res['control-open']
    Check 'volume settings closed to users'         5  $res['set-readonly-via-file']
    Check 'mount points closed to users'            5  $res['mountpoint-via-file']
    Check 'symlink needs the symlink privilege'     1314 $res['symlink']
} finally {
    foreach ($x in $file, "$file.lnk", "$dir\runs.bin", "$dir\sparse.bin") { if ([IO.File]::Exists($x)) { [IO.File]::Delete($x) } }
    if ([IO.Directory]::Exists($dir)) { [IO.Directory]::Delete($dir) }
}
"security: $fail failed"
