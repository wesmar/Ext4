# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
"""Build the code page converters src/nls/nls_<name>.c.

usage: make-nls.py <MAPPINGS dir> [--out src/nls] [--dry-run]
                   [--linux <dir> [--write-established]]

<MAPPINGS dir> holds the published mapping tables under the paths of
https://www.unicode.org/Public/MAPPINGS/ that PAGES names. The driver's
mapping is those tables with the differences listed in
tools/nls/established/<name>.txt: the converters it has always used were
built from earlier revisions of some of them and carry a few errors of
their own, and names already on disk depend on every mapping. A difference
is one line:

    decode <code> <unicode>     the code reads as <unicode> (0: no character)
    encode <unicode> <code>     <unicode> writes as <code> (0: not representable)
    encode-page <high byte>     the encoder keeps this page though it maps nothing

Rules, then the differences, give each code page two maps:
  single byte  decode: every byte through the table. encode: each character
               of the decode map to the lowest byte reading as it.
  double byte  decode: the two-byte codes of the table, by lead byte; a lead
  (936, 949,   byte without one is read as a single byte. encode: each
  950)         character from U+0100 up to its lowest code; U+0001..U+00FF
               are written as one byte.
  Shift JIS    as double byte, and: bytes A1..DF read as U+FF61..U+FF9F and
  (932)        back; U+0000..U+007F are one byte; U+00A0..U+00FF take their
               two-byte codes from a page of their own.

--linux <dir> reads the converters this driver inherited (Linux fs/nls
sources; commit 4706af9 of this repository holds them under src/nls) and
compares every reachable entry of both maps and every page's presence;
--write-established writes the differences it finds.

Representation. A map is pages of 256 entries; an index byte per high byte
gives a page's number plus one (0: no page). A page is eight slices of 32
entries, each one 32-bit word. Its two high bits select:
  constant     f(i) = a
  step         f(i) = a + i
  dense        32 values in the pool
  entry        a Mask/Base/Offset entry and one of six kinds, below.
An entry's Mask marks exceptions; r(i) of them precede position i, the
others are members, j(i) = i - r(i) of them precede it. Exceptions are
Words[Offset + r] (kind run: Offset + r), the members' data follows them:
  constant  Base               step    Base + i
  rank      Base + j           run     Base + j, exceptions Offset + r
  narrow    Base + byte j      select  Base + position of the set bit after j
The rank kinds hold extensions (GBK, UHC Hangul) on the Unicode side, their
consecutive codes interleaved with others; select holds the same sequences
on the code side, the values rising with gaps: a bitmap of the values
present in [Base, Base + 255]. Each slice takes the cheapest form; equal
pages, entries and pool runs are stored once. Every entry of every map is
read back through the emitted form before anything is written.
"""
import os, re, sys
from collections import Counter

def option(name, default=None):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default

MAPPINGS = sys.argv[1]
OUT = option('--out', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'nls'))
LINUX = option('--linux')
WRITE_ESTABLISHED = '--write-established' in sys.argv
ESTABLISHED = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'established')

