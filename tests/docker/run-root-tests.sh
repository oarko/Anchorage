#!/usr/bin/env bash
# Builds Anchorage inside a container and runs the test suite as root.
#
#   tests/docker/run-root-tests.sh          # debug build with ASan/UBSan (default)
#   tests/docker/run-root-tests.sh release  # hardened release build
#
# The source tree is mounted read-only and the container has no network, so nothing
# outside the container can be changed. The build lives only inside the container.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
image=anchorage-test:26.04
preset=${1:-dev}

case "$preset" in
  dev)     cmake_args="-DCMAKE_BUILD_TYPE=Debug -DANCHORAGE_SANITIZE=ON" ;;
  release) cmake_args="-DCMAKE_BUILD_TYPE=RelWithDebInfo" ;;
  *) echo "usage: $0 [dev|release]" >&2; exit 2 ;;
esac

docker build --quiet -t "$image" "$here" >/dev/null

docker run --rm --network none \
  -v "$src:/src:ro" \
  -e QT_QPA_PLATFORM=offscreen -e LANG=C.UTF-8 -e LC_ALL=C.UTF-8 \
  "$image" \
  bash -c "
    set -euo pipefail
    echo \"Running as: \$(id)\"
    cmake -S /src -B /build -G Ninja -DBUILD_TESTING=ON $cmake_args >/dev/null
    cmake --build /build
    ctest --test-dir /build --output-on-failure
    # Prove the root-only tests ran instead of skipping.
    if /build/bin/servicetest 2>&1 | grep -q 'needs root'; then
      echo 'ERROR: root-only tests were skipped' >&2; exit 1
    fi
    echo 'Root-only tests ran:'
    /build/bin/servicetest -v1 2>&1 | grep -E '^PASS.*::root' || true
  "
