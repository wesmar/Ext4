#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# The production name hashes (src/ext4/NameHash.c) against e2fsprogs'
# debugfs dx_hash, in WSL: random names over every byte value a command can
# carry, every hash version, seeded and not (name-hash-model/wsl.c).
#
#   sed 's/\r$//' tests/name-hash-wsl.sh | bash -s -- [names] [seed]
set -euo pipefail
export LC_ALL=C     # names are bytes, not text
here=${NH_TESTS:-$(cd "$(dirname "$0")" && pwd)}
count=${1:-200000}
seed=${2:-1}
work=/dev/shm/nh-work
mkdir -p "$work"
gcc -O2 -Wall -Wextra -Werror -o "$work/wsl" "$here/name-hash-model/wsl.c"
truncate -s 8M "$work/fs.img"
mke2fs -q -F "$work/fs.img"
"$work/wsl" "$count" "$seed" "$work/cmds" "$work/want"
debugfs -f "$work/cmds" "$work/fs.img" 2>/dev/null |
    sed -n 's/^Hash of .* is \(0x[0-9a-f]*\) (minor \(0x[0-9a-f]*\))$/\1 \2/p' > "$work/got"
got=$(wc -l < "$work/got")
differ=$(paste -d' ' "$work/want" "$work/got" | awk '$1 != $3 || $2 != $4' | wc -l)
echo "NAME-HASH-WSL: $count names, $got hashed by debugfs, $differ differ"
rm -rf "$work"
[[ $got == "$count" && $differ == 0 ]] && echo 'NAME-HASH-WSL: ALL PASSED'
