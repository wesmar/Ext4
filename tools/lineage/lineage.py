# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
"""How much of the driver's C code is new: token runs shared with its origins.

usage: lineage.py <src> <Ext3Fsd> <Ext4Fsd> <linux> [--k 12] [--normalize] [--files]

<Ext3Fsd>  https://github.com/matt-wu/Ext3Fsd, commit 1ae6368 (Ext2Fsd as Matt Wu left it)
<Ext4Fsd>  https://github.com/bobranten/Ext4Fsd, commit da27068
<linux>    Linux v6.6: fs/ext4, fs/jbd2, fs/nls, fs/unicode, lib/rbtree.c and
           include/linux/{jbd2,rbtree,rbtree_augmented,nls}.h

Every .c and .h file is read as C tokens, comments dropped. A token of <src>
is inherited when it lies in a run of k consecutive tokens that also occurs
in an origin - Ext3Fsd first, then Ext4Fsd, then Linux - and new otherwise.
The generated tables - code pages, case folding, NTSTATUS names - are data
and are counted apart. --normalize replaces identifiers, numbers and strings by their kind,
so renamed code counts as inherited too (use a longer run, e.g. --k 20).
"""
import os, re, sys
from collections import Counter, defaultdict

TOKEN = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|0[xX][0-9a-fA-F]+[uUlL]*|\d+(?:\.\d+)?[uUlLfF]*'
                   r'|[A-Za-z_]\w*|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|##|\S')
COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
GENERATED = (r'^nls/nls_(cp\d+|iso8859-\d+|ascii|koi8-[ru])\.c$', r'^nls/EucJpTables\.h$', r'^unicode/utf8data\.c$',
             r'^support/StatusNames\.c$')
ORIGINS = ('Ext3Fsd', 'Ext4Fsd', 'Linux')

def option(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

def tokens(path, normalize):
    with open(path, encoding='latin-1') as f:
        found = TOKEN.findall(COMMENT.sub(' ', f.read().replace('\\\n', ' ')))
    if normalize:
        found = ['I' if t[0].isalpha() or t[0] == '_' else 'N' if t[0].isdigit() else 'S' if t[0] in '"\'' else t
                 for t in found]
    return found

def sources(root):
    for directory, subdirectories, names in os.walk(root):
        subdirectories[:] = [d for d in subdirectories if d != '.git']
        for name in names:
            if name.lower().endswith(('.c', '.h')):
                path = os.path.join(directory, name)
                yield os.path.relpath(path, root).replace('\\', '/'), path

def runs(found, k):
    return {hash(tuple(found[i:i + k])) for i in range(len(found) - k + 1)}

def main():
    src, origins = sys.argv[1], sys.argv[2:5]
    k, normalize = int(option('--k', '12')), '--normalize' in sys.argv
    known = []
    for name, root in zip(ORIGINS, origins):
        shared = set()
        for _, path in sources(root):
            shared |= runs(tokens(path, normalize), k)
        known.append((name, shared))
    total, areas, files, generated = Counter(), defaultdict(Counter), [], 0
    for relative, path in sources(src):
        found = tokens(path, normalize)
        if any(re.search(g, relative) for g in GENERATED):
            generated += len(found)
            continue
        owner = [None] * len(found)
        for name, shared in known:
            for i in range(len(found) - k + 1):
                if hash(tuple(found[i:i + k])) in shared:
                    for j in range(i, i + k):
                        owner[j] = owner[j] or name
        count = Counter(o or 'new' for o in owner)
        total.update(count)
        areas[relative.split('/')[0]].update(count)
        files.append((relative, len(found), count))
    code = sum(total.values())
    print('%d code tokens, runs of %d%s; %d tokens of generated tables apart' % (
        code, k, ', identifiers ignored' if normalize else '', generated))
    for name in ('new',) + ORIGINS:
        print('  %-8s %7d  %5.1f %%' % (name, total[name], 100.0 * total[name] / code))
    print('by area:')
    for area, count in sorted(areas.items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(count.values())
        print('  %-10s %7d  ' % (area, n) + '  '.join('%s %5.1f %%' % (o, 100.0 * count[o] / n) for o in ('new',) + ORIGINS))
    if '--files' in sys.argv:
        print('by file, most inherited first:')
        for relative, n, count in sorted(files, key=lambda f: f[2]['new'] - f[1]):
            print('  %-42s %6d  ' % (relative, n) + '  '.join('%s %5.1f %%' % (o, 100.0 * count[o] / n) for o in ('new',) + ORIGINS))

main()
