# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
"""Build the Unicode 12.1 case folding tables of src/unicode/utf8data.c.

usage: make-utf8data.py <ucd-12.1.0 dir> [--out utf8data.c]
                        [--linux utf8data.c_shipped] [--bits 3,3,5]

A casefolded ext4 directory hashes and compares names in their NFDICF
form: the full case folding of CaseFolding.txt (status C and F), then the
full canonical decomposition of UnicodeData.txt, Default_Ignorable_Code_Point
characters dropped (DerivedCoreProperties.txt). Each code point gets a
leaf: its canonical combining class, or DECOMPOSE (255) followed by the
NUL-terminated UTF-8 of its form. Hangul syllables get the marker leaf
DECOMPOSE, 0xFF: the normalizer decomposes them algorithmically.

The leaves are pooled once each. A code point reaches its leaf through
stages that each take some low bits of it (--bits, lowest stage first);
identical blocks are stored once. Every code point is read back through
the tables before anything is written.

--linux checks the result against the NFDICF trie of Linux's
fs/unicode/utf8data.c_shipped (Unicode 12.1), which Linux folds with:
the trie must accept exactly strict UTF-8 and read every byte of each
sequence (so a strict decoder can replace the walk), 12.1 must be the
newest version of its age table (so the leaves' generation carries
nothing), and every code point's leaf must equal the one built here.
"""
import os, re, sys

def option(name, default=None):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default
ucd = sys.argv[1]
target = option('--out')
linux = option('--linux')
bits = [int(x) for x in option('--bits', '3,3,5').split(',')]
DECOMPOSE, HANGUL = 255, 0xFF
SBASE, SCOUNT = 0xAC00, 11172