MS_PC, MS_WIN = 'VENDORS/MICSFT/PC/', 'VENDORS/MICSFT/WINDOWS/'
PAGES = [  # name, alias, family, table
    ('ascii', None, 'single', None),
    ('cp437', None, 'single', MS_PC + 'CP437.TXT'),
    ('cp737', None, 'single', MS_PC + 'CP737.TXT'),
    ('cp775', None, 'single', MS_PC + 'CP775.TXT'),
    ('cp850', None, 'single', MS_PC + 'CP850.TXT'),
    ('cp852', None, 'single', MS_PC + 'CP852.TXT'),
    ('cp855', None, 'single', MS_PC + 'CP855.TXT'),
    ('cp857', None, 'single', MS_PC + 'CP857.TXT'),
    ('cp860', None, 'single', MS_PC + 'CP860.TXT'),
    ('cp861', None, 'single', MS_PC + 'CP861.TXT'),
    ('cp862', None, 'single', MS_PC + 'CP862.TXT'),
    ('cp863', None, 'single', MS_PC + 'CP863.TXT'),
    ('cp864', None, 'single', MS_PC + 'CP864.TXT'),
    ('cp865', None, 'single', MS_PC + 'CP865.TXT'),
    ('cp866', None, 'single', MS_PC + 'CP866.TXT'),
    ('cp869', None, 'single', MS_PC + 'CP869.TXT'),
    ('cp874', 'tis-620', 'single', MS_WIN + 'CP874.TXT'),
    ('cp932', 'sjis', 'shiftjis', MS_WIN + 'CP932.TXT'),
    ('cp936', 'gb2312', 'double', MS_WIN + 'CP936.TXT'),
    ('cp949', 'euc-kr', 'double', MS_WIN + 'CP949.TXT'),
    ('cp950', 'big5', 'double', MS_WIN + 'CP950.TXT'),
    ('cp1250', None, 'single', MS_WIN + 'CP1250.TXT'),
    ('cp1251', None, 'single', MS_WIN + 'CP1251.TXT'),
    ('cp1255', 'iso8859-8', 'single', MS_WIN + 'CP1255.TXT'),
] + [('iso8859-%d' % n, None, 'single', 'ISO8859/8859-%d.TXT' % n) for n in (1, 2, 3, 4, 5, 6, 7, 9, 13, 14, 15)] + [
    ('koi8-r', None, 'single', 'VENDORS/MISC/KOI8-R.TXT'),
    ('koi8-u', None, 'single', 'VENDORS/MISC/KOI8-U.TXT'),
]
DESCRIPTION = {
    'ascii': 'ASCII', 'cp437': 'code page 437 (DOS Latin US)', 'cp737': 'code page 737 (DOS Greek)',
    'cp775': 'code page 775 (DOS Baltic Rim)', 'cp850': 'code page 850 (DOS Latin 1)',
    'cp852': 'code page 852 (DOS Latin 2)', 'cp855': 'code page 855 (DOS Cyrillic)',
    'cp857': 'code page 857 (DOS Turkish)', 'cp860': 'code page 860 (DOS Portuguese)',
    'cp861': 'code page 861 (DOS Icelandic)', 'cp862': 'code page 862 (DOS Hebrew)',
    'cp863': 'code page 863 (DOS Canadian French)', 'cp864': 'code page 864 (DOS Arabic)',
    'cp865': 'code page 865 (DOS Nordic)', 'cp866': 'code page 866 (DOS Cyrillic Russian)',
    'cp869': 'code page 869 (DOS Greek 2)', 'cp874': 'code page 874 (Thai)',
    'cp932': 'code page 932 (Shift JIS)', 'cp936': 'code page 936 (GBK)',
    'cp949': 'code page 949 (Unified Hangul Code)', 'cp950': 'code page 950 (Big5)',
    'cp1250': 'code page 1250 (Central European)', 'cp1251': 'code page 1251 (Cyrillic)',
    'cp1255': 'code page 1255 (Hebrew)', 'koi8-r': 'KOI8-R', 'koi8-u': 'KOI8-U',
}
for n in (1, 2, 3, 4, 5, 6, 7, 9, 13, 14, 15):
    DESCRIPTION['iso8859-%d' % n] = 'ISO 8859-%d' % n
KATAKANA_FIRST, KATAKANA_LAST, KATAKANA_SHIFT = 0xA1, 0xDF, 0xFEC0   # bytes A1..DF <-> U+FF61..U+FF9F

# ---------------------------------------------------------------- the maps

def read_table(path):
    """code -> Unicode of a mapping table; undefined codes are left out."""
    table = {}
    with open(os.path.join(MAPPINGS, path), encoding='latin-1') as f:
        for line in f:
            fields = line.split('#', 1)[0].split()
            if len(fields) >= 2 and fields[0].lower().startswith('0x'):
                code, uni = int(fields[0], 16), int(fields[1], 16)
                if code in table:
                    raise SystemExit('%s: code %#x mapped twice' % (path, code))
                table[code] = uni
    return table

def read_established(name):
    decode, encode, pages = {}, {}, set()
    path = os.path.join(ESTABLISHED, name + '.txt')
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            for number, line in enumerate(f, 1):
                fields = line.split('#', 1)[0].split()
                if not fields:
                    continue
                if fields[0] == 'decode' and len(fields) == 3:
                    decode[int(fields[1], 16)] = int(fields[2], 16)
                elif fields[0] == 'encode' and len(fields) == 3:
                    encode[int(fields[1], 16)] = int(fields[2], 16)
                elif fields[0] == 'encode-page' and len(fields) == 2:
                    pages.add(int(fields[1], 16))
                else:
                    raise SystemExit('%s:%d: unknown line' % (path, number))
    return decode, encode, pages

