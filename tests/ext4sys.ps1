# SPDX-License-Identifier: GPL-2.0-only
# ext4sys.ps1 - driver life-cycle checks, run on the test machine by run-ext4test.ps1.
# Drive letters must be there while the driver runs, `sc stop` must complete promptly
# even with a shell-style directory handle open, and `sc start` must bring the letters
# back without help from user mode.
param([string[]]$Letters = @('D', 'E'), [string]$Service = 'ext4')
$ErrorActionPreference = 'Continue'
if ($Letters.Count -eq 1 -and $Letters[0] -match ',') { $Letters = $Letters[0] -split ',' }
$fail = 0
function Check($name, $cond, $extra = '') { if ($cond) { "  ok   $name $extra" } else { $script:fail++; "  FAIL $name $extra" } }
function Note($name, $extra = '') { "  note $name $extra" }
function Drives { (Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Root -match '^[A-Z]:\\$' }).Name }
function WaitDrives($present, $sec) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do { $d = Drives; $all = $true; foreach ($l in $Letters) { if (($d -contains $l) -ne $present) { $all = $false } }; if ($all) { return $sw.ElapsedMilliseconds }; Start-Sleep -Milliseconds 100 } while ($sw.Elapsed.TotalSeconds -lt $sec)
    return -1
}
Add-Type -AssemblyName System.ServiceProcess
$s = New-Object System.ServiceProcess.ServiceController $Service
Add-Type -Namespace X4S -Name Dos -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint QueryDosDevice(string d, System.Text.StringBuilder t, uint n);'
# kernel device names of the volumes: after the letters are gone, handle holders can only be found by them
$DevNames = foreach ($l in $Letters) { $b = New-Object Text.StringBuilder 512; [void][X4S.Dos]::QueryDosDevice("$l`:", $b, 512); $b.ToString() }
$HandleTool = Join-Path $PSScriptRoot 'handle64.exe'
function Show-Holders($why) {
    Note "stop is slow ($why) - processes holding the volumes:"
    if (-not (Test-Path $HandleTool)) { Note '(handle64.exe not next to this script)'; return }
    foreach ($dn in $DevNames) { & $HandleTool -nobanner -accepteula $dn 2>$null | Where-Object { $_ -match '\S' } | ForEach-Object { "         $_" } }
}
# service state read every 5 ms (a cheap QueryServiceStatus), so times are measured, not quantized
function Poll-Status([string]$want, [int]$ms) {
    $w = [Diagnostics.Stopwatch]::StartNew()
    do { $s.Refresh(); if ($s.Status -eq $want) { return $true }; Start-Sleep -Milliseconds 5 } while ($w.ElapsedMilliseconds -lt $ms)
    return $false
}
# wait for Stopped; a stop that takes longer than a few seconds is diagnosed while it is still hanging
function Wait-Stopped([int]$sec = 10) {
    # ServiceController.WaitForStatus polls every 250 ms, which would dominate the measurement
    if (-not (Poll-Status 'Stopped' 3000)) { Show-Holders "after 3 s"; [void](Poll-Status 'Stopped' (($sec - 3) * 1000)) }
    # a hung stop makes every later step meaningless: report it and end the section
    if ($s.Status -ne 'Stopped') {
        $script:fail++
        "  FAIL service still $($s.Status) $sec s after the stop request - service checks aborted"
        "== SYS RESULT: $script:fail failed"
        exit $script:fail
    }
}
function Wait-Running([int]$sec) { [void](Poll-Status 'Running' ($sec * 1000)) }

