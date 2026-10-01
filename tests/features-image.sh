#!/bin/bash
# features-image.sh <disk size>: the Linux features test disk. One partition,
# ext4 with -O casefold (as SteamOS formats its microSD cards):
#   /cf    a casefolded directory of 300 files with mixed-case, Polish and
#          German names, made by debugfs and indexed by e2fsck -D, which
#          hashes the folded names as the kernel does;
#   /attr  files and directories with chattr +i and +a, SELinux labels.
# A second partition carries large_dir with 1 KiB blocks: a directory with a
# three-level index made by the kernel. Partitions 3-5 carry multi-mount
# protection: released cleanly, held by a running e2fsck, and left behind by
# a node that died (mmp-seq.py stages the last two). Partition 6 carries
# ea_inode: values too large for an xattr block, kept in inodes of their
# own by the kernel (two equal ones share one). The disk is found by its
# size.
size=$1
dev=/dev/$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1; exit}')
[ "$dev" = "/dev/" ] && { echo "disk not found"; exit 2; }
set -e
echo 'label: gpt
size=180MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
size=240MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
size=16MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
size=16MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
size=16MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
size=24MiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
type=0FC63DAF-8483-4772-8E79-3D69D8477DE4' | sfdisk -q "$dev"
sleep 1
p=${dev}1
mkfs.ext4 -q -F -O casefold -E encoding=utf8 -L cftest "$p"

src=$(mktemp -d)
cmds=$(mktemp)
echo "mkdir /cf" >> "$cmds"
echo "mkdir /plain" >> "$cmds"
# +F on the empty directory, keeping the extents flag
echo "set_inode_field /cf flags 0x40080000" >> "$cmds"
names=("Łódź.txt" "straße.txt" "MixedCase.TXT" "ĄĆĘŁŃÓŚŹŻ ąćęłńóśźż.txt" "Ωmega.txt")
for i in $(seq 1 295); do names+=("File-ĄĘŚ-$i.dat"); done
: > "$src/manifest"
for n in "${names[@]}"; do
    f="$src/$(printf '%s' "$n" | sha1sum | cut -c1-16)"
    head -c $((RANDOM * 3 + 1)) /dev/urandom > "$f"
    echo "write $f \"/cf/$n\"" >> "$cmds"
    echo "$(sha256sum < "$f" | cut -d' ' -f1)  $n" >> "$src/manifest"
done
echo "write $src/manifest /cf.sha256" >> "$cmds"

# chattr +i (0x10) and +a (0x20), each with the extents flag (0x80000)
printf 'immutable content\n' > "$src/imm"
printf 'first line\n' > "$src/app"
printf 'inner\n' > "$src/inner"
cat >> "$cmds" <<EOF
mkdir /attr
write $src/imm /attr/immutable.txt
write $src/app /attr/append.log
mkdir /attr/idir
write $src/inner /attr/idir/inner.txt
mkdir /attr/adir
write $src/inner /attr/adir/inner.txt
set_inode_field /attr/immutable.txt flags 0x80010
set_inode_field /attr/append.log flags 0x80020
set_inode_field /attr/idir flags 0x80010
set_inode_field /attr/adir flags 0x80020
mkdir /attr/secdir
write $src/inner /attr/secfile
ea_set /attr/secdir security.selinux unconfined_u:object_r:user_home_t:s0
ea_set /attr/secfile security.selinux unconfined_u:object_r:user_home_t:s0
ea_set /attr/secfile user.color red
EOF

debugfs -w -f "$cmds" "$p" > /dev/null 2>&1
e2fsck -fyD "$p" > /dev/null 2>&1 || true
e2fsck -fn "$p" > /tmp/cf.fsck 2>&1 && echo "image clean" || { echo "image NOT clean"; cat /tmp/cf.fsck; exit 1; }
debugfs -R "stat /cf" "$p" 2>/dev/null | grep -i -o 'Flags: 0x[0-9a-f]*'
debugfs -R "stat /attr/append.log" "$p" 2>/dev/null | grep -i -o 'Flags: 0x[0-9a-f]*'

# partition 2: large_dir with 1 KiB blocks, where names of some 250 bytes
# fill a two-level index at ~35 000 entries. The kernel itself builds
# /big3 past that (a three-level index) and /big (a small one).
p2=${dev}2
mkfs.ext4 -q -F -b 1024 -N 120000 -O large_dir -L ldtest "$p2"
mnt=$(mktemp -d)
mount "$p2" "$mnt"
mkdir "$mnt/big" "$mnt/big3"
for i in $(seq 1 500); do printf 'large_dir\n' > "$mnt/big/entry-$i.txt"; done
pad=$(printf 'x%.0s' $(seq 1 240))
( cd "$mnt/big3" && for i in $(seq -w 1 45000); do : > "k$i-$pad"; done )
umount "$mnt"; rmdir "$mnt"
levels=$(debugfs -R "htree /big3" "$p2" 2>/dev/null | grep -o 'Indirect levels: [0-9]*')
echo "big3: $levels"
[ "$levels" = "Indirect levels: 2" ] || { echo "big3 did not reach a three-level index"; exit 1; }
e2fsck -fn "$p2" > /dev/null 2>&1 && echo "large_dir partition clean" || { echo "large_dir partition NOT clean"; exit 1; }

