#!/usr/bin/env bash
# Tests the Fedora .rpm in the Fedora test VM (SPEC §12a): install, the real authorisation
# chain against the packaged helper with SELinux enforcing, reinstall, and removal.
#
#   packaging/build-rpm.sh 45            build build/rpm/fc45/anchorage-<version>.fc45.x86_64.rpm
#   tests/vm/package-rpm.sh              reset the Fedora VM, run everything, reset again
#   tests/vm/package-rpm.sh --keep       leave the VM as it is afterwards
#   tests/vm/package-rpm.sh --no-e2e     skip the console scenarios (the slow part)
set -euo pipefail

export ANCHORAGE_VM=fedora
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
vm="$here/vm.sh"
keep=0
e2e=1
for a in "$@"; do
  case "$a" in
    --keep) keep=1 ;;
    --no-e2e) e2e=0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

vmrun() { "$vm" ssh -o BatchMode=yes "$1"; }
vmstate=$("$vm" status) # captured first: grep -q would cut the pipe short
grep -q "anchorage-fedora" <<<"$vmstate" || { echo "No Fedora VM: run tests/vm/fedora-install.sh first." >&2; exit 2; }

step() { printf '\n== %s\n' "$*"; }
pass=0
fail=0
check() { # check <name> <command run in the VM>
  local name=$1 out
  if out=$("$vm" ssh -o BatchMode=yes "$2" 2>&1); then
    echo "PASS  $name"
    pass=$((pass + 1))
  else
    echo "FAIL  $name"
    printf '%s\n' "$out" | sed 's/^/      /' | tail -15
    fail=$((fail + 1))
  fi
}

step "Resetting the Fedora VM to the clean base image"
"$vm" reset
"$vm" start >/dev/null
"$vm" wait
rel=$(vmrun '. /etc/os-release; echo $VERSION_ID')
rpm=$(ls "$src"/build/rpm/fc"$rel"/anchorage-[0-9]*.fc"$rel".x86_64.rpm 2>/dev/null | head -1)
[ -n "$rpm" ] || { echo "No package for Fedora $rel: run packaging/build-rpm.sh $rel first." >&2; exit 2; }
version=$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$rpm" 2>/dev/null || basename "$rpm" | sed 's/^anchorage-//; s/\.x86_64\.rpm$//')

step "Installing $(basename "$rpm")"
"$vm" ssh -o BatchMode=yes 'cat > /tmp/anchorage.rpm' < "$rpm"
vmrun 'sudo dnf -q -y install /tmp/anchorage.rpm 2>&1 | tail -3'

step "The installed package"
check "installed at version $version" "[ \"\$(rpm -q --qf '%{VERSION}-%{RELEASE}' anchorage)\" = '$version' ]"
check "SELinux is enforcing" "[ \"\$(getenforce)\" = Enforcing ]"
check "groups driveadmins and netmount exist" "getent group driveadmins && getent group netmount"
check "nobody was added to them" "[ -z \"\$(getent group driveadmins | cut -d: -f4)\" ] && [ -z \"\$(getent group netmount | cut -d: -f4)\" ]"
check "beta: share tools only suggested" "rpm -q --suggests anchorage | grep -qx cifs-utils && rpm -q --suggests anchorage | grep -qx nfs-utils && ! rpm -q --recommends anchorage | grep -q ."
check "helper is root-owned, not setuid, labelled bin_t" "h=/usr/libexec/kf6/kauth/anchorage-helper; [ \"\$(stat -c '%U %a' \$h)\" = 'root 755' ] && ls -Z \$h | grep -q ':bin_t:'"
check "polkit knows the actions" "pkaction --action-id io.github.oarko.anchorage.writefstab >/dev/null && pkaction --action-id io.github.oarko.anchorage.writecredentials >/dev/null"
check "D-Bus can start the helper" "grep -q 'Exec=/usr/libexec/kf6/kauth/anchorage-helper' /usr/share/dbus-1/system-services/io.github.oarko.anchorage.service"
check "the app reports its version" "QT_QPA_PLATFORM=offscreen anchorage --version | grep -q '^anchorage 0'"
check "the app refuses to run as root" "! sudo QT_QPA_PLATFORM=offscreen anchorage --version"
check "menu entry and icon" "grep -qx 'Icon=io.github.oarko.anchorage' /usr/share/applications/io.github.oarko.anchorage.desktop && [ -s /usr/share/icons/hicolor/scalable/apps/io.github.oarko.anchorage.svg ]"
check "man page" "man -w anchorage"
check "nothing written to /etc/anchorage yet" "[ ! -e /etc/anchorage ]"

if [ "$e2e" = 1 ]; then
  step "Building the test client (only the client: the helper is the packaged one)"
  "$vm" sync
  "$vm" deps
  vmrun 'set -e
    cmake -S ~/src -B ~/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DANCHORAGE_E2E=ON >/dev/null
    cmake --build ~/build --target anchorage-e2e-client | tail -1
    sudo install -m 755 ~/build/bin/anchorage-e2e-client /usr/local/bin/
    sudo restorecon /usr/local/bin/anchorage-e2e-client'
  step "Authorisation scenarios on the VM console, against the packaged helper"
  if python3 "$here/e2e_scenarios.py"; then
    echo "PASS  e2e scenarios (including: SELinux denied nothing)"
    pass=$((pass + 1))
  else
    echo "FAIL  e2e scenarios"
    fail=$((fail + 1))
  fi
  check "beta: the helper's INFO lines reach the journal" "sudo journalctl -b --no-pager | grep -q 'INFO: anchorage.helper: writefstab from'"
  check "/etc/anchorage files have their normal SELinux labels" "sudo ls -Zd /etc/anchorage /etc/anchorage/backups | grep -c ':etc_t:' | grep -qx 2"
fi

step "Reinstall: keeps everything"
vmrun 'sudo install -d -m 755 /etc/anchorage/backups && echo "# test" | sudo tee /etc/anchorage/backups/fstab-2026-01-01T00-00-00Z >/dev/null'
vmrun 'sudo dnf -q -y reinstall /tmp/anchorage.rpm 2>&1 | tail -3'
check "backups kept" "sudo test -f /etc/anchorage/backups/fstab-2026-01-01T00-00-00Z"
check "groups still there" "getent group driveadmins && getent group netmount"

step "Remove: keeps /etc/anchorage and /etc/fstab, says so"
before=$(vmrun 'sha256sum /etc/fstab | cut -d" " -f1')
check "remove explains what it kept" "sudo dnf -y remove anchorage 2>&1 | grep -q 'kept /etc/anchorage'"
check "removed" "! rpm -q anchorage"
check "/etc/anchorage kept" "sudo test -f /etc/anchorage/backups/fstab-2026-01-01T00-00-00Z"
check "/etc/fstab unchanged" "[ \"\$(sha256sum /etc/fstab | cut -d' ' -f1)\" = '$before' ]"
check "groups left in place" "getent group driveadmins && getent group netmount"

step "Summary"
echo "$pass passed, $fail failed"

if [ "$keep" = 0 ]; then
  step "Resetting the VM"
  "$vm" reset >/dev/null
  echo "VM reset to the clean base image."
fi
[ "$fail" = 0 ]