"-- service and drive letters"
$s.Refresh(); Check "service running" ($s.Status -eq 'Running') "($($s.Status))"
$d = Drives; Check "letters present: $($Letters -join ' ')" (($Letters | Where-Object { $d -notcontains $_ }).Count -eq 0) "(have $($d -join ' '))"
foreach ($l in $Letters) {
    $vi = Get-Volume -DriveLetter $l -ErrorAction SilentlyContinue
    Check "$l`: reports EXT4 with size" ($vi -and ($vi.FileSystemType -match 'ext' -or $vi.FileSystem -match 'ext') -and $vi.Size -gt 0) "($($vi.FileSystemLabel), $([math]::Round($vi.Size/1MB)) MB)"
    $mv = (mountvol "$l`:\" /L 2>$null); Check "$l`: has a volume GUID path" ($mv -match 'Volume\{')
}

"-- stop with an open directory handle (like an Explorer window)"
$w = New-Object IO.FileSystemWatcher "$($Letters[0]):\"; $w.EnableRaisingEvents = $true
$f = [IO.File]::Open("$($Letters[0]):\ext4sys_open.txt", [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
$f.Write([Text.Encoding]::ASCII.GetBytes('open during stop'), 0, 16); $f.Flush()
$sw = [Diagnostics.Stopwatch]::StartNew()
sc.exe stop $Service | Out-Null
$gone = WaitDrives $false 10
Check "letters withdrawn while handles still open" ($gone -ge 0) "($gone ms)"
$w.Dispose(); $f.Close()
$closed = $sw.ElapsedMilliseconds
Wait-Stopped
Check "service stopped after the handles closed" ($s.Status -eq 'Stopped') "(stopped at $($sw.ElapsedMilliseconds) ms, handles closed at $closed ms)"
Check "stop took no sleep-sized pause" (($sw.ElapsedMilliseconds - $closed) -le 3500)

"-- start"
$sw.Restart()
sc.exe start $Service | Out-Null
Wait-Running 30
$back = WaitDrives $true 30
Check "letters back after start" ($back -ge 0) "($back ms)"
Check "file written before stop survived" (([IO.File]::ReadAllText("$($Letters[0]):\ext4sys_open.txt")) -eq 'open during stop')
[IO.File]::Delete("$($Letters[0]):\ext4sys_open.txt")

"-- second stop/start with nothing open"
Start-Sleep 2
$sw.Restart(); sc.exe stop $Service | Out-Null; Wait-Stopped; $t1 = $sw.ElapsedMilliseconds
Check "idle stop under 3 s" ($t1 -lt 3000) "($t1 ms)"
$sw.Restart(); sc.exe start $Service | Out-Null; Wait-Running 30; $back = WaitDrives $true 30
Check "letters back again" ($back -ge 0) "($back ms)"
"-- repeated stop/start: sc stop and net stop, with and without an open handle"
$worst = 0
for ($i = 1; $i -le 6; $i++) {
    $useNet = ($i % 2 -eq 0); $hold = ($i % 3 -ne 0)
    $f = $null
    if ($hold) { $f = [IO.File]::Open("$($Letters[0]):\ext4sys_cycle$i.txt", [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite); $f.Write([byte[]](1..64), 0, 64) }
    $sw.Restart()
    # net stop blocks until the service is down, so it runs detached: the handle is closed 300 ms later
    if ($useNet) { $p = Start-Process net -ArgumentList 'stop', $Service -WindowStyle Hidden -PassThru } else { sc.exe stop $Service | Out-Null }
    # the handle's buffered data is written at Close, after the volume is gone: that error is expected
    if ($f) { Start-Sleep -Milliseconds 300; try { $f.Close() } catch { $f.Dispose() } }
    Wait-Stopped
    $s.Refresh(); $t1 = $sw.ElapsedMilliseconds; if ($t1 -gt $worst) { $worst = $t1 }
    if ($useNet) { $p.WaitForExit(30000) | Out-Null }
    $stopped = ($s.Status -eq 'Stopped')
    $sw.Restart(); sc.exe start $Service | Out-Null
    Wait-Running 30
    $s.Refresh(); $back = WaitDrives $true 30
    Check "cycle $i ($(if ($useNet) { 'net stop' } else { 'sc stop' })$(if ($hold) { ', handle open' })): stopped in $t1 ms, letters back in $back ms" ($stopped -and $back -ge 0 -and $t1 -lt 3500)
    if ($hold -and $back -ge 0) { try { [IO.File]::Delete("$($Letters[0]):\ext4sys_cycle$i.txt") } catch {} }
}
Note "worst stop time $worst ms"
"== SYS RESULT: $fail failed"
exit $fail
