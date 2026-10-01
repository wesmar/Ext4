#!/bin/bash
# robust-image.sh <disk size> <dir> <test>...: one batch of the robustness
# run. The disk of that size gets one Linux partition per corrupted image
# (<dir>/<test>.img, from the e2fsprogs test suite), each image copied to
# the start of its partition. Prints "<partition> <test>" per image.
size=$1; dir=$2; shift 2
dev=/dev/$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1; exit}')
[ "$dev" = "/dev/" ] && { echo "disk not found"; exit 2; }
set -e
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
