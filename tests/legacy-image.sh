#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Dedicated ext2/ext3 smoke fixture; never repairs Windows-written metadata.
set -euo pipefail
mode=${1:?prepare or check}
size=402653184
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 384 MiB disk'; exit 2; }
dev=${disks[0]}
if lsblk -n -o MOUNTPOINTS "$dev" | grep -q '[^[:space:]]'; then
    echo 'Fixture or child partition is mounted'; exit 2
fi
if [[ $mode == prepare ]]; then
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    printf 'label: gpt\nsize=128MiB, type=linux\ntype=linux\n' | sfdisk -q "$dev"
    udevadm settle
    mkfs.ext2 -q -b 1024 -L smoke-ext2 "${dev}1"
    mkfs.ext3 -q -b 4096 -L smoke-ext3 "${dev}2"
elif [[ $mode != check ]]; then
    echo 'Expected prepare or check'; exit 2
fi
for n in 1 2; do
    part=${dev}${n}
    expected=ext$((n+1))
    [[ $(blkid -s TYPE -o value "$part") == "$expected" ]] || { echo 'Wrong fixture format'; exit 2; }
    e2fsck -fn "$part"
    m=$(mktemp -d)
    # Move out of the mount before cleanup, including on a failed assertion.
    trap 'cd /; if mountpoint -q "$m"; then umount "$m"; fi; rmdir "$m"' EXIT
    if [[ $mode == prepare ]]; then
        mount "$part" "$m"
        python3 - "$m" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
root.joinpath('linux-seed.bin').write_bytes(bytes((i * 17 + 13) % 251 for i in range(655373)))
PY
    else
        options=ro
        # ext2 has no journal and its native Linux driver rejects noload.
        [[ $expected != ext3 ]] || options=ro,noload
        mount -o "$options" "$part" "$m"
        python3 - "$m" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
pattern = lambda count: bytes((i * 17 + 13) % 251 for i in range(count))
assert root.joinpath('linux-seed.bin').read_bytes() == pattern(655373), 'Linux seed changed'
assert root.joinpath('legacy-proof/payload.bin').read_bytes() == pattern(8 * 1024 * 1024 + 123), 'Payload mismatch'
assert root.joinpath('legacy-proof/regrown.bin').read_bytes() == pattern(4097) + bytes(1024 * 1024 - 4097), 'Regrowth mismatch'
assert root.joinpath('legacy-proof/cut-ind.bin').read_bytes() == pattern(102407), 'Cut inside IND mismatch'
assert root.joinpath('legacy-proof/cut-dind.bin').read_bytes() == pattern(6291461), 'Cut inside DIND mismatch'
assert not root.joinpath('legacy-proof/deleted.bin').exists(), 'Deletion did not persist'
assert not root.joinpath('legacy-proof/original.bin').exists(), 'Rename did not persist'
print('Linux independent content verification passed')
PY
        stat=$(debugfs -R 'stat /legacy-proof/payload.bin' "$part" 2>/dev/null)
        [[ $stat == *'(IND)'* && $stat == *'(DIND)'* ]] || { echo 'Indirect levels were not exercised'; exit 1; }
        printf '%s\n' "$stat"
    fi
    cd /
    umount "$m"
    rmdir "$m"
    trap - EXIT
    e2fsck -fn "$part"
    echo "LEGACY-LINUX: $expected $mode passed"
done
echo "LEGACY-LINUX: $mode ALL PASSED"
