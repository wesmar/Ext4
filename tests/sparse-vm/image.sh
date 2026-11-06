#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# image.sh - the disk of tests/sparse-vm.ps1, run as root in WSL.
#
#   make    three empty ext4 volumes: 4 KiB blocks (sp4k), 1 KiB blocks (sp1k:
#           a block smaller than a page), and 1 KiB blocks with the smallest
#           journal mke2fs makes (spjn: long changes must go in steps)
#   check   each volume: e2fsck -fn, no error the driver recorded, every file
#           of sums.txt as Linux reads it, the holes as Linux sees them, the
#           sparse mark where it should be and a file Linux can go on changing
set -euo pipefail
export LC_ALL=C
mode=${1:?make or check}
size=872415232
labels=(sp4k sp1k spjn)
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 832 MiB disk'; exit 2; }
dev=${disks[0]}
mnt=/mnt/sparse-vm
fail=0
mkdir -p "$mnt"

if [[ $mode == make ]]; then
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    printf 'label: gpt\nsize=270MiB, type=linux\nsize=270MiB, type=linux\nsize=270MiB, type=linux\n' | sfdisk -q "$dev"
    udevadm settle
    lazy=lazy_itable_init=0,lazy_journal_init=0
    mkfs.ext4 -q -F -b 4096 -E $lazy -L sp4k "${dev}1"
    mkfs.ext4 -q -F -b 1024 -E $lazy -L sp1k "${dev}2"
    mkfs.ext4 -q -F -b 1024 -J size=1 -E $lazy -L spjn "${dev}3"
    exit 0
fi

check() { # what, condition
    if eval "$2"; then echo "  ok   $label: $1"; else echo "  FAIL $label: $1"; fail=$((fail + 1)); fi
}

for i in 1 2 3; do
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

    check 'the mark is on the disk' "debugfs -R 'ea_list /left-holes.bin' '$p' 2>/dev/null | grep -q 'system.sparse'"
    check 'a cleared mark is gone' "! debugfs -R 'ea_list /left-cleared.bin' '$p' 2>/dev/null | grep -q 'system.sparse'"

    mount -o ro "$p" "$mnt"
    if out=$(cd "$mnt" && sha256sum -c --quiet sums.txt 2>&1); then
        echo "  ok   $label: $(wc -l < "$mnt/sums.txt") files byte-exact as Linux reads them"
    else
        echo "  FAIL $label: files differ as Linux reads them"
        head -20 <<< "$out" | sed 's/^/        /'
        fail=$((fail + 1))
    fi
    check 'the mark is no name Linux lists' "[[ -z \$(getfattr -d -m - '$mnt/left-holes.bin' 2>/dev/null) ]]"
    # 64 MiB with 4 MiB written, at most: blocks plus the extent nodes
    used=$(du --block-size=1 "$mnt/left-holes.bin" | cut -f1)
    check "holes take nothing ($used bytes)" "(( used <= 4400000 ))"
    # SEEK_DATA / SEEK_HOLE, as Cygwin's and everybody's cp asks
    seen=$(python3 -I -c '
import os, sys
f = os.open(sys.argv[1], os.O_RDONLY); e = os.fstat(f).st_size; a = 0; r = []
while a < e:
    try: d = os.lseek(f, a, os.SEEK_DATA)
    except OSError: break
    h = os.lseek(f, d, os.SEEK_HOLE); r.append("%d+%d" % (d, h - d)); a = h
print(",".join(r))' "$mnt/left-holes.bin")
    M=1048576
    want="$((8 * M))+$M,$((32 * M))+$((M / 2)),$((33 * M + M / 2))+$((M / 2)),$((63 * M))+$M"
    check "data where it was written ($seen)" "[[ '$seen' == '$want' ]]"
    umount "$mnt"

    # Linux takes over a file with holes
    mount "$p" "$mnt"
    dd if=/dev/urandom of="$mnt/left-holes.bin" bs=4096 seek=4096 count=16 conv=notrunc status=none
    fallocate -p -o $((8 * M)) -l $M "$mnt/left-holes.bin"
    umount "$mnt"
    if out=$(e2fsck -fn "$p" 2>&1); then
        echo "  ok   $label: e2fsck -fn clean after Linux changed the file"
    else
        echo "  FAIL $label: e2fsck -fn after Linux changed the file"
        grep -v '^Pass\|^e2fsck\|^$' <<< "$out" | head -20 | sed 's/^/        /'
        fail=$((fail + 1))
    fi
done
rmdir "$mnt"
exit $fail
