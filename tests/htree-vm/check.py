#!/usr/bin/env python3
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
"""check.py - /h of a tests/htree-vm.ps1 volume, from Linux (image.sh).

  check <label> <expected> --root <mount>     the kernel's listing of /h equals
        what <expected> ("f name" / "d name") says; every name looked up by
        the kernel (through the index the driver built) with its kind, a
        name with a character more not found, the link count of /h right
  check <label> <expected> --listing <file>   the same listing from debugfs
        "ls -p" (a volume this kernel does not mount)
  linux <label> <expected> --root <mount> --names <file> --seed <n>
        the kernel deletes a tenth of /h, renames a twentieth and adds a
        tenth (one in 16 a directory), then rewrites <expected>
"""
import argparse
import os
import random
import stat
import sys

MORE = '§'.encode()


def expected(path):
    want = {}
    with open(path, 'rb') as f:
        for line in f.read().split(b'\n'):
            if len(line) >= 3:
                want[line[2:]] = line[:1] == b'd'
    return want


def show(names):
    return ''.join(" '%s'" % n.decode('utf-8', 'replace') for n in sorted(names)[:3])


def compare(label, want, seen, twice):
    missing = [n for n in want if n not in seen]
    extra = [n for n in seen if n not in want]
    ok = not missing and not extra and not twice
    print('  %s %s: %d names listed by Linux (twice %d%s, missing %d%s, extra %d%s)' % (
        'ok  ' if ok else 'FAIL', label, len(want), len(twice), show(twice),
        len(missing), show(missing), len(extra), show(extra)))
    return ok


def check(args):
    want = expected(args.expected)
    seen, twice = set(), []
    ok = True
    if args.listing:
        with open(args.listing, 'rb') as f:
            for line in f.read().split(b'\n'):
                # /inode/mode/uid/gid/name/size/ - a name has no '/'; debugfs
                # lists free entries too (inode 0: the empty entry of an
                # index node, a block's first entry deleted)
                parts = line.split(b'/')
                if len(parts) < 8 or parts[1] == b'0' or parts[5] in (b'.', b'..'):
                    continue
                name = parts[5]
                if name in seen:
                    twice.append(name)
                seen.add(name)
                kind = int(parts[2], 8) & 0o170000
                if name in want and (kind == 0o040000) != want[name]:
                    print('  FAIL %s: %s has the wrong kind' % (args.label, name))
                    ok = False
        return compare(args.label, want, seen, twice) and ok

    h = os.path.join(args.root.encode(), b'h')
    for name in os.listdir(h):
        if name in seen:
            twice.append(name)
        seen.add(name)
    ok = compare(args.label, want, seen, twice)

    lost, kind, phantom = [], [], []
    for name, directory in want.items():
        try:
            st = os.lstat(os.path.join(h, name))
            if stat.S_ISDIR(st.st_mode) != directory:
                kind.append(name)
        except FileNotFoundError:
            lost.append(name)
        try:
            os.lstat(os.path.join(h, name + MORE))
            phantom.append(name + MORE)
        except FileNotFoundError:
            pass
    dirs = sum(1 for d in want.values() if d)
    links = os.lstat(h).st_nlink
    good = not lost and not kind and not phantom and links == 2 + dirs
    print('  %s %s: %d looked up by the kernel (not found %d%s, wrong kind %d%s, found but absent %d%s; links %d for %d directories)' % (
        'ok  ' if good else 'FAIL', args.label, 2 * len(want), len(lost), show(lost), len(kind), show(kind),
        len(phantom), show(phantom), links, dirs))
    return ok and good


def linux(args):
    rng = random.Random(args.seed)
    want = expected(args.expected)
    h = os.path.join(args.root.encode(), b'h')
    keys = {n.decode().lower() for n in want}
    fresh = []
    with open(args.names, 'rb') as f:
        for name in f.read().split(b'\n'):
            if name and name.decode().lower() not in keys:
                fresh.append(name)
    fresh.reverse()

    def take():
        while fresh:
            name = fresh.pop()
            if name.decode().lower() not in keys:
                return name
        raise SystemExit('linux: out of fresh names')

    names = sorted(want)
    rng.shuffle(names)
    tenth = len(names) // 10
    for name in names[:tenth]:
        (os.rmdir if want[name] else os.unlink)(os.path.join(h, name))
        keys.discard(name.decode().lower())
        del want[name]
    renamed = names[tenth:tenth + tenth // 2]
    for name in renamed:
        to = take()
        os.rename(os.path.join(h, name), os.path.join(h, to))
        keys.discard(name.decode().lower())
        keys.add(to.decode().lower())
        want[to] = want.pop(name)
    for _ in range(tenth):
        name = take()
        if rng.randrange(16) == 0:
            os.mkdir(os.path.join(h, name))
            want[name] = True
        else:
            open(os.path.join(h, name), 'xb').close()
            want[name] = False
        keys.add(name.decode().lower())
    with open(args.expected, 'wb') as f:
        f.write(b''.join((b'd ' if d else b'f ') + n + b'\n' for n, d in want.items()))
    os.sync()
    print('  ok   %s: Linux deleted %d, renamed %d, added %d; %d names now' % (
        args.label, tenth, len(renamed), tenth, len(want)))
    return True


def main():
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=('check', 'linux'))
    p.add_argument('label')
    p.add_argument('expected')
    p.add_argument('--root')
    p.add_argument('--listing')
    p.add_argument('--names')
    p.add_argument('--seed', type=int, default=1)
    args = p.parse_args()
    ok = check(args) if args.mode == 'check' else linux(args)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
