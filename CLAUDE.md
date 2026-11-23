# CLAUDE.md

KDE Plasma 6 application for managing `/etc/fstab`, local drives, and NFS/SMB network
mounts, with Dolphin and Plasma integration. **App name: Anchorage** (binaries
`anchorage` / `anchorage-helper`, config in `/etc/anchorage/`). The repo directory is still
called `fstabmanager`. Keep the name in ONE place (see "Naming" below).

The full agreed design is in `docs/SPEC.md`. Read it before making design decisions.
Anything marked **[OPEN]** there is not yet agreed: ask the user, don't assume.

## Project status

- Design agreed: layout C (Dolphin-like, QtWidgets), see SPEC §15. **No code has been written yet.**
- SMB share *creation* is a planned phase 2 feature (SPEC §16); keep v1 scope to mounting.
- Ask the user before starting implementation.

## Working with the user

- The user is comfortable with Linux basics but new to mount internals, polkit, and
  KDE development. Explain *why* briefly when introducing a concept, avoid jargon walls.
- The user prefers answering design questions in **plain text** (e.g. "1A 2C") rather
  than popup multiple-choice dialogs. Number the questions and give lettered options
  with a recommendation.
- Ask before making design decisions that aren't covered by `docs/SPEC.md`.
- Update `docs/SPEC.md` whenever a decision is agreed, so the spec stays the source of truth.

## Tech stack (agreed)

- C++20, Qt 6, KDE Frameworks 6, CMake + extra-cmake-modules (ECM).
- Target: Kubuntu 26.04 / Plasma 6.6 / Dolphin 25.12. Packaged as a `.deb`.
- Privileged work goes through a **KAuth helper** (polkit-backed). The GUI never runs
  as root and should refuse to start as root, with a message explaining how to use it.
- Key libraries: KAuth, Solid, KIO, KWallet, KDNSSD, libsmbclient, libmount, libblkid.
- GUI toolkit: QtWidgets/KXmlGui (layout C, Dolphin-like).

Expected build dependencies (verify package names with `apt-cache policy` before
relying on them): `extra-cmake-modules qt6-base-dev libkf6auth-dev libkf6kio-dev
libkf6solid-dev libkf6i18n-dev libkf6coreaddons-dev libkf6xmlgui-dev
libkf6configwidgets-dev libkf6widgetsaddons-dev libkf6wallet-dev libkf6dnssd-dev
libpolkit-qt6-1-dev libsmbclient-dev libmount-dev libblkid-dev`.
Dev/test tools: `clang` (libFuzzer), `clang-tidy`, `cppcheck`.
Runtime: `nfs-common` (installed), `cifs-utils` (**not installed** on the dev machine;
the app should detect it and offer to install it).

## Security rules (non-negotiable)

These come from the user's requirements and apply to every change:

1. **The helper trusts nothing from the GUI.** Re-parse and re-validate every request
   inside the helper. The GUI's validation is only for user experience.
2. **No caller-supplied file paths for writing.** `/etc/fstab`, the backup directory,
   the polkit rules file, and the app config path are compile-time constants in the
   helper. The only caller-supplied paths are mount points, validated per SPEC §7.
3. **Fixed, minimal action set** in the helper (SPEC §6). Adding an action requires
   updating the spec, the polkit policy, and the tests.
4. **No raw buffers.** Use Qt and `std` containers only. No `strcpy`, `sprintf`,
   `char[]` buffers, or manual `new`/`delete` in the helper. Never call a shell: use
   `QProcess` with an argument list and absolute binary paths.
5. **Reject control characters, newlines, NUL, and `#`** in any field that ends up in
   fstab. Escape spaces/tabs as `\040`/`\011` per fstab(5).
6. **Symlink-safe filesystem operations.** Create directories with `openat`/`mkdirat`
   plus `O_NOFOLLOW` walking from `/`. Never follow symlinks in mount-point paths.
7. **Atomic writes.** Write a temp file in the same directory, `fsync`, `rename`,
   `fsync` the directory. Keep mode `0644 root:root` for fstab.
8. **User-requested network mounts always get `nosuid,nodev`.**
9. **Secrets.** SMB passwords for login mounts live in KWallet and are passed to the
   helper only at mount time, via a root-only `0600` temp credentials file deleted
   right after. They are never written to fstab or logs. Boot-mount credentials go in
   root-only `0600` files under `/etc/anchorage/credentials/`.
10. Hardened build flags are always on: `-D_FORTIFY_SOURCE=3 -fstack-protector-strong
    -fPIE -pie -Wl,-z,relro,-z,now -Wall -Wextra -Werror` (helper at minimum).

## Testing

**Never touch the real `/etc/fstab` or host mounts in tests.** Use proxy fstab files:
- The fstab library reads and writes any path; tests use files in `tests/data/` and temp copies.
- Helper logic lives in a library function that takes its paths as parameters. Tests call
  it with a temp directory. The shipped KAuth helper only passes the compile-time
  constants, so there is no test switch in the release binary.
- Check proxy files with the real tools, no root needed (both verified on this machine):
  `findmnt --verify --tab-file <file>` and
  `SYSTEMD_FSTAB=<file> /usr/lib/systemd/system-generators/systemd-fstab-generator <out> <out> <out>`
  (shows the mount/automount units systemd would create).
- Real mount tests run in a throwaway privileged Docker container with
  `mount -T <proxy fstab> -a` (tmpfs, bind, loop images of ext4/btrfs/vfat/exfat, plus
  Samba and NFS server containers). Unprivileged user namespaces don't work here: Ubuntu
  sets `kernel.apparmor_restrict_unprivileged_userns=1`.

- Unit tests (QtTest) for the fstab parser/writer, especially round-trip preservation
  of comments, blank lines, and spacing.
- Privilege-boundary tests: path traversal (`..`), symlink swaps, injected newlines and
  `#`, overlong input, invalid UTF-8, blocked paths, unauthorised callers.
- libFuzzer + ASan/UBSan fuzz targets for the fstab parser, option parser, and helper
  request validation.
- clang-tidy + cppcheck on every change.
- The user wants **periodic security testing**. When asked, or after any change to the
  helper or validation code, run the fuzzers and a security review of the diff.

## License

GPL-3.0-or-later (chosen 2026-11-22). Every source file starts with
`// SPDX-License-Identifier: GPL-3.0-or-later`.

## Naming

Keep the app name, reverse-DNS ID, polkit action prefix, binary names, and config paths
defined in one CMake section (e.g. `set(APP_NAME ...)`, `set(APP_ID ...)`) and generate
the other files from it, so the final rename is a one-line change. Rename before the first
release, because polkit action IDs and config paths become a compatibility promise.
