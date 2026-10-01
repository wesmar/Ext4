#!/bin/bash
# luks-image.sh - build the encrypted test disk the way Qubes OS lays it out (WSL).
#
#   partition 1: LUKS2 (argon2id, aes-xts-plain64) -> ext4 directly
#   partition 2: LUKS2 (same parameters as a Qubes 4 install) -> LVM volume group
#                "qtest" with a linear LV "lin" (ext4) and a thin pool "pool"
#                (64 KiB chunks, like Qubes root-pool) holding a thin LV "root" (ext4)
#   partition 3: LUKS1 (pbkdf2) -> ext4, the older format
#
# Every file system gets the same small tree plus a manifest.sha256, so the
# Windows side can be checked byte for byte.
#
#   bash luks-image.sh <size-in-bytes> 'passphrase'     (as root; run-ext4test -Luks does it)
#
# The disk is found by its exact size, never by name: device names move
# between WSL sessions, and a wrong name here would wipe another disk.
set -euo pipefail
SIZE=$1
PASS=$2
mapfile -t HITS < <(lsblk -b -dn -o NAME,SIZE,TYPE | awk -v s="$SIZE" '$2==s && $3=="disk" {print $1}')
[ ${#HITS[@]} -eq 1 ] || { echo "need exactly one disk of $SIZE bytes, found ${#HITS[@]}"; exit 2; }
DEV=/dev/${HITS[0]}
if lsblk -n -o MOUNTPOINTS "$DEV" | grep -q '[^[:space:]]'; then
    echo "$DEV has mounted file systems, refusing"; exit 2
fi
echo "target: $DEV"
KDF=(--pbkdf argon2id --pbkdf-memory 1048576 --pbkdf-parallel 4 --pbkdf-force-iterations 6)

wipefs -a "$DEV" >/dev/null
sfdisk -q "$DEV" <<'EOT'
label: gpt
size=400MiB,  type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=luks-plain
size=2000MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=luks-lvm
size=300MiB,  type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=luks1-plain
EOT
partprobe "$DEV" 2>/dev/null || true
sleep 1
P1=${DEV}1 P2=${DEV}2 P3=${DEV}3

fill() {                        # fill <mountpoint> <tag>
    local m=$1 t=$2
    mkdir -p "$m/data/sub"
    head -c 1000000  /dev/urandom > "$m/data/random-1M.bin"
    head -c 7654321  /dev/urandom > "$m/data/random-7M.bin"
    printf 'hello from %s\n' "$t" > "$m/data/hello.txt"
    for i in $(seq 1 200); do printf '%s file %d\n' "$t" "$i" > "$m/data/sub/f$i.txt"; done
    (cd "$m" && find data -type f -print0 | sort -z | xargs -0 sha256sum > manifest.sha256)
}

mkfs_fill() {                   # mkfs_fill <device> <tag>
    mkfs.ext4 -q -L "$2" "$1"
    local m; m=$(mktemp -d)
    mount "$1" "$m"; fill "$m" "$2"; umount "$m"; rmdir "$m"
}

printf '%s' "$PASS" | cryptsetup luksFormat -q --type luks2 "${KDF[@]}" --key-file=- "$P1"
printf '%s' "$PASS" | cryptsetup open --key-file=- "$P1" lt1
mkfs_fill /dev/mapper/lt1 luks-plain
cryptsetup close lt1

printf '%s' "$PASS" | cryptsetup luksFormat -q --type luks2 "${KDF[@]}" --key-file=- "$P2"
printf '%s' "$PASS" | cryptsetup open --key-file=- "$P2" lt2
pvcreate -q /dev/mapper/lt2
vgcreate -q qtest /dev/mapper/lt2
lvcreate -q -L 300M -n lin qtest
lvcreate -q -L 1200M --chunksize 64k --thinpool pool qtest
lvcreate -q -V 1000M --thin -n root qtest/pool
mkfs_fill /dev/qtest/lin lvm-linear
mkfs_fill /dev/qtest/root lvm-thin
vgchange -q -an qtest
cryptsetup close lt2

printf '%s' "$PASS" | cryptsetup luksFormat -q --type luks1 --pbkdf-force-iterations 100000 --key-file=- "$P3"
printf '%s' "$PASS" | cryptsetup open --key-file=- "$P3" lt3
mkfs_fill /dev/mapper/lt3 luks1-plain
cryptsetup close lt3

sync
echo "image ready"
