#!/usr/bin/env bash
# Network share part of the mount test (tests/mounttest.cpp) in the test VM: a Samba
# guest share and an NFS export on the VM itself, then every Access combination and
# every cifs/smb3/nfs/nfs4 catalogue option is mounted against them.
#
#   tests/vm/network.sh          reset the VM, install the servers, run, reset again
#   tests/vm/network.sh --keep   leave the VM as it is afterwards
#
# Runs in the VM rather than in Docker so the host never starts an NFS server.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
vm="$here/vm.sh"
keep=0
[ "${1:-}" = "--keep" ] && keep=1
step() { printf '\n== %s\n' "$*"; }

step "Resetting the VM to the clean base image"
"$vm" reset
"$vm" start >/dev/null
"$vm" wait
"$vm" sync

step "Installing Samba and the NFS server in the VM"
"$vm" ssh -o BatchMode=yes 'set -e
  sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -q samba nfs-kernel-server cifs-utils >/tmp/apt.log 2>&1
  sudo mkdir -p /srv/anchorage-smb /srv/anchorage-nfs
  sudo chmod 777 /srv/anchorage-smb /srv/anchorage-nfs
  printf "[global]\n  map to guest = Bad User\n  server min protocol = NT1\n[anchorage]\n  path = /srv/anchorage-smb\n  guest ok = yes\n  read only = no\n" \
    | sudo tee /etc/samba/smb.conf >/dev/null
  echo "/srv/anchorage-nfs 127.0.0.1(rw,no_subtree_check,no_root_squash,insecure)" | sudo tee /etc/exports >/dev/null
  sudo systemctl restart smbd nfs-server
  sudo exportfs -ra'

step "Building and running the network mount test"
status=0
"$vm" deps
"$vm" ssh -o BatchMode=yes 'set -e
  cmake -S ~/src -B ~/build-mt -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
  cmake --build ~/build-mt --target mounttest | tail -1
  sudo env ANCHORAGE_MOUNT_TESTS=1 ANCHORAGE_NETWORK_TESTS=1 ANCHORAGE_MOUNT_TYPES=cifs,smb3,nfs,nfs4 \
    ~/build-mt/bin/mounttest -o /tmp/result.txt,txt >/dev/null 2>&1 || true
  grep -E "^(FAIL|SKIP|QFATAL)|^   (Loc|Actual|Expected)|^Totals|Testing" /tmp/result.txt
  ! grep -q "^FAIL\|^QFATAL" /tmp/result.txt' || status=$?

if [ "$keep" = 0 ]; then
  step "Resetting the VM"
  "$vm" reset >/dev/null
fi
exit "$status"
