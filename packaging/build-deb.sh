#!/usr/bin/env bash
# Build Anchorage's .deb (and source package) in a clean Ubuntu 26.04 container, then check
# it with lintian (SPEC §12a). Needs only Docker; nothing is installed on this computer.
#
#   packaging/build-deb.sh [out-dir]     default out-dir: build/deb
#
# Upload to the GitHub release: anchorage_<version with - for ~>_amd64.deb and SHA256SUMS.
#
# The version comes from debian/changelog (dch -v 0.9.0~beta2 to make the next one) and must
# match project(VERSION) + ANCHORAGE_VERSION_SUFFIX in CMakeLists.txt, or the build stops.
set -euo pipefail

src=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "${1:-$src/build/deb}" && cd "${1:-$src/build/deb}" && pwd)
rm -f "$out"/anchorage*

# Only files git knows about (tracked, or new and not ignored): no build trees or ISO images.
(cd "$src" && git ls-files -z -co --exclude-standard | tar -cf "$out/source.tar" --null -T -)

docker run --rm -v "$out:/out" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" ubuntu:26.04 bash -euo pipefail -c '
  export DEBIAN_FRONTEND=noninteractive
  trap "chown -R \$HOST_UID:\$HOST_GID /out" EXIT
  apt-get update -q >/dev/null
  apt-get install -y -q --no-install-recommends build-essential devscripts equivs lintian >/dev/null
  mkdir -p /build/anchorage && tar -C /build/anchorage -xf /out/source.tar && cd /build/anchorage
  mk-build-deps -i -r -t "apt-get -y -q --no-install-recommends" debian/control >/dev/null
  rm -f anchorage-build-deps_*
  dpkg-buildpackage -us -uc 2>&1 | tee /out/build.log | grep -E "^dpkg-|^ dpkg-|warning|error|tests passed|Test #" || true
  [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "dpkg-buildpackage failed: see build.log" >&2; exit 1; }
  cp ../anchorage_* ../anchorage-dbgsym_* /out/
  cd /out
  echo "== lintian =="
  lintian --info --pedantic --display-experimental anchorage_*.changes | tee lintian.log || true
'
rm -f "$out/source.tar"

# GitHub turns "~" in uploaded file names into ".", so the release gets a copy named like the
# tag (0.9.0~beta1 -> anchorage_0.9.0-beta1_amd64.deb), with SHA256SUMS for that name.
(cd "$out" && for f in anchorage_*_amd64.deb; do
  r=${f//\~/-}
  [ "$r" = "$f" ] || cp "$f" "$r"
  sha256sum "$r" > SHA256SUMS
done)
echo
ls -l "$out"
