#Requires -Version 7.0
# SPDX-License-Identifier: GPL-2.0-only
# Clean Windows restarts with mounted ext volumes; never resets the host or VM.
param([string]$Ip = '', [string]$Roots = 'D:\,U:\',
    [ValidateRange(1,20)][int]$Cycles = 3)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$sshOptions = @(Get-SshOptions) + @('-o','ServerAliveInterval=5','-o','ServerAliveCountMax=2')
$target = "$($TestEnv.User)@$Ip"
$paths = @($Roots -split ',')
if (-not $paths.Count -or @($paths | Where-Object { $_ -notmatch '^[A-Za-z]:\\$' }).Count) {
    throw 'Use explicit drive roots, e.g. D:\,U:\.'
}
if ($TestEnv.Service -notmatch '^[A-Za-z0-9_-]+$') { throw 'Invalid service name' }
$proofName = 'reboot-proof-' + [guid]::NewGuid().ToString('N') + '.bin'
$rootJson = ConvertTo-Json -Compress -InputObject $paths
function Guest([string]$Code) {
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes(
        "`$ProgressPreference='SilentlyContinue'; `$ErrorActionPreference='Stop'; " + $Code))
    $out = @(& ssh @sshOptions $target "powershell -NoProfile -NonInteractive -EncodedCommand $enc")
    if ($LASTEXITCODE) { throw "Guest command failed ($LASTEXITCODE)" }
    return $out
}
function StartDriver {
    Guest @"
`$s=Get-Service '$($TestEnv.Service)'
if (`$s.Status -eq 'Stopped') {
    sc.exe start '$($TestEnv.Service)' | Out-Null
    if (`$LASTEXITCODE) { throw 'Driver start failed' }
}
`$s.WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
`$paths=ConvertFrom-Json '$rootJson'
`$wait=[Diagnostics.Stopwatch]::StartNew()
do {
    `$missing=@(`$paths | Where-Object { -not [IO.Directory]::Exists(`$_) })
    if (-not `$missing.Count) { break }
    Start-Sleep -Milliseconds 100
} while (`$wait.Elapsed.TotalSeconds -lt 30)
if (`$missing.Count) { throw 'Test volumes did not mount' }
"@ | Out-Null
}
function CrashEvidence {
    Guest @'
$event=Get-WinEvent -FilterHashtable @{LogName='System';Id=1001;ProviderName='Microsoft-Windows-WER-SystemErrorReporting'} -MaxEvents 1 -ErrorAction SilentlyContinue
$dumps=@(Get-ChildItem C:\Windows\Minidump -Filter *.dmp -File -ErrorAction SilentlyContinue)
$memory=Get-Item C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue
if ($memory) { $dumps+= $memory }
$stamp=@($dumps | Sort-Object FullName | ForEach-Object { $_.FullName+'|'+$_.LastWriteTimeUtc.Ticks }) -join ';'
[pscustomobject]@{Record=$(if($event){$event.RecordId}else{0});Dumps=$stamp} | ConvertTo-Json -Compress
'@
}
StartDriver
$crashEvidence = CrashEvidence
Guest @"
foreach (`$root in (ConvertFrom-Json '$rootJson')) {
    `$volume=Get-Volume -DriveLetter `$root.Substring(0,1)
    if (`$volume.FileSystem -notmatch '^EXT[234]$') { throw 'Not an ext test volume' }
    `$path=Join-Path `$root '$proofName'
    if ([IO.File]::Exists(`$path)) { throw 'Proof already exists' }
    [IO.File]::WriteAllBytes(`$path,[byte[]](3,7,17,37,79,163,251))
}
"@ | Out-Null
for ($cycle=1; $cycle -le $Cycles; $cycle++) {
    $before=(Get-VM -Name $TestEnv.Vm).Uptime
    Guest 'shutdown.exe /r /t 0; if ($LASTEXITCODE) { throw "Restart request failed" }' | Out-Null
    $wait=[Diagnostics.Stopwatch]::StartNew()
    while ((Get-VM -Name $TestEnv.Vm).Uptime -ge $before -and $wait.Elapsed.TotalSeconds -lt 90) {
        Start-Sleep -Milliseconds 500
    }
    if ((Get-VM -Name $TestEnv.Vm).Uptime -ge $before) {
        throw 'Windows did not restart; no forced reset will be attempted'
    }
    $ready=$false
    while ($wait.Elapsed.TotalSeconds -lt 180) {
        $hello=@(& ssh @sshOptions -o ConnectTimeout=3 $target 'echo ready' 2>$null)
        if ($LASTEXITCODE -eq 0 -and $hello -contains 'ready') { $ready=$true; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $ready) { throw 'Guest SSH did not return after the Windows restart' }
    # WER events may arrive late; changed dump files independently fail the run.
    $evidence=CrashEvidence
    if ($evidence -ne $crashEvidence) { throw 'A new bugcheck event or dump appeared during restart' }
    StartDriver
    Guest @"
foreach (`$root in (ConvertFrom-Json '$rootJson')) {
    `$data=[IO.File]::ReadAllBytes((Join-Path `$root '$proofName'))
    if ((`$data -join ',') -ne '3,7,17,37,79,163,251') { throw 'Restart changed the proof data' }
}
"@ | Out-Null
    "REBOOT: cycle $cycle passed, data preserved, no new bugcheck"
}
Guest @"
foreach (`$root in (ConvertFrom-Json '$rootJson')) {
    [IO.File]::Delete((Join-Path `$root '$proofName'))
}
"@ | Out-Null
"REBOOT: $Cycles clean Windows restarts: ALL PASSED"
