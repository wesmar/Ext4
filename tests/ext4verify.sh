#!/bin/bash
# Linux side of run-ext4test.ps1: e2fsck -fn on every ext4 partition of the test VHDX
# (found by size), then mount each one read-only and check the SHA-256 manifest that
# ext4test.ps1 left behind, so the data written through the Windows driver is verified
# by an independent implementation.
size=${1:-1073741824}
dev=$(lsblk -b -o NAME,SIZE,TYPE -n -d | awk -v s="$size" '$2==s && $3=="disk" {print $1; exit}')
[ -z "$dev" ] && { echo "VERIFY: test disk not found"; exit 2; }
rc=0
for part in $(lsblk -rno NAME,FSTYPE "/dev/$dev" | awk '$2=="ext4" {print $1}'); do
  echo "== /dev/$part"
  out=$(e2fsck -fn "/dev/$part" 2>&1); e=$?
  if [ $e -eq 0 ]; then
    echo "$out" | tail -1
  else
    rc=1
    echo "FSCK: exit $e"
    echo "$out" | grep -v '^Pass \|^e2fsck ' | head -30
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
    umount "$m"
  else
    echo "mount failed for /dev/$part"
  fi
  rmdir "$m"
done
exit $rc
