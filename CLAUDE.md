# CLAUDE.md

KDE Plasma 6 application for managing `/etc/fstab`, local drives, and NFS/SMB network
mounts, with Dolphin and Plasma integration. **App name: Anchorage** (binaries
`anchorage` / `anchorage-helper`, config in `/etc/anchorage/`). The repo directory is still
called `fstabmanager`. Keep the name in ONE place (see "Naming" below).

The full agreed design is in `docs/SPEC.md`. Read it before making design decisions.
Anything marked **[OPEN]** there is not yet agreed: ask the user, don't assume.

## Project status

- Design agreed: layout C (Dolphin-like, QtWidgets), see SPEC §15.
- Done: fstab reader/writer, root helper + permission rules, drive detection, main window,
  entry editor (Basic/Access/Advanced, option catalogue) with pending changes and Apply,
  system-wide settings (`writesettings`), Backups page with side-by-side compare and restore,
  mounting (`mountentry`, `mounttemporary`, `unmount`; Mount At…; test mount after Apply
  with Undo; Remount), SPEC §6.1; mount-point suggestions (where it's mounted now first) with
  Browse, and "Also show it at…" bind entries with the bind-source rule, SPEC §8.
  Hands-on pass in the VM's Plasma session with real drives: done (2026-11-26).
  Network shares step 1 (SPEC §11): Connect to Server (SMB via libsmbclient, NFS exports and
  versions via showmount/rpcinfo), add to fstab or Mount At, saved logins
  (`writecredentials`), moving typed passwords out of fstab (and out of backups),
  client-tool install through PackageKit, NFS version choice, mount error explanations.
  Next: "mount on login" for users (KWallet + login agent), then discovery.
  Later: mounting removable drives as the user through UDisks (like Dolphin).
- Bind entries must carry `x-systemd.requires=<drive>`: `requires-mounts-for` alone binds the
  drive's EMPTY folder from the system disk when the drive has no fstab line (VM, 2026-11-26).
- **Network shares**: `ANCHORAGE_NAS_ENV=~/.ancorage.env tests/vm/shares.sh` (~15 min): Samba
  (login + guest share, `restrict anonymous = 2`) and NFS in the VM, then the developer's NAS
  (TrueNAS, address in the private file: SMB `test_share`, NFS `/mnt/NAS_Pool/TVshows`, both writable
  for tests). Covers listing, saved logins (`0600 root`, never logged), fstab + mount, NFS
  version negotiation, Mount At, the owner rule for `credentials=`, password redaction in
  backups, and installing a removed cifs-utils through PackageKit (needs an administrator).
  The NAS login lives only in `~/.ancorage.env` (mode 600; read only the `NAS_` lines, never
  print). `sharestest` runs live against the NAS with `ANCHORAGE_NAS_ENV` set.
  - TrueNAS requires NFS clients on a privileged port unless "Allow non-root mount" is on
    (switched on for testing); the VM's user-mode network can't use privileged ports.
    Host-side NFS checks: a throwaway `--privileged --network host` container.
  - `safefs::makeDirectories` gives missing parents the mode it is given: create
    `/etc/anchorage` (0755) before `credentials/` (0700), or backups become unreadable.
- `tests/vm/vm.sh deps` installs build dependencies added after the base image was made
  (pkg-config, libsmbclient-dev); every runner calls it, so the base image stays untouched.
- **Real drives**: `tests/vm/realdrives.sh attach|detach|status` attaches the developer's
  Lingames (btrfs) and Windows (NTFS) disks to the VM by serial number (refuses while mounted
  on the host; needs a temporary `setfacl` by the user). `tests/vm/realdrives_test.sh` (~15 min)
  runs `realdrives_scenarios.py` against them (only an `anchorage-test` folder is written and
  removed), then resets the VM and installs Anchorage fresh for hands-on testing.
- **Packaging (SPEC §12a)**: `debian/` (debhelper 13 + CMake/Ninja, native, series
  `resolute`), built by `packaging/build-deb.sh` in a clean `ubuntu:26.04` container with
  lintian (pedantic, clean). Version = `project(VERSION)` + `ANCHORAGE_VERSION_SUFFIX` and must
  equal `debian/changelog` (CMake stops otherwise); `~beta`/`~rc` forces `ANCHORAGE_BETA`
  (launch warning, INFO logging). Bump both together for each beta. `tests/vm/package.sh`
  (~15 min) tests install, e2e scenarios against the packaged helper, upgrade and the
  uninstall rules (repacking the .deb for the upgrade/release cases). Groups come from
  `debian/anchorage.sysusers`; maintainer scripts get the beta flag from debian/rules.
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
Runtime: `nfs-common` and `cifs-utils` (both installed on the dev machine since
2026-11-26; test the "missing" path in the VM or Docker). The app detects missing client tools
and offers to install them (SPEC §11).

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
   Logging goes through the `anchorage::logging` categories (`src/core/logging.h`). Never
   log request maps, fstab content or credentials files; pass anything that might contain
   a password through `logging::scrub()` (`tests/loggingtest.cpp` checks a live SMB scan).
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
- `findmnt` (util-linux 2.41.3) **crashes** right after printing "parse error at line N"
  when a tab file has an unparseable line. Never rely on its exit code: our parser checks
  first, and only findmnt's messages are used (see `tests/fstabtest.cpp`).
- Fuzzing: `cmake --preset fuzz && cmake --build --preset fuzz`, then
  `build/fuzz/bin/fstab_fuzzer -max_total_time=300 -max_len=8192 build/fuzz/corpus tests/data fuzz/seeds/fstab` and
  `build/fuzz/bin/policy_fuzzer -max_total_time=300 -max_len=4096 build/fuzz/corpus-policy fuzz/seeds/policy` and
  `build/fuzz/bin/options_fuzzer -max_total_time=300 -max_len=2048 build/fuzz/corpus-options fuzz/seeds/options`.
  CI runs each for 1 minute on every push. Keep any crash input that turned out to be a
  harness mistake as a regression seed in `fuzz/seeds/`.
- **Root-only code paths** (owners of written files and new folders): run
  `tests/docker/run-root-tests.sh [dev|release]`. It builds inside a throwaway
  `ubuntu:26.04` container as real root, with the source mounted read-only and no network,
  and fails if the root-only tests were skipped. Never run tests as root on the host, and
  don't add sudoers rules for test binaries (a rebuildable binary with NOPASSWD = full root).
- **Drive detection on real file systems**: `tests/vm/drives.sh` (~1 min) creates ext4, btrfs,
  FAT and exFAT disk images in the VM, attaches them as loop devices and checks the scanner.
- **GUI**: `tests/guitest.cpp` builds the real main window from fixtures; set
  `ANCHORAGE_SCREENSHOTS=<dir>` to save screenshots of each view for review.
  Message boxes are answered with `clickInNextDialog()`, which only looks at
  `QApplication::activeModalWidget()` (the editor behind a message box has buttons too).
  ctest runs it with `tests/guitest.lsan.supp`: KMessageBox's Notify option hands the text
  to KDE's notification integration, which is still alive at exit (proved by removing
  Notify). When running guitest by hand under ASan, set
  `LSAN_OPTIONS=suppressions=$PWD/tests/guitest.lsan.supp`.