class Maps:
    """decode: page -> 256 values; encode: high byte -> 256 values."""
    def __init__(self, family):
        self.family, self.decode, self.encode = family, {}, {}

def derive(name, family, table, established):
    """The maps of one code page: the rules, then the differences."""
    fix_decode, fix_encode, fix_pages = established
    maps = Maps(family)
    if family == 'single':
        decode = {code: (code if code < 0x80 else 0) for code in range(256)} if table is None else \
                 {code: table.get(code, 0) for code in range(256)}
    else:
        decode = {code: uni for code, uni in table.items() if code > 0xFF}
    decode.update(fix_decode)
    decode = {code: uni for code, uni in decode.items() if uni or family == 'single'}
    if family == 'shiftjis':
        for byte in range(KATAKANA_FIRST, KATAKANA_LAST + 1):
            if table.get(byte) != byte + KATAKANA_SHIFT:
                raise SystemExit('%s: byte %#x is not the half-width katakana the decoder computes' % (name, byte))
    encode = {}
    for code in sorted(decode):
        uni = decode[code]
        if uni and uni not in encode and (family == 'single' or uni > 0xFF or
                                          (family == 'shiftjis' and uni >= 0xA0)):
            encode[uni] = code
    encode.update(fix_encode)
    if family == 'single':
        maps.decode[0] = [decode[code] for code in range(256)]
    else:
        for code, uni in decode.items():
            maps.decode.setdefault(code >> 8, [0] * 256)[code & 0xFF] = uni
    for uni, code in encode.items():
        if code:
            maps.encode.setdefault(uni >> 8, [0] * 256)[uni & 0xFF] = code
    for high in fix_pages:
        maps.encode.setdefault(high, [0] * 256)
    if family == 'shiftjis':
        latin = maps.encode.setdefault(0, [0] * 256)
        if any(latin[:0xA0]):
            raise SystemExit('%s: U+0000..U+009F are not written through the Latin page' % name)
        for uni in range(KATAKANA_FIRST + KATAKANA_SHIFT, KATAKANA_LAST + KATAKANA_SHIFT + 1):
            maps.encode.get(uni >> 8, [0] * 256)[uni & 0xFF] = 0   # computed, never looked up
    elif family == 'double' and 0 in maps.encode:
        raise SystemExit('%s: U+0001..U+00FF are written as single bytes' % name)
    return maps

# ---------------------------------------------------------------- the inherited converters (--linux)

ARRAY = re.compile(r'(?ms)^static\s+(?:const\s+)?(wchar_t|unsigned char)\s+(\w+)\s*\[([^\]]+)\](?:\s*\[([^\]]+)\])?\s*=\s*\{(.*?)\};')
DIRECTORY = re.compile(r'(?ms)^static\s+(?:const\s+)?(?:wchar_t|unsigned char)\s*\*\s*(?:const\s+)?(page_\w+)\s*\[256\]\s*=\s*\{(.*?)\};')

def numbers(text, count):
    text = re.sub(r'(?s)/\*.*?\*/|//[^\r\n]*', '', text)
    values = [int(v, 16) if v.lower().startswith('0x') else int(v) for v in re.findall(r'0[xX][0-9a-fA-F]+|\b\d+\b', text)]
    if len(values) > count:
        raise SystemExit('too many initializers')
    return values + [0] * (count - len(values))

def dimension(text):
    """An array bound such as 256 - 0xA0."""
    terms = re.findall(r'0[xX][0-9a-fA-F]+|\d+|[-+]', text.replace(' ', ''))
    if ''.join(terms) != text.replace(' ', ''):
        raise SystemExit('array bound %s' % text)
    value, sign = 0, 1
    for term in terms:
        if term in '+-': sign = 1 if term == '+' else -1
        else: value += sign * int(term, 0)
    return value

