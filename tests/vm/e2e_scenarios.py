#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end scenarios for Anchorage's authorisation chain, run by tests/vm/e2e.sh.

Each scenario logs in on the VM's text console tty3 as a test user (a local, active
seat0 session, typed through QEMU's virtual keyboard), runs the KAuth test client with
pkttyagent answering polkit's password prompt, and then checks /etc/fstab, backups and
folders over SSH.

Test users (fresh random passwords per run, kept in memory only, never printed):
  alice  standard user         nora  member of netmount
  dana   member of driveadmins sam   member of sudo (administrator)
"""
import os
import re
import secrets
import select
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
VM = os.path.join(HERE, "vm.sh")
USERS = {"alice": [], "nora": ["netmount"], "dana": ["driveadmins"], "sam": ["sudo"]}
PASSWORDS = {name: secrets.token_urlsafe(18) for name in USERS}
WRONG = "definitely-not-the-password"


# ---- helpers over SSH (as the VM's `test` user, with sudo) ---------------------------
def ssh(cmd, stdin=None):
    r = subprocess.run([VM, "ssh", "-o", "BatchMode=yes", cmd], input=stdin, capture_output=True, text=True, timeout=180)
    return r


def vm_fstab():
    return ssh("cat /etc/fstab").stdout


def backups():
    return sorted(ssh("sudo ls /etc/anchorage/backups 2>/dev/null").stdout.split())


def is_dir(path):
    return ssh(f"test -d '{path}' && echo yes").stdout.strip() == "yes"


def put_file(user, name, content):
    path = f"/home/{user}/{name}"
    r = ssh(f"sudo tee '{path}' >/dev/null && sudo chown {user}: '{path}'", stdin=content)
    assert r.returncode == 0, r.stderr
    return path


def add_line(content, line):
    return content + ("" if content.endswith("\n") else "\n") + line + "\n"


def change_root_options(content, extra):
    out = []
    for line in content.splitlines(keepends=True):
        fields = line.split()
        if len(fields) >= 4 and not line.lstrip().startswith("#") and fields[1] == "/":
            line = line.replace(fields[3], fields[3] + "," + extra, 1)
        out.append(line)
    return "".join(out)


def create_users():
    script = ["set -e", "getent group driveadmins >/dev/null || sudo groupadd driveadmins",
              "getent group netmount >/dev/null || sudo groupadd netmount"]
    for name, groups in USERS.items():
        script.append(f"id {name} >/dev/null 2>&1 || sudo useradd -m -s /bin/bash {name}")
        for g in groups:
            script.append(f"sudo usermod -aG {g} {name}")
    r = ssh("\n".join(script))
    assert r.returncode == 0, r.stderr
    secret = "".join(f"{n}:{p}\n" for n, p in PASSWORDS.items())
    r = ssh("sudo chpasswd", stdin=secret)
    assert r.returncode == 0, "chpasswd failed"


# ---- the VM's real keyboard and screen (text console tty3 on seat0) ------------------
# polkit only treats sessions on a seat as local. SSH and the serial console have no
# seat, so polkit refuses them outright. A login on a virtual terminal (tty3) is a
# local, active seat0 session: exactly a person sitting at the machine. We type through
# QEMU's virtual keyboard (virsh send-key) and read the screen from /dev/vcsa3.
KEYS = {c: ("KEY_" + c.upper(), False) for c in "abcdefghijklmnopqrstuvwxyz"}
KEYS.update({c.upper(): ("KEY_" + c.upper(), True) for c in "abcdefghijklmnopqrstuvwxyz"})
KEYS.update({str(d): (f"KEY_{d}", False) for d in range(10)})
KEYS.update({"-": ("KEY_MINUS", False), "_": ("KEY_MINUS", True), " ": ("KEY_SPACE", False),
             ".": ("KEY_DOT", False), "/": ("KEY_SLASH", False), "\n": ("KEY_ENTER", False)})
TTY = 3
SCREEN = (
    "import struct,sys\n"
    "d=open('/dev/vcsa%d','rb').read()\n"
    "rows,cols=d[0],d[1]\n"
    "chars=d[4::2]\n"
    "for r in range(rows):\n"
    "    print(chars[r*cols:(r+1)*cols].decode('latin-1').rstrip())\n" % TTY
)


class Console:
    def __init__(self):
        r = ssh(f"sudo chvt {TTY}")
        assert r.returncode == 0, r.stderr
        self.consumed = {}

    def screen(self):
        return ssh(f"sudo python3 -c \"{SCREEN}\"").stdout

    def type(self, text):
        for ch in text:
            key, shift = KEYS[ch]
            codes = ["KEY_LEFTSHIFT", key] if shift else [key]
            subprocess.run(["virsh", "-c", "qemu:///session", "send-key", "anchorage-test", "--holdtime", "30", *codes],
                           check=True, capture_output=True)

    def expect(self, patterns, timeout=60):
        """Waits until one regex has more matches on screen than already consumed."""
        end = time.time() + timeout
        while True:
            text = self.screen()
            for i, p in enumerate(patterns):
                n = len(re.findall(p, text, re.M))
                if n > self.consumed.get(p, 0):
                    self.consumed[p] = n
                    return i, text
            if time.time() > end:
                raise TimeoutError(f"waiting for {patterns}; screen ends with: {text.strip()[-400:]!r}")
            time.sleep(0.5)

    def clear_counts(self):
        self.consumed = {}

    def login(self, user):
        self.clear_counts()
        i, _ = self.expect([r"login: *$", r"\$ *$"], 30)
        if i == 1:  # still logged in
            self.type("exit\n")
            self.clear_counts()
            self.expect([r"login: *$"], 30)
        self.type(user + "\n")
        self.expect([r"Password: *$"], 20)
        self.type(PASSWORDS[user] + "\n")
        self.expect([r"\$ *$"], 30)
        self.user = user

    def logout(self):
        self.type("clear\n")
        time.sleep(1)
        self.clear_counts()
        self.type("exit\n")
        self.expect([r"login: *$"], 30)

    def run(self, args, identity=None, password=None):
        """Runs the client; answers prompts as `identity` with `password` (WRONG = typo)."""
        # The client starts in the background, signals it is ready and waits. pkttyagent
        # registers for exactly that client process (KAuth's client-side check asks polkit
        # about the requesting process), reads the password from this console, and tells
        # the client to go once registered (--notify-fd). When the client exits, the agent stops.
        step = ("clear\nrm -f ~/e2e.res ~/e2e.name ~/e2e.go\n"
                f"ANCHORAGE_E2E_HANDSHAKE=$HOME/e2e anchorage-e2e-client {args} > ~/e2e.res 2>&1 &\n"
                "CPID=$!\n"
                "for i in $(seq 100); do [ -s ~/e2e.name ] && break; sleep 0.1; done\n"
                "( while kill -0 $CPID 2>/dev/null; do sleep 0.2; done; pkill -x -u \"$(id -u)\" pkttyagent ) &\n"
                "pkttyagent --process \"$CPID\" --notify-fd 3 3> >(cat >/dev/null; touch ~/e2e.go)\n"
                "wait\necho __BEGIN__\ncat ~/e2e.res\necho __END__\n")
        put_file(self.user, "step.sh", step)
        self.type("clear\n")
        time.sleep(1)
        self.clear_counts()
        self.type("bash step.sh\n")
        prompts = []
        attempts = 0
        while True:
            i, text = self.expect([r"Choose identity to authenticate as \(\d+-\d+\): *$", r"Password: *$", r"^__END__"], 90)
            if i == 0:
                listed = dict((n, num) for num, n in re.findall(r"^\s*(\d+)\.\s+(\S+)", text, re.M))
                prompts.append("identity:" + ",".join(sorted(listed)))
                if identity not in listed:
                    raise AssertionError(f"identity {identity} not offered: {sorted(listed)}")
                self.type(listed[identity] + "\n")
            elif i == 1:
                prompts.append("password")
                attempts += 1
                if attempts > 3:
                    raise AssertionError("asked for a password more than 3 times")
                self.type((password if password is not None else WRONG) + "\n")
            else:
                block = re.search(r"^__BEGIN__\n(.*?)^__END__", text, re.S | re.M)
                lines = block.group(1).splitlines() if block else []
                res = next((l.strip() for l in lines if l.startswith("RESULT")), "RESULT (none) " + " | ".join(lines[-3:]))
                return res, prompts


# ---- scenarios ----------------------------------------------------------------------
results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"\n      {detail}" if detail and not ok else ""))


def main():
    print("Creating test users with random passwords...")
    create_users()
    base = vm_fstab()
    serial = Console()

    # 1. Remote (SSH) sessions are refused before any password prompt.
    f = put_file("test", "f-remote", add_line(base, "LABEL=r /mnt/e2e-remote ext4 nofail 0 0"))
    r = ssh(f"anchorage-e2e-client write {f}")
    check("SSH session is refused without a prompt", "AuthorizationDenied" in r.stdout and vm_fstab() == base, r.stdout.strip())

    # 2-4. alice (standard user): needs an administrator's password.
    serial.login("alice")
    f = put_file("alice", "f1", add_line(base, "LABEL=a /mnt/e2e-alice ext4 nofail 0 0"))
    res, prompts = serial.run(f"write {f} --create", identity="sam", password=WRONG)
    check("alice + wrong admin password: denied, file unchanged",
          res.startswith("RESULT error") and vm_fstab() == base and prompts[:1] == ["identity:sam,test"], f"{res} {prompts}")

    before = backups()
    res, prompts = serial.run(f"write {f} --create", identity="sam", password=PASSWORDS["sam"])
    after_alice = vm_fstab()
    check("alice + admin password: entry added, backup made, folder created",
          res.startswith("RESULT ok") and "/mnt/e2e-alice" in after_alice and len(backups()) == len(before) + 1
          and is_dir("/mnt/e2e-alice") and "identity:sam,test" in prompts, f"{res} {prompts}")

    f = put_file("alice", "f2", change_root_options(after_alice, "x-e2e=1"))
    res, prompts = serial.run(f"write {f}", identity="sam", password=PASSWORDS["sam"])
    check("alice cannot change a system entry even with an admin's password",
          res.startswith("RESULT error") and "system entry" in res and vm_fstab() == after_alice, f"{res} {prompts}")
    serial.logout()

    # 5-7. dana (driveadmins): own password; may change system entries and the override zone.
    serial.login("dana")
    f = put_file("dana", "f1", change_root_options(after_alice, "x-e2e=1"))
    res, prompts = serial.run(f"write {f}", identity="dana", password=PASSWORDS["dana"])
    after_dana = vm_fstab()
    check("dana uses her own password and may change a system entry",
          res.startswith("RESULT ok") and "x-e2e=1" in after_dana and prompts == ["password"], f"{res} {prompts}")

    f = put_file("dana", "f2", add_line(after_dana, "nas.invalid:/cfg /opt/e2e-data nfs4 nofail,noauto 0 0"))
    res, prompts = serial.run(f"write {f} --create", identity="dana", password=PASSWORDS["dana"])
    after_dana2 = vm_fstab()
    # polkit remembers an authorisation for the process that authenticated, so a new client
    # process asks again (the GUI, one long-running process, asks once per few minutes).
    check("dana: override zone allowed, folder created, own password",
          res.startswith("RESULT ok") and "/opt/e2e-data" in after_dana2 and is_dir("/opt/e2e-data")
          and prompts in ([], ["password"]), f"{res} {prompts}")

    f = put_file("dana", "f3", add_line(after_dana2, "nas.invalid:/k /etc/ssh/e2e nfs4 nofail,noauto 0 0"))
    res, prompts = serial.run(f"write {f} --confirm", identity="dana", password=PASSWORDS["dana"])
    check("dana cannot mount on a protected folder (/etc/ssh)",
          res.startswith("RESULT error") and vm_fstab() == after_dana2, f"{res} {prompts}")
    serial.logout()

    # 8-10. nora (netmount): shared mounts with her own password, nothing else.
    serial.login("nora")
    res, prompts = serial.run("add-shared //nas.invalid/Share /mnt/e2e-share cifs guest,vers=3.1.1",
                              identity="nora", password=PASSWORDS["nora"])
    line = next((l for l in vm_fstab().splitlines() if "/mnt/e2e-share" in l), "")
    check("nora adds a shared mount with her own password; safe options forced",
          res.startswith("RESULT ok") and all(o in line for o in ("nosuid", "nodev", "nofail", "_netdev"))
          and is_dir("/mnt/e2e-share") and prompts == ["password"], f"{res} {prompts} {line}")

    res, prompts = serial.run("remove-shared /mnt/e2e-share", identity="nora", password=PASSWORDS["nora"])
    check("nora removes her shared mount", res.startswith("RESULT ok") and "/mnt/e2e-share" not in vm_fstab(), f"{res} {prompts}")

    current = vm_fstab()
    f = put_file("nora", "f1", add_line(current, "LABEL=n /mnt/e2e-nora ext4 nofail 0 0"))
    res, prompts = serial.run(f"write {f}", identity="sam", password=WRONG)
    check("nora needs an administrator for a general fstab write",
          res.startswith("RESULT error") and vm_fstab() == current and prompts[:1] == ["identity:sam,test"], f"{res} {prompts}")
    serial.logout()

    # 11. sam (administrator): protected location only with confirmation.
    serial.login("sam")
    f = put_file("sam", "f1", add_line(current, "nas.invalid:/k /etc/ssh/e2e nfs4 nofail,noauto 0 0"))
    res, prompts = serial.run(f"write {f}", identity="sam", password=PASSWORDS["sam"])
    check("sam: protected location refused without confirmation", res.startswith("RESULT error") and vm_fstab() == current,
          f"{res} {prompts}")
    res, prompts = serial.run(f"write {f} --confirm", identity="sam", password=PASSWORDS["sam"])
    check("sam: protected location allowed with confirmation", res.startswith("RESULT ok") and "/etc/ssh/e2e" in vm_fstab(),
          f"{res} {prompts}")
    serial.logout()

    # 12. dana restores the backup taken before sam's change.
    serial.login("dana")
    newest = backups()[-1]
    res, prompts = serial.run(f"restore {newest}", identity="dana", password=PASSWORDS["dana"])
    check("dana restores the latest backup", res.startswith("RESULT ok") and vm_fstab() == current, f"{res} {prompts}")
    serial.logout()

    # 13-16. Settings (writesettings): driveadmins and administrators only.
    serial.login("alice")
    res, prompts = serial.run("settings 2 ntfs", identity="sam", password=PASSWORDS["sam"])
    check("alice can't change settings, even with an administrator's password",
          res.startswith("RESULT error") and "driveadmins" in res and ssh("sudo test -e /etc/anchorage/config && echo yes").stdout.strip() != "yes",
          f"{res} {prompts}")
    serial.logout()

    serial.login("dana")
    res, prompts = serial.run("settings 2 ntfs", identity="dana", password=PASSWORDS["dana"])
    config = ssh("cat /etc/anchorage/config").stdout
    mode = ssh("stat -c %U:%G:%a /etc/anchorage/config").stdout.strip()
    check("dana sets 'keep 2 backups' with her own password; file is root:root 0644",
          res.startswith("RESULT ok") and "keep-backups = 2" in config and "ntfs-driver = ntfs" in config and mode == "root:root:644"
          and prompts == ["password"], f"{res} {prompts} {mode}")
    res, _ = serial.run("settings 0 ntfs3", identity="dana", password=PASSWORDS["dana"])
    check("out-of-range settings are refused", res.startswith("RESULT error") and "keep-backups = 2" in ssh("cat /etc/anchorage/config").stdout, res)
    count_before = len(backups())
    current = vm_fstab()
    f = put_file("dana", "f9", add_line(current, "tmpfs /mnt/e2e-keep tmpfs nofail,noauto 0 0"))
    res, _ = serial.run(f"write {f}", identity="dana", password=PASSWORDS["dana"])
    check("the next write keeps only the newest 2 backups", res.startswith("RESULT ok") and count_before > 2 and len(backups()) == 2,
          f"{res} before={count_before} after={len(backups())}")
    serial.logout()

    failed = [r for r in results if not r[1]]
    ssh("sudo chvt 1")
    print(f"\n{len(results) - len(failed)}/{len(results)} scenarios passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
