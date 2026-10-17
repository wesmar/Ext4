#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# image.sh - the disk of tests/extent-vm.ps1, run as root in WSL.
#
#   make    two ext4 volumes - 4 KiB blocks (ex4k) and 1 KiB blocks (ex1k,
#           deeper trees) - each with sparse.bin, a 4 MiB file Linux made
#           with data in three places and holes between, sparse2.bin the
#           same, and sparse.want, sparse2.want, their bytes as they read
#   check   each volume: e2fsck -fn, no error the driver recorded, every
#           file of sums.txt (the guest's SHA-256) as Linux reads it, the
#           extents of the fragmented files
set -euo pipefail
export LC_ALL=C
mode=${1:?make or check}
size=1744830464
labels=(ex4k ex1k)
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 1664 MiB disk'; exit 2; }
dev=${disks[0]}
mnt=/mnt/extent-vm
fail=0
mkdir -p "$mnt"

if [[ $mode == make ]]; then
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    printf 'label: gpt\nsize=800MiB, type=linux\nsize=800MiB, type=linux\n' | sfdisk -q "$dev"
    udevadm settle
    lazy=lazy_itable_init=0,lazy_journal_init=0
    mkfs.ext4 -q -F -b 4096 -E $lazy -L ex4k "${dev}1"
    mkfs.ext4 -q -F -b 1024 -E $lazy -L ex1k "${dev}2"
    for i in 1 2; do
        mount "${dev}$i" "$mnt"
        python3 - "$mnt" <<'EOF'
import os, sys
root = sys.argv[1]
want = bytearray(4 << 20)
with open(os.path.join(root, 'sparse.bin'), 'wb') as f:
    f.truncate(len(want))
    for block, count, seed in ((100, 50, 1), (300, 10, 2), (600, 10, 3)):
        data = bytes((seed * 131 + i * 7) & 0xFF for i in range(count * 4096))
        f.seek(block * 4096)
        f.write(data)
        want[block * 4096:(block + count) * 4096] = data
with open(os.path.join(root, 'sparse.want'), 'wb') as f:
    f.write(want)
with open(os.path.join(root, 'sparse.bin'), 'rb') as f, open(os.path.join(root, 'sparse2.bin'), 'wb') as g:
    g.truncate(len(want))
    for block, count, seed in ((100, 50, 1), (300, 10, 2), (600, 10, 3)):
        f.seek(block * 4096)
        g.seek(block * 4096)
        g.write(f.read(count * 4096))
with open(os.path.join(root, 'sparse2.want'), 'wb') as f:
    f.write(want)
EOF
        echo "${labels[$((i - 1))]}: sparse.bin $(filefrag "$mnt/sparse.bin" | sed 's/.*: //')"
        umount "$mnt"
    done
    rmdir "$mnt"
    exit 0
fi

for i in 1 2; do
    p=${dev}$i
    label=${labels[$((i - 1))]}
    if out=$(e2fsck -fn "$p" 2>&1); then
        echo "  ok   $label: e2fsck -fn clean"
    else
        echo "  FAIL $label: e2fsck -fn"
        grep -v '^Pass\|^e2fsck\|^$' <<< "$out" | head -20 | sed 's/^/        /'
        fail=$((fail + 1))
    fi
    if errs=$(dumpe2fs -h "$p" 2>/dev/null | grep -E '^(FS Error count|First error|Last error)'); [[ -n $errs ]]; then
        echo "  FAIL $label: the driver recorded errors"
        sed 's/^/        /' <<< "$errs"
        fail=$((fail + 1))
    fi
    mount -o ro "$p" "$mnt"
    if out=$(cd "$mnt" && sha256sum -c sums.txt 2>&1); then
        echo "  ok   $label: $(wc -l < "$mnt/sums.txt") files byte-exact as Linux reads them"
    else
        echo "  FAIL $label: files differ as Linux reads them"
        sed 's/^/        /' <<< "$out"
        fail=$((fail + 1))
    fi
    for f in frag-a.bin frag-b.bin pieces.bin sparse.bin sparse2.bin; do
        depth=$(debugfs -R "ex /$f" "$p" 2>/dev/null | awk 'NR > 1 { d = $2 } END { print d + 0 }')
        echo "       $label: $f $(filefrag "$mnt/$f" | sed 's/.*: //'), tree depth $depth"
    done
    umount "$mnt"
done
rmdir "$mnt"
exit $fail
