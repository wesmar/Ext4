#!/usr/bin/env python3
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
"""Every crash state of one run: replay the driver's write log onto the volume
as it was before the mount, and judge each prefix.

usage: crash-replay.py <base.img> <writelog.bin> [--final final.img]
                       [--every N] [--reorder] [--keep DIR]
       crash-replay.py <base.img> <writelog.bin> --pick N
       crash-replay.py <base.img> <writelog.bin> --state K --out state.img
       crash-replay.py --compare <expected.img> <mounted.img> [--orphans-done | --lenient]

The log (support/WriteLog.c, a build with EXT4_WRITE_LOG) holds every write
the driver sent to the volume - offset, length, bytes - and every flush, in
the order they were sent. A power cut leaves the disk with some prefix of
them, so each prefix (every N-th with --every) is copied aside and checked:
e2fsck -fy replays the journal, as the next mount would, and may do nothing
but that, process orphans and set the free counts; e2fsck -fn must then find
the volume clean. With --final, the whole log applied must equal the volume
as the driver left it - the log missed no write; and a file whose flush
returned (the log marks it: inode, size, generation) and that is the same
file of the same size at the end must hold, in the state right after the
mark, what it holds at the end.

A disk may also persist the writes between two flushes in any order. With
--reorder, each such epoch is checked once for every one of its writes,
with that write lost and all the others of the epoch persisted - except
what was sent after a write-through write completed (the log marks that):
nothing sent later reaches the media without it, so with such a write lost
only the writes sent before its completion are.

--pick prints N prefixes worth mounting (tests/crash-recover-vm.ps1): the
states on both sides of journal commit blocks, spread over the run; --state
writes prefix K as an image. --compare judges a state the driver mounted -
its own journal replay - against the same state e2fsck replayed: e2fsck -fy
may only process orphans and set the free counts, -fn must then find it
clean, and every name, file and link must be the same (System Volume
Information aside: Windows writes there on every mount). With --orphans-done
e2fsck may not find an orphan either: the driver finishes them at mount
(tests/orphan-vm.ps1); with --lenient (a volume with a bad orphan) it may
repair what it repairs on the original too, the journal still excepted.
"""
import hashlib, os, re, shutil, struct, subprocess, sys

HEAD = struct.Struct('<IIQII')          # Magic, Type, Offset, Length, Sequence
MAGIC, WRITE, FLUSH, FSYNC, DURABLE = 0x474f4c57, 1, 2, 3, 4
# scratch images of this process: two replays may run side by side
WORK, TEST, MOUNTED = ('/dev/shm/crash-%s-%d.img' % (name, os.getpid()) for name in ('work', 'test', 'mounted'))
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

def content(image, ino):
    return subprocess.run(['debugfs', '-R', 'cat <%d>' % ino, image], capture_output=True).stdout

def generation(image, ino):
    found = re.search(r'Generation: (\d+)', run('debugfs', '-R', 'stat <%d>' % ino, image).stdout)
    return int(found.group(1)) if found else None

def judge(k, promises=()):
    """None, or why prefix k is not a state Linux would accept. promises:
    (inode, bytes) a flushed file must hold in this state."""
    shutil.copyfile(WORK, TEST)
    repair = run('e2fsck', '-fy', TEST)
    check = run('e2fsck', '-fn', TEST)
    odd = [l for l in repair.stdout.splitlines() if not EXPECTED.match(l)]
    if repair.returncode > 1 or check.returncode != 0 or odd:
        return 'prefix %d: e2fsck -fy %d, -fn %d\n  %s' % (
            k, repair.returncode, check.returncode, '\n  '.join(odd or check.stdout.splitlines()[-8:]))
    for ino, expected in promises:
        held = content(TEST, ino)
        if held != expected:
            return 'prefix %d: inode %d lost what its flush had returned (%d bytes, %d expected)' % (
                k, ino, len(held), len(expected))
    return None

JBD2_MAGIC, JBD2_COMMIT = 0xc03b3998, 2

