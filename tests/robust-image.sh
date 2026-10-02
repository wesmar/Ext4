#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# robust-image.sh <disk size> <dir> <test>...: one batch of the robustness
# run. The disk of that size gets one Linux partition per corrupted image
# (<dir>/<test>.img, from the e2fsprogs test suite), each image copied to
# the start of its partition. Prints "<partition> <test>" per image.
set -eu
[ "$#" -ge 3 ] || { echo "missing robustness fixture arguments" >&2; exit 2; }
size=$1; dir=$2; shift 2
mapfile -t disks < <(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1}')
[ "${#disks[@]}" -eq 1 ] || { echo "robustness disk not found or ambiguous" >&2; exit 2; }
dev=/dev/${disks[0]}
[ -b "$dev" ] || { echo "robustness target is not a block device" >&2; exit 2; }
if lsblk -nr -o MOUNTPOINTS "$dev" | awk 'NF { found=1 } END { exit !found }'; then
    echo "robustness target is mounted" >&2; exit 2
fi
total=2097152
for t in "$@"; do
    bytes=$(stat -c %s "$dir/$t.img")
    [ "$bytes" -gt 0 ] || { echo "empty robustness image: $t" >&2; exit 2; }
    total=$((total + ((bytes + 1048575) / 1048576) * 1048576))
done
[ "$total" -le "$size" ] || { echo "robustness batch exceeds disk size" >&2; exit 2; }
wipefs -q -a "$dev" > /dev/null 2>&1 || true
{
    echo 'label: gpt'
    for t in "$@"; do
        mib=$(( ($(stat -c %s "$dir/$t.img") + 1048575) / 1048576 ))
        [ "$mib" -lt 1 ] && mib=1
        echo "size=${mib}MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4"
    done
} | sfdisk -q "$dev"
# the partition nodes appear once the kernel has re-read the table
for i in $(seq 1 50); do [ -b "${dev}$#" ] && break; sleep 0.1; done
n=1
for t in "$@"; do
    dd if="$dir/$t.img" of="${dev}${n}" bs=1M conv=fsync status=none
    echo "$n $t"
    n=$((n + 1))
done
