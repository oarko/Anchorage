# Design Specification: Anchorage (fstab and mount manager for KDE)

Status: **Agreed.** Items marked **[OPEN]** need the user's decision.

Last updated: 2026-11-25

---

## 1. Goals

- Create, edit, and delete fstab entries through a GUI, with plain-language help.
- Detect drives, partitions, and filesystems (similar to KDE Partition Manager).
- Add NFS and SMB network shares, with **every** option exposed and described.
- Let ordinary users "mount on login" drives and shares they're allowed to use,
  without admin rights.
- Integrate with Dolphin (right-click menus) and Plasma's Disks & Devices popup.
- Security first: only authorised users can change mounts, and there is no path from
  the app to editing any other system file.

## 2. Platform and stack

| Item | Decision |
|---|---|
| Language | C++20 |
| Frameworks | Qt 6, KDE Frameworks 6, ECM/CMake |
| Target | Kubuntu 26.04, Plasma 6.6, Dolphin 25.12 |
| Privileges | KAuth helper (polkit); GUI always runs as the user |
| GUI toolkit | **QtWidgets/KXmlGui** with KCategorizedView tiles, layout **C (Dolphin-like)**, see §15 |
| Packaging | `.deb` for Kubuntu; `cmake --install` for development |
| Name | **Final (2026-11-27)** — **Anchorage**: binaries `anchorage`, `anchorage-helper`; config in `/etc/anchorage/`. app ID `io.github.oarko.anchorage`; repo https://github.com/oarko/Anchorage (private) |

## 3. Mounting concepts (background)

- **fstab mounts**: system-wide, done by root/systemd, usually at boot.
- **UDisks mounts**: what Dolphin does when you click a drive. Dolphin → Solid →
  UDisks2, which mounts at `/run/media/<user>/<label>` on this system. The base path is
  fixed when the distro compiles UDisks and is not configurable. If a device has an fstab
  entry, UDisks uses the fstab mount point and options instead.
- **"Mount on login"**: per-user. For drives, this is Plasma's Device Automounter setting
  (`kcm_device_automounter`, config `~/.config/kded_device_automounterrc`). For network
  shares, the app's own login autostart asks the helper to mount the share.

## 4. Users, groups and permissions

### 4.1 Groups

- **`netmount`** (name chosen by the setup script, default `netmount`): may mount network shares.
- **`driveadmins`** (name chosen by the setup script, default `driveadmins`):
  full fstab management, including system entries. **Treat this as root-equivalent**: anyone
  who can freely edit fstab can gain root. Only add people you'd trust with sudo.
- **`sudo`**: existing admin group. Has access to everything (via a polkit password prompt).

### 4.2 Permission tiers

Agreed 2026-11-22:

| Tier | Can see | Can do | Authentication |
|---|---|---|---|
| Standard user | Non-system entries, drives, network mounts | Own "mount on login" for drives they're permitted to mount | None |
| `netmount` | Same | + Mount network shares on login (at the user location, §5)<br>+ Create **shared** network mounts under `/mnt/<name>` (fstab entries visible to all users), always `nosuid,nodev` | None for login mounts; **own password** (`auth_self_keep`) for creating `/mnt` fstab entries |
| `driveadmins` | Everything, including system entries | + Add, edit and delete any fstab entry, including system entries (behind the unlock toggle, §8)<br>+ Mount points anywhere in the safe and override zones (§7)<br>+ Change app settings | Own password (polkit `auth_self_keep`) |
| `sudo` | Everything | Everything, including hard-blocked paths after typed confirmation | Admin password (polkit `auth_admin_keep`) |

Notes:
- **Hiding system entries from standard users is cosmetic.** `/etc/fstab` is
  world-readable by design (the system needs that). The app hides those entries from
  standard users, but they are not secret.
- **The GUI is never run with `sudo`.** Running Qt GUIs as root is unsupported on Wayland
  and unsafe. Sudo users get the same "access to everything" through the polkit password
  prompt. If started as root, the app shows a message and exits.
- A member of `sudo` *can* always bypass the app (`sudo nano /etc/fstab`). The app's
  groups are policy for the app, not a protection against sudoers.

### 4.3 Network share scope

Admin setting per user or group in the Network Share Permissions table:
- **Approved servers only** (default), or **Any server**.
- `netmount` is listed by default with "Approved servers only".
- Anyone not listed needs an admin password to mount network shares.
- The admin page can add and remove users from `netmount` and `driveadmins`.

### 4.4 Drive mount permission

A `driveadmins`/`sudo` member can grant "Allow user/group to mount this drive without a
password". The helper writes a polkit rule scoped to that drive's filesystem UUID and the
named users/groups, in its own fixed file (see §6). This is needed because Ubuntu requires
admin authentication for mounting *internal* drives via UDisks.

