#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# image.sh - the image files of tests/filedisk-vm.ps1, run in WSL.
#
#   make <dir>    fd4k.img.gz (1 GiB, 4 KiB blocks), fd1k.img.gz (512 MiB, 1 KiB
#                 blocks), fdplain.img.gz (128 MiB): mkfs.ext4 in plain files,
#                 packed, for the guest to unpack with holes where they are zero
#   check <dir>   each image the guest sent back (fd4n: the 1 KiB one again, from an
#                 NTFS of 4 KiB sectors): e2fsck -fn, no error the
#                 driver recorded, every file of sums.txt as Linux reads it
set -euo pipefail
export LC_ALL=C
mode=${1:?make or check}
dir=${2:?directory}
work=$(mktemp -d /tmp/filedisk.XXXXXX)
trap 'rm -rf "$work"' EXIT
fail=0

if [[ $mode == make ]]; then
    lazy=lazy_itable_init=0,lazy_journal_init=0,nodiscard
    make() { # label size block
        truncate -s "$2" "$work/$1.img"
        mkfs.ext4 -q -F -b "$3" -E $lazy -L "$1" "$work/$1.img"
        gzip -1 -c "$work/$1.img" > "$dir/$1.img.gz"
        rm -f "$work/$1.img"
    }
    make fd4k 1G 4096
    make fd1k 512M 1024
    make fdplain 128M 4096
    exit 0
fi

for label in fd4k fd1k fd4n; do
    img=$work/$label.img
    gzip -dc "$dir/$label.back.gz" > "$img"
    if out=$(e2fsck -fn "$img" 2>&1); then
        echo "  ok   $label: e2fsck -fn clean"
    else
        echo "  FAIL $label: e2fsck -fn"
        grep -v '^Pass\|^e2fsck\|^$' <<< "$out" | head -20 | sed 's/^/        /'
        fail=$((fail + 1))
    fi
    if errs=$(dumpe2fs -h "$img" 2>/dev/null | grep -E '^(FS Error count|First error|Last error)'); [[ -n $errs ]]; then
        echo "  FAIL $label: the driver recorded errors"
        sed 's/^/        /' <<< "$errs"
        fail=$((fail + 1))
    fi
    mnt=$work/mnt
    mkdir -p "$mnt"
    mount -o ro,loop "$img" "$mnt"
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
    rm -f "$img"
done
exit $fail
