#!/usr/bin/env bash
# Tests the .deb in the test VM: install, the real authorisation chain against the
# packaged helper, upgrade, and the uninstall rules of a beta and a release (SPEC §12a).
#
#   packaging/build-deb.sh               build build/deb/anchorage_<version>_amd64.deb first
#   tests/vm/package.sh                  reset the VM, run everything, reset again
#   tests/vm/package.sh --keep           leave the VM as it is afterwards
#   tests/vm/package.sh --no-e2e         skip the console scenarios (the slow part)
#
# The upgrade and release checks need newer and non-beta packages. Rather than compile
# again, they repack the built .deb with a different version (and, for the release, the
# release's maintainer-script setting): only the packaging is under test there.
set -euo pipefail

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

deb=$(ls "$src"/build/deb/anchorage_*_amd64.deb 2>/dev/null | head -1)
[ -n "$deb" ] || { echo "No package in build/deb: run packaging/build-deb.sh first." >&2; exit 2; }
version=$(dpkg-deb -f "$deb" Version)

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
vmrun() { "$vm" ssh -o BatchMode=yes "$1"; }

# Repack the package: repack <out.deb> <version> <beta 0|1>
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
repack() {
  local out=$1 ver=$2 beta=$3 dir="$work/repack"
  rm -rf "$dir"
  dpkg-deb -R "$deb" "$dir"
  sed -i "s/^Version: .*/Version: $ver/" "$dir/DEBIAN/control"
  sed -i "s/^BETA=[01] /BETA=$beta /" "$dir/DEBIAN/prerm"
  if [ "$beta" = 0 ]; then
    sed -i '/^Suggests: /d' "$dir/DEBIAN/control"
  fi
  dpkg-deb --root-owner-group -Zxz -b "$dir" "$out" >/dev/null
}
upgrade_deb="$work/upgrade.deb"
release_deb="$work/release.deb"
repack "$upgrade_deb" "${version}+repack1" 1
repack "$release_deb" "${version%%~*}" 0

put() { "$vm" ssh -o BatchMode=yes "cat > $2" < "$1"; }
apt_install() { vmrun "sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --allow-downgrades $1 2>&1 | grep -E '^(Setting up|Unpacking) anchorage|^E:' || true"; }

step "Resetting the VM to the clean base image"
"$vm" reset
"$vm" start >/dev/null
"$vm" wait

step "Installing $(basename "$deb")"
put "$deb" /tmp/anchorage.deb
put "$upgrade_deb" /tmp/anchorage-upgrade.deb
put "$release_deb" /tmp/anchorage-release.deb
vmrun 'sudo env DEBIAN_FRONTEND=noninteractive apt-get update -q >/dev/null'
apt_install /tmp/anchorage.deb

step "The installed package"
check "installed at version $version" "dpkg-query -W -f '\${Status} \${Version}' anchorage | grep -qx 'install ok installed $version'"
check "groups driveadmins and netmount exist" "getent group driveadmins && getent group netmount"
check "nobody was added to them" "[ -z \"\$(getent group driveadmins | cut -d: -f4)\" ] && [ -z \"\$(getent group netmount | cut -d: -f4)\" ]"
check "beta: share tools only suggested" "[ \"\$(dpkg-query -W -f '\${Suggests}|\${Recommends}' anchorage)\" = 'cifs-utils, nfs-common|' ]"
check "helper is root-owned and not setuid" "[ \"\$(stat -c '%U %a' /usr/lib/kf6/kauth/libexec/anchorage-helper)\" = 'root 755' ]"
check "polkit knows the actions" "pkaction --action-id io.github.oarko.anchorage.writefstab >/dev/null && pkaction --action-id io.github.oarko.anchorage.writecredentials >/dev/null"
check "D-Bus can start the helper" "grep -q 'Exec=/usr/lib/kf6/kauth/libexec/anchorage-helper' /usr/share/dbus-1/system-services/io.github.oarko.anchorage.service"
check "the app reports its version" "QT_QPA_PLATFORM=offscreen anchorage --version | grep -qx 'anchorage $version'"
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
    sudo install -m 755 ~/build/bin/anchorage-e2e-client /usr/local/bin/'
  step "Authorisation scenarios on the VM console, against the packaged helper"
  if python3 "$here/e2e_scenarios.py"; then
    echo "PASS  e2e scenarios"
    pass=$((pass + 1))
  else
    echo "FAIL  e2e scenarios"
    fail=$((fail + 1))
  fi
  check "beta: the helper's INFO lines reach the journal" "sudo journalctl -b --no-pager | grep -q 'INFO: anchorage.helper: writefstab from'"
fi

