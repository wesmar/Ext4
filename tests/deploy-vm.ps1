#Requires -Version 7.0
# deploy-vm.ps1 - install or hot-swap bin\ext4.sys in the test guest.
#
# A copy of the driver is signed with the test-signing certificate (the build
# output itself stays unsigned), copied to the guest, and the guest then stops
# the service, replaces the image and starts it again. The service is created
# on first use (file system driver, demand start). A competing file system
# driver that still claims ext volumes (the old Ext2Fsd) is stopped first.
#
#   pwsh tests\deploy-vm.ps1 [-Ip a.b.c.d] [-NoStart]
param([string]$Ip = '', [switch]$NoStart)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\testenv.ps1"
if (-not $Ip) { $Ip = Get-TestVmIp }
$ssh = Get-SshOptions
$target = "$($TestEnv.User)@$Ip"

$bin = Join-Path (Split-Path $PSScriptRoot) "bin\$($TestEnv.Driver)"
if (-not (Test-Path $bin)) { throw "$bin not found - run build.ps1 first" }
$signtool = Find-SignTool
if (-not $signtool) { throw 'signtool.exe not found in Windows Kits\10\bin' }
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $TestEnv.CertSubject -and $_.HasPrivateKey } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) { throw "no certificate '$($TestEnv.CertSubject)' with a private key in CurrentUser\My" }

$stage = Join-Path ([IO.Path]::GetTempPath()) "ext4-deploy"
New-Item -ItemType Directory $stage -Force | Out-Null
$signed = Join-Path $stage $TestEnv.Driver
Copy-Item $bin $signed -Force
& $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint $signed | Out-Null
if ($LASTEXITCODE) { throw "signtool failed ($LASTEXITCODE)" }
& scp -q @ssh $signed "${target}:C:/Windows/Temp/$($TestEnv.Driver)"
if ($LASTEXITCODE) { throw "scp failed ($LASTEXITCODE)" }

$svc = $TestEnv.Service
$drv = $TestEnv.Driver
$start = if ($NoStart) { '$false' } else { '$true' }
$guest = @"
`$ErrorActionPreference = 'Stop'
foreach (`$old in 'ext2fsd') {
    `$o = Get-Service `$old -ErrorAction SilentlyContinue
    if (`$o -and `$o.Status -ne 'Stopped') { sc.exe stop `$old | Out-Null; `$o.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60)); "stopped competing driver `$old" }
}
`$sw = [Diagnostics.Stopwatch]::StartNew()
`$s = Get-Service $svc -ErrorAction SilentlyContinue
if (`$s -and `$s.Status -ne 'Stopped') { sc.exe stop $svc | Out-Null; `$s.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(90)) }
`$stop = `$sw.ElapsedMilliseconds
Copy-Item C:\Windows\Temp\$drv C:\Windows\System32\drivers\$drv -Force
[IO.File]::Delete('C:\Windows\Temp\$drv')
if (-not `$s) {
    sc.exe create $svc type= filesys start= demand error= normal binPath= System32\drivers\$drv DisplayName= "ext4 file system driver" | Out-Null
    "service $svc created"
}
if ($start) { sc.exe start $svc | Out-Null; (Get-Service $svc).WaitForStatus('Running', [TimeSpan]::FromSeconds(30)) }
"stop {0} ms, now {1}, {2:N0} bytes" -f `$stop, (Get-Service $svc).Status, (Get-Item C:\Windows\System32\drivers\$drv).Length
"@
$enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($guest))
& ssh @ssh $target "powershell -NoProfile -NonInteractive -EncodedCommand $enc"
if ($LASTEXITCODE) { throw "guest deploy failed ($LASTEXITCODE)" }
