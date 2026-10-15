# Legacy character mappings

`compact-legacy.ps1` generates the compact representation from the original
GPL-covered NLS sources. `src/nls/CharsetMap.h` implements the common lookup.
The generated files retain their attribution and licence notices.

## Representation

Each 32-input slice selects one of four forms: a constant, an ascending run,
a dense literal range, or an affine default with explicit exceptions. Sparse
slices use a bit mask and its population count to locate an exception without
a search loop. Lookup is bounded O(1) and requires no optional CPU instruction.
Literal ranges are deduplicated within each code page.

The generator preserves both conversion directions independently. Duplicate
and one-way mappings are data, not inferred inverses. CJK reverse maps pack
the two output bytes into one 16-bit value; the existing adapter retains its
single-byte fallbacks and error behavior. Omitted C initializers remain zero.
Case maps retain the contiguous 256-byte interface and the original values.

A page directory of 256 pointers - 2 KiB on x64 for a handful of pages - is
emitted as `page_X_index[256]`, each high byte's place in `page_X_list` plus
one (0 for no page), and the list of pages that exist; `NLS_PAGE()` reads it.
`nls_base.c`, the NLS registry, is maintained in place: the generator only
compacts the page directory of its default table.

Numeric maps and pointer directories are `const`. Registration objects remain
mutable because the NLS list updates their `next` field. The generated tables
stay resident; this change introduces neither pageable data nor lazy allocation.

## Reproducing the comparison

Commit `4706af9` is the public source baseline before this representation change.
Use a separate reference directory, never the working production directory:

```powershell
$reference = Join-Path $env:TEMP ('ext4-nls-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $reference | Out-Null
git archive 4706af9 src/nls -o "$reference\baseline.zip"
Expand-Archive -LiteralPath "$reference\baseline.zip" -DestinationPath $reference
$original = Join-Path $reference 'src\nls'

# Inspect storage and verify reconstruction without rewriting the source tree.
pwsh tools\nls\compact-legacy.ps1 -Reference $original -DryRun

# Compile both actual converter implementations and compare every input.
pwsh tests\nls-model.ps1 -Reference $original -Architecture x64 -Benchmark
pwsh tests\nls-model.ps1 -Reference $original -Architecture x86
```

The model covers all BMP encodes and all two-byte decode inputs for 37 directly
implemented code pages, at bounds -1, 0, 1, 2 and 3. It compares return codes,
output bytes including error writes, aliases and every lower/upper entry. It
also tests the production dentry builder with allocation and conversion faults.
Delegating EUC-JP/KOI8-RU modules and UTF-8 retain their original representation.

`nls-model.exe --export-probes` exports representable filename samples from the
reference converters. `tests/nls-vm.ps1` uses these in a disposable Windows
guest: live volume-property changes, create/read/list/rename/delete and refusal
of an unrepresentable name. It restores the original properties in `finally`.
The path scaffolding deliberately uses letters: some retained Arabic mappings
do not encode Western digits as ASCII. A standalone byte can also behave
differently before the next character in a DBCS string.

Compaction trades extra integer operations in non-ASCII lookup for fewer stored
values. The benchmark reports conversion cost separately from filesystem I/O;
it is not a claim of higher disk throughput. ASCII fast paths remain direct.