## 5. Mount locations

- **User network mounts (login)**: default to the **auto-detected UDisks base**
  (`/run/media/<user>/<share>` on this system). Detection: look at existing UDisks mounts
  in `/run/mount/utab`, then which of `/run/media/<user>` or `/media/<user>` exists.
- An admin setting overrides the detected location. The helper only accepts locations on
  the admin-approved list.
- **Shared network mounts (`netmount`)**: `/mnt/<name>`, one level deep only.

## 6. Privileged helper (KAuth)

Implemented in `src/helper/helper.cpp` (thin) and `src/core/` (all rules, unit-tested and
fuzzed). The helper finds out who the caller is from KAuth's caller UID and the system
group database; nothing the GUI says about the user is trusted.

| Action (`io.github.oarko.anchorage.*`) | Purpose | Default polkit | Group rule |
|---|---|---|---|
| `writefstab` | Replace fstab: checked against the caller's rights, backed up, atomic; can create new mount-point folders; runs `systemctl daemon-reload` | admin password | driveadmins: own password |
| `restorebackup` | Restore a named backup; goes through the same checks as `writefstab` | admin password | driveadmins: own password |
| `addsharedmount` | Add one shared network mount in `/mnt/<name>` | admin password | netmount, driveadmins: own password |
| `removesharedmount` | Remove one shared network mount | admin password | netmount, driveadmins: own password |
| `writecredentials` | Save an SMB login as `/etc/anchorage/credentials/<caller>-<server>-<user>` (`0600 root:root`, folder `0700`); server/user validated, no line breaks; the password is never logged or returned | admin password | driveadmins, netmount: own password |
| `writesettings` | Replace `/etc/anchorage/config` (system-wide settings); refused unless the caller is in driveadmins or an administrator; values validated | admin password | driveadmins: own password |

How `writefstab` decides (`policy::planFstabWrite`):
- The GUI sends the whole new file plus the SHA-256 of the version it edited. If the file
  changed meanwhile, the write is refused (no lost updates). A lock file serialises writers.
- Only the differences are checked. New lines that don't parse are refused; broken lines
  that were already there may stay.
- System entries (/, /boot…, /home, /usr…, /var…, /tmp, swap): only driveadmins/sudo may
  add, change or remove them.
- New mount points: safe zone for anyone authorised; override zone for driveadmins/sudo
  (folder must be empty or new); blocked zone for sudo with `confirmProtected` only.
