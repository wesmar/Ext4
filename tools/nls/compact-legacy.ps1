# SPDX-License-Identifier: GPL-2.0-only
# Mechanically compact existing GPL-covered code-page data without changing mappings.
param(
    [Parameter(Mandatory)][string]$Reference,
    [string]$Destination = (Join-Path $PSScriptRoot '../../src/nls'),
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Reference = (Resolve-Path -LiteralPath $Reference).Path
$Destination = (Resolve-Path -LiteralPath $Destination).Path
$arrayPattern = '(?ms)^static\s+(wchar_t|unsigned char)\s+(\w+)\s*\[(\d+)\]\s*=\s*\{(.*?)\};'
$summary = @()

function Numbers([string]$text, [int]$count) {
    $text = [regex]::Replace($text, '(?s)/\*.*?\*/|//[^\r\n]*', '')
    $v = @([regex]::Matches($text, '0[xX][0-9a-fA-F]+|\b\d+\b') | ForEach-Object {
        if ($_.Value.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
            [Convert]::ToInt32($_.Value.Substring(2), 16)
        } else { [int]$_.Value }
    })
    if ($v.Count -gt $count) { throw 'Too many array initializers.' }
    if ($v.Count -lt $count) { $v += @(0) * ($count - $v.Count) }
    return ,$v
}

function EmitValues([string]$type, [string]$name, $values, [int]$digits) {
    if (!$values.Count) { $values = @(0) }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("static const $type $name[$($values.Count)] = {")
    for ($i = 0; $i -lt $values.Count; $i += 8) {
        $items = @($values[$i..([Math]::Min($i+7, $values.Count-1))] | ForEach-Object {
            '0x' + ([uint32]$_).ToString("x$digits") + $(if ($digits -eq 8) { 'u' } else { '' })
        })
        $lines.Add('    ' + ($items -join ', ') + ',')
    }
    $lines.Add('};')
    $lines -join "`n"
}

function CompactDirectories([string]$source, [string]$fileName) {
    # A page directory holds 256 pointers, most of them NULL: 2 KiB on x64
    # for a handful of pages. It becomes a byte per high byte - the page's
    # place in a list of the pages that exist, plus one; 0 for none - and
    # the list. Duplicate entries keep pointing at one page.
    $pattern = '(?s)static const (\w+(?: \w+)?) \*const (page_\w+)\[256\]\s*=\s*\{(.*?)\};'
    foreach ($match in @([regex]::Matches($source, $pattern) | Sort-Object Index -Descending)) {
        $type = $match.Groups[1].Value
        $name = $match.Groups[2].Value
        $entries = @(([regex]::Replace($match.Groups[3].Value, '(?s)/\*.*?\*/', '') -split ',') |
            ForEach-Object { $_.Trim() } | Where-Object { $_ })
        if ($entries.Count -gt 256) { throw "Directory too long: $fileName/$name" }
        $list = [Collections.Generic.List[string]]::new()
        $index = [int[]]::new(256)
        for ($i = 0; $i -lt $entries.Count; $i++) {
            if ($entries[$i] -eq 'NULL') { continue }
            if ($entries[$i] -notmatch '^\w+$') { throw "Unexpected directory entry: $fileName/$name/$i" }
            if (!$list.Contains($entries[$i])) { $list.Add($entries[$i]) }
            $index[$i] = $list.IndexOf($entries[$i]) + 1
        }
        if ($list.Count -gt 255) { throw "Too many pages for a byte index: $fileName/$name" }
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add("/* ${name}: the page of each high byte, as its place in ${name}_list plus one (0: none) */")
        $lines.Add("static const NLS_BYTE ${name}_index[256] = {")
        for ($i = 0; $i -lt 256; $i += 16) {
            $lines.Add('    ' + ((@($index[$i..($i+15)] | ForEach-Object { '0x' + $_.ToString('x2') })) -join ', ') + ',')
        }
        $lines.Add('};')
        $lines.Add("static const $type *const ${name}_list[$([Math]::Max(1, $list.Count))] = {")
        $lines.Add('    ' + $(if ($list.Count) { $list -join ', ' } else { 'NULL' }) + ',')
        $lines.Add('};')
        $source = $source.Remove($match.Index, $match.Length).Insert($match.Index, ($lines -join "`n"))
        $source = [regex]::Replace($source, "\b$name\[([^\]]+)\]", "NLS_PAGE($name, `$1)")
    }
    if ($source.Contains('NLS_PAGE(') -and !$source.Contains('#include "CharsetMap.h"')) {
        if (!$source.Contains('#include <linux/errno.h>')) { throw "No include to follow: $fileName" }
        $source = $source.Replace('#include <linux/errno.h>', "#include <linux/errno.h>`n#include `"CharsetMap.h`"")
    }
    return $source
}

foreach ($file in @(Get-ChildItem -LiteralPath $Reference -Filter 'nls_cp*.c' | Sort-Object Name)) {
    $source = [IO.File]::ReadAllText($file.FullName).Replace("`r`n", "`n")
    $doubleByte = $source -match 'wchar_t \*charset2uni;'
    $arrays = [regex]::Matches($source, $arrayPattern)
    if (!$arrays.Count) { throw "No original arrays: $($file.Name)" }
    $wordValues = [Collections.Generic.List[uint32]]::new()
    $byteValues = [Collections.Generic.List[uint32]]::new()
    $poolKeys = @{}
    $exceptionKeys = @{}
    $exceptions = [Collections.Generic.List[object]]::new()
    $replacements = [Collections.Generic.List[object]]::new()
    $wordArrays = @{}
    $byteArrays = @{}
    $plainBytes = 0
    $sliceCount = 0
    $sparseCount = 0
    $simpleCount = 0
    $decodeValues = $null
    $encodeAscii = $null

    foreach ($array in $arrays) {
        $name = $array.Groups[2].Value
        $count = [int]$array.Groups[3].Value
        $values = Numbers $array.Groups[4].Value $count
        if ($name -in @('charset2lower', 'charset2upper')) {
            $lines = [Collections.Generic.List[string]]::new()
            $lines.Add("static const unsigned char $name[$count] = {")
            for ($i = 0; $i -lt $count; $i += 8) {
                $run = $true
                for ($j = 1; $j -lt 8; $j++) {
                    if ($values[$i+$j] -ne $values[$i]+$j) { $run = $false; break }
                }
                $items = if ($run) { 'NLS_ASCENDING_8(0x' + $values[$i].ToString('x2') + ')' }
                    else { (@($values[$i..($i+7)] | ForEach-Object { '0x' + $_.ToString('x2') }) -join ', ') }
                $lines.Add('    ' + $items + ',')
            }
            $lines.Add('};')
            $replacements.Add(@{ Start=$array.Index; Length=$array.Length; Text=($lines -join "`n") })
            continue
        }
        if ($count % 32) { throw "Unsupported slice size: $name" }
        $word = $array.Groups[1].Value -eq 'wchar_t'
        if ($doubleByte -and !$word -and $count -eq 512) {
            # A two-byte result is one map value, not two independent lookups.
            $paired = @()
            for ($i=0; $i -lt $count; $i+=2) {
                $paired += ($values[$i] -shl 8) -bor $values[$i+1]
            }
            $values = $paired
            $count = 256
            $word = $true
        }
        $width = if ($word) { 2 } else { 1 }
        $kind = if ($word) { 'w' } else { 'b' }
        $pool = $byteValues
        if ($word) { $pool = $wordValues }
        if ($word) { $wordArrays[$name] = $true } else { $byteArrays[$name] = $true }
        if ($name -eq 'charset2uni') { $decodeValues = $values }
        if ($name -eq 'page00' -and !$word) { $encodeAscii = $values }
        $plainBytes += $count * $width
        $slices = [Collections.Generic.List[uint32]]::new()
        for ($begin = 0; $begin -lt $count; $begin += 32) {
            $part = @($values[$begin..($begin+31)])
            $frequency = @{}
            for ($i = 0; $i -lt 32; $i++) {
                $constant = '0:' + $part[$i]
                $frequency[$constant] = 1 + [int]$frequency[$constant]
                $base = $part[$i] - $i
                if ($base -ge 0 -and $base + 31 -le $(if ($word) {65535} else {255})) {
                    $ascending = '1:' + $base
                    $frequency[$ascending] = 1 + [int]$frequency[$ascending]
                }
            }
            $best = $frequency.GetEnumerator() | Sort-Object @{Expression='Value';Descending=$true}, @{Expression='Key'} | Select-Object -First 1
            $mode, $baseText = $best.Key -split ':'
            $base = [int]$baseText
            $linear = [int]$mode
            $different = [Collections.Generic.List[uint32]]::new()
            [uint32]$mask = 0
            for ($i = 0; $i -lt 32; $i++) {
                if ($part[$i] -ne $base + $linear*$i) {
                    $mask = $mask -bor ([uint32]1 -shl $i)
                    $different.Add([uint32]$part[$i])
                }
            }
            if (!$different.Count) {
                $slices.Add(([uint32]$(if ($linear) {2147483648} else {1073741824})) -bor [uint32]$base)
                $simpleCount++
                continue
            }
            $sparse = 12 + $different.Count*$width -lt 32*$width
            $literals = if ($sparse) { @($different) } else { $part }
            $key = $kind + ':' + ($literals -join ',')
            if ($poolKeys.ContainsKey($key)) { $offset = $poolKeys[$key] }
            else {
                $offset = $pool.Count
                foreach ($value in $literals) { $pool.Add([uint32]$value) }
                $poolKeys[$key] = $offset
            }
            if ($sparse) {
                $basis = $base -bor ($linear -shl 16)
                $key = "$kind/$mask/$basis/$offset"
                if ($exceptionKeys.ContainsKey($key)) { $entry = $exceptionKeys[$key] }
                else {
                    $entry = $exceptions.Count
                    $exceptions.Add(@{ Mask=$mask; Base=$basis; Offset=$offset })
                    $exceptionKeys[$key] = $entry
                }
                $slices.Add(([uint32]3221225472) -bor [uint32]$entry)
                $sparseCount++
            } else { $slices.Add([uint32]$offset) }
        }
        # Independently reconstruct every slot before writing any generated source.
        for ($index = 0; $index -lt $count; $index++) {
            [uint32]$slice = $slices[$index -shr 5]
            $local = $index -band 31
            $payload = $slice -band [uint32]1073741823
            $actual = switch ($slice -shr 30) {
                0 { $pool[$payload+$local] }
                1 { $payload }
                2 { $payload+$local }
                3 {
                    $entry = $exceptions[$payload]
                    if (($entry.Mask -band ([uint32]1 -shl $local)) -eq 0) {
                        ($entry.Base -band 65535) + (($entry.Base -shr 16) -band 1)*$local
                    } else {
                        $rank = 0
                        for ($i=0; $i -lt $local; $i++) { if ($entry.Mask -band ([uint32]1 -shl $i)) { $rank++ } }
                        $pool[$entry.Offset+$rank]
                    }
                }
            }
            if ($actual -ne $values[$index]) { throw "Reconstruction mismatch: $($file.Name)/$name/$index" }
        }
        $sliceCount += $slices.Count
        $replacements.Add(@{ Start=$array.Index; Length=$array.Length; Text=(EmitValues 'NLS_BITS' $name @($slices) 8) })
    }
    foreach ($replacement in @($replacements | Sort-Object Start -Descending)) {
        $source = $source.Remove($replacement.Start, $replacement.Length).Insert($replacement.Start, $replacement.Text)
    }
    $source = $source.Replace('static unsigned char *page_uni2charset[256]', 'static const NLS_BITS *const page_uni2charset[256]')
    $source = $source.Replace('static wchar_t *page_charset2uni[256]', 'static const NLS_BITS *const page_charset2uni[256]')
    $source = $source.Replace('unsigned char *uni2charset;', 'const NLS_BITS *uni2charset;')
    $source = $source.Replace('wchar_t *charset2uni;', 'const NLS_BITS *charset2uni;')
    $source = [regex]::Replace($source, '(?m)^static unsigned char (u2c_\w+)\[([^\]\r\n]+)\]\[(\d+)\]', 'static const unsigned char $1[$2][$3]')
    $source = [regex]::Replace($source, '(?m)^    /\* \.charset2(lower|upper) \*/', '    /* .charset2$1 */')
    if ($doubleByte) {
        $source = $source.Replace('const NLS_BITS *uni2charset;', "const NLS_BITS *uni2charset;`n    NLS_WORD code;")
        $pairPattern = 'out\[0\]\s*=\s*uni2charset\[cl\s*\*\s*2\];\s*out\[1\]\s*=\s*uni2charset\[cl\s*\*\s*2\s*\+\s*1\];'
        if (![regex]::IsMatch($source,$pairPattern)) { throw "Two-byte encoder not found: $($file.Name)" }
        $source = [regex]::Replace($source, $pairPattern,
            "code = NlsMapWord(uni2charset, MapWords, MapExceptions, cl);`n        out[0] = (unsigned char)(code >> 8);`n        out[1] = (unsigned char)code;")
    } else {
        $source = [regex]::Replace($source, '\buni2charset\[([^\]]+)\]', 'NlsMapByte(uni2charset, MapBytes, MapExceptions, $1)')
    }
    $source = [regex]::Replace($source, '(?m)(\*uni\s*=\s*)charset2uni\[([^\]]+)\]', '${1}NlsMapWord(charset2uni, MapWords, MapExceptions, $2)')
    if ($decodeValues) {
        # The decoder's direct table has no page directory. Preserve even NUL/error output.
        $exceptionsAscii = @(0..127 | Where-Object { $decodeValues[$_] -ne $_ })
        $identity = if (!$exceptionsAscii.Count) { '*uni = *rawstring;' } else {
            '*uni = ' + (($exceptionsAscii | ForEach-Object { "*rawstring == $_ ? 0x$($decodeValues[$_].ToString('x4')) : " }) -join '') + '*rawstring;'
        }
        $marker = '    *uni = NlsMapWord(charset2uni, MapWords, MapExceptions, *rawstring);'
        if (!$source.Contains($marker)) { throw "Decoder marker missing: $($file.Name)" }
        $source = $source.Replace($marker, "    if (*rawstring < 0x80) {`n        $identity`n    } else {`n        *uni = NlsMapWord(charset2uni, MapWords, MapExceptions, *rawstring);`n    }")
        if (!$encodeAscii) { throw "Missing single-byte reverse map: $($file.Name)" }
        # The longest exact ASCII identity run bypasses the reverse-page lookup.
        $bestStart=0; $bestLength=0; $start=1
        while ($start -lt 128) {
            if ($encodeAscii[$start] -ne $start) { $start++; continue }
            $end=$start
            while ($end -lt 128 -and $encodeAscii[$end] -eq $end) { $end++ }
            if ($end-$start -gt $bestLength) { $bestStart=$start; $bestLength=$end-$start }
            $start=$end
        }
        if ($bestLength) {
            $marker = '    uni2charset = page_uni2charset[ch];'
            $fast = "    /* Exact identity run from the original reverse map. */`n    if (uni >= $bestStart && uni < $($bestStart+$bestLength)) {`n        out[0] = (unsigned char)uni;`n        return 1;`n    }`n`n"
            if (!$source.Contains($marker)) { throw 'Reverse-map adapter not found.' }
            $source = $source.Replace($marker, $fast+$marker)
        }
    }
    $data = [Collections.Generic.List[string]]::new()
    $data.Add('/* Generated from the original mappings; all implicit zero slots are retained. */')
    $data.Add((EmitValues 'NLS_WORD' 'MapWords' @($wordValues) 4))
    $data.Add((EmitValues 'NLS_BYTE' 'MapBytes' @($byteValues) 2))
    $data.Add("static const NLS_MAP_EXCEPTIONS MapExceptions[$([Math]::Max(1,$exceptions.Count))] = {")
    if (!$exceptions.Count) { $data.Add('    { 0, 0, 0 },') }
    foreach ($entry in $exceptions) {
        $data.Add('    { 0x' + ([uint32]$entry.Mask).ToString('x8') + 'u, 0x' + ([uint32]$entry.Base).ToString('x8') + 'u, ' + $entry.Offset + 'u },')
    }
    $data.Add('};')
    $source = $source.Replace('#include <linux/errno.h>', '#include <linux/errno.h>' + "`n" + '#include "CharsetMap.h"' + "`n`n" + ($data -join "`n"))
    $source = CompactDirectories $source $file.Name
    $storage = $sliceCount*4 + $wordValues.Count*2 + $byteValues.Count + $exceptions.Count*12
    $summary += [pscustomobject]@{File=$file.Name;Original=$plainBytes;Compact=$storage;Slices=$sliceCount;Affine=$simpleCount;Sparse=$sparseCount}
    if (!$DryRun) {
        # Generated data and mechanical adapter substitutions, validated above.
        [IO.File]::WriteAllText((Join-Path $Destination $file.Name), $source.Replace("`n","`r`n"), [Text.UTF8Encoding]::new($false))
    }
}
$summary | Format-Table -AutoSize
$summary | Measure-Object Original,Compact,Slices,Affine,Sparse -Sum | Select-Object Property,Sum

# Other registered NLS maps retain their representation, but their data is immutable.
# UTF-8's identity array is initialized at registration and has no initializer here.
# nls_base.c is the NLS registry, maintained in place: only its default table's
# page directory is compacted below.
foreach ($file in @(Get-ChildItem -LiteralPath $Reference -Filter 'nls_*.c' |
        Where-Object { $_.Name -notlike 'nls_cp*' -and $_.Name -ne 'nls_base.c' })) {
    $source = [IO.File]::ReadAllText($file.FullName).Replace("`r`n", "`n")
    $source = [regex]::Replace($source,
        '(?m)^static (wchar_t|unsigned char) (\w+)\[([^\r\n]*?)\]\s*=\s*\{',
        'static const $1 $2[$3] = {')
    $source = [regex]::Replace($source,
        '(?m)^static (wchar_t|unsigned char) \*(page_\w+)\[([^\r\n]*?)\]\s*=\s*\{',
        'static const $1 *const $2[$3] = {')
    $source = $source.Replace('unsigned char *uni2charset;', 'const unsigned char *uni2charset;')
    $source = $source.Replace('wchar_t *charset2uni;', 'const wchar_t *charset2uni;')
    $source = CompactDirectories $source $file.Name
    if (!$DryRun) {
        [IO.File]::WriteAllText((Join-Path $Destination $file.Name), $source.Replace("`n","`r`n"), [Text.UTF8Encoding]::new($false))
    }
}

$base = Join-Path $Destination 'nls_base.c'
$source = [IO.File]::ReadAllText($base).Replace("`r`n", "`n")
$source = CompactDirectories $source 'nls_base.c'
if (!$DryRun) {
    [IO.File]::WriteAllText($base, $source.Replace("`n","`r`n"), [Text.UTF8Encoding]::new($false))
}
