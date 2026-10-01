#!/bin/bash
# features.sh <disk size>: what Windows did on the Linux features disk,
# seen from Linux. e2fsck checks every index block of the casefolded
# directory against the folded hashes (an entry hashed from its unfolded
# name lands outside its block's range and is reported); the files Windows
# wrote there must be byte-exact and the directory it made casefolded too.
# Of the chattr +i / +a files only what Linux allows may have changed.
# Read-only; the disk is found by its size.
size=$1
dev=/dev/$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1; exit}')
[ "$dev" = "/dev/" ] && { echo "disk not found"; exit 2; }
p=${dev}1
fail=0
if e2fsck -fn "$p" > /tmp/cf.fsck 2>&1; then echo "  e2fsck clean"; else echo "  e2fsck FAILED"; tail -15 /tmp/cf.fsck; fail=1; fi

tmp=$(mktemp -d)
debugfs -R "dump /winwrite.sha256 $tmp/manifest" "$p" > /dev/null 2>&1
n=0; bad=0
while read -r sum name; do
    [ -z "$name" ] && continue
    n=$((n + 1))
    debugfs -R "dump \"/cf/$name\" $tmp/f" "$p" > /dev/null 2>&1
    [ "$(sha256sum < "$tmp/f" | cut -d' ' -f1)" = "$sum" ] || bad=$((bad + 1))
done < "$tmp/manifest"
[ "$n" -gt 0 ] && [ "$bad" -eq 0 ] && echo "  windows files ok ($n)" || { echo "  windows files: $bad of $n differ"; fail=1; }
flags=$(debugfs -R "stat /cf/Subdir-Ś" "$p" 2>/dev/null | grep -o 'Flags: 0x[0-9a-f]*' | cut -d' ' -f2)
[ $(( flags & 0x40000000 )) -ne 0 ] && echo "  new directory casefolded ($flags)" || { echo "  new directory NOT casefolded ($flags)"; fail=1; }

# chattr +i / +a: content as Linux allows it to be
want() {    # want <path> <expected content>
    if debugfs -R "dump \"$1\" $tmp/f" "$p" > /dev/null 2>&1 && [ "$(cat "$tmp/f")" = "$(printf '%b' "$2")" ]; then
        echo "  $1 as expected"
    else
        echo "  $1 NOT as expected: $(cat "$tmp/f" 2>/dev/null | head -c 80)"; fail=1
    fi
}
absent() {  # absent <path>
    if debugfs -R "stat \"$1\"" "$p" 2>/dev/null | grep -q '^Inode:'; then echo "  $1 exists, must not"; fail=1; else echo "  $1 absent"; fi
}
want /attr/immutable.txt 'immutable content'
want /attr/append.log 'first line\nfrom windows'
want /attr/idir/inner.txt 'changed'
want /attr/adir/inner.txt 'inner'
want /attr/adir/added.txt 'added'
absent /attr/moved.txt
absent /attr/imm-link.txt
absent /attr/idir/new.txt
absent /attr/idir/x.txt
absent /attr/adir/x.txt
for f in /attr/immutable.txt:0x80010 /attr/append.log:0x80020 /attr/idir:0x80010 /attr/adir:0x80020; do
    got=$(debugfs -R "stat ${f%%:*}" "$p" 2>/dev/null | grep -o 'Flags: 0x[0-9a-f]*' | cut -d' ' -f2)
    [ "$got" = "${f##*:}" ] || { echo "  ${f%%:*} flags $got, want ${f##*:}"; fail=1; }
done
# extended attributes: labels inherited on create, Linux namespaces kept
# when Windows overwrites a file with its own EA set (the user namespace)
label='unconfined_u:object_r:user_home_t:s0'
ea() {      # ea <path> <name>: the value, or nothing
    debugfs -R "ea_get \"$1\" $2" "$p" 2>/dev/null | tr -d '\n' | sed 's/^[^=]*= //; s/^"//; s/"$//; s/ (.*$//'
}
for f in /attr/secdir/new.txt /attr/secdir/sub /attr/secfile; do
    got=$(ea "$f" security.selinux)
    [ "$got" = "$label" ] && echo "  $f labelled" || { echo "  $f label '$got', want '$label'"; fail=1; }
