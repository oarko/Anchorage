#!/usr/bin/env bash
# Attach two real drives of the developer's machine to the test VM, for testing Anchorage
# against real hardware (agreed 2026-11-26; both drives hold nothing irreplaceable):
#
#   Lingames  Samsung 870 QVO 2TB (SATA), one btrfs partition
#   Windows   Samsung 980 PRO 2TB (NVMe), a clean Windows install (EFI, C: NTFS, recovery)
#
#   tests/vm/realdrives.sh status   what is attached, and whether this computer uses them
#   tests/vm/realdrives.sh attach   add both to the VM (VM must be shut off)
#   tests/vm/realdrives.sh detach   remove them from the VM again (VM must be shut off)
#
# Writes inside the VM go to the REAL drives: `vm.sh reset` does not undo them.
#
# Safety:
#  * Drives are named by serial number (/dev/disk/by-id), never sdb/nvme0n1, so a changed
#    device order can't attach the wrong disk.
#  * attach refuses while any partition of either drive is mounted on this computer (two
#    systems writing to one file system corrupts it). Don't open them in Dolphin here while
#    the VM runs.
#  * The VM boots from its own disk only (boot order); the Windows disk is never booted.
#  * The VM runs as you (qemu:///session), so you need read-write access to the two disks.
#    Grant it for these two only, until the next restart (never via the `disk` group):
#      sudo setfacl -m u:$USER:rw /dev/disk/by-id/<both ids below>
#    and remove it afterwards with `sudo setfacl -x u:$USER <same paths>`.
set -euo pipefail

CONN=qemu:///session
VM=anchorage-test
LINGAMES=/dev/disk/by-id/ata-Samsung_SSD_870_QVO_2TB_S5VWNJ0RA03814T
WINDOWS=/dev/disk/by-id/nvme-Samsung_SSD_980_PRO_2TB_S6B0NL0W222630H
# Target names inside the VM's configuration (sda is the VM's CD-ROM).
declare -A TARGET=([$LINGAMES]=sdb [$WINDOWS]=sdc)
declare -A SERIAL=([$LINGAMES]=S5VWNJ0RA03814T [$WINDOWS]=S6B0NL0W222630H)

virsh_() { virsh -c "$CONN" "$@"; }
state() { virsh_ domstate "$VM" 2>/dev/null | head -1; }

# Mounted on this computer? (any partition of the disk)
host_mounts() {
  local dev
  dev=$(readlink -f "$1")
  findmnt -rn -o SOURCE,TARGET | awk -v d="$dev" 'index($1, d) == 1 {print $1 " on " $2}'
}

attached() { virsh_ domblklist "$VM" --details | awk -v s="$1" '$4 == s {found=1} END {exit !found}'; }

check_drive() {
  local d=$1 name=$2 ok=0
  if [ ! -b "$d" ]; then
    echo "$name: $d not found (is the drive connected?)" >&2
    return 1
  fi
  local m
  m=$(host_mounts "$d")
  if [ -n "$m" ]; then
    echo "$name is mounted on this computer: $m" >&2
    echo "  Unmount it first (Dolphin: Unmount, or: udisksctl unmount -b <device>)." >&2
    ok=1
  fi
  local real
  real=$(readlink -f "$d")
  if [ ! -r "$real" ] || [ ! -w "$real" ]; then
    echo "$name: no read-write access to $real for $(id -un). Run:" >&2
    echo "  sudo setfacl -m u:$(id -un):rw $d" >&2
    ok=1
  fi
  return $ok
}

case "${1:-status}" in
  status)
    echo "VM: $(state)"
    for d in "$LINGAMES" "$WINDOWS"; do
      name=$([ "$d" = "$LINGAMES" ] && echo Lingames || echo Windows)
      real=$(readlink -f "$d" 2>/dev/null || echo "missing")
      here=$(host_mounts "$d" | tr '\n' ' ')
      printf '%-9s %-14s attached:%-3s mounted here: %s\n' "$name" "$real" \
        "$(attached "$d" && echo yes || echo no)" "${here:-no}"
    done
    ;;
  attach)
    if [ "$(state)" != "shut off" ]; then
      echo "Shut the VM down first: tests/vm/vm.sh stop" >&2
      exit 1
    fi
    fail=0
    check_drive "$LINGAMES" Lingames || fail=1
    check_drive "$WINDOWS" Windows || fail=1
    [ "$fail" = 0 ] || exit 1
    for d in "$LINGAMES" "$WINDOWS"; do
      if attached "$d"; then
        echo "$(basename "$d") is already attached."
        continue
      fi
      # SATA bus, so the guest sees ordinary disks; cache=none so nothing is held in the
      # host's page cache; the real serial, so lsblk/blkid in the VM look like hardware.
      virsh_ attach-disk "$VM" "$d" "${TARGET[$d]}" --config --targetbus sata --sourcetype block \
        --driver qemu --subdriver raw --cache none --serial "${SERIAL[$d]}" >/dev/null
      echo "Attached $(basename "$d") as ${TARGET[$d]}."
    done
    echo "The VM still boots from its own disk only. Start it with: tests/vm/vm.sh start"
    ;;
  detach)
    if [ "$(state)" != "shut off" ]; then
      echo "Shut the VM down first: tests/vm/vm.sh stop" >&2
      exit 1
    fi
    for d in "$LINGAMES" "$WINDOWS"; do
      if attached "$d"; then
        virsh_ detach-disk "$VM" "${TARGET[$d]}" --config >/dev/null
        echo "Detached $(basename "$d")."
      fi
    done
    echo "Remove your access again with:"
    echo "  sudo setfacl -x u:$(id -un) $LINGAMES $WINDOWS"
    ;;
  *)
    echo "usage: $0 status|attach|detach" >&2
    exit 2
    ;;
esac
