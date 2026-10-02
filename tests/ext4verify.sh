#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Linux side of run-ext4test.ps1: e2fsck -fn on every ext4 partition of the test VHDX
# (found by size), then mount each one read-only and check the SHA-256 manifest that
# ext4test.ps1 left behind, so the data written through the Windows driver is verified
# by an independent implementation.
size=${1:-1073741824}
dev=$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1}')
[ -n "$dev" ] && [ "$(printf '%s\n' "$dev" | wc -l)" -eq 1 ] || { echo "VERIFY: test disk missing or ambiguous"; exit 2; }
parts=$(lsblk -rno NAME,FSTYPE "/dev/$dev" | awk '$2=="ext4" {print $1}')
[ -n "$parts" ] || { echo "VERIFY: no ext4 partition found"; exit 2; }
rc=0
for part in $parts; do
  echo "== /dev/$part"
  out=$(e2fsck -fn "/dev/$part" 2>&1); e=$?
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