done
[ -z "$(ea /attr/secfile user.color)" ] && echo "  /attr/secfile: old user EA replaced" || { echo "  /attr/secfile: user.color survived"; fail=1; }
[ "$(ea /attr/secfile user.shade)" = "blue" ] && echo "  /attr/secfile: new user EA set" || { echo "  /attr/secfile: user.shade '$(ea /attr/secfile user.shade)'"; fail=1; }
# partition 2: large_dir. e2fsck checks every index level; the kernel must
# find Windows's names through the index (a read-only mount, no replay)
p2=${dev}2
if e2fsck -fn "$p2" > /tmp/ld.fsck 2>&1; then echo "  large_dir: e2fsck clean"; else echo "  large_dir: e2fsck FAILED"; tail -15 /tmp/ld.fsck; fail=1; fi
levels=$(debugfs -R "htree /grow" "$p2" 2>/dev/null | grep -o 'Indirect levels: [0-9]*')
[ "$levels" = "Indirect levels: 2" ] && echo "  /grow: $levels" || { echo "  /grow: '$levels', want a three-level index"; fail=1; }
pad=$(printf 'x%.0s' $(seq 1 240))
mnt=$(mktemp -d)
if mount -o ro,noload "$p2" "$mnt"; then
    n=$(ls "$mnt/grow" | wc -l); [ "$n" -eq 44550 ] && echo "  /grow: $n entries" || { echo "  /grow: $n entries, want 44550"; fail=1; }
    n=$(ls "$mnt/big3" | wc -l); [ "$n" -eq 46900 ] && echo "  /big3: $n entries" || { echo "  /big3: $n entries, want 46900"; fail=1; }
    miss=0
    for i in $(seq 7 977 45000); do [ -e "$mnt/grow/g$(printf %05d "$i")-$pad" ] || [ $((i % 100)) -eq 1 ] || miss=$((miss + 1)); done
    for i in $(seq 3 97 2000); do [ -e "$mnt/big3/w$(printf %05d "$i")-$pad" ] || miss=$((miss + 1)); done
    for i in $(seq 1 450 45000); do [ -e "$mnt/big3/k$(printf %05d "$i")-$pad" ] && miss=$((miss + 1)); done
    [ "$miss" -eq 0 ] && echo "  kernel lookups through the index agree" || { echo "  $miss kernel lookups wrong"; fail=1; }
    umount "$mnt"
else
    echo "  large_dir: cannot mount"; fail=1
fi
rmdir "$mnt"

# multi-mount protection: what Windows left in the MMP blocks
mmp() {     # mmp <partition> <field>: a field of debugfs dump_mmp
    debugfs -c -R "dump_mmp" "$1" 2>/dev/null | awk -v f="$2" '$1 == f":" {print $2}'
}
for n in 3 5; do
    p=${dev}$n
    seq=$(mmp "$p" sequence); devn=$(mmp "$p" device_name)
    [ "$seq" = "ff4d4d50" ] && [ "$devn" = "ext4.sys" ] && echo "  partition $n: released by ext4.sys" || { echo "  partition $n: MMP sequence $seq device '$devn'"; fail=1; }
    if e2fsck -fn "$p" > /tmp/mmp.fsck 2>&1; then echo "  partition $n: e2fsck clean"; else echo "  partition $n: e2fsck FAILED"; tail -5 /tmp/mmp.fsck; fail=1; fi
    debugfs -c -R "cat windows.txt" "$p" 2>/dev/null | grep -q '^mmp$' && echo "  partition $n: Windows's file there" || { echo "  partition $n: Windows's file missing"; fail=1; }
done
seq=$(mmp "${dev}4" sequence)
[ "$seq" = "e24d4d50" ] && echo "  partition 4: left alone (e2fsck's sequence)" || { echo "  partition 4: MMP sequence $seq"; fail=1; }
debugfs -c -R "stat windows.txt" "${dev}4" 2>/dev/null | grep -q '^Inode:' && { echo "  partition 4: written to"; fail=1; }

# partition 6: ea_inode. e2fsck counts every value inode's references
# against the entries that name them, and finds the ones nobody names; the
# kernel checks each value against its crc32c and the entry's hash as it
# reads it
p6=${dev}6
if e2fsck -fn "$p6" > /tmp/ea.fsck 2>&1; then echo "  ea_inode: e2fsck clean"; else echo "  ea_inode: e2fsck FAILED"; tail -15 /tmp/ea.fsck; fail=1; fi
mnt=$(mktemp -d)
if mount -o ro,noload "$p6" "$mnt"; then
    python3 - "$mnt" <<'PY' || fail=1
