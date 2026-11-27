#!/usr/bin/env bash
# Anchorage against the real drives attached with tests/vm/realdrives.sh attach.
#
#   tests/vm/realdrives_test.sh            run the scenarios, then reset the VM and install
#                                          Anchorage fresh for a hands-on session (VM left running)
#   tests/vm/realdrives_test.sh --stop     run the scenarios, then reset and shut the VM down
#
# Writes go to the real drives (only an "anchorage-test" folder, removed at the end).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
vm="$here/vm.sh"
stop=0
[ "${1:-}" = "--stop" ] && stop=1
step() { printf '\n== %s\n' "$*"; }

install_anchorage() {
  "$vm" sync
"$vm" deps
  "$vm" ssh -o BatchMode=yes 'set -e
    cmake -S ~/src -B ~/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
          -DBUILD_TESTING=OFF -DANCHORAGE_E2E=ON >/dev/null
    cmake --build ~/build | tail -1
    sudo cmake --install ~/build >/dev/null
    sudo install -m 755 ~/build/bin/anchorage-e2e-client /usr/local/bin/
    sudo systemctl reload dbus'
}

drives=$("$here/realdrives.sh" status) # captured first: grep -q would cut the pipe short
if ! grep -q "Lingames.*attached:yes" <<<"$drives" || ! grep -q "Windows.*attached:yes" <<<"$drives"; then
  echo "Attach the drives first: tests/vm/realdrives.sh attach" >&2
  exit 1
fi

step "Resetting the VM to the clean base image (the real drives stay attached)"
"$vm" stop >/dev/null 2>&1 || true
"$vm" reset
"$vm" start >/dev/null
"$vm" wait

step "Building and installing Anchorage in the VM"
install_anchorage

step "Running the real-drive scenarios on the VM console (tty3)"
status=0
python3 "$here/realdrives_scenarios.py" || status=$?

step "Resetting the VM"
"$vm" stop >/dev/null
"$vm" reset >/dev/null
if [ "$stop" = 1 ]; then
  echo "VM reset and shut down."
  exit "$status"
fi

step "Installing Anchorage fresh for the hands-on session"
"$vm" start >/dev/null
"$vm" wait
install_anchorage
echo
echo "Ready: open the VM's desktop with tests/vm/vm.sh view (user 'test', an administrator)."
echo "Anchorage is in the application menu, or run 'anchorage' in Konsole."
exit "$status"
