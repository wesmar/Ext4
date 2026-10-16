#!/usr/bin/env python3
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
"""Every crash state of one run: replay the driver's write log onto the volume
as it was before the mount, and judge each prefix.

usage: crash-replay.py <base.img> <writelog.bin> [--final final.img]
                       [--every N] [--reorder] [--keep DIR]

The log (support/WriteLog.c, a build with EXT4_WRITE_LOG) holds every write
the driver sent to the volume - offset, length, bytes - and every flush, in
the order they were sent. A power cut leaves the disk with some prefix of
them, so each prefix (every N-th with --every) is copied aside and checked:
e2fsck -fy replays the journal, as the next mount would, and may do nothing
but that, process orphans and set the free counts; e2fsck -fn must then find
the volume clean. With --final, the whole log applied must equal the volume
as the driver left it - the log missed no write.

A disk may also persist the writes between two flushes in any order. With
--reorder, each such epoch is checked once for every one of its writes,
with that write lost and all the others of the epoch persisted.
"""
import os, re, shutil, struct, subprocess, sys

HEAD = struct.Struct('<IIQII')          # Magic, Type, Offset, Length, Sequence
MAGIC, WRITE, FLUSH = 0x474f4c57, 1, 2
WORK, TEST = '/dev/shm/crash-work.img', '/dev/shm/crash-test.img'
# what e2fsck -fy may say about a crash state: the journal, orphans, counts
EXPECTED = re.compile(r'^(e2fsck \d|Pass \d|crashlog: (recovering journal|clean|\d+/\d+ files|\*\*\*\*\* FILE SYSTEM WAS MODIFIED)'
                      r'|Clearing orphaned inode|Truncating orphaned inode'
                      r'|Free (blocks|inodes) count wrong \(|Setting free (blocks|inodes) count|Fix\? yes|\s*$)')

def option(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

def records(path):
    data = open(path, 'rb').read()
    magic, full, used, _, count = HEAD.unpack_from(data, 0)
    if magic != MAGIC:
        raise SystemExit('not a write log')
    if full:
        raise SystemExit('the log overflowed: enlarge WLOG_BYTES or the workload is too big')
    found, pos = [], HEAD.size
    while pos < HEAD.size + used:
        magic, kind, offset, length, sequence = HEAD.unpack_from(data, pos)
        if magic != MAGIC or sequence != len(found):
            raise SystemExit('broken record at %d' % pos)
        found.append((kind, offset, length, pos + HEAD.size))
        pos += HEAD.size + ((length + 7) & ~7)
    if len(found) != count:
        raise SystemExit('%d records, the header says %d' % (len(found), count))
    return data, found

def run(*command):
    return subprocess.run(command, capture_output=True, text=True)

def judge(k):
    """None, or why prefix k is not a state Linux would accept."""
    shutil.copyfile(WORK, TEST)
    repair = run('e2fsck', '-fy', TEST)
    check = run('e2fsck', '-fn', TEST)
    odd = [l for l in repair.stdout.splitlines() if not EXPECTED.match(l)]
    if repair.returncode > 1 or check.returncode != 0 or odd:
        return 'prefix %d: e2fsck -fy %d, -fn %d\n  %s' % (
            k, repair.returncode, check.returncode, '\n  '.join(odd or check.stdout.splitlines()[-8:]))
    return None

def main():
    base, log = sys.argv[1], sys.argv[2]
    every, keep, final = int(option('--every', '1')), option('--keep'), option('--final')
    data, recs = records(log)
    writes = sum(1 for r in recs if r[0] == WRITE)
    print('%d records: %d writes, %d flushes, %d MB written' % (
        len(recs), writes, len(recs) - writes, sum(r[2] for r in recs) >> 20))
    shutil.copyfile(base, WORK)
    failures, checked = [], 0
    with open(WORK, 'r+b') as image:
        for k in range(len(recs) + 1):
            if k % every == 0 or k == len(recs):
                image.flush()
                why = judge(k)
                checked += 1
                if why:
                    failures.append(why)
                    if keep and len(failures) <= 3:
                        os.makedirs(keep, exist_ok=True)
                        shutil.copyfile(WORK, os.path.join(keep, 'prefix-%d.img' % k))
            if k < len(recs) and recs[k][0] == WRITE:
                kind, offset, length, start = recs[k]
                image.seek(offset)
                image.write(data[start:start + length])
    if '--reorder' in sys.argv:
        epoch, start = [], 0
        for k in range(len(recs) + 1):
            if k == len(recs) or recs[k][0] == FLUSH:
                for lost in epoch if len(epoch) > 1 else []:
                    shutil.copyfile(base, WORK)
                    with open(WORK, 'r+b') as image:
                        for j in list(range(start)) + [e for e in epoch if e != lost]:
                            kind, offset, length, first = recs[j]
                            if kind == WRITE:
                                image.seek(offset)
                                image.write(data[first:first + length])
                    why = judge(k)
                    checked += 1
                    if why:
                        failures.append('write %d lost: %s' % (lost, why))
                epoch, start = [], k + 1
            elif recs[k][0] == WRITE:
                epoch.append(k)
        shutil.copyfile(base, WORK)
        with open(WORK, 'r+b') as image:
            for kind, offset, length, first in recs:
                if kind == WRITE:
                    image.seek(offset)
                    image.write(data[first:first + length])
    if final:
        same = open(WORK, 'rb').read() == open(final, 'rb').read()
        print('whole log applied %s the volume the driver left' % ('equals' if same else 'DIFFERS FROM'))
        if not same:
            failures.append('the log is incomplete')
    for why in failures[:10]:
        print(why)
    print('CRASH-REPLAY: %d states checked, %d rejected%s' % (
        checked, len(failures), ': ALL PASSED' if not failures else ''))
    sys.exit(1 if failures else 0)

main()
