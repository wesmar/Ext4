#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Linux side of run-ext4test.ps1: e2fsck -fn on every ext4 partition of the test VHDX
# (found by size), then mount each one read-only and check the SHA-256 manifest that
# ext4test.ps1 left behind, so the data written through the Windows driver is verified
# by an independent implementation.
# e2fsck -fn passes a volume whose only fault is an error the driver
# recorded in the superblock (EXT4_ERROR_FS, s_error_count), so a false
# alarm of the driver's checks would go unseen: the record is read too,
# with the place it names
fsck_clean() {     # fsck_clean <device>: e2fsck -fn, and no error recorded
    e2fsck -fn "$1" || return $?
    dumpe2fs -h "$1" 2>/dev/null | awk -F': *' '
        $1 == "Filesystem state" && $2 ~ /error/ { bad = 1 }
        $1 == "FS Error count" && $2 + 0 > 0 { bad = 1 }
        $1 ~ /^(First|Last) error/ { info = info "\n  " $0 }
        END { if (bad) { print "errors recorded in the superblock:" info; exit 8 } }'
}
size=${1:-1073741824}
dev=$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1}')
[ -n "$dev" ] && [ "$(printf '%s\n' "$dev" | wc -l)" -eq 1 ] || { echo "VERIFY: test disk missing or ambiguous"; exit 2; }
parts=$(lsblk -rno NAME,FSTYPE "/dev/$dev" | awk '$2=="ext4" {print $1}')
[ -n "$parts" ] || { echo "VERIFY: no ext4 partition found"; exit 2; }
rc=0
for part in $parts; do
  echo "== /dev/$part"
  out=$(fsck_clean "/dev/$part" 2>&1); e=$?
  if [ $e -eq 0 ]; then
    echo "$out" | tail -1
  else
    rc=1
    echo "FSCK: exit $e"
    echo "$out"
    # Do not replay a journal or mount a known-corrupt fixture for data checks.
    continue
  fi
  m=$(mktemp -d)
  if mount -o ro "/dev/$part" "$m" 2>/dev/null; then
    if [ -f "$m/ext4test/manifest.sha256" ]; then
      n=$(wc -l < "$m/ext4test/manifest.sha256")
      if (cd "$m/ext4test/verify" && sha256sum -c --quiet ../manifest.sha256); then
        echo "VERIFY: $n files match from Linux"
      else
        echo "VERIFY: MISMATCH"; rc=1
      fi
    fi
    if ! umount "$m"; then
      echo "VERIFY: unmount failed for /dev/$part"; rc=1
    fi
  else
    echo "VERIFY: mount failed for /dev/$part"; rc=1
  fi
  rmdir "$m"
done
exit $rc
