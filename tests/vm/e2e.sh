#!/usr/bin/env bash
# End-to-end test of the real authorisation chain in the test VM:
#   test client -> KAuth -> D-Bus -> polkit (password prompts) -> anchorage-helper (root)
#
#   tests/vm/e2e.sh          reset the VM, install Anchorage, run all scenarios, reset again
#   tests/vm/e2e.sh --keep   leave the VM as it is afterwards (for investigating)
#
# Scenarios run on the VM's text console tty3, typed through QEMU's virtual keyboard: a
# local, active seat0 session, like sitting at the machine. polkit refuses sessions
# without a seat (SSH, serial console) outright, which scenario 1 checks.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
vm="$here/vm.sh"
keep=0
[ "${1:-}" = "--keep" ] && keep=1

step() { printf '\n== %s\n' "$*"; }

step "Resetting the VM to the clean base image"
"$vm" reset
"$vm" start >/dev/null
"$vm" wait

step "Copying the source into the VM"
"$vm" sync

step "Building and installing Anchorage in the VM"
"$vm" deps
"$vm" ssh -o BatchMode=yes 'set -e
  cmake -S ~/src -B ~/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
        -DBUILD_TESTING=OFF -DANCHORAGE_E2E=ON >/dev/null
  cmake --build ~/build | tail -1
  sudo cmake --install ~/build | grep -E "helper|actions|polkit|dbus" | sed "s/^-- //"
  sudo install -m 755 ~/build/bin/anchorage-e2e-client /usr/local/bin/
  sudo systemctl reload dbus'

step "Running the scenarios on the VM console (tty3)"
status=0
python3 "$here/e2e_scenarios.py" || status=$?

if [ "$keep" = 0 ]; then
  step "Resetting the VM"
  "$vm" reset >/dev/null
  echo "VM reset to the clean base image."
fi
exit "$status"