# A saved login that a share in /etc/fstab uses, and one nothing uses. Written directly:
# this tests the packaging, not the helper.
fake_logins() {
  vmrun 'set -e
    sudo install -d -m 755 /etc/anchorage /etc/anchorage/backups
    sudo install -d -m 700 /etc/anchorage/credentials
    printf "username=u\npassword=p\n" | sudo tee /etc/anchorage/credentials/test-nas-used >/dev/null
    printf "username=u\npassword=p\n" | sudo tee /etc/anchorage/credentials/test-nas-unused >/dev/null
    sudo sh -c "chmod 600 /etc/anchorage/credentials/*"
    echo "# test" | sudo tee /etc/anchorage/backups/fstab-2026-01-01T00-00-00Z >/dev/null
    grep -q test-nas-used /etc/fstab ||
      echo "//nas.example.com/media /mnt/pkgtest cifs credentials=/etc/anchorage/credentials/test-nas-used,noauto,nofail 0 0" | sudo tee -a /etc/fstab >/dev/null'
}
drop_share() { vmrun "sudo sed -i '\\|/mnt/pkgtest|d' /etc/fstab"; }
fstab_sum() { vmrun 'sha256sum /etc/fstab | cut -d" " -f1'; }

step "Upgrade: keeps everything, asks nothing"
fake_logins
apt_install /tmp/anchorage-upgrade.deb
check "upgraded" "dpkg-query -W -f '\${Version}' anchorage | grep -qx '${version}+repack1'"
check "saved logins and backups kept" "sudo test -f /etc/anchorage/credentials/test-nas-used && sudo test -f /etc/anchorage/credentials/test-nas-unused && sudo test -f /etc/anchorage/backups/fstab-2026-01-01T00-00-00Z"
check "groups still there" "getent group driveadmins && getent group netmount"

step "Beta remove while a share uses a saved login: default is No"
before=$(fstab_sum)
vmrun 'sudo env DEBIAN_FRONTEND=noninteractive apt-get remove -y -q anchorage 2>&1 | grep anchorage: || true'
check "removed" "! dpkg -s anchorage 2>/dev/null | grep -q '^Status: install ok installed'"
check "/etc/anchorage kept" "sudo test -f /etc/anchorage/credentials/test-nas-used"
check "/etc/fstab unchanged" "[ \"\$(sha256sum /etc/fstab | cut -d' ' -f1)\" = '$before' ]"
check "groups left in place" "getent group driveadmins && getent group netmount"

step "Beta remove asks, and says how many shares would stop mounting"
apt_install /tmp/anchorage.deb
# The readline frontend needs a terminal: script(1) gives it one, and the answer is typed
# into it a few seconds later, like a person at the keyboard.
answer() { echo "(sleep 8; echo $1) | script -qec 'sudo env DEBIAN_FRONTEND=readline apt-get remove -y -q anchorage' /dev/null 2>&1"; }
check "question names the count, answer No keeps the folder" \
  "$(answer no) | grep -q '1 network share(s) in /etc/fstab still use' && sudo test -f /etc/anchorage/credentials/test-nas-used"
apt_install /tmp/anchorage.deb
check "answer Yes deletes the folder anyway" "$(answer yes) >/dev/null; ! sudo test -e /etc/anchorage"
apt_install /tmp/anchorage.deb
fake_logins

step "Beta remove with no share using a saved login: default is Yes"
apt_install /tmp/anchorage.deb
drop_share
before=$(fstab_sum)
vmrun 'sudo env DEBIAN_FRONTEND=noninteractive apt-get remove -y -q anchorage 2>&1 | grep anchorage: || true'
check "/etc/anchorage deleted" "! sudo test -e /etc/anchorage"
check "/etc/fstab unchanged" "[ \"\$(sha256sum /etc/fstab | cut -d' ' -f1)\" = '$before' ]"
vmrun 'sudo env DEBIAN_FRONTEND=noninteractive apt-get purge -y -q anchorage >/dev/null 2>&1 || true'

step "Release: remove keeps /etc/anchorage; purge deletes only unused saved logins"
apt_install /tmp/anchorage-release.deb
check "release installed" "dpkg-query -W -f '\${Version}' anchorage | grep -qx '${version%%~*}'"
fake_logins
before=$(fstab_sum)
vmrun 'sudo env DEBIAN_FRONTEND=noninteractive apt-get remove -y -q anchorage >/dev/null 2>&1'
check "remove kept everything" "sudo test -f /etc/anchorage/credentials/test-nas-used && sudo test -f /etc/anchorage/credentials/test-nas-unused"
check "purge reports what it kept" \
  "sudo env DEBIAN_FRONTEND=noninteractive apt-get purge -y -q anchorage 2>&1 | grep -q 'kept these saved logins'"
check "unused login deleted" "! sudo test -e /etc/anchorage/credentials/test-nas-unused"
check "login a share uses kept" "sudo test -f /etc/anchorage/credentials/test-nas-used"
check "backups kept" "sudo test -f /etc/anchorage/backups/fstab-2026-01-01T00-00-00Z"
check "/etc/fstab unchanged" "[ \"\$(sha256sum /etc/fstab | cut -d' ' -f1)\" = '$before' ]"
check "debconf answers purged" "! sudo debconf-show anchorage 2>/dev/null | grep -q ."

step "Summary"
echo "$pass passed, $fail failed"

if [ "$keep" = 0 ]; then
  step "Resetting the VM"
  "$vm" reset >/dev/null
  echo "VM reset to the clean base image."
fi
[ "$fail" = 0 ]
