#!/usr/bin/env bash
# Real mounts of what the entry editor produces (tests/mounttest.cpp), in a throwaway
# PRIVILEGED container:
#
#   tests/docker/run-mount-tests.sh
#
# Why privileged: mounting needs CAP_SYS_ADMIN and loop devices. The container has its
# own mount namespace, so nothing is mounted on the host, and it has no network. It does
# share the host kernel's loop devices: the script detaches exactly the ones the test
# attached (found by their image files), never others (snap uses loop devices too).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
image=anchorage-test:26.04

docker build --quiet -t "$image" "$here" >/dev/null

docker run --rm --privileged --network none \
  -v "$src:/src:ro" \
  -e QT_QPA_PLATFORM=offscreen -e LANG=C.UTF-8 -e LC_ALL=C.UTF-8 -e ANCHORAGE_MOUNT_TESTS=1 \
  "$image" \
  bash -c '
    set -uo pipefail
    cleanup() {
      # Unmount and detach only what this test created.
      for t in $(findmnt -rn -o TARGET | grep "^/mnt/anchorage-test/" | sort -r); do umount -l "$t" || true; done
      for img in /tmp/anchorage-mounttest-*/*.img; do
        [ -e "$img" ] || continue
        for dev in $(losetup -n -O NAME -j "$img"); do losetup -d "$dev" || true; done
      done
    }
    trap cleanup EXIT
    # The container got a copy of /dev at start; loop devices the kernel creates later
    # (losetup --find) need their device nodes made here.
    for i in $(seq 0 255); do [ -e /dev/loop$i ] || mknod /dev/loop$i b 7 $i; done
    cmake -S /src -B /build -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
    cmake --build /build --target mounttest | tail -1
    /build/bin/mounttest -o /tmp/result.txt,txt >/dev/null 2>&1
    status=$?
    grep -E "^(FAIL|SKIP|QFATAL)|^   (Loc|Actual|Expected)|^Totals|Testing" /tmp/result.txt
    if grep -q "^SKIP.*initTestCase" /tmp/result.txt; then echo "ERROR: mount tests were skipped" >&2; exit 1; fi
    exit $status
  '