import os, sys
m = sys.argv[1]
def pattern(n, seed): return bytes((i * 7 + seed) % 251 for i in range(n))
bad = 0
def want(name, attr, value):
    global bad
    try:
        got = os.getxattr(os.path.join(m, name), 'user.' + attr)
    except OSError as e:
        got = e
    if got == value:
        print('  ea_inode: %s %s as expected' % (name, attr))
    else:
        print('  ea_inode: %s %s NOT as expected (%s)' % (name, attr, got if isinstance(got, OSError) else len(got)))
        bad += 1
def absent(name):
    global bad
    if os.path.exists(os.path.join(m, name)):
        print('  ea_inode: %s exists, must not' % name); bad += 1
    else:
        print('  ea_inode: %s absent' % name)
want('win-big.txt', 'winbig', pattern(40000, 4))
want('big.txt', 'big', b'tiny')
want('big.txt', 'small', b's')
want('keep.txt', 'big', pattern(25000, 5))
want('shared2.txt', 'same', pattern(9000, 2))
absent('shared1.txt')
# the shared block given up by one inode only; every name found (Linux
# looks entries up in a block in sorted order)
for f in ('blk-a.txt', 'blk-b.txt'):
    want(f, 'zeta', pattern(400, 8)); want(f, 'alpha', pattern(400, 9)); want(f, 'mid', pattern(400, 10))
want('blk-a.txt', 'win', b'own block')
try:
    os.getxattr(os.path.join(m, 'blk-b.txt'), 'user.win'); print('  ea_inode: blk-b.txt got win'); bad += 1
except OSError:
    pass
absent('win-gone.txt')
try:
    os.getxattr(os.path.join(m, 'win-drop.txt'), 'user.drop'); print('  ea_inode: win-drop.txt drop still there'); bad += 1
except OSError:
    print('  ea_inode: win-drop.txt drop removed')
sys.exit(1 if bad else 0)
PY
    umount "$mnt"
else
    echo "  ea_inode: cannot mount"; fail=1
fi
rmdir "$mnt"

# partition 7: inline_data. e2fsck checks the inline files and directories
# Windows left alone and the ones it moved out to blocks; the kernel reads
# them all back
p7=${dev}7
if e2fsck -fn "$p7" > /tmp/inl.fsck 2>&1; then echo "  inline_data: e2fsck clean"; else echo "  inline_data: e2fsck FAILED"; tail -15 /tmp/inl.fsck; fail=1; fi
mnt=$(mktemp -d)
if mount -o ro,noload "$p7" "$mnt"; then
    python3 - "$mnt" <<'PY' || fail=1
import os, sys
m = sys.argv[1]
bad = 0
def want(path, content):
    global bad
    try:
        got = open(os.path.join(m, path), 'rb').read()
    except OSError as e:
        got = e
    if got == content:
        print('  inline_data: %s as expected' % path)
    else:
        print('  inline_data: %s NOT as expected (%r)' % (path, got if isinstance(got, OSError) else got[:40]))
        bad += 1
def names(path, expected):
    global bad
    got = sorted(os.listdir(os.path.join(m, path)))
    if got == sorted(expected):
        print('  inline_data: /%s lists %s' % (path, ','.join(got)))
    else:
        print('  inline_data: /%s lists %s, want %s' % (path, got, sorted(expected)))
        bad += 1
alpha = ''.join(chr(97 + i % 26) for i in range(100)).encode()
want('small.txt', b'hello inline' + bytes([10]))
want('mid.txt', alpha)
want('grow.txt', b'grow me from windows')
want('trunc.txt', b'')
want('dir/new.txt', b'new' + bytes([10]))
want('dir/a.txt', b'a' + bytes([10]))
names('dir', ['a.txt', 'b.txt', 'new.txt'])
names('dir2', ['y.txt'])
names('many', ['m1-renamed.txt', 'm2.txt', 'm3.txt', 'm4.txt', 'm5.txt', 'm6.txt'])
names('nonempty', ['keep.txt'])
for gone in ('del.txt', 'deldir'):
    if os.path.exists(os.path.join(m, gone)):
        print('  inline_data: %s exists, must not' % gone); bad += 1
sys.exit(1 if bad else 0)
PY
    umount "$mnt"
else
    echo "  inline_data: cannot mount"; fail=1
fi
rmdir "$mnt"
rm -rf "$tmp"
echo "FEATURES-LINUX: $fail"