- `KCategorizedView` (KF 6.24) crashes (ASan: negative-size-param) when a filter change
  removes rows across categories; `InventoryFilter` uses `invalidate()` instead.
- **End-to-end** (real KAuth -> D-Bus -> polkit -> installed helper): `tests/vm/e2e.sh`
  (~8 min; `--keep` leaves the VM up for investigating). It resets the VM, builds and
  installs Anchorage there, creates test users alice/nora/dana/sam (standard/netmount/
  driveadmins/sudo) with random in-memory passwords, and runs `tests/vm/e2e_scenarios.py`.
  Run it after any change to the helper, polkit files or permission rules. Findings that
  shaped it:
  - polkit only treats sessions **on a seat** as local. SSH and the serial console are
    refused outright (scenario 1 checks SSH). Scenarios therefore log in on tty3 (seat0),
    typed via `virsh send-key`, screen read from `/dev/vcsa3` over SSH.
  - KAuth's client-side check asks polkit about the client **process**, so `pkttyagent`
    must be registered with `--process <client pid>`; the client waits for a handshake
    (`ANCHORAGE_E2E_HANDSHAKE`) until the agent is registered.
  - `auth_*_keep` is remembered per authenticating process: a new client process asks
    again; the long-running GUI asks once per few minutes.
- **Test VM** (Kubuntu 26.04, libvirt per-user session `qemu:///session`, name
  `anchorage-test`, disk and VM-only SSH key in `~/.local/share/anchorage-vm/`, SSH forwarded
  to `127.0.0.1:2222` only). Use `tests/vm/vm.sh start|stop|view|status|ssh`.
  Networking is QEMU user-mode (slirp) added through `<qemu:commandline>` (its own
  pcie-root-port `anc-rp0` at slot 0x10, NIC MAC 52:54:00:62:5b:11, hostfwd
  127.0.0.1:2222->22). libvirt's default passt backend gave the guest no IPv4 (no DHCP
  answer), left stale `passt.avx2` helpers holding the port after stops, and AppArmor
  blocks its log file. Bridged networking isn't possible in a per-user session.
  Never kill a process with a `pkill -f` pattern typed in the same shell command (it
  matches the shell itself).
