#!/usr/bin/env bash
# Drive detection against real file systems in the test VM: creates small ext4, btrfs,
# FAT and exFAT disk images with labels, attaches them as loop devices, mounts one, and
# checks that anchorage-e2e-scan reports each of them correctly.
#
#   tests/vm/drives.sh     starts the VM if needed, copies the source, builds the scanner,
#                          runs the checks. The loop devices are removed afterwards.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
vm="$here/vm.sh"
"$vm" start >/dev/null 2>&1 || true
"$vm" wait
"$vm" sync

"$vm" deps
"$vm" ssh -o BatchMode=yes 'bash -s' <<'VM'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
command -v mkfs.exfat >/dev/null || sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq exfatprogs >/dev/null
cmake -S ~/src -B ~/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DANCHORAGE_E2E=ON >/dev/null 2>&1
cmake --build ~/build --target anchorage-e2e-scan >/dev/null
dir=$(mktemp -d)
cleanup() {
  sudo umount /mnt/e2e-ext4 2>/dev/null || true
  for d in $(losetup -j "$dir/ext4.img" -O NAME -n) $(losetup -j "$dir/btrfs.img" -O NAME -n) \
           $(losetup -j "$dir/fat.img" -O NAME -n) $(losetup -j "$dir/exfat.img" -O NAME -n); do sudo losetup -d "$d"; done
  rm -rf "$dir"
}
trap cleanup EXIT
truncate -s 64M "$dir/ext4.img" "$dir/fat.img" "$dir/exfat.img"
truncate -s 128M "$dir/btrfs.img"
mkfs.ext4 -q -L E2E-Ext4 "$dir/ext4.img"
mkfs.btrfs -q -L E2E-Btrfs "$dir/btrfs.img"
mkfs.vfat -n E2EFAT "$dir/fat.img" >/dev/null
mkfs.exfat -L E2E-exFAT "$dir/exfat.img" >/dev/null
for f in ext4 btrfs fat exfat; do sudo losetup -f "$dir/$f.img"; done
sudo udevadm settle
sudo mkdir -p /mnt/e2e-ext4 && sudo mount -L E2E-Ext4 /mnt/e2e-ext4
~/build/bin/anchorage-e2e-scan > "$dir/scan.txt"
echo "--- scanner output (loop images):"
grep -E 'E2E' "$dir/scan.txt" || true
fail=0
expect() { # label fstype mounted
  if grep -qE "\|$2\|$1\|$3\|" "$dir/scan.txt"; then echo "PASS  $1 ($2, $3)"; else echo "FAIL  $1 ($2, $3)"; fail=1; fi
}
expect E2E-Ext4 ext4 mounted
expect E2E-Btrfs btrfs -
expect E2EFAT vfat -
expect E2E-exFAT exfat -
grep -q '|/mnt/e2e-ext4|ext4|E2E-Ext4|mounted|' "$dir/scan.txt" && echo "PASS  mount point of E2E-Ext4" || { echo "FAIL  mount point of E2E-Ext4"; fail=1; }
exit $fail
VM
