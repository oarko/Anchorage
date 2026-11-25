#!/usr/bin/env bash
# Manage the Anchorage test VM (libvirt, per-user session: no root needed).
#
#   tests/vm/vm.sh start     start the VM (cleans up a stale network helper first)
#   tests/vm/vm.sh view      open the VM's screen in virt-manager
#   tests/vm/vm.sh stop      shut down cleanly (forces off after 60 s)
#   tests/vm/vm.sh status    show state and the SSH port
#   tests/vm/vm.sh ssh [cmd] log in as the test user (after setup)
#   tests/vm/vm.sh reset     throw away all changes: back to the clean base image
#   tests/vm/vm.sh wait      wait until the VM accepts SSH (up to 4 minutes)
#   tests/vm/vm.sh sync      copy the source tree into the VM (~/src)
#   tests/vm/vm.sh console   serial console (boot messages, emergency shell); Ctrl+] exits
#
# Disks: anchorage-base.qcow2 is the clean, read-only install (plus its UEFI settings
# anchorage-base_VARS.fd). The VM runs on anchorage-test.qcow2, a thin overlay on top of
# it; `reset` replaces the overlay and UEFI settings with fresh copies.
#
# Networking is QEMU user-mode (see CLAUDE.md). The passt cleanup below is kept for VMs
# still configured with libvirt's passt backend, which can leave its helper running
# after a stop and block the next start with "Address already in use".
set -euo pipefail

CONN=qemu:///session
VM=anchorage-test
PORT=2222
DIR="$HOME/.local/share/anchorage-vm"
KEY="$DIR/id_ed25519"
NVRAM="$HOME/.config/libvirt/qemu/nvram/${VM}_VARS.fd"

virsh_() { virsh -c "$CONN" "$@"; }

state() { virsh_ domstate "$VM" 2>/dev/null | head -1; }

# passt helpers started for this VM (matched on their socket path, never on our own command line).
stale_helpers() {
  pgrep -u "$(id -u)" -f -- "--socket /run/user/$(id -u)/libvirt/qemu/run/passt/[0-9]+-${VM}-net0\.socket" || true
}

cleanup_helpers() {
  if [ "$(state)" = "running" ]; then
    return 0 # the running VM's helper is in use
  fi
  local pids
  pids=$(stale_helpers)
  if [ -n "$pids" ]; then
    echo "Stopping leftover network helper(s): $pids"
    kill $pids 2>/dev/null || true
    sleep 1
  fi
}

case "${1:-status}" in
  start)
    if [ "$(state)" = "running" ]; then
      echo "$VM is already running."
      exit 0
    fi
    cleanup_helpers
    if ss -ltn | grep -q "127.0.0.1:${PORT} "; then
      echo "Port $PORT is used by something else:" >&2
      ss -ltnp | grep ":${PORT} " >&2
      exit 1
    fi
    virsh_ start "$VM"
    ;;
  wait)
    timeout 240 bash -c "until '$0' ssh -o BatchMode=yes -o ConnectTimeout=3 true 2>/dev/null; do sleep 3; done"
    ;;
  sync)
    src=$(cd "$(dirname "$0")/../.." && pwd)
    rsync -a --delete --exclude /build/ --exclude /iso/ --exclude /.git/ --exclude /.cache/ --exclude __pycache__/ \
      -e "ssh -i $KEY -p $PORT -o IdentitiesOnly=yes -o UserKnownHostsFile=$DIR/known_hosts" "$src/" test@127.0.0.1:src/
    ;;
  console)
    exec virsh -c "$CONN" console "$VM"
    ;;
  view)
    exec virt-manager --connect "$CONN" --show-domain-console "$VM"
    ;;
  stop)
    if [ "$(state)" = "running" ]; then
      # Ask the VM itself first: with a desktop session running, Plasma answers the
      # virtual power button with a logout dialog instead of shutting down.
      "$0" ssh -o BatchMode=yes -o ConnectTimeout=5 'sudo systemctl poweroff' >/dev/null 2>&1 || virsh_ shutdown "$VM" >/dev/null
      for _ in $(seq 60); do
        [ "$(state)" = "running" ] || break
        sleep 1
      done
      if [ "$(state)" = "running" ]; then
        echo "Not shutting down by itself; forcing it off."
        virsh_ destroy "$VM" >/dev/null
      fi
    fi
    cleanup_helpers
    echo "$VM: $(state)"
    ;;
  reset)
    if [ "$(state)" = "running" ]; then
      virsh_ destroy "$VM" >/dev/null # no need to shut down cleanly: everything is discarded
    fi
    cleanup_helpers
    rm -f "$DIR/anchorage-test.qcow2"
    qemu-img create -q -f qcow2 -b "$DIR/anchorage-base.qcow2" -F qcow2 "$DIR/anchorage-test.qcow2"
    install -m 600 "$DIR/anchorage-base_VARS.fd" "$NVRAM"
    echo "$VM reset to the clean base image."
    ;;
  status)
    echo "$VM: $(state)"
    ss -ltn | grep -q "127.0.0.1:${PORT} " && echo "SSH forward: 127.0.0.1:${PORT}" || echo "SSH forward: not listening"
    ;;
  ssh)
    shift
    exec ssh -i "$KEY" -p "$PORT" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new \
      -o UserKnownHostsFile="$HOME/.local/share/anchorage-vm/known_hosts" test@127.0.0.1 "$@"
    ;;
  *)
    sed -n '2,18p' "$0"
    exit 2
    ;;
esac