# partitions 3-5: multi-mount protection (debugfs takes the volume through
# MMP itself and leaves it released)
# (the runner starts a copy in /tmp and names the real directory)
here=${EXT4_TESTS:-$(cd "$(dirname "$0")" && pwd)}
printf 'mmp\n' > "$src/mmp"
for n in 3:mmpclean 4:mmpfsck 5:mmpstale; do
    p=${dev}${n%%:*}
    mkfs.ext4 -q -F -O mmp -L "${n##*:}" "$p"
    debugfs -w -R "write $src/mmp linux.txt" "$p" > /dev/null 2>&1
done
python3 "$here/mmp-seq.py" "${dev}4" 0xE24D4D50 || { echo "cannot stage the MMP sequence"; exit 1; }  # e2fsck running
python3 "$here/mmp-seq.py" "${dev}5" 12345 || { echo "cannot stage the MMP sequence"; exit 1; }       # a node that died

# partition 6: ea_inode, the values set by the kernel itself
p6=${dev}6
mkfs.ext4 -q -F -O ea_inode -L eatest "$p6"
mnt=$(mktemp -d)
mount "$p6" "$mnt"
python3 - "$mnt" <<'PY'
import os, sys
m = sys.argv[1]
def pattern(n, seed): return bytes((i * 7 + seed) % 251 for i in range(n))
def make(name, attrs):
    f = os.path.join(m, name)
    open(f, 'w').write(name + chr(10))
    for k, v in attrs.items(): os.setxattr(f, 'user.' + k, v)
make('big.txt', {'big': pattern(20000, 1), 'small': b's'})
make('keep.txt', {'big': pattern(30000, 3)})
make('shared1.txt', {'same': pattern(9000, 2)})
make('shared2.txt', {'same': pattern(9000, 2)})
# too large for the inode body, equal: one xattr block for both (h_refcount 2)
blockset = {'zeta': pattern(400, 8), 'alpha': pattern(400, 9), 'mid': pattern(400, 10)}
make('blk-a.txt', blockset)
make('blk-b.txt', blockset)
PY
umount "$mnt"; rmdir "$mnt"
e2fsck -fn "$p6" > /dev/null 2>&1 && echo "ea_inode partition clean" || { echo "ea_inode partition NOT clean"; exit 1; }
acl() { debugfs -R "stat /$1" "$p6" 2>/dev/null | grep -o 'File ACL: [0-9]*'; }
[ "$(acl blk-a.txt)" = "$(acl blk-b.txt)" ] && echo "xattr block shared ($(acl blk-a.txt))" || { echo "xattr block not shared"; exit 1; }

# partition 7: inline_data - small files and directories kept in their
# inodes by the kernel: data in i_block and in the system.data attribute,
# directory entries the same way (some past i_block, in system.data)
p7=${dev}7
mkfs.ext4 -q -F -I 256 -O inline_data -L inltest "$p7"
mnt=$(mktemp -d)
mount "$p7" "$mnt"
printf 'hello inline\n' > "$mnt/small.txt"
python3 -c "import sys; sys.stdout.write(''.join(chr(97 + i % 26) for i in range(100)))" > "$mnt/mid.txt"
printf 'grow me' > "$mnt/grow.txt"
python3 -c "import sys; sys.stdout.write('t' * 80)" > "$mnt/trunc.txt"
printf 'delete me\n' > "$mnt/del.txt"
mkdir "$mnt/dir" "$mnt/dir2" "$mnt/deldir" "$mnt/nonempty" "$mnt/many"
printf 'a\n' > "$mnt/dir/a.txt"; printf 'b\n' > "$mnt/dir/b.txt"
printf 'x\n' > "$mnt/dir2/x.txt"; printf 'y\n' > "$mnt/dir2/y.txt"
printf 'keep\n' > "$mnt/nonempty/keep.txt"
for i in 1 2 3 4 5 6; do printf "m$i\n" > "$mnt/many/m$i.txt"; done
umount "$mnt"; rmdir "$mnt"
e2fsck -fn "$p7" > /dev/null 2>&1 && echo "inline_data partition clean" || { echo "inline_data partition NOT clean"; exit 1; }
inl() { debugfs -R "stat /$1" "$p7" 2>/dev/null | grep -c 'Size of inline data'; }
for f in small.txt mid.txt dir many; do
    [ "$(inl $f)" = "1" ] || { echo "/$f is not inline"; exit 1; }
done
echo "inline_data: files and directories inline (many: $(debugfs -R 'stat /many' "$p7" 2>/dev/null | grep -o 'Size of inline data: [0-9]*'))"
rm -rf "$src" "$cmds"
