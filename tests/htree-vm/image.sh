#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# image.sh - the disk of tests/htree-vm.ps1, run as root in WSL. HT_DIR is
# the directory of this script (tests/htree-vm), seen from WSL.
#
#   make            five partitions, one indexed-directory case each:
#                     ht4k   ext4, 4 KiB blocks, half_md4, signed (mkfs defaults)
#                     ht1k   ext4, 1 KiB blocks, tea, unsigned
#                     ht2k3  ext3, 2 KiB blocks, legacy, signed, no checksums
#                     ht1kl  ext4, 1 KiB blocks, large_dir (three-level index), unsigned
#                     htcf   ext4, 4 KiB blocks, casefold: /h made casefolded
#                   prints "<label> <hash> <unsigned> <seed> <casefold>" for each
#   names <dir> <label> <count> <rng> <hash> <unsigned> <seed> <min> <max> <pairs>
#                   <dir>/<label>.txt: the names for the case (names.c)
#   check <stage>   each volume: e2fsck -fn, the depth of the index of /h,
#                   /h against /expected.txt (check.py; by the kernel where
#                   it mounts the volume, through debugfs where not)
#   linux <dir>     the kernel changes /h of each volume it mounts, with
#                   names from <dir>/linux-<label>.txt
set -euo pipefail
export LC_ALL=C.UTF-8
mode=${1:?make, names, check or linux}
shift
here=${HT_DIR:?the directory of image.sh}
size=1610612736
labels=(ht4k ht1k ht2k3 ht1kl htcf)
mapfile -t disks < <(lsblk -bdn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print "/dev/"$1}')
[[ ${#disks[@]} == 1 ]] || { echo 'Expected exactly one dedicated 1536 MiB disk'; exit 2; }
dev=${disks[0]}
fail=0

part() { echo "${dev}$(( $1 + 1 ))"; }
casefolded() { [[ $1 == htcf ]]; }

if [[ $mode == make ]]; then
    [[ -z $(wipefs -n "$dev") ]] || { echo 'Refusing a nonblank disk'; exit 2; }
    printf 'label: gpt\n%s' "$(for _ in 1 2 3 4 5; do printf 'size=300MiB, type=linux\n'; done)" | sfdisk -q "$dev"
    udevadm settle
    lazy=lazy_itable_init=0,lazy_journal_init=0
    mkfs.ext4 -q -F -b 4096 -E $lazy -L ht4k "$(part 0)"
    mkfs.ext4 -q -F -b 1024 -E $lazy -L ht1k "$(part 1)"
    tune2fs -E hash_alg=tea "$(part 1)" >/dev/null 2>&1
    debugfs -w -R 'ssv flags 2' "$(part 1)" 2>/dev/null
    mke2fs -q -F -t ext3 -b 2048 -E $lazy -L ht2k3 "$(part 2)"
    tune2fs -E hash_alg=legacy "$(part 2)" >/dev/null 2>&1
    mkfs.ext4 -q -F -b 1024 -O large_dir -E $lazy -L ht1kl "$(part 3)"
    debugfs -w -R 'ssv flags 2' "$(part 3)" 2>/dev/null
    mkfs.ext4 -q -F -b 4096 -O casefold -E encoding=utf8,$lazy -L htcf "$(part 4)"
    # +F on the empty directory, keeping the extents flag
    printf 'mkdir /h\nset_inode_field /h flags 0x40080000\n' | debugfs -w -f - "$(part 4)" >/dev/null 2>&1
    for i in 0 1 2 3 4; do
        p=$(part $i)
        e2fsck -fn "$p" >/dev/null 2>&1
        h=$(dumpe2fs -h "$p" 2>/dev/null)
        hash=$(sed -n 's/^Default directory hash: *//p' <<< "$h")
        case $hash in legacy) n=0 ;; half_md4) n=1 ;; tea) n=2 ;; *) echo "hash $hash?"; exit 2 ;; esac
        uns=0; grep -q '^Filesystem flags:.*unsigned_directory_hash' <<< "$h" && uns=1
        seed=$(sed -n 's/^Directory Hash Seed: *//p' <<< "$h")
        cf=0; casefolded "${labels[$i]}" && cf=1
        echo "${labels[$i]} $n $uns $seed $cf"
    done
    exit 0
fi

if [[ $mode == names ]]; then
    out=$1 label=$2
    shift 2
    [[ -x /root/htn/names ]] || { mkdir -p /root/htn && gcc -O2 -Wall -Wextra -Werror -o /root/htn/names "$here/names.c"; }
    echo "$label: $(/root/htn/names "$out/$label.txt" "$@")"
    exit 0
fi

mnt=/mnt/htree-vm
mkdir -p "$mnt"
for i in 0 1 2 3 4; do
    label=${labels[$i]}
    p=$(part $i)
    if [[ $mode == check ]]; then
        stage=${1:?stage}
        if out=$(e2fsck -fn "$p" 2>&1); then
            echo "  ok   $label: e2fsck -fn clean ($stage)"
        else
            echo "  FAIL $label: e2fsck -fn ($stage)"
            grep -v '^Pass\|^e2fsck\|^$' <<< "$out" | head -20 | sed 's/^/        /'
            fail=$((fail + 1))
        fi
        # e2fsck passes a volume whose only fault is an error the driver
        # recorded; such a record is a failure here, with the place it names
        if errs=$(dumpe2fs -h "$p" 2>/dev/null | grep -E '^(FS Error count|First error|Last error)'); [[ -n $errs ]]; then
            echo "  FAIL $label: the driver recorded errors ($stage)"
            sed 's/^/        /' <<< "$errs"
            fail=$((fail + 1))
        fi
        levels=$(set +o pipefail; debugfs -R 'htree /h' "$p" 2>/dev/null | grep -m1 'Indirect levels' | tr -dc '0-9')
        if [[ -n $levels ]]; then
            echo "       $label: /h indexed, $(( levels + 1 )) level(s) below the root block"
        else
            echo "  FAIL $label: /h is not indexed"
            fail=$((fail + 1))
        fi
        if casefolded "$label"; then
            debugfs -R 'cat /expected.txt' "$p" 2>/dev/null > /dev/shm/ht-expected.txt
            debugfs -R 'ls -p /h' "$p" 2>/dev/null > /dev/shm/ht-ls.txt
            python3 "$here/check.py" check "$label" /dev/shm/ht-expected.txt --listing /dev/shm/ht-ls.txt || fail=$((fail + 1))
        else
            mount -o ro "$p" "$mnt"
            python3 "$here/check.py" check "$label" "$mnt/expected.txt" --root "$mnt" || fail=$((fail + 1))
            umount "$mnt"
        fi
    elif [[ $mode == linux ]]; then
        casefolded "$label" && continue      # this kernel has no casefold
        mount "$p" "$mnt"
        python3 "$here/check.py" linux "$label" "$mnt/expected.txt" --root "$mnt" --names "${1:?names dir}/linux-$label.txt" --seed $((i + 11)) || fail=$((fail + 1))
        umount "$mnt"
    else
        echo 'Expected make, names, check or linux'; exit 2
    fi
done
rmdir "$mnt"
rm -f /dev/shm/ht-expected.txt /dev/shm/ht-ls.txt
exit $fail
