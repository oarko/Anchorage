#!/usr/bin/env bash
# Build Anchorage's RPMs in clean Fedora containers and check them with rpmlint (SPEC §12a).
# Needs only Docker; nothing is installed on this computer.
#
#   packaging/build-rpm.sh [fedora-release...]     default: 45 44 (current and previous)
#
# Output in build/rpm/fc<release>/. The version comes from packaging/anchorage.spec and must
# match CMakeLists.txt, or the build stops.
# GitHub turns "~" in uploaded file names into ".", so build/rpm/ also gets copies named like
# the tag (anchorage-0.9.0-beta1-1.fc45.x86_64.rpm) and SHA256SUMS for them: upload those.
set -euo pipefail

src=$(cd "$(dirname "$0")/.." && pwd)
releases=("$@")
[ "${#releases[@]}" -gt 0 ] || releases=(45 44)
version=$(sed -n 's/^Version: *//p' "$src/packaging/anchorage.spec")
out="$src/build/rpm"
mkdir -p "$out"

# Only files git knows about (tracked, or new and not ignored): no build trees or ISO images.
tarball="$out/anchorage-$version.tar.gz"
(cd "$src" && git ls-files -z -co --exclude-standard |
  tar -czf "$tarball" --null -T - --transform "s,^,anchorage-$version/," --owner=0 --group=0)

for rel in "${releases[@]}"; do
  dest="$out/fc$rel"
  rm -rf "${dest:?}" && mkdir -p "$dest"
  echo "== Fedora $rel"
  docker run --rm -v "$out:/in:ro" -v "$src/packaging:/pkg:ro" -v "$src/debian:/deb:ro" -v "$dest:/out" \
    -e LC_ALL=C.UTF-8 -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" "fedora:$rel" bash -euo pipefail -c '
    trap "chown -R \$HOST_UID:\$HOST_GID /out" EXIT
    dnf -q -y install rpm-build rpmlint glibc-langpack-en "dnf-command(builddep)" >/dev/null
    mkdir -p ~/rpmbuild/SOURCES ~/rpmbuild/SPECS
    cp /in/anchorage-*.tar.gz ~/rpmbuild/SOURCES/
    cp /deb/anchorage.sysusers ~/rpmbuild/SOURCES/
    cp /pkg/anchorage.spec ~/rpmbuild/SPECS/
    dnf -q -y builddep ~/rpmbuild/SPECS/anchorage.spec >/dev/null
    if ! rpmbuild -ba ~/rpmbuild/SPECS/anchorage.spec > /out/build.log 2>&1; then
      tail -40 /out/build.log; echo "rpmbuild failed: see build.log" >&2; exit 1
    fi
    grep -E "tests passed|^Wrote:" /out/build.log || true
    cp ~/rpmbuild/RPMS/*/*.rpm ~/rpmbuild/SRPMS/*.rpm /out/
    echo "-- groups the package creates:"; rpm -qp --provides /out/anchorage-[0-9]*.x86_64.rpm | grep -i group || echo "(none!)"
    echo "-- rpmlint"
    rpmlint -r /pkg/anchorage.rpmlintrc ~/rpmbuild/SPECS/anchorage.spec /out/*.rpm | tee /out/rpmlint.log || true
  '
done
cd "$out"
rm -f anchorage-[0-9]*.rpm SHA256SUMS
for f in fc*/anchorage-[0-9]*.x86_64.rpm; do
  b=$(basename "$f")
  cp "$f" "${b//\~/-}"
done
sha256sum anchorage-[0-9]*.x86_64.rpm > SHA256SUMS
cat SHA256SUMS
