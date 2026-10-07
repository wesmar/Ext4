#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# corrupt-image.sh prepare|check <disk size>: a disk with damage planted on
# purpose, for what the driver does when it finds it (corrupt.ps1).
#   partition 1 (errcont, s_errors continue, two groups): group 0's block
#     bitmap disagrees with its checksum, badinode.txt's inode checksum is
#     wrong, the one leaf of /baddir has a wrong tail checksum;
#   partition 2 (errro, s_errors remount-ro): badinode.txt as above.
# prepare builds it; check verifies from Linux what Windows did: the
# superblock records the errors, new data went around the corrupt group,
# the deleted file is gone, nothing was written after remount-ro.
# Never repairs anything. The disk is found by its size.
set -euo pipefail
mode=${1:?prepare or check}
size=${2:?disk size}
dev=/dev/$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1; exit}')
[ "$dev" != "/dev/" ] || { echo "disk not found"; exit 2; }
p1=${dev}1
p2=${dev}2

corrupt() {     # corrupt <partition>: plant the damage of that partition
    python3 - "$1" "$2" <<'PY'
import os, re, subprocess, sys
dev, kind = sys.argv[1], sys.argv[2]
def dbg(cmd):
    return subprocess.run(['debugfs', '-R', cmd, dev], capture_output=True, text=True).stdout
def flip(offset):
    with open(dev, 'r+b', buffering=0) as f:
        f.seek(offset)
        b = f.read(1)
        f.seek(offset)
        f.write(bytes([b[0] ^ 0x5A]))
        os.fsync(f.fileno())
layout = subprocess.run(['dumpe2fs', dev], capture_output=True, text=True).stdout
bs = int(re.search(r'Block size:\s+(\d+)', layout).group(1))
isz = int(re.search(r'Inode size:\s+(\d+)', layout).group(1))
ipg = int(re.search(r'Inodes per group:\s+(\d+)', layout).group(1))
def group_field(group, name):
    part = re.search(r'^Group %d:.*?(?=^Group |\Z)' % group, layout, re.S | re.M).group(0)
    return int(re.search(name + r' at (\d+)', part).group(1))
# the inode's own checksum (i_checksum_lo, at 0x7C)
ino = int(re.search(r'Inode:\s+(\d+)', dbg('stat /badinode.txt')).group(1))
group, index = (ino - 1) // ipg, (ino - 1) % ipg
flip(group_field(group, 'Inode table') * bs + index * isz + 0x7C)
if kind == 'cont':
    # the tail checksum of /baddir's one block (its last four bytes)
    blk = int(dbg('blocks /baddir').split()[0])
    flip(blk * bs + bs - 1)
    # group 0's block bitmap: a free block of it marked in use, so that the
    # bitmap no longer matches its checksum
    bitmap = group_field(0, 'Block bitmap')
    free = int(dbg('ffb 1 1000').split()[-1])
    assert free < 32768, free
    with open(dev, 'r+b', buffering=0) as f:
        f.seek(bitmap * bs + free // 8)
        b = f.read(1)
        f.seek(bitmap * bs + free // 8)
        f.write(bytes([b[0] | (1 << (free % 8))]))
        os.fsync(f.fileno())
print('planted on %s: inode %d%s' % (dev, ino, ', /baddir tail, group 0 block bitmap' if kind == 'cont' else ''))
PY
}

if [ "$mode" = prepare ]; then
    [ -z "$(wipefs -n "$dev")" ] || { echo "refusing a non-blank disk"; exit 2; }
    printf 'label: gpt\nsize=200MiB, type=linux\ntype=linux\n' | sfdisk -q "$dev"
    udevadm settle
    mkfs.ext4 -q -F -L errcont "$p1"
    mkfs.ext4 -q -F -L errro "$p2"
    tune2fs -e continue "$p1" > /dev/null
    tune2fs -e remount-ro "$p2" > /dev/null
    for p in "$p1" "$p2"; do
        m=$(mktemp -d)
        mount "$p" "$m"
        printf 'good\n' > "$m/good.txt"
        printf 'bad\n' > "$m/badinode.txt"
        mkdir "$m/baddir"
        for n in a b c; do printf '%s\n' "$n" > "$m/baddir/$n.txt"; done
        umount "$m"; rmdir "$m"
        e2fsck -fn "$p" > /dev/null || { echo "$p not clean before the damage"; exit 1; }
    done
    corrupt "$p1" cont
    corrupt "$p2" ro
    echo "CORRUPT-LINUX: prepare passed"
    exit 0
fi

fail=0
field() { dumpe2fs -h "$1" 2>/dev/null | awk -F: -v k="$2" '$1 == k {sub(/^[ \t]+/, "", $2); print $2}'; }
for p in "$p1" "$p2"; do
    state=$(field "$p" "Filesystem state")
    count=$(field "$p" "FS Error count")
    func=$(field "$p" "First error function")
    case "$state" in *error*) echo "  $p: state '$state', $count error(s), first in $func";;
                     *) echo "  $p: state '$state' does not record the errors"; fail=1;; esac
    [ -n "$count" ] && [ "$count" -ge 1 ] || { echo "  $p: no error count"; fail=1; }
done
# partition 1: the new file went around group 0, the deleted one is gone
debugfs -n -R "stat /new.bin" "$p1" 2>/dev/null | python3 -c '
import re, sys
runs = [(int(a), int(b or a)) for a, b in re.findall(r"\(\d+(?:-\d+)?\):(\d+)(?:-(\d+))?", sys.stdin.read())]
group0 = [r for r in runs if r[0] < 32768]
print("  new.bin lies outside the corrupt group (%d runs)" % len(runs) if runs and not group0 else "  new.bin: runs %s" % runs)
sys.exit(0 if runs and not group0 else 1)' || fail=1
tmp=$(mktemp)
debugfs -n -R "dump /new.bin $tmp" "$p1" > /dev/null 2>&1
python3 - "$tmp" <<'PY' || fail=1
import sys
data = open(sys.argv[1], 'rb').read()
want = bytes((i * 7 + 3) % 251 for i in range(10 * 1024 * 1024))
print('  new.bin content as written' if data == want else '  new.bin content differs (%d bytes)' % len(data))
sys.exit(0 if data == want else 1)
PY
rm -f "$tmp"
debugfs -n -R "stat /good.txt" "$p1" 2>/dev/null | grep -q '^Inode:' && { echo "  good.txt still there"; fail=1; } || echo "  good.txt deleted"
e2fsck -fn "$p1" 2>&1 | grep -q 'zero dtime' && { echo "  a failed create left an inode e2fsck calls deleted with zero dtime"; fail=1; } || echo "  failed creates left no stray inode"
# partition 2: nothing written once it stopped writing
debugfs -n -R "stat /after-error.txt" "$p2" 2>/dev/null | grep -q '^Inode:' && { echo "  errro was written after its error"; fail=1; } || echo "  errro: no write after the error"
echo "CORRUPT-LINUX: $fail"
exit $fail
