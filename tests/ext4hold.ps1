# ext4hold.ps1 - keep a file on the ext4 volume open for a few seconds; the host pulls
# the disk meanwhile to test surprise removal with a live handle. Prints what the close
# and a later access reported.
param([string]$Path, [int]$Seconds = 8)
try {
    $f = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    Start-Sleep $Seconds
    try { $n = $f.Read((New-Object byte[] 16), 0, 16); "read after removal returned $n" } catch { "read after removal: $($_.Exception.InnerException.Message)" }
    $f.Close(); "closed"
} catch { "open failed: $($_.Exception.Message)" }