def read_linux(name, family):
    with open(os.path.join(LINUX, 'nls_%s.c' % name), encoding='latin-1') as f:
        source = f.read()
    arrays = {}
    for m in ARRAY.finditer(source):
        count = dimension(m.group(3)) * dimension(m.group(4) or '1')
        arrays[m.group(2)] = numbers(m.group(5), count)
    directories = {}
    for m in DIRECTORY.finditer(source):
        entries = [e.strip() for e in re.sub(r'(?s)/\*.*?\*/', '', m.group(2)).split(',') if e.strip()]
        directories[m.group(1)] = entries + ['NULL'] * (256 - len(entries))
    maps = Maps(family)
    if family == 'single':
        maps.decode[0] = arrays['charset2uni']
    else:
        for high, page in enumerate(directories['page_charset2uni']):
            if page != 'NULL':
                maps.decode[high] = arrays[page]
    for high, page in enumerate(directories['page_uni2charset']):
        if page == 'NULL':
            continue
        values = arrays[page]
        maps.encode[high] = values[:256] if family == 'single' else \
            [(values[2 * i] << 8) | values[2 * i + 1] for i in range(256)]
    if family == 'shiftjis':
        if 0 in maps.encode:
            raise SystemExit('%s: inherited encoder has a page for U+00xx' % name)
        latin = arrays['u2c_00hi']
        maps.encode[0] = [0] * 0xA0 + [(latin[2 * i] << 8) | latin[2 * i + 1] for i in range(0x60)]
        for uni in range(KATAKANA_FIRST + KATAKANA_SHIFT, KATAKANA_LAST + KATAKANA_SHIFT + 1):
            maps.encode[uni >> 8][uni & 0xFF] = 0
    return maps

def differences(name, family, table, linux):
    """The lines that turn the rules' maps into the inherited ones."""
    lines = []
    plain = derive(name, family, table, ({}, {}, set()))
    width = 2 if family == 'single' else 4
    for high in sorted(set(plain.decode) | set(linux.decode)):
        ours, theirs = plain.decode.get(high, [0] * 256), linux.decode.get(high, [0] * 256)
        for low in range(256):
            if ours[low] != theirs[low]:
                code = (high << 8 | low) if family != 'single' else low
                lines.append('decode 0x%0*X 0x%04X' % (width, code, theirs[low]))
    fix_decode = {int(l.split()[1], 16): int(l.split()[2], 16) for l in lines}
    decoded = derive(name, family, table, (fix_decode, {}, set()))
    for high in sorted(set(decoded.encode) | set(linux.encode)):
        ours, theirs = decoded.encode.get(high, [0] * 256), linux.encode.get(high, [0] * 256)
        for low in range(256):
            if ours[low] != theirs[low]:
                lines.append('encode 0x%04X 0x%0*X' % (high << 8 | low, width, theirs[low]))
    for high in sorted(linux.encode):
        if not any(linux.encode[high]) and not (family == 'shiftjis' and high == 0):
            lines.append('encode-page 0x%02X' % high)
    return lines

def same(a, b):
    return set(a.decode) == set(b.decode) and set(a.encode) == set(b.encode) and \
        all(a.decode[h] == b.decode[h] for h in a.decode) and all(a.encode[h] == b.encode[h] for h in a.encode)

# ---------------------------------------------------------------- the slices

MODE_DENSE, MODE_CONSTANT, MODE_STEP, MODE_ENTRY = 0, 1, 2, 3   # CharsetMap.c
KIND_CONSTANT, KIND_STEP, KIND_RANK, KIND_RUN, KIND_NARROW, KIND_SELECT = range(6)
KIND_NAMES = ('constant', 'step', 'rank', 'run', 'narrow', 'select')
SLICE, PAGE = 32, 256
ENTRY_BYTES, SLICE_BYTES, WORD_BYTES = 8, 4, 2

def popcount(x):
    return bin(x).count('1')

def chain(values, positions):
    """Longest run of positions whose values rise by exactly one."""
    ends = {}
    for i in positions:
        run = ends.get(values[i] - 1, []) + [i]
        if len(run) > len(ends.get(values[i], [])):
            ends[values[i]] = run
    return max(ends.values(), key=len) if ends else []

def rising(values, positions):
    """Longest strictly rising subsequence of the values at positions."""
    tails, links, last = [], {}, []
    for i in positions:
        lo, hi = 0, len(tails)
        while lo < hi:
            mid = (lo + hi) // 2
            if values[tails[mid]] < values[i]: lo = mid + 1
            else: hi = mid
        links[i] = tails[lo - 1] if lo else None
        if lo == len(tails): tails.append(i)
        else: tails[lo] = i
    run, i = [], tails[-1] if tails else None
    while i is not None:
        run.append(i); i = links[i]
    return run[::-1]

