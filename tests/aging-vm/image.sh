#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# image.sh - the disk of tests/aging-vm.ps1, run as root in WSL.
#
#   make    one empty ext4 volume with 4 KiB blocks (age4k)
#   check   e2fsck -fn, no error the driver recorded; then how the free
#           space and the measured files lie: e2freefrag's summary and
#           histogram, and the extents filefrag counts per measured file
set -euo pipefail
export LC_ALL=C
mode=${1:?make or check}
size=2214592512
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 2112 MiB disk'; exit 2; }
dev=${disks[0]}
part=${dev}1
mnt=/mnt/aging-vm
fail=0

if [[ $mode == make ]]; then
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    # every block written once: a region of the disk file the host never
    # wrote is slower to write first, and would be timed as the placement's
    dd if=/dev/zero of="$dev" bs=16M count=$((size / 16777216)) oflag=direct status=none
    printf 'label: gpt\nsize=2048MiB, type=linux\n' | sfdisk -q "$dev"
    udevadm settle
    mkfs.ext4 -q -F -b 4096 -E lazy_itable_init=0,lazy_journal_init=0 -L age4k "$part"
    exit 0
fi

if out=$(e2fsck -fn "$part" 2>&1); then
    echo "  ok   e2fsck -fn clean"
else
    echo "  FAIL e2fsck -fn"
    grep -v '^Pass\|^e2fsck\|^$' <<< "$out" | head -20 | sed 's/^/        /'
    fail=$((fail + 1))
fi
if errs=$(dumpe2fs -h "$part" 2>/dev/null | grep -E '^(FS Error count|First error|Last error)'); [[ -n $errs ]]; then
    echo "  FAIL the driver recorded errors"
    sed 's/^/        /' <<< "$errs"
    fail=$((fail + 1))
fi

echo "  free space (e2freefrag):"
e2freefrag "$part" | sed -n '/^Total blocks/,$p' | grep -v '^$' | sed 's/^/        /'

mkdir -p "$mnt"
mount -o ro "$part" "$mnt"
echo "  extents of the measured files (filefrag):"
for d in "$mnt"/m/*/; do
    [[ -d $d ]] || continue
    n=0; e=0; worst=0
    while IFS= read -r line; do
        x=${line##*: }; x=${x%% *}
        n=$((n + 1)); e=$((e + x)); ((x > worst)) && worst=$x
    done < <(find "$d" -type f -exec filefrag {} + 2>/dev/null)
    printf '        %-10s %5d files %7d extents (most %d)\n' "$(basename "$d")" "$n" "$e" "$worst"
done
umount "$mnt"
rmdir "$mnt"
exit $fail
