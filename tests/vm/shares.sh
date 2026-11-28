#!/usr/bin/env bash
# Network shares end to end in the test VM (SPEC §11): a Samba share with a login, a guest
# share and an NFS export inside the VM, plus the developer's NAS when ANCHORAGE_NAS_ENV
# points at a private file with NAS_HOST, NAS_USER and NAS_PASSWORD (mode 600).
#
#   ANCHORAGE_NAS_ENV=~/.ancorage.env tests/vm/shares.sh          reset, run, reset
#   ANCHORAGE_NAS_ENV=~/.ancorage.env tests/vm/shares.sh --keep   leave the VM afterwards
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
vm="$here/vm.sh"
keep=0
[ "${1:-}" = "--keep" ] && keep=1
step() { printf '\n== %s\n' "$*"; }

step "Resetting the VM to the clean base image"
"$vm" stop >/dev/null 2>&1 || true
"$vm" reset
"$vm" start >/dev/null
"$vm" wait
"$vm" sync

step "Installing Samba, the NFS server and Anchorage in the VM"
"$vm" deps
"$vm" ssh -o BatchMode=yes 'set -e
  if command -v dnf >/dev/null; then
    sudo dnf -q -y install samba nfs-utils cifs-utils >/dev/null 2>&1
  else
    sudo env DEBIAN_FRONTEND=noninteractive apt-get update -q >/dev/null
    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -q samba nfs-kernel-server cifs-utils >/dev/null 2>&1
  fi
  cmake -S ~/src -B ~/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
        -DBUILD_TESTING=OFF -DANCHORAGE_E2E=ON >/dev/null
  cmake --build ~/build | tail -1
  sudo cmake --install ~/build >/dev/null
  sudo install -m 755 ~/build/bin/anchorage-e2e-client /usr/local/bin/
  sudo systemctl reload dbus'

step "Running the share scenarios on the VM console (tty3)"
status=0
python3 "$here/shares_scenarios.py" || status=$?

if [ "$keep" = 0 ]; then
  step "Resetting the VM"
  "$vm" stop >/dev/null 2>&1 || true
  "$vm" reset >/dev/null
fi
exit "$status"
