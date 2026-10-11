#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Linux-created indexed directories and fresh relative/absolute aliases.
set -euo pipefail
size=${1:-1073741824}
root=${2:-symlink-index-fixture}
[[ "$root" =~ ^[a-zA-Z0-9-]+$ ]] || { echo 'Invalid fixture name'; exit 2; }
dev=$(lsblk -b -dn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print $1}')
[[ -n "$dev" && "$dev" != *$'\n'* ]] || { echo 'Test disk must be unique'; exit 2; }
part=$(lsblk -brno NAME,SIZE,TYPE "/dev/$dev" | awk '$3=="part" {print $2,$1}' | sort -nr | head -1 | awk '{print $2}')
[[ -n "$part" ]] || { echo 'Test partition missing'; exit 2; }
mnt=$(mktemp -d)
trap 'umount "$mnt" 2>/dev/null || true; rmdir "$mnt"' EXIT
mount "/dev/$part" "$mnt"
[[ ! -e "$mnt/$root" ]] || { echo 'Use a new fixture root'; exit 2; }
for name in lib lib32 bin; do
    mkdir -p "$mnt/$root/usr/$name"
    for i in $(seq 0 999); do
        printf "payload/%s/%s" "$name" "$i" > "$mnt/$root/usr/$name/$(printf 'entry-%04d-payload.txt' "$i")"
    done
    ln -s "usr/$name" "$mnt/$root/$name"
done
mkdir "$mnt/$root/var"
ln -s "../usr/lib" "$mnt/$root/var/run"
ln -s "/$root/usr/lib32" "$mnt/$root/var/lock"
ln -s lib "$mnt/$root/chain"
sync
umount "$mnt"
for name in lib lib32 bin; do
    debugfs -R "stat /$root/usr/$name" "/dev/$part" 2>/dev/null | head -5
done
e2fsck -fn "/dev/$part"
echo "SYMLINK-FIXTURE: $root, 1000 files per target, Linux metadata clean"
