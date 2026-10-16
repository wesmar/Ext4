#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# crash-image.sh - the crash-state fixture (tests/crash-vm.ps1), run as root in WSL.
#   prepare <dir>   one 128 MiB disk: a GPT partition with ext4 labelled crashlog,
#                   and <dir>/base.img, the partition as the driver will first see it
#   final <dir>     <dir>/final.img, the partition as the driver left it
set -euo pipefail
mode=${1:?prepare or final}
dir=${2:?work directory}
size=134217728
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 128 MiB disk'; exit 2; }
dev=${disks[0]}
mkdir -p "$dir"
if [[ $mode == prepare ]]; then
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    printf 'label: gpt\ntype=linux\n' | sfdisk -q "$dev"
    udevadm settle
    mkfs.ext4 -q -L crashlog -b 4096 -E lazy_itable_init=0,lazy_journal_init=0 "${dev}1"
    e2fsck -fn "${dev}1" >/dev/null
    dd if="${dev}1" of="$dir/base.img" bs=1M status=none
elif [[ $mode == final ]]; then
    dd if="${dev}1" of="$dir/final.img" bs=1M status=none
else
    echo 'Expected prepare or final'; exit 2
fi
echo "CRASH-IMAGE: $mode done"
