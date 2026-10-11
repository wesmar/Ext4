# SPDX-License-Identifier: GPL-2.0-only
# Fresh directory aliases: enumerate a link before the target is opened.
param([string]$Root = 'U:\symlink-walk', [switch]$Prepare, [int]$Repeat = 5,
    [int]$Expected = 64, [switch]$LinuxFixture)
$ErrorActionPreference = 'Stop'
$fail = 0
if ($Prepare) {
    if (Test-Path -LiteralPath $Root) { throw 'Use a new fixture root.' }
    foreach ($part in 'lib','lib32','bin') {
        $target = Join-Path $Root "usr\$part"
        [void][IO.Directory]::CreateDirectory($target)
        for ($i=0; $i -lt 64; $i++) {
            [IO.File]::WriteAllText((Join-Path $target ("entry-{0:D4}-payload.txt" -f $i)),"payload/$part/$i")
        }
        & cmd.exe /d /c "mklink /D `"$Root\$part`" `"usr\$part`"" | Out-Host
        if ($LASTEXITCODE) { throw 'Relative directory link creation failed.' }
    }
}
$paths = [ordered]@{lib='lib'; lib32='lib32'; bin='bin'}
if ($LinuxFixture) { $paths['var\run']='lib'; $paths['var\lock']='lib32'; $paths['chain']='lib' }
foreach ($part in $paths.Keys) {
    $link = Join-Path $Root $part
    $targetName = $paths[$part]
    $target = Join-Path $Root "usr\$targetName"
    for ($i=0; $i -lt $Repeat; $i++) {
        try {
            $through = @([IO.Directory]::GetFileSystemEntries($link) | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object)
            $direct = @([IO.Directory]::GetFileSystemEntries($target) | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object)
            if ($through.Count -ne $Expected -or (Compare-Object $through $direct)) { throw "Listing mismatch: link=$($through.Count), target=$($direct.Count), expected=$Expected" }
            if ([IO.File]::ReadAllText((Join-Path $link 'entry-0000-payload.txt')) -cne "payload/$targetName/0") { throw 'Content mismatch.' }
            "  ok $part pass ${i}: $Expected names and payload"
        } catch {
            $fail++
            "  FAIL $part pass ${i}: $($_.Exception.Message)"
        }
    }
    try {
        $name = 'write-' + [Guid]::NewGuid().ToString('N') + '.txt'
        $created = Join-Path $link $name
        $renamed = Join-Path $link ('renamed-' + $name)
        [IO.File]::WriteAllText($created,'alias write proof')
        if ([IO.File]::ReadAllText((Join-Path $target $name)) -cne 'alias write proof') { throw 'Alias write did not reach the target.' }
        [IO.File]::Move($created,$renamed)
        if ([IO.File]::Exists((Join-Path $target $name)) -or
            [IO.File]::ReadAllText((Join-Path $target ('renamed-' + $name))) -cne 'alias write proof') { throw 'Alias rename mismatch.' }
        [IO.File]::Delete($renamed)
        if ([IO.Directory]::GetFiles($target).Length -ne $Expected) { throw 'Alias delete changed the target contents.' }
        "  ok ${part}: write, rename and delete through alias"
    } catch {
        $fail++
        "  FAIL $part mutation: $($_.Exception.Message)"
    }
}
"SYMLINK-WALK: $fail failed"
exit $fail