- Everyone else who passes polkit (a standard user with an admin's password) can change
  non-system entries in the safe zone.
- **Saved logins** (`credentials=`): unless the caller is in driveadmins or an administrator,
  a new or changed entry may only name the caller's own files
  (`/etc/anchorage/credentials/<caller>-…`), so nobody can make the computer send another
  person's password to a server of their choice.
- **Bind sources** (agreed 2026-11-26): a new or changed `bind`/`rbind` entry must show a
  folder from the safe zone or from inside another (non-system) entry's mount point;
  anything else (system folders) is for driveadmins/sudo only.

Shared mounts (`policy::planSharedMount`): network type only; `/mnt/<one folder>`; options
from an allow-list only; `uid`/`gid` only the caller's own; `credentials=` only the caller's
own files (`/etc/anchorage/credentials/<user>-*`), so nobody can send another user's saved
password to a server they control; `nosuid,nodev,nofail,_netdev` always added. Removal is
only for entries that look like shared mounts.

Mount points: `/etc/anchorage/blocked-paths` can only **add** protected locations; the
built-in list always applies.

Fixed paths (compile-time constants): `/etc/fstab`, `/etc/anchorage/` (lock file,
`blocked-paths`, `config`), `/etc/anchorage/backups/` (newest N kept, see §9),
`/etc/anchorage/credentials/`.
Polkit group rule installed to `/usr/share/polkit-1/rules.d/60-anchorage-groups.rules`.

### 6.1 Mounting and unmounting (agreed 2026-11-26)

| Action | Purpose | Who (checked by the helper) |
|---|---|---|
| `mountentry` | Mount fstab entries, named by their mount points (a list of 1–64, in order, stopping at the first failure; §8); options come only from `/etc/fstab` | Non-system entries: anyone who passes polkit. System entries: driveadmins/sudo |
| `mounttemporary` | Mount a drive or share at a chosen folder **without** changing fstab (gone after a restart): device, mount point, type, options | driveadmins/sudo only; same mount-point zone rules as fstab (§7), typed confirmation for protected locations |
| `unmount` | Unmount whatever is mounted at each folder in a list (1–64, in order, stopping at the first failure) | fstab entries: anyone who passes polkit. Not in fstab: driveadmins/sudo. System entries (`/`, `/home`, `/boot`…): driveadmins/sudo **after a typed confirmation** ("it is Linux; people like to break it from time to time") |

Polkit defaults as for `writefstab`: admin password; driveadmins their own password.

- **How** (hybrid): entries with `x-systemd.automount` ("mounted when the folder is opened")
  use `systemctl start/stop <unit>.mount`, so systemd keeps watching the folder; the real
  error is read from the journal. Everything else uses `/usr/bin/mount --fstab /etc/fstab
  <mount point>`, `mount -t <type> -o <options> <device> <folder>` (temporary) and
  `/usr/bin/umount`, whose own error messages are shown.
- Temporary mounts refuse options that would go around the folder rules or run other
  programs: `bind`, `rbind`, `move`, `remount`, `helper=`, `X-mount.*`.
- Pseudo file systems (`/proc`, `/sys`, cgroups, …) are never unmounted; only fstab
  entries, block devices, network shares and FUSE drives.
- Missing mount-point folders are created with the same rules as when saving (§7).
- **In use**: when unmounting fails because the drive is busy, the helper lists the names
  of the programs using it (looked up as root in `/proc`), and the GUI offers Try Again. No
  lazy unmount.
- **Entries marked "any user"** (`user`/`users`): the GUI runs `/usr/bin/mount` and
  `/usr/bin/umount` as the person, with no helper and no password; Linux enforces who may
  unmount.
- **Mount At…** (drives and shares not in fstab, driveadmins/sudo): the folder is
  pre-filled with `/mnt/<label>` or `/mnt/<device name>` (e.g. `/mnt/sda2`), with a Browse
  button (also in the entry editor).
- **Test mount after Apply** (§9): each new or changed entry that isn't mounted right now
  is mounted, then unmounted again unless it mounts at start-up. Failures show the error
  and offer **Undo** (restore the backup just made). Entries mounted right now aren't
  touched; the bar offers **Remount** to use the new options. Swap and drives that aren't
  connected are not test-mounted.
- Later version: mounting removable drives as the user through UDisks, like Dolphin.

Not yet implemented: login mounts (`mount.usershare`), credentials files,
drive polkit rules, settings, group membership (later milestones).

## 7. Mount-point zones

| Zone | Paths | Who | Rule |
|---|---|---|---|
| Safe | `/mnt`, `/media`, `/srv`, `/home/*`, the UDisks base | Any tier allowed to create that kind of mount | Created if missing |
| Override | Anywhere else, e.g. `/etc/someapp`, `/var/lib/plex` | `driveadmins`, `sudo` | **Target must be an empty directory, or a new one.** Warning shown, and suggests `nofail,_netdev,x-systemd.automount` for network mounts |
| Hard-blocked | `/`, the top-level dirs themselves (`/etc`, `/usr`, `/var`, `/boot`, `/proc`, `/sys`, `/dev`, `/run`, `/root`, `/tmp`, `/bin`, `/sbin`, `/lib*`), plus `/etc/sudoers.d`, `/etc/pam.d`, `/etc/polkit-1`, `/etc/systemd`, `/etc/ssh`, `/usr/*`, `/var/lib/dpkg` | Nobody via the app, except `sudo` after typing the path to confirm | List lives in root-only `/etc/anchorage/blocked-paths`, editable only by hand |

General rules for every zone:
- Absolute, normalised paths only. No `..`, no symlinks anywhere in the path.
- Mounting onto a non-empty directory always warns, because it hides the existing files.

## 8. fstab editing

- Preserve comments, blank lines, ordering, and column alignment on round-trip.
- **System entries** (`/`, `/boot`, `/boot/efi`, `/home`, swap, plus anything mounted at a
  top-level system dir) are hidden from standard users. For `driveadmins`/`sudo` they are
  shown read-only with a shield icon, until "Allow editing system entries" is switched on.
  A red warning banner shows while it's on.
- Entry editor:
  - **Basic**: device/share, mount point, filesystem type.
  - **Access** (plain language), showing the options each choice generates:
    - *When it mounts*: at boot / when clicked (`noauto`) / when the folder is opened (`x-systemd.automount`)
    - *Who can mount*: admins only / any user (`user`) / any user, anyone unmounts (`users`)
    - *Who owns the files*: user picker → `uid`/`gid`/`umask` (FAT/NTFS/exFAT/SMB only)
    - *Boot safety*: `nofail`, `_netdev`
  - **Advanced**: every option for the filesystem type with description, default, and
    valid values, including rarely used SMB/NFS options.
- Drives are identified by `UUID=` by default (with an option for `LABEL=`/`PARTUUID=`/path).
- **Mount point** (agreed 2026-11-26): an editable list with a Browse button. For a drive
  that is mounted right now, the default is **where it is mounted** (e.g.
  `/run/media/oarko/Lingames`; UDisks then uses the fstab entry, so Dolphin keeps the same
  path). The list also offers `/mnt/<label>`, `/media/<label>` and the personal UDisks
  location, detected (§5) rather than assumed: `/run/media/<user>` (UDisks' default: Fedora,
  Arch, openSUSE, NixOS, SteamOS, Kubuntu 26.04) or `/media/<user>` (Debian, Mint, older
  Ubuntu). Immutable distributions (`/mnt` -> `/var/mnt`) are out of scope for now.
- **Also show it at… (bind mounts)**: extra locations in the drive's editor, each saved as
  its own entry, e.g. `/mnt/Lingames  /run/media/oarko/Lingames  none
  bind,nofail,x-systemd.requires=/mnt/Lingames,x-systemd.requires-mounts-for=/mnt/Lingames  0 0`.
  `x-systemd.requires` (Requires= + After= on the drive's mount unit) makes systemd refuse
  the bind when the drive isn't mounted **or has no fstab line at all**; `requires-mounts-for`
  alone binds the empty folder from the system disk in that case (found in the VM,
  2026-11-26). Binds written before this get the option when their drive is next edited.
- **Orphaned binds** (a bind whose drive no entry mounts): the tile shows "Drive not in
  fstab", the editor explains, Mount is unavailable, and a banner offers **Remove Them**. A bind may show a sub-folder
  ("Folder on the drive", empty = whole drive). Binds follow the drive's "when it mounts"
  choice. When a mounted drive is given a new mount point, the editor offers to keep it
  reachable at the old location too. Bind entries also appear as their own tiles.
- **Removing a drive** also removes every bind that shows it (or a folder on it), after a
  warning listing them (agreed 2026-11-26, implemented).
- **Mount/Unmount on a drive include its binds** (implemented 2026-11-27): Mount sends the
  drive, then its binds that aren't mounted; Unmount sends its mounted binds, then the drive.
  `mountentry` and `unmount` take a list of 1–64 mount points (no repeats), each checked by
  the same rules, handled in order and stopping at the first failure; the reply names the
  ones done and the one that failed, so "in use" → Try Again continues with the rest. One
  request means one password prompt. Remount sends one unmount (binds first) and one mount
  (drive first).
- **Binds can't be "any user" mounts**: the kernel refuses bind mounts by ordinary users
  ("must be superuser", util-linux 2.41.3 on Kubuntu 26.04, tested in the VM 2026-11-27),
  so binds never get `user`/`users`. A drive with binds therefore can't be "any user" +
  "only when asked" (agreed 2026-11-27): the editor refuses that combination and suggests
  "the first time someone opens the folder" (systemd mounts the drive and its binds with no
  password) or administrators only. At start-up and when opened work for everyone. An
  older entry with the combination shows the error when its editor is opened; Mount still
  mounts the drive as the user and asks for a password for the binds.
- Planned (agreed 2026-11-26, not built yet): after Apply, new binds of a mounted drive are
  mounted in the same request as the test mount.

Implemented (2026-11-25) in `src/app/entryeditor.*`, `src/core/access.*`, `src/core/options.*`:
- **Option catalogue**: `src/core/data/filesystems.json` (21 types) and `options.json` (~390
  options: general, systemd, desktop and per-file-system, including every common
  `mount.cifs(8)` and `nfs(5)` option). Each has a plain-language description, default, value
  type and valid values, warnings for risky ones (`password=`, `nobarrier`, `noperm`, ...).
  Loaded and validated at start (duplicates, unknown types, bad examples are refused).
- **Access tab mapping** (only the options a choice owns are rewritten; everything else
  keeps its place, including values Anchorage can't represent such as `uid=name`):

  | Choice | Options |
  |---|---|
  | While the computer starts / only when asked / when the folder is opened | – / `noauto` / `noauto,x-systemd.automount` |
  | Administrators only / any user / any user, anyone unmounts | – / `user` / `users` |
  | Owner (FAT, exFAT, NTFS, SMB only) | `uid=`, `gid=` (the person's primary group) |
  | Permissions: only the owner / others read / everyone | `umask=077/022/000` (SMB: `file_mode`/`dir_mode` 0600+0700, 0644+0755, 0666+0777) |
  | Start even if missing / needs the network | `nofail` / `_netdev` |

- **New entries** default to `nofail`; network types also to `_netdev` and "when the
  folder is opened". NTFS drives are suggested as `ntfs3`. The check order (pass) follows
  the type (ext* 2, others 0).
- **Checks while editing** (OK is disabled on errors): field syntax and escaping, option
  values, contradictions (`ro`+`rw`), unknown options (warning), mount point used twice,
  mount-point zone for the caller (§7), non-empty folder, system entry without the unlock,
  missing `nofail` for boot mounts (warning), bind mounts without `bind`.
- **ntfs3 finding** (kernel 7.0, `tests/mounttest.cpp`): the driver shows the drive's top
  folder as `0777` whatever the umask, so every user can create files there (subfolders
  follow the umask; ntfs-3g gets it right). Agreed 2026-11-25:
  - Both drivers are offered for every NTFS entry. The driver suggested for new NTFS
    entries is the system-wide setting `ntfs-driver` (default **ntfs3**, the faster one).
  - Soft notice in the editor while ntfs3 is combined with "only the owner" or "others can
    read", and a confirmation when the entry is added or kept: **Use ntfs-3g Instead** /
    **Keep ntfs3** / **Cancel**.
- Pending changes: the main window keeps an edited copy of the file. The Apply bar shows
  Discard / Preview… / Apply; Apply checks the plan locally (`policy::planFstabWrite`),
  asks for typed confirmation for protected locations (sudo), runs `findmnt --verify`, and
  sends the file with the SHA-256 of the version it started from. If the file changes on
  disk meanwhile, Apply is disabled until the changes are discarded.

## 9. Save safety

| Feature | Default | Notes |
|---|---|---|
| Timestamped backup | **On** | Keep the last N (setting); "Restore backup" menu |
| Validate before writing | **On** | Own checks + `findmnt --verify` (implemented: run in the GUI before the password prompt; missing folders the helper will create, and missing devices of `nofail`/`noauto` entries, are not reported) |
| Test mount + undo offer | **On** | After saving: `daemon-reload`, mount the new/changed entries, offer to restore on failure (§6.1) |
| Preview changes | **Off** | "Preview…" button always available; admin setting forces it on for everyone |

Backups and restore (agreed 2026-11-25, implemented):
- A backup is made by the helper before every write, including a restore (so a restore
  can be undone by restoring again).
- Number kept: setting `keep-backups` in `/etc/anchorage/config`, **1-100, default 10**
  (never 0). Lowering it deletes the extra backups at the **next write**, never straight away.
- Settings (`/etc/anchorage/config`, `key = value` lines) are **system-wide**, changed in
  Settings > Configure Anchorage by driveadmins/administrators through `writesettings`;
  everyone else sees them read-only. Reading is forgiving (bad values fall back to the
  defaults), writing is strict.
- The Backups page lists the versions (newest first, local time) and compares the
  selected one with the file on disk **side by side**: left = now, right = backup.
  - Line only on the left (removed by restoring): red; line only on the right (added
    back): green. The other side shows a hatched filler line.
  - Similar lines (≥ 50 % of characters in common) are paired as one **changed** line:
    yellow on both sides, the differing characters underlined in red (left) and green (right).
  - Every real line ends with a faint `↵`, so a real blank line (just `↵`) can't be
    mistaken for a filler (no `↵`). Line numbers on both sides; both sides scroll together.
  - Everything is shown (no folding of unchanged lines); only whole files are restored.
- Restore: unsaved editor changes are discarded first (after asking); the same local checks
  as Apply (rights, typed confirmation for protected locations), then a confirmation with
  the number of changed lines, then the password prompt.

## 10. Drives

Implemented (read-only) in `src/core/drives`, `mounts` and `inventory`:
- Drives come from `lsblk --json` (util-linux): the same libblkid/udev data Solid uses,
  no root needed, and its output can be captured as test fixtures. Solid's device
  notifier, `/etc/fstab` and `/proc/self/mountinfo` are watched to refresh automatically.
- Current mounts come from `/proc/self/mountinfo`.
- The inventory joins fstab entries with their devices (`UUID=`, `LABEL=`, `PARTUUID=`,
  `PARTLABEL=`, `/dev/disk/by-*`, plain paths) and current mounts, then adds drives that
  aren't in fstab (e.g. Lingames, mounted by Dolphin at /run/media/<user>/...) and mounted
  network shares that aren't in fstab.
- Hidden: snap images (squashfs loop devices), whole disks without a file system, RAID/LVM
  members, encrypted containers, CD/DVD, RAM disks. Attached disk images are shown.
- Unlabelled partitions are named from their partition type ("EFI system partition",
  "Windows recovery partition"), otherwise "<size> <FS> partition".
- Anything on the file system mounted at "/" (e.g. the btrfs `/swap` subvolume) counts as
  a system entry, in the display and in the helper's permission rules.
- Per-drive status (planned, with editing): users may mount ✓ · mounts on login · in fstab.
- Actions (planned): **User**: Mount on login / stop. **Admin**: Add to fstab, Allow users
  to mount without password.

## 11. Network shares

- **Discovery**: mDNS/Avahi (KDNSSD: `_smb._tcp`, `_nfs._tcp`) and WS-Discovery for
  Windows/Samba hosts, plus manual address entry.
- **Share listing**:
  - NFS: export list from mountd, with a fallback for NFSv4-only servers.
  - SMB: libsmbclient (prompts for credentials when the server requires them).
- **Actions per share**: Mount on login (user) · Shared mount in `/mnt` (`netmount`) ·
  Mount on boot / add to fstab (admin).
- **SMB credentials**:
  - Login mounts: KWallet.
  - Boot mounts: root-only `/etc/anchorage/credentials/<id>` files, referenced via `credentials=`.
- **`cifs-utils` missing**: detect it and offer to install it (PackageKit).
- **Option catalogue**: data file listing every `mount.cifs(8)` and `nfs(5)` option with
  description, type, default, valid values, and NFS version/SMB dialect applicability.

Agreed 2026-11-26:
- **Order of work**: (1) type a server's address, list its shares, add one to fstab (at
  start-up, or shared in `/mnt`), with password files and the `cifs-utils`/NFS client
  check; (2) "mount on login" for users (KWallet + login agent); (3) automatic discovery.
- **Password files for fstab mounts**: one root-only (`0600 root:root`) file per Linux owner,
  server and remote user name, `/etc/anchorage/credentials/<owner>-<server>-<user>`, written
  by a new helper action (driveadmins/sudo; netmount members only their own `<owner>-` files,
  matching the shared-mount rule in §6). Passwords are never shown again or logged.
- **Missing client tools** (`mount.cifs`, `mount.nfs`): if PackageKit's D-Bus service is
  available, offer **Install** (PackageKit asks for the password through its own polkit
  rules; Anchorage's helper is not involved). Always also show the command for the
  distribution's package manager, detected from `/etc/os-release` (`ID`, `ID_LIKE`) and the
  tool actually present: apt (Debian/Ubuntu/Mint: `cifs-utils`, `nfs-common`), dnf
  (Fedora/RHEL family: `cifs-utils`, `nfs-utils`), zypper (openSUSE: `cifs-utils`,
  `nfs-client`), pacman (Arch family: `cifs-utils`, `nfs-utils`); otherwise the package name.
  PackageKit is used over D-Bus directly (no extra library).
- **Passwords typed into fstab** (`password=`/`pass=` on cifs/smb3 lines; agreed 2026-11-26):
  `/etc/fstab` is readable by every user. For driveadmins/sudo only, a banner says how many
  entries store a password and offers **Move to a Safe File**: each login is saved with
  `writecredentials` and the line's `username=`/`password=`/`domain=` become `credentials=…`,
  as pending changes for Apply. Standard users are **not** told (the editor's general
  warning on `password=` stays), so nobody is pointed at the password. After the move,
  a note recommends changing the password on the server.
- **Backups with a password that has left fstab**: whenever a write removes a password
  from fstab, the helper redacts it in every backup (including the one just made):
  the option becomes `password=removed-by-anchorage`, the rest of each backup is
  unchanged. Restoring such a backup gives a line whose login must be re-entered.
- **NFS versions**: listing a server asks rpcbind which NFS versions it offers (`rpcinfo`,
  no root needed); NFSv4-only servers without rpcbind count as "NFS 4" if port 2049 answers.
  The editor offers Automatic (type `nfs`, newest both sides support; the default) or a
  fixed 4.2/4.1/4/3 (`vers=`), flags a version the server doesn't offer, and the test mount
  after Apply confirms it. Mount errors are explained (version not offered, access denied
  by export rules or the privileged-port requirement, root squashing).
- **Why mounting needs cifs-utils when Dolphin doesn't**: Dolphin's `smb://`/`nfs://` are
  KIO workers that talk the protocol themselves (libsmbclient), only inside KDE programs and
  only for the logged-in person. A mount is an ordinary folder for every program, which is
  what `mount.cifs`/`mount.nfs` provide.
- **Testing**: Samba and NFS servers in the VM (test-only passwords), plus the developer's
  NAS. Its address and login live in a private file in the developer's home folder (mode
  600), never in the repository or logs; it is used read-only except for a share the owner
  names for writing.

## 12. Desktop integration

- **Dolphin service menus** (`.desktop` files in `kio/servicemenus`):
  - On `smb://` / `nfs://` share folders: "Mount on login…", "Add to fstab…"
  - On mounted drive folders: "Mount on login", "Add to fstab…"
  - On empty space in Network view: "Connect network share…"
  - Each opens the app's dialog pre-filled.
- **Limitation**: Dolphin's sidebar Devices section can't have custom menu items.
  Drive actions are offered instead via a **Plasma Disks & Devices (Solid) action**.
- **Login autostart**: a small user agent that mounts the user's network login mounts
  via the helper (`mount.usershare`), using KWallet credentials.

## 12a. Packaging and beta builds (agreed 2026-11-27)

- **Package**: a proper `debian/` folder (debhelper + CMake), checked with lintian, built in
  a clean `ubuntu:26.04` container; suitable for a PPA later. First version `0.9.0~beta1`
  (`~` sorts before the final `0.9.0`, so the release is an upgrade).
- **Groups**: the package creates the system groups `driveadmins` and `netmount` and adds
  nobody. Whoever installs it has sudo, so is already an administrator; driveadmins is
  effectively root and is only ever granted by hand.
- **Beta flag**: CMake option `ANCHORAGE_BETA`. A version containing `~beta` or `~rc` turns it
  on automatically (with a notice), so a beta can't ship without it. A beta build:
  - shows a warning at every launch ("beta; it changes system files; use a virtual machine
    or a computer you don't depend on, never production systems") with "Don't show again
    for this version";
  - logs more detail by default (Qt logging categories, e.g. "INFO: Starting network scan",
    "INFO: Found share at …"); release builds keep these off unless switched on with
    `QT_LOGGING_RULES` for a bug report. **Passwords are never logged** in either build.
- **Uninstalling**:
  - Release: remove keeps `/etc/anchorage`. Purge deletes saved logins that no line in
    `/etc/fstab` uses any more, keeps the ones still used (an SMB line's `credentials=`
    needs its file, or the share stops mounting), and keeps backups and settings; it lists
    what was kept and why.
  - Beta: remove asks (debconf) whether to delete all of `/etc/anchorage` (saved logins,
    backups, settings). Default Yes, but **No** when shares in `/etc/fstab` still use saved
    logins, and the question then says how many would stop mounting.
  - Never touched on uninstall: `/etc/fstab` itself. Entries Anchorage wrote are plain
    fstab, so drives, binds and NFS shares keep working without it; mounted shares stay
    mounted.
- **Dependencies**: KDE/Qt libraries and libsmbclient (from the build). `cifs-utils` and
  `nfs-common`: the release *Recommends* them (installed by default, like from a repo); a
  beta only *Suggests* them, so testers see Anchorage's own Install offer.
- **Build and test**: `packaging/build-deb.sh` builds the source and binary packages in a
  clean `ubuntu:26.04` container and runs lintian (pedantic); `tests/vm/package.sh` installs
  the `.deb` in the test VM and checks the package, runs the authorisation scenarios against
  the packaged helper, an upgrade, and the beta and release uninstall rules. The groups are
  made with systemd-sysusers (`debian/anchorage.sysusers`). The changelog's distribution is
  the Ubuntu series (`resolute` for 26.04).
- **Icon**: a fouled anchor in KDE Breeze style (flat, simple shapes, readable at 16 px),
  SVG, drawn first by Claude for review; if it isn't right, the user tries Recraft. A
  ship's-side view with the anchor in its hawsepipe may serve as larger artwork (README /
  GitHub banner). Don't copy official US Navy insignia.

## 13. Repository

- Git repository, to be hosted on GitHub. GitHub Actions CI will run the build, unit tests,
  clang-tidy/cppcheck, and short fuzzing runs on each push and PR, plus a scheduled weekly
  longer fuzz run.

## 14. Open items

1. None. Phase 2 sharing (§16) is agreed but scheduled after v1.

## 15. GUI layout (chosen 2026-11-22: layout C, Dolphin-like)

- Main window: toolbar (breadcrumb, search, "Connect to server…", "New fstab entry"),
  **Places** panel on the left (Views: All Mounts / Drives / Network Shares / System;
  Network: discovered + manually added servers, "Add server…"; Admin: Backups, Settings),
  **tile grid** in the centre (icon, name, type, mount point, usage bar, status chips),
  **info panel** on the right split into "For me" and "Administrator" actions.
- Right-click on any tile shows the same menu Dolphin's service menu offers.
- Selecting a server lists its shares as folder tiles; a share offers Mount on login /
  Shared in /mnt / Mount on boot.
- Pending fstab changes show an Apply bar (Discard / Preview… / Apply).
- System entries: hidden for standard users; locked banner + "Allow editing system
  entries" for driveadmins/sudo.
- Entry editor dialog with Basic / Access / Advanced tabs and a live fstab-line preview.
- Reference prototype (behaviour, not pixel-exact): https://claude.ai/artifact/SnKURj5AnLo3vmpDNNZVKV
  (private) and mockups https://claude.ai/artifact/S53zCX8hpRJzGid3bipqFg.
- Possible later addition: a list/details view toggle (B-style rows), like Dolphin's view modes.

## 16. Phase 2 (planned, not in v1): sharing folders (SMB and NFS)

Agreed 2026-11-22. Principle: **integrate with existing tools, don't reinvent them.**

### 16.1 Facts found on the dev machine
- `samba` (the server) is not installed; `samba-common-bin` is.
- `kdenetwork-filesharing` is installed: Dolphin's folder Properties has a **Share** tab
  that creates Samba *usershares*.
- Usershares are currently disabled (`/var/lib/samba/usershares` is created by the
  `samba` package). The `sambashare` group exists; `oarko` is a member.
- Ubuntu's default `smb.conf` sets `usershare allow guests = yes`.
- `ufw` is installed; `nfs-kernel-server` is not.

### 16.2 SMB shares: both kinds, tiered

| | Usershares | System shares |
|---|---|---|
| Who | Members of **`sambashare`** (Samba's own group, reused) | `driveadmins` / `sudo` |
| What | Folders the user owns | Any folder, subject to the path rules below |
| How | `net usershare add/delete/info`, run **as the user** (no helper, no root) | Helper writes its own file `/etc/samba/anchorage.conf` and makes sure `smb.conf` has one `include = /etc/samba/anchorage.conf` line; then `smbcontrol smbd reload-config` |
| Shared with Dolphin | **Yes**: the same usershares appear in Dolphin's Share tab and in Anchorage | — |

- Every share-option field gets a description, as with mount options (read only, guest
  OK, valid users, browseable, ACLs, etc.).
- **Samba passwords**: Samba keeps its own password list. Anchorage lets a user set their
  own Samba password (helper action, password passed on stdin to `smbpasswd -s`, never
  logged). driveadmins/sudo can set or disable other users' Samba passwords.
- **Guest access**: admin setting, **off by default**. Off means the helper sets
  `usershare allow guests = no` and refuses `guest ok = yes` on system shares (this
  overrides Ubuntu's default of yes).
- **Path rules for system shares**: absolute, normalised, no symlinks; never `/`, `/etc`,
  `/root`, `/boot`, `/proc`, `/sys`, `/dev`, `/run`, `/usr`, `/var/lib`, or another
  user's home folder without a typed confirmation (sudo only).

### 16.3 NFS exports (same treatment as SMB system shares)
- driveadmins / sudo only. The helper writes its own file
  `/etc/exports.d/anchorage.exports`, then runs `exportfs -ra`.
- Every export option with a description (`rw`/`ro`, `sync`/`async`, `root_squash`/
  `no_root_squash`, `all_squash`, `anonuid`/`anongid`, `subtree_check`, `sec=`, `fsid=`…).
- A client restriction is required (host, subnet, or `*` only after a warning).
  `no_root_squash` needs a warning and a typed confirmation, because it gives the client's
  root user root access to the exported files.

### 16.4 Firewall
- If `ufw` is active, offer to allow Samba (`ufw allow from <local subnet> to any app Samba`)
  and/or NFS for the **local subnet only**. Never open to the internet. Shown as a
  suggestion when the first share is created; also in Settings.

### 16.5 Optional components (also applies to `cifs-utils`)
- The **setup script** asks whether to install each optional component: `cifs-utils`
  (mount SMB), `samba` (share over SMB), `nfs-kernel-server` (share over NFS).
- **Settings → Optional components** lists them with Install buttons (via PackageKit,
  which handles its own authorisation).
- **If a component is missing, its features are hidden** (not shown greyed out). Settings
  explains how to enable them.

### 16.6 Additional helper actions (phase 2)
`samba.share.write` (own include file only), `samba.password` (self, or others for
admins), `samba.guest` (global guest setting), `nfs.exports.write` (own exports file only),
`firewall.allow` (fixed ufw profiles, local subnet only). Each is a separate polkit action
with tests and fuzz targets, per the security rules in `CLAUDE.md`.

### 16.7 UI (layout C)
- Places gets a **Sharing** view (only when `samba` or `nfs-kernel-server` is installed):
  My shares (usershares), System shares, NFS exports. Same tiles + info panel pattern.
- Dolphin: the existing Share tab stays as is. Anchorage adds "Share this folder…" to its
  service-menu submenu, which opens the share editor pre-filled.
- Nice to have: show who is connected (`smbstatus`), read-only.
