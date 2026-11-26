#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Anchorage against real drives attached to the test VM (tests/vm/realdrives.sh), run by
tests/vm/realdrives_test.sh.

  Lingames  btrfs, 1.4 TiB of games. Only a folder "anchorage-test" is created and
            removed again; nothing else is touched.
  Windows   a clean Windows install the owner will reformat: C: (NTFS), its recovery and
            EFI partitions. Mounted read-write with ntfs3 and ntfs-3g.

Everything goes through the installed helper with real polkit prompts on tty3 (dana is
in driveadmins, alice is a standard user), using the same code paths as the GUI.
"""
import re
import sys
import time

from e2e_scenarios import PASSWORDS, Console, check, create_users, results, ssh, vm_fstab
from editor_scenarios import as_user, fs_at, reboot

LINGAMES = "16ee7fab-7ac0-4e9a-8bb9-ba953c4cdffe"
WIN_C = "902494B524949FB0"
WIN_RECOVERY = "56AC1FCFAC1FA90F"
WIN_EFI = "2E93-E8D5"
TEST_DIR = "anchorage-test"


def device(uuid):
    return ssh(f"sudo blkid -U {uuid}").stdout.strip()


def line_for(mount_point):
    return next((l for l in vm_fstab().splitlines() if not l.lstrip().startswith("#") and len(l.split()) > 1
                 and l.split()[1] == mount_point), "")


def opts_mounted(path):
    return ssh(f"findmnt -n -o OPTIONS --mountpoint {path} | tail -1").stdout.strip()


def main():
    print("Creating test users with random passwords...")
    create_users()
    alice_uid = ssh("id -u alice").stdout.strip()
    alice_gid = ssh("id -g alice").stdout.strip()
    ling = device(LINGAMES)
    win = device(WIN_C)
    check("the VM sees both real drives by UUID", ling.startswith("/dev/sd") and win.startswith("/dev/sd"), f"{ling} {win}")

    # ---- Lingames, as Dolphin leaves it: mounted at /run/media/<user>/Lingames ---------
    ssh(f"sudo mkdir -p /run/media/test/Lingames && sudo mount {ling} /run/media/test/Lingames")
    r = ssh(f"USER=test anchorage-e2e-client suggest {ling}")
    sugg = re.findall(r"^SUGGEST (\S+)", r.stdout, re.M)
    check("suggestions start where Dolphin mounted it, then /mnt, /media",
          sugg[:3] == ["/run/media/test/Lingames", "/mnt/Lingames", "/media/Lingames"], r.stdout)
    games = ssh("ls /run/media/test/Lingames | wc -l").stdout.strip()

    con = Console()
    con.login("dana")
    # Move it to /mnt/Lingames, keeping the old place as a bind (the editor's "keep it
    # reachable at … too").
    res, prompts = con.run(f"edit UUID={LINGAMES} /mnt/Lingames btrfs boot admins bind=/run/media/test/Lingames", "dana", PASSWORDS["dana"])
    bind = line_for("/run/media/test/Lingames")
    check("Lingames added at /mnt/Lingames with a bind at its old place",
          res.startswith("RESULT ok") and line_for("/mnt/Lingames") and "x-systemd.requires-mounts-for=/mnt/Lingames" in bind
          and prompts == ["password"], f"{res} {bind!r}")
    res, _ = con.run("mount-entry /mnt/Lingames", "dana", PASSWORDS["dana"])
    count = ssh("ls /mnt/Lingames | wc -l").stdout.strip()
    check("mounting it from fstab shows the same games", res.startswith("RESULT ok") and count == games and int(count) > 0,
          f"{res} {count} vs {games}")
    ssh(f"sudo mkdir -p /mnt/Lingames/{TEST_DIR} && echo real-drive | sudo tee /mnt/Lingames/{TEST_DIR}/marker >/dev/null")
    ssh("sudo umount /run/media/test/Lingames")  # Dolphin's old mount goes away
    res, _ = con.run("mount-entry /run/media/test/Lingames", "dana", PASSWORDS["dana"])
    check("the bind at the old place shows the drive", res.startswith("RESULT ok")
          and ssh(f"cat /run/media/test/Lingames/{TEST_DIR}/marker").stdout.strip() == "real-drive", res)

    # Drive in use.
    ssh(f"sudo -u nora sh -c 'cd /mnt/Lingames/{TEST_DIR} && exec sleep 300' >/dev/null 2>&1 &")
    time.sleep(1)
    res, _ = con.run("unmount /mnt/Lingames", "dana", PASSWORDS["dana"])
    check("unmounting Lingames while a program uses it names the program", res.startswith("RESULT error") and "sleep" in res, res)
    ssh("sudo pkill -u nora -x sleep")
    time.sleep(1)

    # btrfs options on the real drive: compression, through the Advanced tab path.
    res, _ = con.run("unmount /run/media/test/Lingames", "dana", PASSWORDS["dana"])
    res2, _ = con.run("unmount /mnt/Lingames", "dana", PASSWORDS["dana"])
    res3, _ = con.run(f"edit UUID={LINGAMES} /mnt/Lingames btrfs boot admins opt=compress=zstd:3 opt=noatime", "dana", PASSWORDS["dana"])
    res4, _ = con.run("mount-entry /mnt/Lingames", "dana", PASSWORDS["dana"])
    o = opts_mounted("/mnt/Lingames")
    check("Lingames remounted with compress=zstd:3,noatime", all(x.startswith("RESULT ok") for x in (res, res2, res3, res4))
          and "compress=zstd:3" in o and "noatime" in o, f"{res} {res2} {res3} {res4} {o}")

    # ---- Windows C: with ntfs3, owned by alice, owner only ------------------------------
    res, _ = con.run(f"edit UUID={WIN_C} /mnt/windows ntfs3 request admins uid={alice_uid} gid={alice_gid} sharing=owner", "dana",
                     PASSWORDS["dana"])
    res2, _ = con.run("mount-entry /mnt/windows", "dana", PASSWORDS["dana"])
    top = ssh("stat -c %U:%a /mnt/windows").stdout.strip()
    inner = ssh("sudo stat -c %U:%a /mnt/windows/Windows").stdout.strip()
    check("Windows C: mounts with ntfs3; files belong to alice", res.startswith("RESULT ok") and res2.startswith("RESULT ok")
          and fs_at("/mnt/windows") == "ntfs3" and inner == "alice:700", f"{res} {res2} top={top} inner={inner}")
    check("ntfs3 leaves the top folder open (0777), as the editor warns", top.endswith(":777"), top)
    rc, out = as_user("alice", f"mkdir -p /mnt/windows/{TEST_DIR} && echo hello > /mnt/windows/{TEST_DIR}/from-linux && cat /mnt/windows/{TEST_DIR}/from-linux")
    check("alice writes to the Windows drive", rc == 0 and out.strip() == "hello", out)
    rc, out = as_user("nora", "ls /mnt/windows/Windows")
    check("nora can't look inside Windows' folders", rc != 0, out)

    # ---- the same with ntfs-3g ----------------------------------------------------------
    res, _ = con.run("unmount /mnt/windows", "dana", PASSWORDS["dana"])
    res2, _ = con.run(f"edit UUID={WIN_C} /mnt/windows ntfs request admins", "dana", PASSWORDS["dana"])
    line = line_for("/mnt/windows")
    res3, _ = con.run("mount-entry /mnt/windows", "dana", PASSWORDS["dana"])
    top = ssh("stat -c %U:%a /mnt/windows").stdout.strip()
    check("switched to ntfs-3g (only the type changed); the top folder is closed too",
          all(x.startswith("RESULT ok") for x in (res, res2, res3)) and line.split()[2] == "ntfs" and top == "alice:700"
          and "fuseblk" in fs_at("/mnt/windows"), f"{res} {res2} {res3} {line!r} {top}")
    rc, out = as_user("nora", "ls /mnt/windows")
    check("with ntfs-3g nora can't even list the drive", rc != 0, out)
    rc, out = as_user("alice", f"cat /mnt/windows/{TEST_DIR}/from-linux")
    check("alice's file is still there", rc == 0 and out.strip() == "hello", out)
    res, _ = con.run("unmount /mnt/windows", "dana", PASSWORDS["dana"])
    check("unmounting the FUSE (ntfs-3g) drive works", res.startswith("RESULT ok") and fs_at("/mnt/windows") == "", res)

    # ---- Mount At… (temporary) for the other Windows partitions --------------------------
    before = vm_fstab()
    res, _ = con.run(f"mount-temp UUID={WIN_RECOVERY} /mnt/recovery ntfs3 ro", "dana", PASSWORDS["dana"])
    res2, _ = con.run(f"mount-temp UUID={WIN_EFI} /mnt/win-efi vfat ro,uid={alice_uid}", "dana", PASSWORDS["dana"])
    efi = ssh("ls /mnt/win-efi/EFI 2>/dev/null").stdout
    check("Mount At: Windows' recovery and EFI partitions, read-only, fstab unchanged",
          res.startswith("RESULT ok") and res2.startswith("RESULT ok") and "ro" in opts_mounted("/mnt/recovery").split(",")
          and "Microsoft" in efi and vm_fstab() == before, f"{res} {res2} {efi!r}")
    for mp in ("/mnt/recovery", "/mnt/win-efi"):
        res, _ = con.run(f"unmount {mp}", "dana", PASSWORDS["dana"])
        check(f"unmount {mp}", res.startswith("RESULT ok") and fs_at(mp) == "", res)
    con.logout()

    # ---- a standard user ---------------------------------------------------------------
    con.login("alice")
    res, _ = con.run(f"mount-temp UUID={WIN_C} /mnt/sneaky ntfs3", "sam", PASSWORDS["sam"])
    check("alice can't Mount At, even with sam's password", res.startswith("RESULT error") and fs_at("/mnt/sneaky") == "", res)
    res, _ = con.run("mount-entry /mnt/windows", "sam", PASSWORDS["sam"])
    check("alice mounts the Windows entry with sam's password", res.startswith("RESULT ok") and fs_at("/mnt/windows") != "", res)
    con.logout()

    # ---- restart -------------------------------------------------------------------------
    print("Restarting the VM...")
    check("the VM restarts with the real drives in fstab", reboot())
    check("Lingames is mounted at start-up with compression", "compress=zstd:3" in opts_mounted("/mnt/Lingames"), opts_mounted("/mnt/Lingames"))
    check("its bind at the old place is mounted too",
          ssh(f"cat /run/media/test/Lingames/{TEST_DIR}/marker").stdout.strip() == "real-drive", ssh("findmnt /run/media/test/Lingames").stdout)
    check("Windows (on request) is not mounted at start-up", fs_at("/mnt/windows") == "", fs_at("/mnt/windows"))

    # ---- clean up the real drives --------------------------------------------------------
    con = Console()
    con.login("dana")
    res, _ = con.run("mount-entry /mnt/windows", "dana", PASSWORDS["dana"])
    ssh(f"sudo rm -rf /mnt/windows/{TEST_DIR} /mnt/Lingames/{TEST_DIR}")
    gone = ssh(f"sudo test -e /mnt/windows/{TEST_DIR} || sudo test -e /mnt/Lingames/{TEST_DIR} || echo gone").stdout.strip()
    for mp in ("/run/media/test/Lingames", "/mnt/Lingames", "/mnt/windows"):
        con.run(f"unmount {mp}", "dana", PASSWORDS["dana"])
    con.logout()
    check("the test folders are removed from both real drives", gone == "gone", gone)

    failed = [r for r in results if not r[1]]
    ssh("sudo chvt 1")
    print(f"\n{len(results) - len(failed)}/{len(results)} scenarios passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
