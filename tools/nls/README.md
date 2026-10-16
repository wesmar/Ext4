# Code page converters

`make-nls.py` builds the 37 converters `src/nls/nls_<code page>.c` from the
published mapping tables; `src/nls/CharsetMap.c` is the conversion code they
share. Both are MIT-licensed. The tables are data of unicode.org MAPPINGS:
Microsoft's code page tables (`VENDORS/MICSFT`), ISO/IEC 8859 (`ISO8859`) and
KOI8 (`VENDORS/MISC`).

## The mapping

Each code page maps by the published table, by its family's rules and by the
lines of `established/<code page>.txt`:

| Family | Code pages | Rules |
|---|---|---|
| single byte | ASCII, DOS, Windows, ISO 8859, KOI8 | every byte through the table; a character writes as the lowest byte that reads as it |
| double byte | 936, 949, 950 | two-byte codes by lead byte; a lead byte without one reads as a single byte; U+0001..U+00FF write as one byte |
| Shift JIS | 932 | as double byte, and bytes A1..DF are U+FF61..U+FF9F both ways; U+00A0..U+00FF write through a page of their own |

`established/` lists, one line each, where the driver's mapping differs from
the current tables. The converters it has always used were built from
earlier revisions of some of them, and names already on disk depend on every
mapping, so these differences are kept on purpose: compatibility ideographs
in 936, half-width jamo in 949, one-way characters and duplicate choices in
the CJK encoders, older Greek, Hebrew and Thai revisions, Arabic-Indic digits
for bytes 30..39 of ISO 8859-6, and a few entries of the
inherited 1251 and ISO 8859-14 encoders that write a character as an
unrelated byte (`Ó` as `»` in 1251, for example). Removing a line changes
the mapping of that character.

## Representation

A map is pages of 256 entries, reached through a byte per high byte: the
page's number plus one, 0 for none. A page is eight slices of 32 entries,
each one 32-bit word. Constant and ascending slices are the word itself; a
slice with no structure keeps its 32 values in the pool. The rest name an
entry - a mask of exceptions, a base, an offset - and a kind. With `r` the
exceptions and `j` the other positions (members) before position `i`:

| Kind | Member | Exception | Holds |
|---|---|---|---|
| constant | `Base` | pool | unmapped ranges with scattered characters |
| step | `Base + i` | pool | runs with a few exceptions |
| rank | `Base + j` | pool | an extension in its encoder: consecutive codes skipping the base set's characters |
| run | `Base + j` | `Offset + r` | two interleaved runs (UHC Hangul: KS X 1001 and extension codes) |
| narrow | `Base +` byte `j` | pool | values within 256 of each other |
| select | `Base +` position of the `j`-th set bit | pool | an extension in its decoder: rising values with gaps, as a bitmap |

Rank and select are the two directions of one structure. GBK's 14 240 and
UHC's 8 822 extension characters fill, in Unicode order, the code points
their base sets leave free: encoding counts the members before a position,
decoding finds the position of a member. Both are a population count, with
no search; a lookup takes a bounded number of steps and no optional CPU
instruction.

Every slice takes its cheapest form; identical pages, entries and pool runs
are stored once. Before writing anything the generator reads every entry of
every map back through the emitted form, in the same arithmetic as
`CharsetMap.c`.

| | 4706af9 | compact (9cc72e8) | now |
|---|---|---|---|
| CJK tables (932, 936, 949, 950) | 423 KB | 277 KB | 201 KB |
| all 37 code pages | | | 227 KB |
| case maps, never read by the driver | 18 KB | 18 KB | removed |
| `ext4.sys` | 987 KB | 719 KB | 598 KB |

## Reproducing

Download into one directory, keeping the paths of
https://www.unicode.org/Public/MAPPINGS/, the tables `PAGES` in
`make-nls.py` names: `VENDORS/MICSFT/PC/CP437.TXT` ... `CP874.TXT`,
`VENDORS/MICSFT/WINDOWS/CP874.TXT`, `CP932.TXT`, `CP936.TXT`, `CP949.TXT`,
`CP950.TXT`, `CP1250.TXT`, `CP1251.TXT`, `CP1255.TXT`,
`ISO8859/8859-{1,2,3,4,5,6,7,9,13,14,15}.TXT` and
`VENDORS/MISC/KOI8-{R,U}.TXT` and, for EUC-JP, `OBSOLETE/EASTASIA/JIS/JIS0212.TXT`.

```powershell
python tools\nls\make-nls.py <MAPPINGS>                     # writes src\nls\nls_*.c
python tools\nls\make-nls.py <MAPPINGS> --dry-run           # sizes only
```

`--linux <dir>` compares both maps of every code page, entry by entry and
page by page, with the converters the driver inherited (Linux `fs/nls`;
commit `4706af9` holds them under `src/nls`) and stops at the first
difference; `--write-established` rewrites `established/` from that
comparison instead:

```powershell
$reference = Join-Path $env:TEMP ('ext4-nls-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $reference | Out-Null
git archive 4706af9 src/nls -o "$reference\baseline.zip"
Expand-Archive -LiteralPath "$reference\baseline.zip" -DestinationPath $reference
python tools\nls\make-nls.py <MAPPINGS> --linux "$reference\src\nls"

# Compile the inherited and the generated converters; compare every input
pwsh tests\nls-model.ps1 -Reference "$reference\src\nls" -Architecture x64 -Benchmark
pwsh tests\nls-model.ps1 -Reference "$reference\src\nls" -Architecture x86
```

The model covers every BMP encode and every two-byte decode input of the 37
code pages at bounds -1, 0, 1, 2 and 3: return codes, every output byte -
including those written before a failure - and aliases. One difference is
intended: for U+00A0..U+00FF the inherited Shift JIS encoder wrote two bytes
whatever the bound; the new one refuses a bound of 1 (the driver always
passes 16). It also checks koi8-ru, which delegates to koi8-u: its inherited
decoder had the test for the two bytes that differ inverted and read every
other byte as `Ў` or `ў`. EUC-JP goes through Shift JIS by the OSF/JVC
conversion specification; `EucJpTables.h` places the IBM extensions, derived
from CP932 and `OBSOLETE/EASTASIA/JIS/JIS0212.TXT`: a duplicate of a JIS X
0208 character takes its code, a duplicate of an NEC row 13 character or one
outside JIS X 0212 the next G3 user code, the others their JIS X 0212 code.
The model compares it with the converter it replaced on every BMP encode and
every three-byte decode input, 126 567 306 checks in all. The model further runs the production dentry
builder with allocation and conversion faults.

`nls-model.exe --export-probes` exports representable filename samples;
`tests/nls-vm.ps1` uses them in a disposable Windows guest: live volume
property changes, create/read/list/rename/delete and refusal of an
unrepresentable name, the original properties restored in `finally`. Its
path scaffolding uses letters, since some Arabic mappings do not write
Western digits as ASCII.

The benchmark compares the inherited flat arrays with the generated form:
a character costs about 1 ns there and 2 ns (single byte) to 4..7 ns (CJK)
here, the price of 121 KB less resident data. ASCII stays on constant and
ascending slices.