def pick(data, recs, n):
    """n prefixes spread over the run, those around a commit block first"""
    commits = [k for k, (kind, offset, length, start) in enumerate(recs)
               if kind == WRITE and length >= 8 and
               struct.unpack_from('>II', data, start) == (JBD2_MAGIC, JBD2_COMMIT)]
    around = sorted({p for k in commits for p in (k, k + 1)})
    spread = lambda pool, m: [pool[i * len(pool) // m] for i in range(m)] if len(pool) > m else pool
    chosen = set(spread(around, n * 2 // 3))
    chosen |= set(spread([k for k in range(1, len(recs) + 1) if k not in chosen], n - len(chosen)))
    return sorted(chosen)

def state(base, data, recs, k, out):
    shutil.copyfile(base, out)
    with open(out, 'r+b') as image:
        for kind, offset, length, start in recs[:k]:
            if kind == WRITE:
                image.seek(offset)
                image.write(data[start:start + length])

def tree(image, into):
    """every name below the root: its kind and content"""
    shutil.rmtree(into, ignore_errors=True)
    os.makedirs(into)
    run('debugfs', '-R', 'rdump / %s' % into, image)
    found = {}
    for top, dirs, files in os.walk(into):
        rel = os.path.relpath(top, into)
        if rel.split(os.sep)[0] == 'System Volume Information':
            continue
        for name in dirs + files:
            path = os.path.join(top, name)
            key = os.path.normpath(os.path.join(rel, name))
            if key == 'System Volume Information':
                continue
            if os.path.islink(path):
                found[key] = 'link ' + os.readlink(path)
            elif os.path.isdir(path):
                found[key] = 'dir'
            else:
                found[key] = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    shutil.rmtree(into)
    return found

def compare(expected, mounted, orphans_done=False, lenient=False):
    judged = MOUNTED
    shutil.copyfile(mounted, judged)
    repair = run('e2fsck', '-fy', judged)
    check = run('e2fsck', '-fn', judged)
    # the journal is the driver's to replay: e2fsck must find nothing in it;
    # with --orphans-done no orphan either (the driver finishes them); with
    # --lenient e2fsck may repair anything else (a volume it repairs too)
    odd = [l for l in repair.stdout.splitlines()
           if 'recovering journal' in l or
           (not lenient and not EXPECTED.match(l)) or
           (orphans_done and ('orphaned inode' in l or 'orphan' in l.lower()))]
    if repair.returncode > 1 or check.returncode != 0 or odd:
        print('the mounted state: e2fsck -fy %d, -fn %d\n  %s' % (
            repair.returncode, check.returncode, '\n  '.join((odd or check.stdout.splitlines())[-12:])))
        return 1
    if repair.returncode:
        print('e2fsck -fy: ' + '; '.join(sorted({l.split(' (')[0] for l in repair.stdout.splitlines()
                                                 if l.startswith(('Clearing', 'Truncating', 'Free'))})))
    want, got = tree(expected, '/dev/shm/crash-want-%d' % os.getpid()), tree(judged, '/dev/shm/crash-got-%d' % os.getpid())
    differ = sorted(k for k in set(want) | set(got) if want.get(k) != got.get(k))
    for key in differ[:10]:
        print('%s: %s after e2fsck, %s after the driver' % (
            key, (want.get(key) or 'missing')[:16], (got.get(key) or 'missing')[:16]))
    print('RECOVER: %d names compared, %d differ%s' % (
        len(want), len(differ), '' if differ else ': SAME'))
    return 1 if differ else 0

def main():
    if '--compare' in sys.argv:
        at = sys.argv.index('--compare')
        sys.exit(compare(sys.argv[at + 1], sys.argv[at + 2], '--orphans-done' in sys.argv,
                         '--lenient' in sys.argv))
    base, log = sys.argv[1], sys.argv[2]
    if '--pick' in sys.argv or '--state' in sys.argv:
        data, recs = records(log)
        if '--pick' in sys.argv:
            print(' '.join(str(k) for k in pick(data, recs, int(option('--pick')))))
        else:
            state(base, data, recs, int(option('--state')), option('--out'))
        return
    every, keep, final = int(option('--every', '1')), option('--keep'), option('--final')
    data, recs = records(log)
    count = lambda kind: sum(1 for r in recs if r[0] == kind)
    print('%d records: %d writes (%d written through), %d flushes, %d file flushes, %d MB written' % (
        len(recs), count(WRITE), count(DURABLE), count(FLUSH), count(FSYNC),
        sum(r[2] for r in recs if r[0] == WRITE) >> 20))
    # the last flush of each file, held to what the file holds at the end
    # when it is still the same file (generation) of the same size
    promises, kept = {}, 0
    if final:
        last = {}
        for k, (kind, offset, length, start) in enumerate(recs):
            if kind == FSYNC:
                last[offset] = (k,) + struct.unpack_from('<qq', data, start)
        for ino, (k, size, gen) in last.items():
            if generation(final, ino) == gen:
                expected = content(final, ino)
                if len(expected) == size:
                    promises[k] = (ino, expected)
        print('%d files flushed, %d the same at the end' % (len(last), len(promises)))
    shutil.copyfile(base, WORK)
    failures, checked = [], 0
    with open(WORK, 'r+b') as image:
        for k in range(len(recs) + 1):
            promise = [promises[k - 1]] if k - 1 in promises else []
            if k % every == 0 or k == len(recs) or promise:
                image.flush()
                why = judge(k, promise)
                kept += len(promise) if not why else 0
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
        epoch, start, durable = [], 0, {}
        for k in range(len(recs) + 1):
            if k == len(recs) or recs[k][0] == FLUSH:
                for lost in epoch if len(epoch) > 1 else []:
                    shutil.copyfile(base, WORK)
                    persisted = [e for e in epoch if e != lost and e < durable.get(lost, k)]
                    with open(WORK, 'r+b') as image:
                        for j in list(range(start)) + persisted:
                            kind, offset, length, first = recs[j]
                            if kind == WRITE:
                                image.seek(offset)
                                image.write(data[first:first + length])
                    why = judge(k)
                    checked += 1
                    if why:
                        failures.append('write %d lost: %s' % (lost, why))
                epoch, start, durable = [], k + 1, {}
            elif recs[k][0] == WRITE:
                epoch.append(k)
            elif recs[k][0] == DURABLE:
                # the latest write of the epoch to that offset completed
                done = [e for e in epoch if recs[e][1] == recs[k][1]]
                if done:
                    durable[done[-1]] = k
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
    if promises:
        print('%d of them held what their flush had returned' % kept)
    print('CRASH-REPLAY: %d states checked, %d rejected%s' % (
        checked, len(failures), ': ALL PASSED' if not failures else ''))
    sys.exit(1 if failures else 0)

try:
    main()
finally:
    for scratch in (WORK, TEST, MOUNTED):
        if os.path.exists(scratch):
            os.remove(scratch)
