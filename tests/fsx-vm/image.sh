#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# image.sh - the disk of tests/fsx-vm.ps1, run as root in WSL.
#
#   make    two empty ext4 volumes: 4 KiB blocks (fx4k) and 1 KiB blocks
#           (fx1k: more extents per byte, deeper trees)
#   check   each volume: e2fsck -fn, no error the driver recorded, every
#           file of sums.txt (the model's SHA-256) as Linux reads it
set -euo pipefail
export LC_ALL=C
mode=${1:?make or check}
size=939524096
labels=(fx4k fx1k)
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 896 MiB disk'; exit 2; }
dev=${disks[0]}
mnt=/mnt/fsx-vm
fail=0
mkdir -p "$mnt"

if [[ $mode == make ]]; then
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    printf 'label: gpt\nsize=440MiB, type=linux\nsize=440MiB, type=linux\n' | sfdisk -q "$dev"
    udevadm settle
    lazy=lazy_itable_init=0,lazy_journal_init=0
    mkfs.ext4 -q -F -b 4096 -E $lazy -L fx4k "${dev}1"
    mkfs.ext4 -q -F -b 1024 -E $lazy -L fx1k "${dev}2"
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
    if [[ ! -s $mnt/sums.txt ]]; then
        echo "  FAIL $label: no sums.txt"
        fail=$((fail + 1))
    elif out=$(cd "$mnt" && sha256sum -c --quiet sums.txt 2>&1); then
        echo "  ok   $label: $(wc -l < "$mnt/sums.txt") files byte-exact as Linux reads them"
    else
        echo "  FAIL $label: files differ as Linux reads them"
        head -20 <<< "$out" | sed 's/^/        /'
        fail=$((fail + 1))
    fi
    umount "$mnt"
done
rmdir "$mnt"
exit $fail
