# Case folding tables

A casefolded ext4 directory hashes and compares names in their NFDICF form
(Unicode 12.1): full case folding, then full canonical decomposition, with
default-ignorable code points dropped. `make-utf8data.py` builds the tables of
`src/unicode/utf8data.c` from the Unicode Character Database; the normalizer in
`src/unicode/utf8norm.c` emits the bytes Linux emits, combining marks ordered
by class. All of it is MIT-licensed, written from the behaviour it must match
rather than from the Linux code; the tables hold Unicode data.

## The normalizer

Linux hashes whatever its normalizer emits, including for input that is not
valid UTF-8, so the definition matters byte for byte. The string is read as
tokens - a character of class k (one the tables leave alone, or one character
of an expansion), an ignorable (expands to nothing), a stray continuation byte,
invalid input, the end. Outside a run of combining marks a class-0 character
is emitted, an ignorable skipped, a stray byte emitted. A mark starts a run;
passes go over it, the first emitting no character, each later one the marks
of the next class in their order; every pass emits the stray bytes it crosses.
A class-0 character, an ignorable or the end closes a pass; after the last
class it is read again outside the run. `utf8norm.c` states the same rules.

## Representation

Linux stores the forms in a trie that is walked over the bits of each UTF-8
sequence: about 64 KB for both normalizations and every Unicode version since
3.1, of which the 12.1 NFDICF trie alone reaches 52 KB. Three facts, checked by
the generator against Linux's `fs/unicode/utf8data.c_shipped`, allow a smaller
and faster form:

1. The trie accepts exactly strict UTF-8 - every scalar value and nothing else -
   and reads every byte of a sequence. A strict decoder followed by a code point
   lookup finds the same leaf for every input and rejects the same inputs.
2. Unicode 12.1 is the newest version in the trie's age table, so the
   normalizer's test for code points newer than the requested version never
   holds. Leaves carry no version.
3. A leaf is the canonical combining class, or DECOMPOSE followed by the
   folded form. There are 2644 distinct leaves; each is stored once.

A code point reaches its leaf offset through four lookups: 2048-code-point
pages, then blocks of 32, 8 and 8 entries, identical blocks stored once. The
tables take 29 329 bytes instead of 64 256, and a name folds about 2.5 times
faster (`tests/unicode-model.ps1` reports both implementations).

Strict mode follows Linux's `utf8_validate()` (`utf8nlen()`): each character
position, stepping by the length its first byte announces, must start a
scalar value. The trie-era driver checked validity through the folding cursor,
which passes stray continuation bytes after a character.

## Reproducing the tables

Download `UnicodeData.txt`, `CaseFolding.txt` and `DerivedCoreProperties.txt`
of https://www.unicode.org/Public/12.1.0/ucd/ into one directory. With a Linux
6.6 `fs/unicode/utf8data.c_shipped` (commit `c117aa7` of this repository holds
it as `src/unicode/utf8data.c`), the generator also proves the facts above and
compares all 1 112 064 leaves:

```powershell
python tools\unicode\make-utf8data.py <ucd-12.1.0> --linux <utf8data.c_shipped> --out src\unicode\utf8data.c
pwsh tests\unicode-model.ps1          # every code point, every string of up to 3 bytes
pwsh tests\unicode-model.ps1 -Full    # and every 4-byte string
```

`--bits` chooses the stage split (default `3,3,5`); the generator reads every
code point back through the emitted tables before it writes them.
