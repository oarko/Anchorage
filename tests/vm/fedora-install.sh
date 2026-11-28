#!/usr/bin/env bash
# One-time setup of the Fedora test VM (SPEC §12a), the counterpart of the Kubuntu VM:
# an unattended kickstart install from the Fedora "Everything" network-install image.
#
#   tests/vm/fedora-install.sh [release]     default 45; ~30-60 min (downloads ~3 GB)
#
# Needs iso/Fedora-Everything-netinst-x86_64-<release>-*.iso (checked against Fedora's
# signed CHECKSUM file when it was downloaded). Afterwards use the VM like the other one,
# with ANCHORAGE_VM=fedora:  ANCHORAGE_VM=fedora tests/vm/vm.sh start|ssh|view|reset|...
#
# The test user's password is random and kept only in $DIR/fedora-password (mode 600);
# tests log in with the VM-only SSH key and use passwordless sudo.
set -euo pipefail

release=${1:-45}
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
CONN=qemu:///session
VM=anchorage-fedora
PORT=2223
DIR="$HOME/.local/share/anchorage-vm"
BASE="$DIR/fedora-base.qcow2"
NVRAM="$HOME/.config/libvirt/qemu/nvram/${VM}_VARS.fd"
KSPORT=8765
virsh_() { virsh -c "$CONN" "$@"; }

iso=$(ls "$src"/iso/Fedora-Everything-netinst-x86_64-"$release"-*.iso 2>/dev/null | head -1)
[ -n "$iso" ] || { echo "No Fedora $release network-install image in $src/iso" >&2; exit 2; }
if [ -e "$BASE" ] || virsh_ dominfo "$VM" >/dev/null 2>&1; then
  echo "$VM already exists. To redo it: virsh -c $CONN undefine --nvram $VM; rm -f $BASE" >&2
  exit 2
fi

work="$DIR/fedora-install"
rm -rf "${work:?}"
install -d -m 700 "$work"
cleanup() {
  [ -n "${server:-}" ] && kill "$server" 2>/dev/null || true
  rm -rf "${work:?}"
}
trap cleanup EXIT

# Random password for the test user; only its hash goes into the kickstart.
if [ ! -s "$DIR/fedora-password" ]; then
  (umask 077 && head -c 18 /dev/urandom | base64 | tr -d '/+=' > "$DIR/fedora-password")
fi
hash=$(openssl passwd -6 -stdin < "$DIR/fedora-password")
key=$(cat "$DIR/id_ed25519.pub")
sed -e "s|@RELEASE@|$release|g" -e "s|@PASSWORD_HASH@|$hash|" -e "s|@SSH_KEY@|$key|" \
  "$here/fedora.ks.in" > "$work/ks.cfg"

# The installer's kernel and initrd, booted directly with the kickstart on the command line.
7z e -y -o"$work" "$iso" images/pxeboot/vmlinuz images/pxeboot/initrd.img >/dev/null
label=$(isoinfo -d -i "$iso" | sed -n 's/^Volume id: //p')

# The guest reaches this computer's 127.0.0.1 as 10.0.2.2 (QEMU user-mode network).
python3 -m http.server "$KSPORT" --bind 127.0.0.1 --directory "$work" >/dev/null 2>&1 &
server=$!

qemu-img create -q -f qcow2 "$BASE" 60G
render() { # render <kernel-boot xml> <cdrom xml> <disk>
  sed -e "s|@VM@|$VM|g" -e "s|@PORT@|$PORT|g" -e "s|@NVRAM@|$NVRAM|g" -e "s|@DISK@|$3|g" \
      -e "s|@SERIAL_LOG@|$DIR/fedora-serial.log|g" \
      -e "s|@KERNEL_BOOT@|$1|" -e "s|@CDROM@|$2|" "$here/fedora-domain.xml.in"
}
boot="    <kernel>$work/vmlinuz</kernel><initrd>$work/initrd.img</initrd><cmdline>inst.stage2=hd:LABEL=$label inst.ks=http://10.0.2.2:$KSPORT/ks.cfg inst.text console=ttyS0,115200</cmdline>"
render "$boot" "      <source file='$iso'/>" "$BASE" > "$work/install.xml"
virsh_ define "$work/install.xml" >/dev/null
rm -f "$NVRAM"
virsh_ start "$VM" >/dev/null
echo "Installing Fedora $release (log: $DIR/fedora-serial.log). This takes a while."

# The kickstart ends with poweroff.
for _ in $(seq 720); do
  [ "$(virsh_ domstate "$VM" | head -1)" = "running" ] || break
  sleep 10
done
if [ "$(virsh_ domstate "$VM" | head -1)" = "running" ]; then
  echo "The install didn't finish within 2 hours; see $DIR/fedora-serial.log" >&2
  exit 1
fi

# The final definition boots from the disk; the VM runs on an overlay like the other one.
# Keep the VM's UUID: libvirt refuses to redefine a domain under the same name without it.
uuid=$(virsh_ domuuid "$VM")
render "" "" "$DIR/fedora-test.qcow2" | sed "s|<name>$VM</name>|<name>$VM</name><uuid>$uuid</uuid>|" > "$work/final.xml"
virsh_ define "$work/final.xml" >/dev/null
install -m 444 "$NVRAM" "$DIR/fedora-base_VARS.fd"
export ANCHORAGE_VM=fedora
"$here/vm.sh" reset

# First boot: Fedora makes its SSH host keys (and other first-boot state) now. Fold that into
# the base image, or every reset would give the VM new host keys that SSH refuses.
ssh-keygen -q -f "$DIR/known_hosts" -R "[127.0.0.1]:$PORT" >/dev/null 2>&1 || true
"$here/vm.sh" start >/dev/null
"$here/vm.sh" wait
"$here/vm.sh" stop >/dev/null
qemu-img commit -q "$DIR/fedora-test.qcow2"
install -m 444 "$NVRAM" "$DIR/fedora-base_VARS.fd"
chmod 444 "$BASE"
"$here/vm.sh" reset
echo "Done. Start it with: ANCHORAGE_VM=fedora tests/vm/vm.sh start"