- Shell deletes that use a variable must guard it and stay narrow:
  `rm -f -- "${DIR:?}"/*.png`, never `rm -f $DIR/*` (an empty variable would mean `/*`).
- VM contents: Kubuntu 26.04.1, btrfs, UEFI, user `test` (password in `~/.ancorage.env`,
  mode 600; never print it), passwordless sudo inside the VM only, SSH keys only, serial
  console on ttyS0 (`vm.sh console`), build dependencies installed.
- `anchorage-base.qcow2` (+ `anchorage-base_VARS.fd`) is the clean, read-only install; the VM
  runs on the overlay `anchorage-test.qcow2`. `tests/vm/vm.sh reset` discards every change in
  seconds. Reset before and after any test that edits the VM's real `/etc/fstab`.
- In the VM use `sudo env DEBIAN_FRONTEND=noninteractive apt-get ...`: Ubuntu 26.04's sudo ignores `-E`.
- To change the base image (e.g. add packages): reset, start, make the change, stop, then
  `qemu-img commit` the overlay into the base (make the base writable first) and copy the
  overlay's VARS back to the base VARS.
- **Real mounts**: `tests/docker/run-mount-tests.sh` (~3 min with the build) runs
  `tests/mounttest.cpp` in a throwaway **privileged** container (no network, own mount
  namespace). It formats loop images of every testable type in the catalogue (14: ext2-4,
  btrfs, xfs, f2fs, vfat, exfat, ntfs3, ntfs-3g, udf, tmpfs, bind), mounts every Access tab
  combination with `mount -T <proxy fstab>` (checks owner, mode, user-mount flags and the
  units systemd-fstab-generator makes), and sweeps every catalogue option one by one, so a
  typo or an option the kernel dropped fails the test. Options marked `"t": {"skip": ...}`
  are not swept; `"skip": "network"` ones are swept by `tests/vm/network.sh` (~6 min), which
  installs Samba (guest share) and an NFS server **in the VM** (never on the host) and runs
  the same test for cifs, smb3, nfs and nfs4. It found that mount.cifs refuses ten general
  options (`atime`, `strictatime`, `nouser`, `nosymfollow`, `silent`, ...): the catalogue's
  `"not"` field excludes them for SMB.
  - Privileged containers share the host's loop devices: the script detaches only the
    ones backed by its own images (`losetup -j`), never `losetup -D` (snap uses loops).
  - The container's `/dev` is a copy from its start, so the script `mknod`s loop nodes.
  - Kernel 7.0 findings it made: ext4 no longer accepts `noacl`; btrfs refuses bare
    `space_cache` (v1 is gone); `journal_async_commit` needs `data=writeback`; ntfs3 shows
    the top folder as 0777 whatever the umask (editor warns, test records it).
  Unprivileged user namespaces don't work here: Ubuntu sets
  `kernel.apparmor_restrict_unprivileged_userns=1`.
- **Mount rules**: `tests/mountplantest.cpp` checks who may mount what and the exact
  commands the service runs (fake runner); `mounttest.cpp::serviceMountsForReal` runs them
  for real as root in the privileged container, including "in use" with a live process.
  The service always runs `mount --fstab <the fstab it read>`, so tests can use proxy files.
- **Editor end-to-end**: `tests/vm/editor.sh` (~12 min) builds entries with the editor's
  code (`anchorage-e2e-client edit`, the GUI's `anchorage::client` KAuth path), saves them
  with real password prompts, then checks user mounts (alice/nora via `sudo -u`), owners,
  `users` vs `user`, and after a **reboot**: boot mounts, automount, a missing `nofail`
  drive and an unreachable NFS server not blocking start-up, then the mount actions
  (systemd automount path, "in use" naming the program, temporary mounts, typed
  confirmation for /boot/efi, refusals for standard users), binds (whole drive and a
  folder on it; a missing drive's bind stays unmounted after the reboot; a standard user
  can't bind /etc), remove + restore.
  Disk images are attached at boot by a oneshot unit before `local-fs-pre.target`.

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
the other files from it. The name is final (2026-11-27) and a beta has shipped: polkit action
IDs, the helper ID and config paths are now a compatibility promise.
