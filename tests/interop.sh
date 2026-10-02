#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# interop.sh - Linux side of the metadata interoperability test.
#
#   prepare: on every ext partition of the test disk, build /interop-linux with
#            what Linux can express and Windows has to read correctly: modes,
#            owners, setuid/setgid/sticky, symlinks of every kind, hard links,
#            a FIFO, nanosecond timestamps, user xattrs. The facts the Windows
#            side checks go to /interop-linux/expect.txt.
#   check:   verify what the Windows side left in /interop-win and what it did
#            to /interop-linux, as Linux sees it, then e2fsck -fn.
#
#   sudo bash interop.sh prepare|check [disk-size-in-bytes]
set -u
mode=$1
size=${2:-1073741824}
dev=$(lsblk -b -dn -o NAME,SIZE,TYPE | awk -v s="$size" '$2==s && $3=="disk" {print $1; exit}')
[ -z "$dev" ] && { echo "INTEROP: test disk not found"; exit 2; }
fail=0
check() {   # check <name> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: expected '$2', got '$3'"; fail=$((fail+1)); fi
}

for part in $(lsblk -rno NAME,FSTYPE "/dev/$dev" | awk '$2=="ext4" || $2=="ext3" || $2=="ext2" {print $1}'); do
    m=$(mktemp -d)
    mount "/dev/$part" "$m" || { echo "INTEROP: mount /dev/$part failed"; fail=$((fail+1)); rmdir "$m"; continue; }
    echo "== /dev/$part"
    L=$m/interop-linux
    W=$m/interop-win

    if [ "$mode" = prepare ]; then
        rm -rf "$L" "$W"
        mkdir -p "$L/sub/deep"
        printf 'target data\n' > "$L/target.txt"
        printf 'deep data\n' > "$L/sub/deep/file.txt"
        for p in 600 644 755 444 4755 2755; do printf 'mode %s\n' $p > "$L/m$p"; chmod $p "$L/m$p"; done
        mkdir "$L/private" && chmod 700 "$L/private"
        mkdir "$L/sticky"  && chmod 1777 "$L/sticky"
        mkdir "$L/setgid"  && chmod 2775 "$L/setgid" && chown 1000:1234 "$L/setgid"
        printf 'owned\n' > "$L/owned.txt" && chown 1000:1000 "$L/owned.txt" && chmod 755 "$L/owned.txt"
        ln -s target.txt           "$L/rel-link"
        ln -s "sub/deep/file.txt"  "$L/rel-deep-link"
        ln -s sub                  "$L/dir-link"
        ln -s nowhere              "$L/dangling-link"
        ln -s rel-link             "$L/chain-link"
        ln -s ../interop-linux/target.txt "$L/up-link"
        printf 'linked\n' > "$L/hard1" && ln "$L/hard1" "$L/hard2" && ln "$L/hard1" "$L/sub/hard3"
        mkfifo "$L/fifo"
        printf 'timed\n' > "$L/timed.txt"
        touch -m -d '2021-03-04 05:06:07.123456789 UTC' "$L/timed.txt"
        printf 'xattr\n' > "$L/xattr.txt"
        setfattr -n user.color -v blue "$L/xattr.txt" 2>/dev/null || python3 -c "import os; os.setxattr('$L/xattr.txt', b'user.color', b'blue')"
        {
            echo "mtime_timed $(stat -c %Y "$L/timed.txt") $(date -r "$L/timed.txt" +%N)"
            echo "size_target $(stat -c %s "$L/target.txt")"
        } > "$L/expect.txt"
        echo "  prepared $(find "$L" | wc -l) entries"
    else
        # --- what Windows created
        check "win file is regular"            "regular file"   "$(stat -c %F "$W/file.txt" 2>&1)"
        check "win file mode (parent 0755)"    "644"            "$(stat -c %a "$W/file.txt" 2>&1)"
        check "win dir mode"                   "755"            "$(stat -c %a "$W/dir" 2>&1)"
        check "win read-only file has no w"    "444"            "$(stat -c %a "$W/readonly.txt" 2>&1)"
        check "win relative symlink target"    "file.txt"       "$(readlink "$W/rel-link" 2>&1)"
        check "win relative symlink resolves"  "from windows"   "$(cat "$W/rel-link" 2>&1)"
        check "win dir symlink target"         "dir"            "$(readlink "$W/dir-link" 2>&1)"
        check "win dir symlink is a link"      "symbolic link"  "$(stat -c %F "$W/dir-link" 2>&1)"
        check "win deep relative symlink"      "dir/inner.txt"  "$(readlink "$W/deep-link" 2>&1)"
        check "win hard link count"            "2"              "$(stat -c %h "$W/file.txt" 2>&1)"
        check "win hard link same inode"       "$(stat -c %i "$W/file.txt" 2>&1)" "$(stat -c %i "$W/hard.txt" 2>&1)"
        check "win set mtime"                  "1600000000"     "$(stat -c %Y "$W/timed.txt" 2>&1)"
        check "win set mtime ns (100ns units)" "123456700"      "$(date -r "$W/timed.txt" +%N 2>&1)"
        check "win dir in setgid dir: setgid"  "2775"           "$(stat -c %a "$L/setgid/windir" 2>&1)"
        check "win dir in setgid dir: group"   "1234"           "$(stat -c %g "$L/setgid/windir" 2>&1)"
        check "win file in setgid dir: group"  "1234"           "$(stat -c %g "$L/setgid/winfile.txt" 2>&1)"
        check "win file in setgid dir: no sgid" "664"           "$(stat -c %a "$L/setgid/winfile.txt" 2>&1)"
        check "win EA visible as user xattr"   "hello"          "$(getfattr --only-values -n user.WINEA "$W/file.txt" 2>/dev/null || python3 -c "import os; print(os.getxattr('$W/file.txt','user.WINEA').decode(), end='')" 2>&1)"
        # --- what Windows did to Linux objects
        check "linux 0755 file keeps mode after write"  "755"   "$(stat -c %a "$L/owned.txt" 2>&1)"
        check "linux owner kept after write"   "1000:1000"      "$(stat -c %u:%g "$L/owned.txt" 2>&1)"
        check "linux file content written"     "owned+windows"  "$(tr -d '\n' < "$L/owned.txt" 2>&1)"
        check "linux setuid bit kept"          "4755"           "$(stat -c %a "$L/m4755" 2>&1)"
        check "linux xattr kept after EA write" "blue"          "$(getfattr --only-values -n user.color "$L/xattr.txt" 2>/dev/null || python3 -c "import os; print(os.getxattr('$L/xattr.txt','user.color').decode(), end='')" 2>&1)"
        check "linux renamed by windows"       "target data"    "$(cat "$L/renamed.txt" 2>&1)"
        check "linux hard link count after delete" "2"          "$(stat -c %h "$L/hard1" 2>&1)"
        check "linux symlink still a link"     "target.txt"     "$(readlink "$L/rel-link" 2>&1)"
    fi
    umount "$m"; rmdir "$m"
    if [ "$mode" = check ]; then
        out=$(e2fsck -fn "/dev/$part" 2>&1); e=$?
        check "e2fsck clean" "0" "$e"
        [ $e -ne 0 ] && echo "$out" | grep -v '^Pass \|^e2fsck ' | head -15
    fi
done
echo "INTEROP: $fail failed"
exit $fail