def lines(name):
    with open(os.path.join(ucd, name), encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line:
                yield [field.strip() for field in line.split(';')]

# UnicodeData.txt: combining classes and canonical decompositions
ccc, nfd = {}, {}
for f in lines('UnicodeData.txt'):
    cp = int(f[0], 16)
    if f[3] != '0':
        ccc[cp] = int(f[3])
    if f[5] and not f[5].startswith('<'):
        nfd[cp] = [int(x, 16) for x in f[5].split()]
# CaseFolding.txt: the full folding
fold = {int(f[0], 16): [int(x, 16) for x in f[2].split()] for f in lines('CaseFolding.txt') if f[1] in ('C', 'F')}
# DerivedCoreProperties.txt: ignorables
ignorable = set()
for f in lines('DerivedCoreProperties.txt'):
    if f[1] == 'Default_Ignorable_Code_Point':
        a, _, b = f[0].partition('..')
        ignorable.update(range(int(a, 16), int(b or a, 16) + 1))

# NFDICF: a folding where there is one, else the decomposition; ignorables
# map to nothing; then every code point of a mapping is replaced by its own
# mapping until none has one
mapping = {cp: list(m) for cp, m in nfd.items()}
mapping.update({cp: list(m) for cp, m in fold.items()})
for cp in ignorable:
    mapping[cp] = []
def expand(cp, depth=0):
    if depth > 32:
        sys.exit(f'U+{cp:04X}: mapping does not end')
    out = []
    for c in mapping[cp]:
        out += expand(c, depth + 1) if c in mapping else [c]
    return out
nfdicf = {cp: expand(cp) for cp in mapping}

def leaf(cp):
    if SBASE <= cp < SBASE + SCOUNT:
        return bytes([DECOMPOSE, HANGUL, 0])
    if cp in nfdicf:
        return bytes([DECOMPOSE]) + ''.join(map(chr, nfdicf[cp])).encode('utf-8') + b'\0'
    return bytes([ccc.get(cp, 0)])

scalars = [cp for cp in range(0x110000) if not 0xD800 <= cp <= 0xDFFF]
built = {cp: leaf(cp) for cp in scalars}

if linux:
    text = open(linux, encoding='utf-8').read()
    def body(name):
        m = re.search(r'\b' + name + r'\s*\[\d*\]\s*=\s*\{(.*?)\};', text, re.S)
        return re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    ages = [int(x, 0) for x in re.findall(r'0[xX][0-9a-fA-F]+|\b\d+\b', body('utf8agetab'))]
    data = bytes(int(x, 16) for x in re.findall(r'0[xX]([0-9a-fA-F]{2})\b', body('utf8data')))
    roots = {int(a, 0): int(b) for a, b in
             re.findall(r'\{\s*(0[xX][0-9a-fA-F]+|\d+)\s*,\s*(\d+)\s*\}', body('utf8nfdicfdata'))}
    root = roots[0xc0100]
    NEED, MISS = -1, -2
    def lookup(s):
        trie, i, n, node = root, 0, len(s), True
        while node:
            b = data[trie]
            offlen = (b & 0x30) >> 4
            if b & 0x08:
                n -= 1
                if n == 0:
                    return NEED, 0
                i += 1
            if s[i] & (1 << (b & 7)):
                if offlen:
                    node = bool(b & 0x40)
                    off = 0
                    for k in range(offlen, 0, -1):
                        off = (off << 8) | data[trie + k]
                    trie += off
                elif b & 0x40:
                    node, trie = bool(b & 0x80), trie + 1
                else:
                    return MISS, 0
            elif offlen:
                node, trie = bool(b & 0x80), trie + offlen + 1
            elif b & 0x40:
                return MISS, 0
            else:
                node, trie = bool(b & 0x80), trie + 1
        return trie, i + 1
    accepted = {}
    def walk(prefix):
        at, used = lookup(prefix)
        if at == NEED:
            for b in range(256):
                walk(prefix + bytes([b]))
        elif at != MISS:
            if used != len(prefix):
                sys.exit(f'{prefix.hex()}: the trie stopped after {used} bytes')
            accepted[prefix] = at
    for b in range(256):
        walk(bytes([b]))
    strict = {chr(cp).encode('utf-8'): cp for cp in scalars}
    if set(accepted) != set(strict):
        sys.exit('the Linux trie does not accept exactly strict UTF-8')
    if max(ages) != 0xc0100 or any(data[at] >= len(ages) for at in accepted.values()):
        sys.exit('12.1 is not the newest version of the Linux age table')
    differ = 0
    for s, cp in strict.items():
        at = accepted[s]
        theirs = bytes([data[at + 1]])
        if data[at + 1] == DECOMPOSE and data[at + 2] == HANGUL:
            theirs += bytes([HANGUL, 0])      # a bare marker in Linux: nothing reads past it
        elif data[at + 1] == DECOMPOSE:
            end = at + 2
            while data[end]:
                end += 1
            theirs += data[at + 2:end + 1]
        if theirs != built[cp]:
            differ += 1
            if differ <= 10:
                print(f'U+{cp:04X}: Linux {theirs.hex()} built {built[cp].hex()}')
    if differ:
        sys.exit(f'{differ} code points differ from Linux')
    print(f'Linux 12.1 NFDICF trie: {len(accepted)} sequences = strict UTF-8, '
          f'all {len(strict)} leaves equal')

pool, offset_of, table = bytearray(), {}, [0] * 0x110000
for cp in scalars:
    lf = built[cp]
    if lf not in offset_of:
        offset_of[lf] = len(pool)
        pool += lf
    table[cp] = offset_of[lf]
for cp in range(0xD800, 0xE000):              # never looked up: the decoder refuses
    table[cp] = table[0xD7FF]                 # surrogates; share their neighbour's block

stages, level = [], table
for b in bits:
    n, blocks, index, numbers = 1 << b, [], {}, []
    for i in range(0, len(level), n):
        blk = tuple(level[i:i + n])
        if blk not in index:
            index[blk] = len(blocks)
            blocks.append(blk)
        numbers.append(index[blk])
    stages.append([v for blk in blocks for v in blk])
    level = numbers
top = level

def resolve(cp):
    shift = sum(bits)
    b = top[cp >> shift]
    for k in range(len(bits) - 1, -1, -1):
        shift -= bits[k]
        b = stages[k][(b << bits[k]) | ((cp >> shift) & ((1 << bits[k]) - 1))]
    return b
for cp in range(0x110000):
    if resolve(cp) != table[cp]:
        sys.exit(f'U+{cp:04X} reads back wrong')

def width(values):
    m = max(values)
    return 1 if m < 1 << 8 else 2 if m < 1 << 16 else sys.exit('entry too wide')
size = len(pool) + sum(len(s) * width(s) for s in stages) + len(top) * width(top)
print(f'{len(offset_of)} leaves, pool {len(pool)} B; stages {[len(s) for s in stages]} '
      f'+ top {len(top)}: {size} B')
if not target:
    sys.exit(0)

def emit(name, values):
    w = width(values)
    per, fmt = (16, '0x{:02x}') if w == 1 else (12, '0x{:04x}')
    rows = ['\t' + ','.join(fmt.format(v) for v in values[i:i + per]) for i in range(0, len(values), per)]
    ctype = 'unsigned char' if w == 1 else 'unsigned short'
    return f'static const {ctype} {name}[{len(values)}] = {{\n' + ',\n'.join(rows) + '\n};\n'

names = [f'utf8stage{k}' for k in range(len(bits))]
out = ['''/**
 * utf8data.c - Unicode 12.1.0 case folding: the NFDICF form of code points.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Generated by tools/unicode/make-utf8data.py from the Unicode Character
 * Database 12.1.0 (UnicodeData.txt, CaseFolding.txt, DerivedCoreProperties.txt;
 * Copyright (c) 2019 Unicode, Inc., Unicode Data Files and Software License).
 * Do not edit: regenerate. The generator checks every code point against
 * the trie Linux folds names with (fs/unicode/utf8data.c_shipped).
 *
 * A leaf is the canonical combining class of a code point, or DECOMPOSE
 * followed by the NUL-terminated UTF-8 of its form: the full case folding,
 * then the full canonical decomposition, ignorables dropped. Hangul
 * syllables carry only the marker DECOMPOSE, 0xFF and are decomposed by
 * the normalizer. Every leaf is stored once; a code point finds its leaf
 * through stages that each take some low bits of it, identical blocks
 * stored once.
 */

#include "utf8n.h"

/* Names are folded at PASSIVE_LEVEL only: the tables live in PAGERO, pageable and read-only. */
#pragma const_seg("PAGERO")

/* the leaves */
''', emit('utf8leaves', list(pool))]
for k in range(len(bits)):
    what = 'leaf offsets' if k == 0 else f'{names[k - 1]} block numbers'
    out.append(f'\n/* blocks of {1 << bits[k]} {what} */\n' + emit(names[k], stages[k]))
out.append(f'\n/* the {names[-1]} block of each {1 << sum(bits)} code points */\n' + emit('utf8top', top))
out.append('#pragma const_seg()\n')
shift = sum(bits)
body = [f'    unsigned int b = utf8top[cp >> {shift}];']
for k in range(len(bits) - 1, 0, -1):
    shift -= bits[k]
    body.append(f'    b = {names[k]}[(b << {bits[k]}) | ((cp >> {shift}) & {(1 << bits[k]) - 1})];')
body.append(f'    return utf8leaves + {names[0]}[(b << {bits[0]}) | (cp & {(1 << bits[0]) - 1})];')
out.append('\n/* the leaf of a scalar value cp (not a surrogate, at most U+10FFFF) */\n'
           'const unsigned char *utf8leaf(unsigned int cp)\n{\n' + '\n'.join(body) + '\n}\n')
with open(target, 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(''.join(out))
print(f'written {target}')
