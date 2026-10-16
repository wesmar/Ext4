#!/bin/bash
# Copyright (c) 2026 Marek Wesolowski (WESMAR)
# SPDX-License-Identifier: MIT
# orphan-image.sh <dir> - volumes with orphans for tests/orphan-vm.ps1 (root, WSL).
#
# Every image is the 126 MiB partition of the crash-state fixture, labelled
# crashlog. Next to each, <name>.kind says what the driver must do with it:
#   valid  finish every orphan: e2fsck then finds none left, and the names
#          and bytes equal e2fsck's own processing of the same volume
#   bad    a bad orphan: recorded as an error, the rest as e2fsck leaves it
#   ro     an orphan file that is not sound: the volume stays read-only
#
# Real orphans come from the kernel: files opened, deleted while open, the
# image copied after a sync (a crash state: the journal needs recovery) or
# while frozen (a clean journal, orphans left). Truncations under way and
# bad orphans are made with debugfs.
set -euo pipefail
dir=${1:?output directory}
size=132120576
work=/dev/shm/orphan-work
mkdir -p "$dir" "$work/m"
rm -f "$dir"/*.img "$dir"/*.kind

blank() {   # blank <image> <mke2fs options...>
    local img=$1; shift
    rm -f "$img"
    truncate -s $size "$img"
    mke2fs -q -F -L crashlog -E lazy_itable_init=0,lazy_journal_init=0 "$@" "$img"
}

random_file() {     # random_file <path> <bytes>
    head -c "$2" /dev/urandom > "$1"
}

# --- orphans the kernel makes ---------------------------------------------

kernel_case() {     # kernel_case <name> <count> <freeze|crash> <mke2fs options...>
    local name=$1 count=$2 how=$3; shift 3
    local img=$work/$name.img i
    blank "$img" "$@"
    mount -o loop "$img" "$work/m"
    mkdir -p "$work/m/dir/sub"
    random_file "$work/m/kept" 300000
    for ((i = 0; i < count; i++)); do
        random_file "$work/m/dir/sub/o$i" $(( (i * 7919) % 3000000 ))
    done
    random_file "$work/m/dir/big" 4000000
    sync
    # hold every orphan-to-be open in one process, then delete them
    (
        for ((i = 0; i < count; i++)); do exec {fd}<"$work/m/dir/sub/o$i"; done
        exec {fd}<"$work/m/dir/big"
        rm -f "$work/m/dir/sub/o"* "$work/m/dir/big"
        sync
        if [[ $how == freeze ]]; then
            fsfreeze -f "$work/m"
            cp --sparse=always "$img" "$dir/$name.img"
            fsfreeze -u "$work/m"
        else
            sleep 1
            cp --sparse=always "$img" "$dir/$name.img"
        fi
    )
    umount "$work/m"
    echo valid > "$dir/$name.kind"
}

kernel_case k-orphanfile 6 crash -t ext4 -b 4096 -O orphan_file
kernel_case k-classic 6 crash -t ext4 -b 4096 -O ^orphan_file
kernel_case k-1k 5 crash -t ext4 -b 1024 -O orphan_file
kernel_case k-ext3 4 crash -t ext3
kernel_case k-many 60 crash -t ext4 -b 4096 -O orphan_file
kernel_case k-many-classic 60 crash -t ext4 -b 4096 -O ^orphan_file
kernel_case k-frozen 5 freeze -t ext4 -b 4096 -O orphan_file

# --- orphans made with debugfs --------------------------------------------

ino() {     # ino <image> <path>
    debugfs -R "stat $2" "$1" 2>/dev/null | awk '/^Inode:/ { print $2; exit }'
}

# files f0..f<n-1>, written from random sources
make_files() {      # make_files <image> <n>
    local img=$1 n=$2 i cmds=$work/cmds
    : > "$cmds"
    for ((i = 0; i < n; i++)); do
        random_file "$work/src$i" $(( 4096 * (1 + (i * 37) % 300) + i * 11 ))
        echo "write $work/src$i f$i" >> "$cmds"
    done
    debugfs -w -f "$cmds" "$img" >/dev/null 2>&1
}

# an orphan chain: "d" deletes (name gone, no links), "t<bytes>" truncates
crafted_case() {    # crafted_case <name> <kind> <mke2fs options> -- <ops...>
    local name=$1 kind=$2; shift 2
    local opts=() img=$work/$name.img
    while [[ $1 != -- ]]; do opts+=("$1"); shift; done
    shift
    blank "$img" "${opts[@]}"
    make_files "$img" $#
    local i=0 op cmds=$work/cmds prev=0 inos=()
    : > "$cmds"
    for op in "$@"; do
        local n
        n=$(ino "$img" "/f$i")
        inos+=("$n")
        case $op in
            d)  echo "unlink /f$i" >> "$cmds"
                echo "sif <$n> links_count 0" >> "$cmds" ;;
            t*) echo "sif <$n> size ${op#t}" >> "$cmds" ;;
            p*) echo "fallocate /f$i 0 ${op#p}" >> "$cmds" ;;   # blocks past the size, kept
        esac
        i=$((i + 1))
    done
    # the chain: the first is the head, each names the next in i_dtime
    for ((i = 0; i < ${#inos[@]}; i++)); do
        local next=0
        (( i + 1 < ${#inos[@]} )) && next=${inos[i + 1]}
        [[ ${!i+x} ]] || true
        echo "sif <${inos[i]}> dtime $next" >> "$cmds"
    done
    echo "ssv last_orphan ${inos[0]}" >> "$cmds"
    debugfs -w -f "$cmds" "$img" >/dev/null 2>&1
    cp --sparse=always "$img" "$dir/$name.img"
    echo "$kind" > "$dir/$name.kind"
}

crafted_case c-mixed valid -t ext4 -b 4096 -O ^orphan_file -- d t5000 d t0 t70001 d
crafted_case c-mixed-of valid -t ext4 -b 4096 -O orphan_file -- t12345 d d t4096
crafted_case c-1k valid -t ext4 -b 1024 -- d t1500 t1 d
crafted_case c-ext3 valid -t ext3 -- t3000 d t0
crafted_case c-delete-only valid -t ext4 -b 4096 -- d d d d d d d d

# a truncation that frees what fallocate kept past the size
img=$work/c-prealloc.img
blank "$img" -t ext4 -b 4096 -O ^orphan_file
make_files "$img" 1
n=$(ino "$img" /f0)
size0=$(debugfs -R "stat /f0" "$img" 2>/dev/null | awk '/Size:/ { for (i = 1; i <= NF; i++) if ($i == "Size:") print $(i + 1); exit }')
printf 'fallocate /f0 0 4000\nsif <%s> size %s\nsif <%s> dtime 0\nssv last_orphan %s\n' "$n" "$size0" "$n" "$n" > "$work/cmds"
debugfs -w -f "$work/cmds" "$img" >/dev/null 2>&1
cp --sparse=always "$img" "$dir/c-prealloc.img"
echo valid > "$dir/c-prealloc.kind"

# --- bad orphans ------------------------------------------------------------

bad_case() {        # bad_case <name> <kind> <debugfs commands after f0..f2 exist>
    local name=$1 kind=$2 cmds=$3 img=$work/$1.img
    blank "$img" -t ext4 -b 4096 -O ^orphan_file
    make_files "$img" 3
    local a b c
    a=$(ino "$img" /f0); b=$(ino "$img" /f1); c=$(ino "$img" /f2)
    cmds=${cmds//A/$a}; cmds=${cmds//B/$b}; cmds=${cmds//C/$c}
    printf '%b' "$cmds" > "$work/cmds"
    debugfs -w -f "$work/cmds" "$img" >/dev/null 2>&1
    cp --sparse=always "$img" "$dir/$name.img"
    echo "$kind" > "$dir/$name.kind"
}

# the head is a free inode
bad_case b-free-head bad 'ssv last_orphan 3000\n'
# a delete, then a next one past the inode count
bad_case b-next-range bad 'unlink /f0\nsif <A> links_count 0\nsif <A> dtime 4000000000\nssv last_orphan A\n'
# a truncate orphan that is a character device: cannot be cut
bad_case b-device bad 'mknod dev0 c 1 3\nssv last_orphan 0\n'
img=$work/b-device.img
d=$(ino "$img" /dev0)
printf 'sif <%s> dtime 0\nssv last_orphan %s\n' "$d" "$d" > "$work/cmds"
debugfs -w -f "$work/cmds" "$img" >/dev/null 2>&1
cp --sparse=always "$img" "$dir/b-device.img"
# a reserved inode at the head
bad_case b-reserved bad 'ssv last_orphan 5\n'

# an orphan file block that fails its checksum: read-only
img=$work/b-orphanfile.img
blank "$img" -t ext4 -b 4096 -O orphan_file,metadata_csum
of=$(dumpe2fs -h "$img" 2>/dev/null | awk -F: '/Orphan file inode/ { gsub(/ /, "", $2); print $2 }')
blk=$(debugfs -R "bmap <$of> 0" "$img" 2>/dev/null)
printf '\x2a' | dd of="$img" bs=1 seek=$(( blk * 4096 + 8 )) conv=notrunc status=none
debugfs -w -R "feature orphan_present" "$img" >/dev/null 2>&1
cp --sparse=always "$img" "$dir/b-orphanfile.img"
echo ro > "$dir/b-orphanfile.kind"

rm -rf "$work"
ls "$dir"/*.img | wc -l | xargs echo 'ORPHAN-IMAGE: images'
