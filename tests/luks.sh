#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# luks.sh <disk size> <passphrase>: open the ext4-in-LUKS partitions written
# from Windows, check what Windows wrote (and what Linux wrote before),
# e2fsck, close again; then the LVM volumes of partition 2 (e2fsck).
# Read-only; the disk is found by its size.
size=$1; pass=$2
dev=/dev/$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1; exit}')
[ "$dev" = "/dev/" ] && { echo "disk not found"; exit 2; }
fail=0
for n in 1 3; do
    p=${dev}$n; name=cc$n
    printf '%s' "$pass" | cryptsetup open --readonly --key-file=- "$p" $name || { echo "open $p failed"; fail=1; continue; }
    echo "== $p ($(cryptsetup status $name | awk '/type:/{print $2}'), sector $(cryptsetup status $name | awk '/sector size:/{print $3}'))"
    if e2fsck -fn /dev/mapper/$name >/tmp/fsck.$n 2>&1; then echo "  e2fsck clean"; else echo "  e2fsck FAILED"; tail -5 /tmp/fsck.$n; fail=1; fi
    mkdir -p /mnt/$name && mount -o ro,noload /dev/mapper/$name /mnt/$name
    ( cd /mnt/$name && sha256sum --quiet -c manifest.sha256 && echo "  linux files ok" ) || fail=1
    if [ -f /mnt/$name/winwrite.sha256 ]; then
        ( cd /mnt/$name && sha256sum --quiet -c winwrite.sha256 && echo "  windows files ok ($(wc -l < winwrite.sha256))" ) || fail=1
    else
        echo "  no windows manifest"; fail=1
    fi
    umount /mnt/$name; cryptsetup close $name
done
# partition 2: LVM inside LUKS2, which Windows reads only; the volumes must
# still be clean. The VG goes down before the LUKS device can close.
p=${dev}2; name=cc2
if printf '%s' "$pass" | cryptsetup open --readonly --key-file=- "$p" $name; then
    echo "== $p (LVM inside LUKS2)"
    vgchange -ay qtest >/tmp/vgchange.log 2>&1 || { echo "  vgchange failed"; tail -3 /tmp/vgchange.log; fail=1; }
    for lv in lin root; do
        if e2fsck -fn /dev/qtest/$lv >/tmp/fsck.$lv 2>&1; then echo "  qtest/$lv e2fsck clean"
        else echo "  qtest/$lv e2fsck FAILED"; tail -5 /tmp/fsck.$lv; fail=1; fi
    done
    vgchange -an qtest >/dev/null 2>&1
    cryptsetup close $name
else
    echo "open $p failed"; fail=1
fi
echo "LUKS-LINUX: $fail"
