# Anchorage

**A KDE Plasma 6 app for drives, network shares and `/etc/fstab`, without hand-editing config files.**

Anchorage shows every drive, partition and network share on the computer in a Dolphin-style
window, and lets you decide where and when each one is mounted, and who may use it. You make
these choices in plain language ("mount it the first time the folder is opened", "any user may
mount it", "only the owner can open the files"). Anchorage writes the matching `/etc/fstab`
line, checks it, backs up the old file and test-mounts the result. Every option is explained:
the catalogue covers about 390 mount options for 21 file systems, including SMB and NFS.

> **Status: beta (0.9.0~beta1).** The core works and is tested heavily, including against
> real drives and real file servers in a test VM, and there is a `.deb` for testers (see
> [Installing the beta](#installing-the-beta-kubuntu-2604)). Server discovery, "mount on login" and desktop integration are still to come
> (see [Roadmap](#roadmap)). Try it in a virtual machine, not on a computer you depend on.

## What works today

**Seeing what is there**
- Drives, partitions, `/etc/fstab` entries and current mounts in one view (Places | tiles |
  details, like Dolphin), filtered by All / Drives / Network / System, with search.
- System entries (`/`, `/home`, `/boot`, swap…) are hidden from standard users. For
  driveadmins and administrators they are locked until "Allow Editing System Entries" is on.

**Editing `/etc/fstab`**
- Entry editor with three tabs and a live preview of the fstab lines:
  - **Basic**: pick a detected drive, choose how it is identified (UUID, label, partition UUID,
    path), the file system, and the mount point. Mount points come from a list that starts with
    where the drive is mounted right now, then `/mnt/<label>`, `/media/<label>` and your
    personal folder; there is also a Browse button.
  - **Access**: when it mounts (at start-up / when asked / when its folder is opened), who may
    mount it, who owns the files and with which permissions (FAT, exFAT, NTFS, SMB), and boot
    safety (`nofail`, `_netdev`). Each choice shows the options it writes.
  - **Advanced**: every option for the file system, with description, default and valid
    values; risky ones are flagged.
- **Also show it at…**: the same drive (or a folder on it) at more places, as bind entries.
  They wait for the drive and are never mounted without it, so programs never see an empty
  folder in its place.
- Changes are collected until you click **Apply** (with Discard and a line-by-line Preview).
  The original layout, comments and alignment of `/etc/fstab` are kept byte for byte.
- Before saving: Anchorage checks your rights, asks you to type the path for protected system
  locations, and runs `findmnt --verify`. After saving: each new or changed entry is
  test-mounted, with **Undo** if that fails.

**Mounting**
- Mount and Unmount for fstab entries. Entries marked "any user" are mounted as you, with no
  password, like in a terminal.
- **Mount At…**: mount a drive at a chosen folder without changing fstab (driveadmins and
  administrators).
- When a drive is in use, Anchorage names the programs using it and offers Try Again.
- Mounts that are opened on first access go through systemd, so it keeps watching the folder.

**Network shares**
- **Connect to Server…**: type a server's name or address (`nas.local`, `smb://nas`,
  `//nas/share`, `nas:/export`) to list its SMB shares (with a login when needed) and NFS
  exports, including which NFS versions it offers.
- Add a share to fstab through the editor, or mount it at a folder with **Mount At…**. For
  NFS, pick a version or leave it on Automatic; Anchorage warns if the server doesn't offer
  the chosen one.
- **Saved logins**: SMB passwords go into a root-only file
  (`/etc/anchorage/credentials/<you>-<server>-<user>`), never into `/etc/fstab`, which every
  user can read. Standard users can only use their own saved logins.
- Passwords already typed into `/etc/fstab` are flagged for driveadmins and administrators,
  with **Move to a Safe File**; once moved, they are removed from every backup too.
- Missing `cifs-utils` or NFS tools: **Install** through PackageKit, or copy the right
  command for apt, dnf, zypper or pacman.
- Mount errors are explained in plain language (NFS version not offered, access refused,
  wrong SMB login).

**Backups and settings**
- A backup is made before every change (the newest 10 are kept; adjustable from 1 to 100).
- The Backups page compares the current file with any backup **side by side**: red for lines a
  restore would remove, green for lines it would bring back, yellow for changed lines with the
  differing characters underlined. Restoring is itself backed up, so it can be undone.
- System-wide settings (backups to keep, driver suggested for NTFS drives) for driveadmins and
  administrators.

## How it keeps the system safe

- **The app never runs as root.** It refuses to start as root. Changes go through a small
  privileged helper (KAuth + polkit), which asks for a password.
- **The helper trusts nothing from the app.** It decides for itself who you are and checks
  every request again: who may change system entries, where mount points may go (protected
  locations such as `/etc`, `/usr` or `/boot` are refused or need typed confirmation),
  which folders may be shown elsewhere, and that the file didn't change since you opened it.
- **It only ever writes fixed files**: `/etc/fstab`, its backups, `/etc/anchorage/config` and
  saved logins in `/etc/anchorage/credentials/` (root only).
  Writes are atomic, folders are created without following symbolic links, and no shell
  is ever used.
- Permission tiers (see [`docs/SPEC.md`](docs/SPEC.md) §4):

  | Who | Can |
  |---|---|
  | Everyone | See drives and shares; mount "any user" entries |
  | Standard users with an administrator's password | Change non-system entries in the usual places (`/mnt`, `/media`, `/srv`, home folders) |
  | `driveadmins` group (own password) | Everything, including system entries, other locations, Mount At… and settings |
  | `sudo` group (administrators) | Everything, including protected locations after typing the path |

  Membership of `driveadmins` is effectively root access: anyone who can edit fstab freely
  can take over the computer. Only add people you would trust with `sudo`.

## Roadmap

1. A hands-on pass in a real Plasma session (in progress).
2. **"Mount on login"** for ordinary users (passwords in KWallet), and per-drive "allow users
   to mount without a password".
3. **Finding servers** on the network (mDNS, WS-Discovery).
4. **Desktop integration**: Dolphin right-click menus and a Disks & Devices action.
5. **Packaging**: a `.deb` for Kubuntu and a setup script for the `driveadmins` and `netmount`
   groups. The final name is settled before the first release.
6. Later: sharing folders over SMB and NFS (SPEC §16), and mounting removable drives as the
   user through UDisks.

The full design, including every agreed decision, is in [`docs/SPEC.md`](docs/SPEC.md).

## Installing the beta (Kubuntu 26.04, Fedora 45 and 44)

> **This is a beta.** Anchorage changes system files such as `/etc/fstab`, and a mistake there
> can stop a computer from starting. Try it in a virtual machine or on a computer you don't
> depend on, never on production systems. It warns you about this at every start (you can
> turn that off for each version).

Download the package for your system (and `SHA256SUMS`) from the
[GitHub release](https://github.com/oarko/Anchorage/releases/tag/v0.9.0-beta1) and install it
with your package manager, which also installs the KDE libraries it needs:

```sh
sha256sum -c --ignore-missing SHA256SUMS                # optional: check the download

sudo apt install ./anchorage_0.9.0-beta1_amd64.deb      # Kubuntu / Ubuntu 26.04
sudo dnf install ./anchorage-0.9.0-beta1-1.fc45.x86_64.rpm   # Fedora 45 (fc44: Fedora 44)
```

Then open **Anchorage** from the application menu. As an administrator (a member of `sudo`, or
`wheel` on Fedora) you can do everything with your own password. The package creates two empty groups for everyone
else:

```sh
sudo usermod -aG netmount alice      # may add network shares and save logins for them
sudo usermod -aG driveadmins alice   # may change everything (effectively root: be careful)
```

They need to log out and in again first. On Fedora, SELinux stays on: Anchorage is tested
with it enforcing.

**Reporting a problem:** the beta logs what it does. Start it from a terminal (`anchorage`) to
see the app's lines, and `journalctl -b | grep anchorage` shows the root helper's. Passwords
are never logged. Please attach both to an issue at
<https://github.com/oarko/Anchorage/issues>.

**Uninstalling** (`sudo apt remove anchorage`) never changes `/etc/fstab`: drives, binds and
shares that Anchorage added keep working without it. The beta asks whether to delete
`/etc/anchorage` too (saved logins, backups, settings); it suggests keeping it when shares
still use a saved login there, because they stop mounting without it. A release keeps the
folder on remove, and `purge` deletes only the saved logins that nothing uses any more.
On Fedora (`sudo dnf remove anchorage`), the package can't ask, so it always keeps
`/etc/anchorage` and says how to delete it.

## Building (Kubuntu 26.04)

```sh
sudo apt install build-essential cmake ninja-build extra-cmake-modules qt6-base-dev qt6-tools-dev \
  libkf6coreaddons-dev libkf6i18n-dev libkf6xmlgui-dev libkf6widgetsaddons-dev libkf6auth-dev \
  libkf6itemviews-dev libkf6colorscheme-dev libkf6solid-dev pkg-config libsmbclient-dev
```

```sh
cmake --preset dev          # Debug build with AddressSanitizer + UndefinedBehaviorSanitizer
cmake --build --preset dev
ctest --preset dev
./build/dev/bin/anchorage
```

Use the `release` preset for a hardened, optimised build (`build/release`).

Run from the build folder, Anchorage can show everything but can't change anything: the
privileged helper and its polkit rules have to be installed for that
(`sudo cmake --install build/release`), or build the package. Please do either in a virtual
machine.

To build the `.deb` in a clean Ubuntu 26.04 container (needs only Docker) and check it with
lintian: `packaging/build-deb.sh` (output in `build/deb`). The Fedora `.rpm`s, in clean
Fedora 45 and 44 containers and checked with rpmlint: `packaging/build-rpm.sh` (`build/rpm`). The version comes from
`debian/changelog` and must match `CMakeLists.txt`; a `~beta` or `~rc` version is always a
beta build.

## Testing

Nothing in the test suite touches the real `/etc/fstab` or mounts of the computer it runs on.

- **Unit and GUI tests** (`ctest`): the fstab reader/writer, permission rules, option
  catalogue, mount rules, and the real main window built from fixtures (screenshots with
  `ANCHORAGE_SCREENSHOTS=<dir>`).
- **Fuzzing** with libFuzzer for the fstab parser, the permission rules and the option parser.
- **As root, in throwaway containers**: `tests/docker/run-root-tests.sh`, and
  `tests/docker/run-mount-tests.sh`, which really mounts every Access choice and every catalogue
  option on 14 file systems (about 1,460 mounts).
- **In a test VM with real password prompts**: permissions (`tests/vm/e2e.sh`), the editor,
  mounting and a reboot (`tests/vm/editor.sh`), SMB and NFS options against real servers
  (`tests/vm/network.sh`), browsing, saving logins and mounting shares against Samba and NFS
  servers plus a real NAS (`tests/vm/shares.sh`), and real drives attached to the VM
  (`tests/vm/realdrives_test.sh`), and the `.deb`: install, permissions against the packaged
  helper, upgrade, and the uninstall rules (`tests/vm/package.sh`). The same suites run in a
  Fedora KDE VM with SELinux enforcing (`ANCHORAGE_VM=fedora`, made once by
  `tests/vm/fedora-install.sh`; `.rpm` test: `tests/vm/package-rpm.sh`), and fail on any
  SELinux denial.

These tests have corrected the option catalogue for kernel 7.0 and `mount.cifs`, and found
real-world surprises that Anchorage now handles: `mount` reporting success for a missing
`nofail` drive, ntfs3 leaving an NTFS drive's top folder open to every user, and bind mounts
showing an empty folder when their drive is gone. See `CLAUDE.md` for how each test works.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
