// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountpolicy.h"

#include <QTest>

using namespace anchorage;
using namespace anchorage::policy;
using fstab::Entry;

Q_DECLARE_METATYPE(anchorage::policy::Zone)
Q_DECLARE_METATYPE(anchorage::fstab::Entry)

namespace
{
Caller makeCaller(bool sudo, bool driveAdmin, bool netMount)
{
    Caller c;
    c.uid = 1000;
    c.gid = 1000;
    c.name = QStringLiteral("oarko");
    c.home = QStringLiteral("/home/oarko");
    c.sudo = sudo;
    c.driveAdmin = driveAdmin;
    c.netMount = netMount;
    return c;
}
const Caller standard = makeCaller(false, false, false);
const Caller netmount = makeCaller(false, false, true);
const Caller driveadmin = makeCaller(false, true, false);
const Caller sudoer = makeCaller(true, false, false);

const QByteArray baseFile =
    "# test\n"
    "UUID=root / ext4 errors=remount-ro 0 1\n"
    "UUID=home /home ext4 defaults 0 2\n"
    "/swapfile none swap sw 0 0\n"
    "nas:/media /mnt/media nfs4 _netdev,nofail 0 0\n";

QByteArray withLine(const QByteArray &line)
{
    return baseFile + line + '\n';
}

Entry share(const QString &spec, const QString &mp, const QString &type, const QStringList &opts)
{
    return Entry{spec, mp, type, opts, 0, 0};
}
} // namespace

class PolicyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void classify_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<Zone>("zone");
        QTest::newRow("mnt") << QStringLiteral("/mnt/games") << Zone::Safe;
        QTest::newRow("media deep") << QStringLiteral("/media/backup/disk1") << Zone::Safe;
        QTest::newRow("srv") << QStringLiteral("/srv/backups") << Zone::Safe;
        QTest::newRow("home sub") << QStringLiteral("/home/oarko/NAS") << Zone::Safe;
        QTest::newRow("udisks") << QStringLiteral("/run/media/oarko/Games") << Zone::Safe;
        QTest::newRow("whole home") << QStringLiteral("/home/oarko") << Zone::Override;
        QTest::newRow("etc app") << QStringLiteral("/etc/someapp") << Zone::Override;
        QTest::newRow("var lib app") << QStringLiteral("/var/lib/plex") << Zone::Override;
        QTest::newRow("opt app") << QStringLiteral("/opt/data") << Zone::Override;
        QTest::newRow("tmp sub") << QStringLiteral("/tmp/x") << Zone::Override;
        QTest::newRow("root") << QStringLiteral("/") << Zone::Blocked;
        QTest::newRow("etc") << QStringLiteral("/etc") << Zone::Blocked;
        QTest::newRow("mnt itself") << QStringLiteral("/mnt") << Zone::Blocked;
        QTest::newRow("home itself") << QStringLiteral("/home") << Zone::Blocked;
        QTest::newRow("ssh") << QStringLiteral("/etc/ssh") << Zone::Blocked;
        QTest::newRow("ssh sub") << QStringLiteral("/etc/ssh/sshd_config.d") << Zone::Blocked;
        QTest::newRow("sudoers.d") << QStringLiteral("/etc/sudoers.d") << Zone::Blocked;
        QTest::newRow("pam") << QStringLiteral("/etc/pam.d") << Zone::Blocked;
        QTest::newRow("polkit") << QStringLiteral("/etc/polkit-1/rules.d") << Zone::Blocked;
        QTest::newRow("own config") << QStringLiteral("/etc/anchorage/backups") << Zone::Blocked;
        QTest::newRow("usr local") << QStringLiteral("/usr/local/bin") << Zone::Blocked;
        QTest::newRow("boot efi") << QStringLiteral("/boot/efi") << Zone::Blocked;
        QTest::newRow("dpkg") << QStringLiteral("/var/lib/dpkg/info") << Zone::Blocked;
        QTest::newRow("rpm") << QStringLiteral("/var/lib/rpm") << Zone::Blocked;
        QTest::newRow("dnf") << QStringLiteral("/var/lib/dnf/history") << Zone::Blocked;
        QTest::newRow("yum repos") << QStringLiteral("/etc/yum.repos.d") << Zone::Blocked;
        QTest::newRow("apt sources") << QStringLiteral("/etc/apt/sources.list.d") << Zone::Blocked;
        QTest::newRow("selinux") << QStringLiteral("/etc/selinux/targeted") << Zone::Blocked;
        QTest::newRow("run other") << QStringLiteral("/run/systemd") << Zone::Blocked;
        QTest::newRow("run media user") << QStringLiteral("/run/media/oarko") << Zone::Blocked;
        QTest::newRow("prefix trick") << QStringLiteral("/etc/sshkeys") << Zone::Override; // not /etc/ssh
        QTest::newRow("relative") << QStringLiteral("mnt/x") << Zone::Invalid;
        QTest::newRow("dotdot") << QStringLiteral("/mnt/../etc") << Zone::Invalid;
        QTest::newRow("dot") << QStringLiteral("/mnt/./x") << Zone::Invalid;
        QTest::newRow("double slash") << QStringLiteral("/mnt//x") << Zone::Invalid;
        QTest::newRow("trailing slash") << QStringLiteral("/mnt/x/") << Zone::Invalid;
        QTest::newRow("newline") << QStringLiteral("/mnt/x\n/etc") << Zone::Invalid;
        QTest::newRow("hash") << QStringLiteral("/mnt/#x") << Zone::Invalid;
        QTest::newRow("long component") << (QStringLiteral("/mnt/") + QString(256, QLatin1Char('a'))) << Zone::Invalid;
    }
    void classify()
    {
        QFETCH(QString, path);
        QFETCH(Zone, zone);
        QCOMPARE(classifyMountPoint(path).zone, zone);
    }

    void extraBlockedOnlyAddsProtection()
    {
        QCOMPARE(classifyMountPoint(QStringLiteral("/srv/secret/x"), {QStringLiteral("/srv/secret")}).zone, Zone::Blocked);
        QCOMPARE(classifyMountPoint(QStringLiteral("/srv/secretive"), {QStringLiteral("/srv/secret")}).zone, Zone::Safe);
        // A malformed line in the file must not unblock anything or crash.
        QCOMPARE(classifyMountPoint(QStringLiteral("/etc/ssh"), {QStringLiteral("../x"), QString()}).zone, Zone::Blocked);
    }

    void systemEntries()
    {
        QVERIFY(isSystemEntry({QStringLiteral("UUID=x"), QStringLiteral("/"), QStringLiteral("ext4"), {}, 0, 1}));
        QVERIFY(isSystemEntry({QStringLiteral("UUID=x"), QStringLiteral("/boot/efi"), QStringLiteral("vfat"), {}, 0, 1}));
        QVERIFY(isSystemEntry({QStringLiteral("UUID=x"), QStringLiteral("/var/log"), QStringLiteral("ext4"), {}, 0, 2}));
        QVERIFY(isSystemEntry({QStringLiteral("/swapfile"), QStringLiteral("none"), QStringLiteral("swap"), {}, 0, 0}));
        QVERIFY(!isSystemEntry({QStringLiteral("LABEL=Lingames"), QStringLiteral("/mnt/lingames"), QStringLiteral("btrfs"), {}, 0, 0}));
    }

    // ---- planFstabWrite ----
    void unchangedFileIsFine()
    {
        QVERIFY(planFstabWrite(baseFile, baseFile, standard, false).ok());
    }

    void standardCanAddSafeEntry()
    {
        const Plan p = planFstabWrite(baseFile, withLine("LABEL=Lingames /mnt/lingames btrfs nofail 0 0"), standard, false);
        QVERIFY2(p.ok(), qPrintable(p.errors.join(QLatin1Char('\n'))));
        QCOMPARE(p.added.size(), 1);
        QCOMPARE(p.newMountPoints, QStringList{QStringLiteral("/mnt/lingames")});
    }

    void standardCannotTouchSystemEntries()
    {
        QByteArray changed = baseFile;
        changed.replace("UUID=home /home ext4 defaults 0 2", "UUID=home /home ext4 defaults,noexec 0 2");
        QVERIFY(!planFstabWrite(baseFile, changed, standard, false).ok());
        QVERIFY(!planFstabWrite(baseFile, changed, netmount, false).ok());
        QVERIFY(planFstabWrite(baseFile, changed, driveadmin, false).ok());
        QVERIFY(planFstabWrite(baseFile, changed, sudoer, false).ok());

        QByteArray removed = baseFile;
        removed.replace("/swapfile none swap sw 0 0\n", "");
        QVERIFY(!planFstabWrite(baseFile, removed, standard, false).ok());
        QVERIFY(planFstabWrite(baseFile, removed, driveadmin, false).ok());
    }

    void subvolumesOfTheRootFileSystemAreSystem()
    {
        // /swap is a btrfs subvolume on the same file system as "/" (Kubuntu's layout).
        const QByteArray f = baseFile + "UUID=root /swap btrfs subvol=/@swap 0 0\n";
        QByteArray changed = f;
        changed.replace("subvol=/@swap", "subvol=/@swap,noatime");
        QVERIFY(!planFstabWrite(f, changed, standard, false).ok());
        QVERIFY(planFstabWrite(f, changed, driveadmin, false).ok());
        QByteArray removed = f;
        removed.replace("UUID=root /swap btrfs subvol=/@swap 0 0\n", "");
        QVERIFY(!planFstabWrite(f, removed, standard, false).ok());
        // Adding a new subvolume mount of the root file system is also a system change.
        QVERIFY(!planFstabWrite(baseFile, f, standard, false).ok());
        QVERIFY(isSystemEntry({QStringLiteral("UUID=root"), QStringLiteral("/data"), QStringLiteral("btrfs"), {}, 0, 0}, {QStringLiteral("UUID=root")}));
    }

    void movingAnEntryOntoASystemFolderIsCaught()
    {
        QByteArray moved = baseFile;
        moved.replace("/mnt/media", "/usr/share");
        QVERIFY(!planFstabWrite(baseFile, moved, standard, false).ok());
        QVERIFY(!planFstabWrite(baseFile, moved, driveadmin, false).ok());
    }

    void overrideZoneNeedsDriveAdmin()
    {
        const QByteArray f = withLine("nas:/cfg /etc/someapp nfs4 nofail 0 0");
        QVERIFY(!planFstabWrite(baseFile, f, standard, false).ok());
        const Plan p = planFstabWrite(baseFile, f, driveadmin, false);
        QVERIFY(p.ok());
        QCOMPARE(p.overrideMountPoints, QStringList{QStringLiteral("/etc/someapp")});
    }

    void blockedZoneNeedsSudoAndConfirmation()
    {
        const QByteArray f = withLine("nas:/keys /etc/ssh/keys nfs4 nofail 0 0");
        QVERIFY(!planFstabWrite(baseFile, f, driveadmin, true).ok());
        QVERIFY(!planFstabWrite(baseFile, f, sudoer, false).ok());
        QVERIFY(planFstabWrite(baseFile, f, sudoer, true).ok());
    }

    void newBrokenLinesAreRefusedOldOnesKept()
    {
        QVERIFY(!planFstabWrite(baseFile, withLine("only two"), driveadmin, false).ok());
        const QByteArray withBroken = withLine("only two");
        QVERIFY(planFstabWrite(withBroken, QByteArray(withBroken + "LABEL=x /mnt/x ext4 defaults 0 0\n"), standard, false).ok());
    }

    void duplicateNewMountPointRefused()
    {
        QVERIFY(!planFstabWrite(baseFile, withLine("a /mnt/x ext4 defaults 0 0\nb /mnt/x ext4 defaults 0 0"), driveadmin, false).ok());
    }

    void unsafeTextRefusedEvenForSudo()
    {
        // Validation errors (here: a negative pass) apply to everybody.
        QVERIFY(!planFstabWrite(baseFile, withLine("a /mnt/x ext4 defaults 0 -1"), sudoer, true).ok());
    }

    // ---- shared mounts ----
    void sharedMountForcesSafeOptions()
    {
        const auto p = planSharedMount(share(QStringLiteral("//nas.local/Games"), QStringLiteral("/mnt/games"), QStringLiteral("cifs"),
                                             {QStringLiteral("vers=3.1.1"), QStringLiteral("uid=1000"), QStringLiteral("guest")}),
                                       netmount, baseFile, QStringLiteral("/etc/anchorage/credentials"));
        QVERIFY2(p.ok(), qPrintable(p.errors.join(QLatin1Char('\n'))));
        for (const auto *o : {"nosuid", "nodev", "nofail", "_netdev", "vers=3.1.1", "uid=1000", "guest"}) {
            QVERIFY2(p.entry.options.contains(QLatin1String(o)), o);
        }
    }

    void sharedMountRefusals_data()
    {
        QTest::addColumn<Entry>("entry");
        QTest::addColumn<bool>("asStandard");
        const auto smb = [](const QString &mp, const QStringList &opts) {
            return share(QStringLiteral("//nas/Games"), mp, QStringLiteral("cifs"), opts);
        };
        QTest::newRow("standard user") << smb(QStringLiteral("/mnt/g"), {}) << true;
        QTest::newRow("not network") << share(QStringLiteral("/dev/sdb1"), QStringLiteral("/mnt/g"), QStringLiteral("ext4"), {}) << false;
        QTest::newRow("outside /mnt") << smb(QStringLiteral("/srv/g"), {}) << false;
        QTest::newRow("nested") << smb(QStringLiteral("/mnt/a/b"), {}) << false;
        QTest::newRow("dotdot") << smb(QStringLiteral("/mnt/.."), {}) << false;
        QTest::newRow("existing") << share(QStringLiteral("nas:/x"), QStringLiteral("/mnt/media"), QStringLiteral("nfs4"), {}) << false;
        QTest::newRow("suid") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("suid")}) << false;
        QTest::newRow("dev") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("dev")}) << false;
        QTest::newRow("exec") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("exec")}) << false;
        QTest::newRow("unknown option") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("x-systemd.requires=evil.service")}) << false;
        QTest::newRow("uid of root") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("uid=0")}) << false;
        QTest::newRow("someone's credentials") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("credentials=/etc/anchorage/credentials/nas-backups")}) << false;
        QTest::newRow("system file as credentials") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("credentials=/etc/shadow")}) << false;
        QTest::newRow("credentials traversal") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("credentials=/etc/anchorage/credentials/oarko-../../shadow")}) << false;
        QTest::newRow("bad smb spec") << share(QStringLiteral("nas/Games"), QStringLiteral("/mnt/g"), QStringLiteral("cifs"), {}) << false;
        QTest::newRow("bad nfs spec") << share(QStringLiteral("//nas/x"), QStringLiteral("/mnt/g"), QStringLiteral("nfs4"), {}) << false;
        QTest::newRow("odd value") << smb(QStringLiteral("/mnt/g"), {QStringLiteral("vers=3;rm")}) << false;
    }
    void sharedMountRefusals()
    {
        QFETCH(Entry, entry);
        QFETCH(bool, asStandard);
        const auto p = planSharedMount(entry, asStandard ? standard : netmount, baseFile, QStringLiteral("/etc/anchorage/credentials"));
        QVERIFY(!p.ok());
    }

    void ownCredentialsAllowed()
    {
        const auto p = planSharedMount(share(QStringLiteral("//nas/Games"), QStringLiteral("/mnt/g"), QStringLiteral("cifs"),
                                             {QStringLiteral("credentials=/etc/anchorage/credentials/oarko-nas")}),
                                       netmount, baseFile, QStringLiteral("/etc/anchorage/credentials"));
        QVERIFY2(p.ok(), qPrintable(p.errors.join(QLatin1Char('\n'))));
    }

    void sharedRemoval()
    {
        const QByteArray f = withLine("//nas/G /mnt/g cifs guest,nosuid,nodev,nofail,_netdev 0 0");
        QVERIFY(checkSharedMountRemoval(f, QStringLiteral("/mnt/g"), netmount).isEmpty());
        QVERIFY(!checkSharedMountRemoval(f, QStringLiteral("/mnt/g"), standard).isEmpty());
        // /mnt/media has no nosuid/nodev: it isn't a shared mount, so netmount can't remove it
        QVERIFY(!checkSharedMountRemoval(f, QStringLiteral("/mnt/media"), netmount).isEmpty());
        QVERIFY(!checkSharedMountRemoval(f, QStringLiteral("/home"), netmount).isEmpty());
        QVERIFY(!checkSharedMountRemoval(f, QStringLiteral("/mnt/none"), netmount).isEmpty());
    }
};

QTEST_GUILESS_MAIN(PolicyTest)
#include "policytest.moc"