def candidates(v):
    """Forms of one slice: (mode, kind, mask, base, offset_or_None, blob)."""
    if len(set(v)) == 1:
        return [(MODE_CONSTANT, None, 0, v[0], None, [])]
    if all(v[i] == v[0] + i for i in range(SLICE)) and v[0] + SLICE - 1 <= 0xFFFF:
        return [(MODE_STEP, None, 0, v[0], None, [])]
    forms = [(MODE_DENSE, None, 0, 0, None, list(v))]
    def entry(kind, members, base, data=(), run=None):
        mask = sum(1 << i for i in range(SLICE) if i not in members)
        exceptions = [v[i] for i in range(SLICE) if mask >> i & 1]
        if run is not None:
            forms.append((MODE_ENTRY, kind, mask, base, run, list(data)))
        else:
            forms.append((MODE_ENTRY, kind, mask, base, None, exceptions + list(data)))
    every = range(SLICE)
    value, _ = Counter(v).most_common(1)[0]
    entry(KIND_CONSTANT, {i for i in every if v[i] == value}, value)
    base, _ = Counter((v[i] - i) & 0xFFFF for i in every).most_common(1)[0]
    entry(KIND_STEP, {i for i in every if (base + i) & 0xFFFF == v[i]}, base)
    members = chain(v, every)
    if len(members) > 1:
        entry(KIND_RANK, set(members), v[members[0]])
        rest = [i for i in every if i not in members]
        if rest and chain(v, rest) == rest:
            entry(KIND_RUN, set(members), v[members[0]], run=v[rest[0]])
    ordered = sorted(every, key=lambda i: v[i])
    best, lo = None, 0
    for hi in range(SLICE):
        while v[ordered[hi]] - v[ordered[lo]] > 0xFF: lo += 1
        if best is None or hi - lo > best[1] - best[0]: best = (lo, hi)
    members = sorted(ordered[best[0]:best[1] + 1])
    if len(members) > 1:
        base = v[ordered[best[0]]]
        data = [v[i] - base for i in members]
        data += [0] * (len(data) & 1)
        entry(KIND_NARROW, set(members), base, [data[k] | data[k + 1] << 8 for k in range(0, len(data), 2)])
    best = None
    for start in sorted(set(v)):
        run = rising(v, [i for i in every if start <= v[i] <= start + 0xFF])
        for end in range(1, len(run)):
            span = v[run[end]] - v[run[0]] + 1
            cost = (SLICE - end - 1) + (span + 15) // 16
            if best is None or cost < best[0]: best = (cost, run[:end + 1])
    if best:
        members = best[1]
        base = v[members[0]]
        bits = sum(1 << (v[i] - base) for i in members)
        words = (v[members[-1]] - base) // 16 + 1
        entry(KIND_SELECT, set(members), base, [bits >> (16 * k) & 0xFFFF for k in range(words)])
    return forms

class Packer:
    def __init__(self):
        self.words, self.runs, self.entries, self.keys = [], {}, [], {}
        self.kinds = Counter()

    def place(self, blob):
        key = tuple(blob)
        if key not in self.runs:
            self.runs[key] = len(self.words)
            self.words.extend(blob)
        return self.runs[key]

    def cost(self, form):
        mode, kind, mask, base, run, blob = form
        cost = SLICE_BYTES
        if blob and tuple(blob) not in self.runs:
            cost += WORD_BYTES * len(blob)
        if mode == MODE_ENTRY:
            offset = run if run is not None else self.runs.get(tuple(blob), -1) if blob else 0
            if (mask, base, offset) not in self.keys:
                cost += ENTRY_BYTES
        return cost

    def slice(self, values):
        forms = candidates(values)
        mode, kind, mask, base, run, blob = min(forms, key=self.cost)
        if mode in (MODE_CONSTANT, MODE_STEP):
            self.kinds[('constant', 'step')[mode == MODE_STEP]] += 1
            return mode << 30 | base
        if mode == MODE_DENSE:
            self.kinds['dense'] += 1
            return self.place(blob)
        offset = run if run is not None else self.place(blob) if blob else 0
        if offset > 0xFFFF:
            raise SystemExit('the pool outgrew 16-bit offsets')
        key = (mask, base, offset)
        if key not in self.keys:
            self.keys[key] = len(self.entries)
            self.entries.append(key)
        self.kinds[KIND_NAMES[kind]] += 1
        return MODE_ENTRY << 30 | kind << 27 | self.keys[key]

    def pages(self, pages):
        """Slices of the distinct pages, and each high byte's page number plus one."""
        order, index, slices = {}, [0] * 256, []
        for high in sorted(pages):
            key = tuple(pages[high])
            if key not in order:
                order[key] = len(order)
                for s in range(0, PAGE, SLICE):
                    slices.append(self.slice(pages[high][s:s + SLICE]))
            index[high] = order[key] + 1
        if len(order) > 255:
            raise SystemExit('more than 255 pages')
        return slices, index

