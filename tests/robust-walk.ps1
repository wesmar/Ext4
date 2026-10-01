# robust-walk.ps1 - the guest side of the robustness run (robust.ps1).
#
#   powershell -File robust-walk.ps1 -DiskSize <bytes> [-Seconds 60]
#       every partition of the disk of that size, one child process each,
#       killed after -Seconds: "<partition> <verdict> <details>"
#   powershell -File robust-walk.ps1 -Root <path>
#       one volume: list it, read every file, write, rename and delete,
#       each step on its own; a corrupt volume may refuse any of them -
#       what it may not do is crash the system or hang
param([long]$DiskSize = 0, [string]$Root = '', [int]$Seconds = 60)
$ErrorActionPreference = 'Continue'

if ($Root) {
    # passed as "F:" - a quoted "F:\" would end in \" and lose its quote
    $Root = $Root.TrimEnd('\') + '\'
    $out = @()
    $entries = @(); $files = 0; $bytes = 0; $readErr = 0
    try {
        $entries = @(Get-ChildItem -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue | Select-Object -First 5000)
        $out += "entries=$($entries.Count)"
    } catch { $out += "list=error" }
    foreach ($e in $entries) {
        if ($e.PSIsContainer -or $files -ge 300) { continue }
        $files++
        try {
            if ($e.Length -le 4MB) { $bytes += ([IO.File]::ReadAllBytes($e.FullName)).Length }
        } catch { $readErr++ }
    }
    $out += "read=$files/$readErr"
    $d = $Root + 'robust-win'
    try {
        [void][IO.Directory]::CreateDirectory($d)
        $data = New-Object byte[] 65536; (New-Object Random 7).NextBytes($data)
        [IO.File]::WriteAllBytes("$d\w.bin", $data)
        [IO.File]::AppendAllText("$d\w.bin", 'tail')
        $back = [IO.File]::ReadAllBytes("$d\w.bin")
        [IO.File]::Move("$d\w.bin", "$d\w2.bin")
        [IO.File]::Delete("$d\w2.bin")
        [IO.Directory]::Delete($d)
        $out += $(if ($back.Length -eq 65540) { 'write=ok' } else { "write=short($($back.Length))" })
    } catch { $out += "write=refused" }
    $out -join ' '
    exit 0
}

$disk = $null
for ($i = 0; $i -lt 100 -and -not $disk; $i++) {
    $disk = Get-Disk | Where-Object Size -eq $DiskSize | Select-Object -First 1
    if (-not $disk) { Start-Sleep -Milliseconds 100 }
}
if (-not $disk) { "no disk of $DiskSize bytes"; exit 2 }
function PathOf($p) { @($p.AccessPaths | Where-Object { $_ -match '^[A-Z]:\\$' }) | Select-Object -First 1 }
# mounts settle on their own time (an MMP wait, a journal replay): until
# every partition has a drive letter, or the deadline
$deadline = (Get-Date).AddSeconds(25)
do {
    $parts = @(Get-Partition -DiskNumber $disk.Number | Sort-Object PartitionNumber)
    $pending = @($parts | Where-Object { -not (PathOf $_) }).Count
    if ($pending) { Start-Sleep -Milliseconds 250 }
} while ($pending -and (Get-Date) -lt $deadline)
foreach ($p in $parts) {
    $n = $p.PartitionNumber
    $path = PathOf $p
    if (-not $path) { "$n unmounted"; continue }
    $fs = (Get-Volume -Partition $p -ErrorAction SilentlyContinue).FileSystem
    if ($fs -notin 'EXT2', 'EXT3', 'EXT4') { "$n not-ext ($fs)"; continue }
    $psi = New-Object Diagnostics.ProcessStartInfo 'powershell.exe', "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Root `"$($path.TrimEnd('\'))`""
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true
    $child = [Diagnostics.Process]::Start($psi)
    $text = $child.StandardOutput.ReadToEndAsync()
    if ($child.WaitForExit($Seconds * 1000)) {
        "$n done $($text.Result.Trim())"
    } else {
        try { $child.Kill() } catch {}
        "$n HANG after $Seconds s"
    }
}
