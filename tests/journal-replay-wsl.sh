#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# The production journal replay against e2fsck on real ext4 images (WSL):
# generated logs, damaged ones included, written into the journal of a fresh
# file system with 1 KiB and with 4 KiB blocks; e2fsck -E journal_only
# replays one copy, src/journal/JournalReplay.c another, and the home
# blocks, the errors and the failed commits must agree (journal-replay-model/wsl.c).
#
#   sed 's/\r$//' tests/journal-replay-wsl.sh | bash -s -- [cases per block size] [seed]
set -euo pipefail
here=${JR_TESTS:-$(cd "$(dirname "$0")" && pwd)}
cases=${1:-500}
seed=${2:-1}
work=/dev/shm/jr-work
mkdir -p "$work"
gcc -O2 -Wall -Wextra -Werror -o "$work/wsl" "$here/journal-replay-model/wsl.c"
for bs in 1024 4096; do
    img=$work/base-$bs.img
    rm -f "$img"
    truncate -s 32M "$img"
    mke2fs -q -F -t ext4 -b "$bs" -O 64bit,metadata_csum -J size=$((bs == 1024 ? 1 : 4)) "$img"
    debugfs -R "blocks <8>" "$img" 2>/dev/null | tr ' ' '\n' | grep -v '^$' > "$work/journal-$bs.txt"
    total=$(dumpe2fs -h "$img" 2>/dev/null | awk -F: '/^Block count/ { gsub(/ /, "", $2); print $2 }')
    base=$((total - 100))
    if debugfs -R "testb $base 80" "$img" 2>/dev/null | grep -q 'marked in use'; then
        echo "the home blocks are in use"; exit 1
    fi
    "$work/wsl" "$img" "$work/journal-$bs.txt" "$base" "$cases" $((seed * 7919 + bs))
done
rm -f /dev/shm/jr-a.img /dev/shm/jr-b.img /dev/shm/jr-a.out
echo 'JOURNAL-REPLAY-WSL: ALL PASSED'
