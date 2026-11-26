#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end scenarios for the entry editor, run by tests/vm/editor.sh.

Entries are built with the editor's own code (anchorage-e2e-client edit: access::apply +
fstab::File) and saved through the GUI's KAuth client with real polkit password prompts
on tty3. Then the VM's real mount, systemd and a reboot show what each Access choice does:

  * user / users mounts by people without root, with the owner and permissions chosen;
  * "while the computer starts", "only when asked", "the first time the folder is opened";
  * "start even if missing" (nofail): a missing drive and an unreachable NFS server don't
    stop the computer from starting;
  * editing an existing entry, removing one, restoring a backup.

Disk images are attached as loop devices by a small boot service, so they are there
again after the reboot, like real drives.
"""
import re
import sys
import time

import subprocess

from e2e_scenarios import PASSWORDS, VM, Console, backups, check, create_users, put_file, results, ssh, vm_fstab

IMAGES = {  # label: (mkfs, size)
    "E2EBOOT": ("mkfs.ext4 -q -F -L E2EBOOT", "64M"),
    "E2EFAT": ("mkfs.vfat -n E2EFAT", "64M"),
    "E2EAUTO": ("mkfs.ext4 -q -F -L E2EAUTO", "64M"),
}
MISSING_UUID = "0e2e0e2e-dead-4bee-8f00-000000000000"


def setup_images():
    lines = ["set -e", "sudo mkdir -p /var/lib/anchorage-e2e"]
    for label, (mkfs, size) in IMAGES.items():
        img = f"/var/lib/anchorage-e2e/{label}.img"
        lines += [f"sudo truncate -s {size} {img}", f"sudo {mkfs} {img} >/dev/null"]
    # A folder with a marker file on the start-up drive, for the bind scenarios.
    lines += ["sudo mkdir -p /tmp/e2e-prep", "sudo mount -o loop /var/lib/anchorage-e2e/E2EBOOT.img /tmp/e2e-prep",
              "sudo mkdir -p /tmp/e2e-prep/data", "echo on-the-drive | sudo tee /tmp/e2e-prep/data/marker >/dev/null",
              "sudo umount /tmp/e2e-prep"]
    attach = " ".join(f"/var/lib/anchorage-e2e/{l}.img" for l in IMAGES)
    unit = f"""[Unit]