def rank(bits):
    return popcount(bits)

def lookup(packer, slices, page, low):
    """The value the C lookup returns: CharsetMap.c, NlsValue."""
    slice = slices[page * (PAGE // SLICE) + (low >> 5)]
    i, payload = low & 31, slice & 0x3FFFFFFF
    mode = slice >> 30
    if mode == MODE_DENSE: return packer.words[payload + i]
    if mode == MODE_CONSTANT: return payload & 0xFFFF
    if mode == MODE_STEP: return (payload + i) & 0xFFFF
    kind = payload >> 27
    mask, base, offset = packer.entries[payload & 0x07FFFFFF]
    bit = 1 << i
    before = rank(mask & (bit - 1))
    if mask & bit:
        return (offset + before) & 0xFFFF if kind == KIND_RUN else packer.words[offset + before]
    j = i - before
    data = offset + rank(mask)
    if kind == KIND_CONSTANT: return base
    if kind == KIND_STEP: return (base + i) & 0xFFFF
    if kind == KIND_NARROW: return (base + (packer.words[data + (j >> 1)] >> (8 * (j & 1)) & 0xFF)) & 0xFFFF
    if kind == KIND_SELECT:
        position = 0
        while j >= rank(packer.words[data]):
            j -= rank(packer.words[data]); data += 1; position += 16
        bits = packer.words[data]
        for _ in range(j): bits &= bits - 1
        return (base + rank((bits & -bits) - 1) + position) & 0xFFFF
    return (base + j) & 0xFFFF

# ---------------------------------------------------------------- the source

ADAPTERS = {
    'single': ('NlsUnicodeToSingle(&Charset, uni, out, boundlen)', 'NlsSingleToUnicode(&Charset, rawstring, boundlen, uni)'),
    'double': ('NlsUnicodeToDouble(&Charset, uni, out, boundlen)', 'NlsDoubleToUnicode(&Charset, rawstring, boundlen, uni)'),
    'shiftjis': ('NlsUnicodeToShiftJis(&Charset, uni, out, boundlen)', 'NlsShiftJisToUnicode(&Charset, rawstring, boundlen, uni)'),
}

def rows(items, per_line, indent='    '):
    return '\n'.join(indent + ', '.join(items[k:k + per_line]) + ',' for k in range(0, len(items), per_line))

def emit(name, alias, family, table, maps, packer, decode, encode):
    symbol = name.replace('-', '_')
    source = 'unicode.org MAPPINGS/%s' % table if table else 'the ASCII rule'
    established = os.path.join(ESTABLISHED, name + '.txt')
    if os.path.exists(established):
        source += ',\n * with the differences in tools/nls/established/%s.txt' % name
    words = packer.words or [0]
    entries = packer.entries or [(0, 0, 0)]
    text = []
    text.append('/**\n * nls_%s.c - %s and Unicode.\n *\n'
                ' * Copyright (c) 2026 Marek Wesolowski (WESMAR)\n * SPDX-License-Identifier: MIT\n *\n'
                ' * Generated by tools/nls/make-nls.py from %s.\n'
                ' * Do not edit: regenerate.\n */\n' % (name, DESCRIPTION[name], source))
    text.append('#include <linux/module.h>\n#include <linux/errno.h>\n#include "CharsetMap.h"\n')
    text.append('static const NLS_WORD Words[%d] = {\n%s\n};\n' % (len(words), rows(['0x%04x' % w for w in words], 12)))
    text.append('static const NLS_MAP_ENTRY Entries[%d] = {\n%s\n};\n' % (len(entries), rows(
        ['{ 0x%08xu, 0x%04x, 0x%04x }' % e for e in entries], 3)))
    def slices(label, data, index):
        owner = {}
        for high in range(256):
            if index[high] and index[high] not in owner:
                owner[index[high]] = high
        lines = []
        for page in range(len(data) // 8):
            lines.append('    /* %02X */ ' % owner[page + 1] + ', '.join('0x%08xu' % s for s in data[8 * page:8 * page + 8]) + ',')
        return 'static const NLS_BITS %s[%d] = {\n%s\n};\n' % (label, len(data), '\n'.join(lines))
    text.append(slices('DecodeSlices', decode[0], decode[1]))
    text.append(slices('EncodeSlices', encode[0], encode[1]))
    if family != 'single':
        text.append('static const NLS_BYTE DecodePages[256] = {\n%s\n};\n' % rows(['%3d' % p for p in decode[1]], 16))
    text.append('static const NLS_BYTE EncodePages[256] = {\n%s\n};\n' % rows(['%3d' % p for p in encode[1]], 16))
    text.append('static const NLS_CHARSET Charset = {\n    Words, Entries, DecodeSlices, EncodeSlices, %s, EncodePages,\n};\n'
                % ('NULL' if family == 'single' else 'DecodePages'))
    to_code, to_unicode = ADAPTERS[family]
    text.append('static int uni2char(wchar_t uni, unsigned char *out, int boundlen)\n{\n    return %s;\n}\n' % to_code)
    text.append('static int char2uni(const unsigned char *rawstring, int boundlen, wchar_t *uni)\n{\n    return %s;\n}\n' % to_unicode)
    text.append('static struct nls_table table = {\n    .charset = "%s",\n    .alias = %s,\n'
                '    .uni2char = uni2char,\n    .char2uni = char2uni,\n    .owner = THIS_MODULE,\n};\n'
                % (name, '"%s"' % alias if alias else 'NULL'))
    text.append('static int __init init_nls_%s(void)\n{\n    return register_nls(&table);\n}\n' % symbol)
    text.append('static void __exit exit_nls_%s(void)\n{\n    unregister_nls(&table);\n}\n' % symbol)
    text.append('module_init(init_nls_%s)\nmodule_exit(exit_nls_%s)\n' % (symbol, symbol))
    return '\n'.join(text)

# ---------------------------------------------------------------- main

summary, compared = [], 0
for name, alias, family, table_path in PAGES:
    table = read_table(table_path) if table_path else None
    if LINUX:
        linux = read_linux(name, family)
        lines = differences(name, family, table, linux)
        if WRITE_ESTABLISHED:
            path = os.path.join(ESTABLISHED, name + '.txt')
            if lines:
                os.makedirs(ESTABLISHED, exist_ok=True)
                with open(path, 'w', encoding='utf-8', newline='\n') as f:
                    f.write('# %s: where the driver\'s mapping differs from %s.\n' % (name, table_path or 'the ASCII rule'))
                    f.write('# The driver keeps the mapping its converters have always had: names on disk\n')
                    f.write('# depend on it. Written by make-nls.py --linux --write-established.\n')
                    f.write('\n'.join(lines) + '\n')
            elif os.path.exists(path):
                os.remove(path)
    maps = derive(name, family, table, read_established(name))
    if LINUX:
        if not same(maps, linux):
            raise SystemExit('%s: the maps differ from the inherited converter (--write-established lists why)' % name)
        compared += 1
    packer = Packer()
    decode = packer.pages(maps.decode)
    encode = packer.pages(maps.encode)
    for (slices, index), pages in ((decode, maps.decode), (encode, maps.encode)):
        for high, values in pages.items():
            for low in range(256):
                if lookup(packer, slices, index[high] - 1, low) != values[low]:
                    raise SystemExit('%s: %02X%02X reads back wrong' % (name, high, low))
    size = (len(packer.words) * WORD_BYTES + len(packer.entries) * ENTRY_BYTES +
            (len(decode[0]) + len(encode[0])) * SLICE_BYTES + (256 if family != 'single' else 0) + 256)
    summary.append((name, size, len(packer.words), len(packer.entries), len(decode[0]) // 8, len(encode[0]) // 8, packer.kinds))
    if OUT and '--dry-run' not in sys.argv:
        with open(os.path.join(OUT, 'nls_%s.c' % name), 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(emit(name, alias, family, table_path, maps, packer, decode, encode))
total = 0
for name, size, words, entries, dpages, epages, kinds in summary:
    total += size
    print('%-10s %7d bytes  words %5d  entries %4d  pages %3d+%3d  %s' % (
        name, size, words, entries, dpages, epages, ' '.join('%s %d' % kv for kv in sorted(kinds.items()))))
print('total %d bytes of tables in %d code pages' % (total, len(summary)))
if LINUX:
    print('%d code pages equal the inherited converters in every reachable entry and page' % compared)
