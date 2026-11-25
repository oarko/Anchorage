#!/usr/bin/env bash
# Manage the Anchorage test VM (libvirt, per-user session: no root needed).
#
#   tests/vm/vm.sh start     start the VM (cleans up a stale network helper first)
#   tests/vm/vm.sh view      open the VM's screen in virt-manager
#   tests/vm/vm.sh stop      shut down cleanly (forces off after 60 s)
#   tests/vm/vm.sh status    show state and the SSH port
#   tests/vm/vm.sh ssh [cmd] log in as the test user (after setup)
#
# Why the cleanup: when the VM stops, libvirt can leave its "passt" network helper
# running. It keeps holding the SSH port (127.0.0.1:2222), and the next start fails
# with "Address already in use" (the IPv6 lines in that error are harmless noise).
set -euo pipefail

CONN=qemu:///session
VM=anchorage-test
PORT=2222
KEY="$HOME/.local/share/anchorage-vm/id_ed25519"

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
  view)
    exec virt-manager --connect "$CONN" --show-domain-console "$VM"
    ;;
  stop)
    if [ "$(state)" = "running" ]; then
      virsh_ shutdown "$VM" >/dev/null
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
    sed -n '2,10p' "$0"
    exit 2
    ;;
esac