Description=Attach Anchorage e2e disk images
DefaultDependencies=no
Before=local-fs-pre.target
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for i in {attach}; do /usr/sbin/losetup -f "$i"; done; /usr/bin/udevadm settle'
[Install]
WantedBy=local-fs-pre.target
"""
    r = ssh("\n".join(lines))
    assert r.returncode == 0, r.stderr
    r = ssh("sudo tee /etc/systemd/system/anchorage-e2e-loops.service >/dev/null && sudo systemctl daemon-reload "
            "&& sudo systemctl enable --now anchorage-e2e-loops.service >/dev/null 2>&1", stdin=unit)
    assert r.returncode == 0, r.stderr
    uuids = {}
    for label in IMAGES:
        uuids[label] = ssh(f"sudo blkid -o value -s UUID -t LABEL={label} | head -1").stdout.strip()
        assert uuids[label], f"no UUID for {label}"
    return uuids


def line_for(mount_point):
    return next((l for l in vm_fstab().splitlines() if not l.lstrip().startswith("#") and len(l.split()) > 1
                 and l.split()[1] == mount_point), "")


def opts(line):
    fields = line.split()
    return set(fields[3].split(",")) if len(fields) > 3 else set()


def as_user(user, cmd):
    r = ssh(f"sudo -u {user} sh -c '{cmd}'")
    return r.returncode, (r.stdout + r.stderr).strip()


def fs_at(path):
    return ssh(f"findmnt -n -o FSTYPE --mountpoint {path} | tail -1").stdout.strip()


def unit_state(path):
    unit = ssh(f"systemd-escape --path {path}").stdout.strip()
    return ssh(f"systemctl show -p ActiveState --value '{unit}.mount'").stdout.strip()


def reboot():
    ssh("sudo systemctl reboot")
    time.sleep(10)
    deadline = time.time() + 300
    while time.time() < deadline:
        r = subprocess.run([VM, "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "true"], capture_output=True)
        if r.returncode == 0:
            return True
        time.sleep(3)
    return False


def main():
    print("Creating test users with random passwords...")
    create_users()
    print("Creating disk images (ext4, FAT, ext4) attached at every boot...")
    uuids = setup_images()
    alice_uid = ssh("id -u alice").stdout.strip()
    alice_gid = ssh("id -g alice").stdout.strip()
    con = Console()

    # ---- dana (driveadmins) builds entries the way the editor does -------------------
    con.login("dana")
    before = len(backups())
    res, prompts = con.run(f"edit UUID={uuids['E2EBOOT']} /mnt/e2e-boot ext4 boot admins bind=/srv/e2e-bind bind=/srv/e2e-data:data",
                           "dana", PASSWORDS["dana"])
    line = line_for("/mnt/e2e-boot")
    check("dana adds a drive mounted at start-up (editor default: nofail)",
          res.startswith("RESULT ok") and line.split()[3:4] == ["nofail"] and prompts == ["password"]
          and len(backups()) == before + 1, f"{res} {prompts} {line!r}")
    unit = ssh("systemd-escape --path /mnt/e2e-boot").stdout.strip()
    check("the helper reloaded systemd: mount unit exists from /etc/fstab",
          "/etc/fstab" in ssh(f"systemctl show -p SourcePath --value '{unit}.mount'").stdout, line)

    res, _ = con.run(f"edit UUID={uuids['E2EFAT']} /mnt/e2e-fat vfat request user uid={alice_uid} gid={alice_gid} sharing=owner",
                     "dana", PASSWORDS["dana"])
    line = line_for("/mnt/e2e-fat")
    check("dana adds a FAT drive: on request, any user, owned by alice, owner only",
          res.startswith("RESULT ok") and opts(line) == {"nofail", "noauto", "user", f"uid={alice_uid}", f"gid={alice_gid}", "umask=077"},
          f"{res} {line!r}")
    fat_line = line

    res, _ = con.run(f"edit UUID={uuids['E2EAUTO']} /mnt/e2e-auto ext4 opened admins", "dana", PASSWORDS["dana"])
    check("dana adds a drive mounted when its folder is opened", res.startswith("RESULT ok")
          and opts(line_for("/mnt/e2e-auto")) == {"nofail", "noauto", "x-systemd.automount"}, res)

    res, _ = con.run(f"edit UUID={MISSING_UUID} /mnt/e2e-missing ext4 boot admins opt=x-systemd.device-timeout=5s bind=/srv/e2e-missing-bind",
                     "dana", PASSWORDS["dana"])
    check("dana adds a drive that isn't connected (nofail, 5 s device timeout)", res.startswith("RESULT ok"), res)

    res, _ = con.run("edit nas.invalid:/export /mnt/e2e-nfs nfs4 opened admins", "dana", PASSWORDS["dana"])
    nfs = line_for("/mnt/e2e-nfs")
    check("dana adds an NFS share with the editor's network defaults",
          res.startswith("RESULT ok") and opts(nfs) == {"noauto", "x-systemd.automount", "nofail", "_netdev"}, f"{res} {nfs!r}")
    con.logout()

    # ---- user mounts without root ---------------------------------------------------
    rc, out = as_user("alice", "mount /mnt/e2e-fat")
    check("alice mounts the 'any user' FAT drive without a password", rc == 0 and fs_at("/mnt/e2e-fat") == "vfat", out)
    rc, out = as_user("alice", "stat -c %U:%a /mnt/e2e-fat && touch /mnt/e2e-fat/alice.txt")
    check("the files belong to alice, permissions 700", rc == 0 and out.splitlines()[0] == "alice:700", out)
    rc, out = as_user("nora", "ls /mnt/e2e-fat")
    check("nora can't look inside (owner only)", rc != 0, out)
    rc, out = as_user("nora", "umount /mnt/e2e-fat")
    check("nora can't unmount alice's 'user' mount", rc != 0 and fs_at("/mnt/e2e-fat") == "vfat", out)
    rc, out = as_user("alice", "umount /mnt/e2e-fat")
    check("alice unmounts it again", rc == 0 and fs_at("/mnt/e2e-fat") == "", out)
    rc, out = as_user("alice", "mount /mnt/e2e-boot")
    check("alice can't mount an 'administrators only' drive", rc != 0, out)

    # ---- changing an existing entry: user -> users ------------------------------------
    con.login("dana")
    res, _ = con.run(f"edit UUID={uuids['E2EFAT']} /mnt/e2e-fat vfat request users", "dana", PASSWORDS["dana"])
    line = line_for("/mnt/e2e-fat")
    check("dana changes it to 'anyone can unmount' (only that option changes)",
          res.startswith("RESULT ok") and line.split() == fat_line.replace(",user,", ",users,").split(), f"{res} {line!r} was {fat_line!r}")
    con.logout()
    rc1, _ = as_user("alice", "mount /mnt/e2e-fat")
    rc2, out = as_user("nora", "umount /mnt/e2e-fat")
    check("with 'users', nora may unmount what alice mounted", rc1 == 0 and rc2 == 0 and fs_at("/mnt/e2e-fat") == "", out)

    # ---- a standard user with an administrator's password -----------------------------
    con.login("alice")
    f = put_file("alice", "bind-etc", vm_fstab() + "/etc /mnt/e2e-etc none bind,nofail 0 0\n")
    res, _ = con.run(f"write {f} --create", "sam", PASSWORDS["sam"])
    check("alice can't show a system folder (/etc) elsewhere, even with sam's password",
          res.startswith("RESULT error") and "/mnt/e2e-etc" not in vm_fstab(), res)
    res, prompts = con.run("edit tmpfs /mnt/e2e-alice tmpfs request admins", "sam", PASSWORDS["sam"])
    check("alice adds an entry with sam's (administrator) password", res.startswith("RESULT ok") and "/mnt/e2e-alice" in vm_fstab()
          and prompts[:1] == ["identity:sam,test"], f"{res} {prompts}")
    con.logout()

    # ---- restart: what each choice does at start-up ------------------------------------
    print("Restarting the VM...")
    check("the VM restarts and comes back (missing drive and unreachable server don't block it)", reboot())
    state = ssh("systemctl is-system-running").stdout.strip()
    check("system state is running or degraded, not emergency/maintenance", state in ("running", "degraded"), state)
    check("the start-up drive is mounted", fs_at("/mnt/e2e-boot") == "ext4", fs_at("/mnt/e2e-boot"))
    check("its binds show the drive: the whole drive and a folder on it",
          ssh("cat /srv/e2e-bind/data/marker").stdout.strip() == "on-the-drive" and ssh("cat /srv/e2e-data/marker").stdout.strip() == "on-the-drive",
          ssh("findmnt /srv/e2e-bind; findmnt /srv/e2e-data").stdout)
    check("the missing drive's bind stays unmounted (no empty folder shown in its place)",
          fs_at("/srv/e2e-missing-bind") == "", ssh("findmnt /srv/e2e-missing-bind").stdout)
    check("the 'only when asked' drive is not mounted", fs_at("/mnt/e2e-fat") == "", fs_at("/mnt/e2e-fat"))
    check("the missing drive's mount simply didn't happen", unit_state("/mnt/e2e-missing") in ("inactive", "failed")
          and fs_at("/mnt/e2e-missing") == "", unit_state("/mnt/e2e-missing"))
    check("the 'when opened' drive waits (autofs placeholder only)", fs_at("/mnt/e2e-auto") == "autofs", fs_at("/mnt/e2e-auto"))
    ssh("ls /mnt/e2e-auto >/dev/null")
    check("opening the folder mounts it", "ext4" in ssh("findmnt -n -o FSTYPE --mountpoint /mnt/e2e-auto").stdout,
          ssh("findmnt --mountpoint /mnt/e2e-auto").stdout)
    check("the NFS share waits for its folder to be opened", fs_at("/mnt/e2e-nfs") == "autofs", fs_at("/mnt/e2e-nfs"))
    slow = ssh("systemd-analyze blame 2>/dev/null | head -3").stdout
    check("start-up didn't wait for the missing drive (no 90 s timeout)", not re.search(r"^\s*1min|^\s*[3-9]\d(\.\d+)?s ", slow, re.M), slow)

    # ---- mounting and unmounting through the helper (SPEC §6.1) ----------------------
    con = Console()
    con.login("dana")
    res, prompts = con.run("unmount /mnt/e2e-boot", "dana", PASSWORDS["dana"])
    check("dana unmounts the start-up drive with her own password", res.startswith("RESULT ok") and fs_at("/mnt/e2e-boot") == ""
          and prompts == ["password"], f"{res} {prompts}")
    res, _ = con.run("mount-entry /mnt/e2e-boot", "dana", PASSWORDS["dana"])
    check("... and mounts it again from fstab", res.startswith("RESULT ok") and fs_at("/mnt/e2e-boot") == "ext4", res)

    # In use: a program sitting in the folder is named; nothing is unmounted.
    ssh("sudo -u nora sh -c 'cd /mnt/e2e-boot && exec sleep 300' >/dev/null 2>&1 &")
    time.sleep(1)
    res, _ = con.run("unmount /mnt/e2e-boot", "dana", PASSWORDS["dana"])
    check("unmounting a drive in use names the program and leaves it mounted",
          res.startswith("RESULT error") and "sleep" in res and fs_at("/mnt/e2e-boot") == "ext4", res)
    ssh("sudo pkill -u nora -x sleep")
    time.sleep(1)
    res, _ = con.run("unmount /mnt/e2e-boot", "dana", PASSWORDS["dana"])
    check("... and works once the program has gone", res.startswith("RESULT ok") and fs_at("/mnt/e2e-boot") == "", res)

    # "Mounted when the folder is opened": systemd stays in charge.
    ssh("ls /mnt/e2e-auto >/dev/null")
    res, _ = con.run("unmount /mnt/e2e-auto", "dana", PASSWORDS["dana"])
    check("unmounting an automount drive leaves systemd watching the folder", res.startswith("RESULT ok") and fs_at("/mnt/e2e-auto") == "autofs", res)
    res, _ = con.run("mount-entry /mnt/e2e-auto", "dana", PASSWORDS["dana"])
    check("... and mount-entry starts it through systemd",
          res.startswith("RESULT ok") and "ext4" in ssh("findmnt -n -o FSTYPE --mountpoint /mnt/e2e-auto").stdout, res)
    res, _ = con.run("mount-entry /mnt/e2e-missing", "dana", PASSWORDS["dana"])
    check("mounting a 'start even if missing' drive that isn't there is reported, not 'done'",
          res.startswith("RESULT error") and "isn't connected" in res, res)

    # Temporary mount: not in fstab, gone after unmount.
    before_fstab = vm_fstab()
    res, _ = con.run(f"mount-temp UUID={uuids['E2EFAT']} /mnt/e2e-temp vfat ro,uid={alice_uid}", "dana", PASSWORDS["dana"])
    owner = ssh("stat -c %U /mnt/e2e-temp").stdout.strip()
    check("dana mounts the FAT drive at a chosen folder (read-only, owned by alice); fstab unchanged",
          res.startswith("RESULT ok") and fs_at("/mnt/e2e-temp") == "vfat" and owner == "alice" and vm_fstab() == before_fstab, f"{res} {owner}")
    res, _ = con.run("unmount /mnt/e2e-temp", "dana", PASSWORDS["dana"])
    check("... and unmounts it", res.startswith("RESULT ok") and fs_at("/mnt/e2e-temp") == "", res)
    res, _ = con.run(f"mount-temp UUID={uuids['E2EFAT']} /mnt/e2e-temp vfat bind", "dana", PASSWORDS["dana"])
    check("options that bypass the rules are refused (bind)", res.startswith("RESULT error") and fs_at("/mnt/e2e-temp") == "", res)

    # System entries: only with the typed confirmation.
    res, _ = con.run("unmount /boot/efi", "dana", PASSWORDS["dana"])
    check("unmounting /boot/efi without confirmation is refused", res.startswith("RESULT error") and fs_at("/boot/efi") == "vfat", res)
    res, _ = con.run("unmount /boot/efi --confirm", "dana", PASSWORDS["dana"])
    check("... and allowed with it", res.startswith("RESULT ok") and fs_at("/boot/efi") == "", res)
    res, _ = con.run("mount-entry /boot/efi", "dana", PASSWORDS["dana"])
    check("dana mounts /boot/efi again", res.startswith("RESULT ok") and fs_at("/boot/efi") == "vfat", res)
    con.logout()

    con.login("alice")
    res, prompts = con.run("mount-entry /mnt/e2e-boot", "sam", PASSWORDS["sam"])
    check("alice mounts a non-system entry with sam's password", res.startswith("RESULT ok") and fs_at("/mnt/e2e-boot") == "ext4", f"{res} {prompts}")
    res, _ = con.run(f"mount-temp UUID={uuids['E2EFAT']} /mnt/e2e-temp vfat", "sam", PASSWORDS["sam"])
    check("alice can't mount at a chosen folder, even with sam's password", res.startswith("RESULT error") and fs_at("/mnt/e2e-temp") == "", res)
    res, _ = con.run("unmount /boot/efi --confirm", "sam", PASSWORDS["sam"])
    check("alice can't unmount a system entry, even with sam's password", res.startswith("RESULT error") and fs_at("/boot/efi") == "vfat", res)
    con.logout()

    # ---- remove and restore ---------------------------------------------------------
    con.login("dana")
    before = vm_fstab()
    res, _ = con.run("edit-remove /mnt/e2e-missing", "dana", PASSWORDS["dana"])
    check("dana removes the missing drive's entry", res.startswith("RESULT ok") and line_for("/mnt/e2e-missing") == "", res)
    newest = backups()[-1]
    res, _ = con.run(f"restore {newest}", "dana", PASSWORDS["dana"])
    check("restoring the backup brings it back exactly", res.startswith("RESULT ok") and vm_fstab() == before, res)
    con.logout()

    failed = [r for r in results if not r[1]]
    ssh("sudo chvt 1")
    print(f"\n{len(results) - len(failed)}/{len(results)} scenarios passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
