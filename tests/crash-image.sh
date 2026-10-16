#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# crash-image.sh - the crash-state fixture (tests/crash-vm.ps1), run as root in WSL.
#   prepare <dir> [mkfs options]
#                   one 128 MiB disk: a GPT partition with ext4 (or what the options
#                   make: -t ext3, -b 1024 ...) labelled crashlog, and <dir>/base.img,
#                   the partition as the driver will first see it
#   final <dir>     <dir>/final.img, the partition as the driver left it
#   put <image>     the partition (made on a blank disk) gets <image>
#                   (tests/crash-recover-vm.ps1)
#   get <image>     <image> gets the partition
set -euo pipefail
mode=${1:?prepare, final, put or get}
dir=${2:?work directory or image}
shift 2
size=134217728
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 128 MiB disk'; exit 2; }
dev=${disks[0]}
if [[ $mode == prepare ]]; then
    mkdir -p "$dir"
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    printf 'label: gpt\ntype=linux\n' | sfdisk -q "$dev"
    udevadm settle
    [[ $# -gt 0 ]] || set -- -t ext4 -b 4096
    mke2fs -q -L crashlog -E lazy_itable_init=0,lazy_journal_init=0 "$@" "${dev}1"
    e2fsck -fn "${dev}1" >/dev/null
    dd if="${dev}1" of="$dir/base.img" bs=1M status=none
elif [[ $mode == final ]]; then
    dd if="${dev}1" of="$dir/final.img" bs=1M status=none
elif [[ $mode == put ]]; then
    if [[ ! -b ${dev}1 ]]; then
        [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
        printf 'label: gpt\ntype=linux\n' | sfdisk -q "$dev"
        udevadm settle
    fi
    [[ $(blockdev --getsize64 "${dev}1") == $(stat -c %s "$dir") ]] || { echo 'The image does not fit the partition'; exit 2; }
    dd if="$dir" of="${dev}1" bs=1M conv=fsync status=none
elif [[ $mode == get ]]; then
    dd if="${dev}1" of="$dir" bs=1M status=none
else
    echo 'Expected prepare, final, put or get'; exit 2
fi
echo "CRASH-IMAGE: $mode done"
