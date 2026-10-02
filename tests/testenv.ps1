# SPDX-License-Identifier: GPL-2.0-only
# testenv.ps1 - test environment settings shared by the host-side scripts.
#
# Defaults describe a Hyper-V guest reachable over ssh. Machine-specific values
# (VM name, ssh key, test image, signing certificate) go into testenv.local.psd1
# next to this file, which is not tracked by git, e.g.:
#
#   @{ Vm = 'Win11'; SshKey = 'C:\Users\me\.ssh\id_vm'; Vhd = 'D:\images\ext4-test.vhdx' }
#
# Dot-source it:  . "$PSScriptRoot\testenv.ps1"   -> $TestEnv hashtable + helpers

$TestEnv = @{
    Vm          = 'Win11'                   # Hyper-V guest used for tests
    User        = 'Administrator'           # ssh account in the guest
    SshKey      = Join-Path $HOME '.ssh\id_ed25519'
    Vhd         = Join-Path $PSScriptRoot 'ext4-test.vhdx'
    VhdSize     = 1073741824                # identifies the test disk inside WSL
    Service     = 'ext4'                    # driver service name
    Driver      = 'ext4.sys'
    CertSubject = 'CN=Ext4Fsd Test Signing' # test-signing certificate, CurrentUser\My
    ScsiSlot    = 1                         # SCSI location of the test disk in the guest
    LuksVhd     = Join-Path $PSScriptRoot 'luks-test.vhdx'   # -Luks: built by luks-image.sh when missing
    LuksSize    = 3221225472                # identifies the LUKS test disk inside WSL
    LuksPass    = '<TEST_LUKS_PASSPHRASE>'  # passphrase of the LUKS test disk: set it in testenv.local.psd1
    FeaturesVhd = Join-Path $PSScriptRoot 'features-test.vhdx'  # -Features: rebuilt by features-image.sh on every run
    FeaturesSize = 536870912                # identifies the features test disk inside WSL
    RobustVhd   = Join-Path $PSScriptRoot 'robust-test.vhdx'    # robust.ps1: the corrupted images, a batch at a time
    RobustSize  = 314572800                 # identifies the robustness disk inside WSL and the guest
    RobustCorpus = '~/robust'               # WSL: img/<test>.img + list.txt (robust-corpus.sh)
}
$local = Join-Path $PSScriptRoot 'testenv.local.psd1'
if (Test-Path $local) {
    $over = Import-PowerShellDataFile $local
    foreach ($k in $over.Keys) { $TestEnv[$k] = $over[$k] }
}

function Get-TestVmIp {
    $ip = (Get-VM $TestEnv.Vm | Get-VMNetworkAdapter).IPAddresses |
          Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+$' } | Select-Object -First 1
    if (-not $ip) { throw "VM $($TestEnv.Vm) has no IPv4 address yet" }
    return $ip
}

function Get-SshOptions {
    @('-o', 'StrictHostKeyChecking=no', '-o', 'ConnectTimeout=10', '-o', 'BatchMode=yes', '-i', $TestEnv.SshKey)
}

function Find-SignTool {
    # highest Windows Kits\10\bin\<version>\x64\signtool.exe, compared as [version]
    $bin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    Get-ChildItem $bin -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'x64\signtool.exe')) } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1 |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' }
}
