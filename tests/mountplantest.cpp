// SPDX-License-Identifier: GPL-3.0-or-later
// Mount and unmount rules (src/core/mountplan.cpp) and the commands the service runs for
// them (src/core/fstabservice.cpp, with a fake command runner). Real mounts are tested
// in tests/mounttest.cpp (Docker) and tests/vm/editor_scenarios.py (VM).
#include "fstabservice.h"
#include "mountplan.h"
#include "requests.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace anchorage;
using mounting::Method;

namespace
{
Caller makeCaller(bool sudo, bool driveAdmin)
{
    Caller c;
    c.uid = 1000;
    c.gid = 1000;
    c.name = QStringLiteral("anna");
    c.home = QStringLiteral("/home/anna");
    c.sudo = sudo;
    c.driveAdmin = driveAdmin;
    return c;
}
const Caller standard = makeCaller(false, false);
const Caller driveadmin = makeCaller(false, true);
const Caller sudoer = makeCaller(true, false);

const QByteArray fstabData = "UUID=r / btrfs defaults 0 0\n"
                             "UUID=h /home btrfs subvol=@home 0 0\n"
                             "LABEL=data /mnt/data ext4 nofail 0 2\n"
                             "LABEL=auto /mnt/auto ext4 nofail,noauto,x-systemd.automount 0 0\n"
                             "/swapfile none swap sw 0 0\n";

mounts::Mount mnt(const char *source, const char *target, const char *type)
{
    return {QString::fromLatin1(source), QString::fromLatin1(target), QString::fromLatin1(type), QStringLiteral("/"), QStringLiteral("rw")};
}

const QList<mounts::Mount> current = {
    mnt("/dev/nvme0n1p2", "/", "btrfs"),       mnt("/dev/nvme0n1p2", "/home", "btrfs"), mnt("proc", "/proc", "proc"),
    mnt("systemd-1", "/mnt/auto", "autofs"),   mnt("/dev/sdb1", "/run/media/anna/Stick", "vfat"), mnt("//nas/x", "/srv/x", "cifs"),
    mnt("/dev/fuse", "/mnt/win", "fuseblk"),   mnt("tmpfs", "/run/user/1000", "tmpfs"),
};
} // namespace

class MountPlanTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void unitNamesMatchSystemd_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("unit");
        QTest::newRow("root") << "/" << "-";
        QTest::newRow("simple") << "/mnt/data" << "mnt-data";
        QTest::newRow("dash") << "/mnt/e2e-boot" << "mnt-e2e\\x2dboot";
        QTest::newRow("space") << "/mnt/my disk" << "mnt-my\\x20disk";
        QTest::newRow("dot in a folder") << "/mnt/.hidden" << "mnt-.hidden";
        QTest::newRow("dot at the start") << "/.hidden" << "\\x2ehidden";
        QTest::newRow("dot inside") << "/mnt/a.b" << "mnt-a.b";
        QTest::newRow("utf8") << "/mnt/Ü" << "mnt-\\xc3\\x9c";
    }
    void unitNamesMatchSystemd()
    {
        QFETCH(QString, path);
        QFETCH(QString, unit);
        QCOMPARE(mounting::unitName(path), unit);
        if (QFile::exists(QStringLiteral("/usr/bin/systemd-escape"))) {
            QProcess p;
            p.start(QStringLiteral("/usr/bin/systemd-escape"), {QStringLiteral("--path"), path});
            QVERIFY(p.waitForFinished());
            QCOMPARE(QString::fromUtf8(p.readAllStandardOutput()).trimmed(), unit);
        }
    }

    void mountEntryRules()
    {
        auto p = mounting::planMountEntry(fstabData, QStringLiteral("/mnt/data"), standard, current);
        QVERIFY(p.ok());
        QCOMPARE(p.method, Method::MountCommand);
        p = mounting::planMountEntry(fstabData, QStringLiteral("/mnt/auto"), standard, current); // autofs placeholder isn't "mounted"
        QVERIFY(p.ok());
        QCOMPARE(p.method, Method::Systemd);
        QVERIFY(!mounting::planMountEntry(fstabData, QStringLiteral("/mnt/nothere"), sudoer, current).ok());
        QVERIFY(!mounting::planMountEntry(fstabData, QStringLiteral("none"), sudoer, current).ok()); // swap
        QVERIFY(!mounting::planMountEntry(fstabData, QStringLiteral("/home"), standard, current).ok()); // system
        QVERIFY(!mounting::planMountEntry(fstabData, QStringLiteral("/home"), driveadmin, current).ok()); // already mounted
        QVERIFY(!mounting::planMountEntry(fstabData, QStringLiteral("/mnt/../etc"), sudoer, current).ok());

        // A bind waits for its drive (mount itself ignores requires-mounts-for).
        const QByteArray withBind = fstabData + "/mnt/data /srv/data none bind,nofail,x-systemd.requires-mounts-for=/mnt/data 0 0\n";
        p = mounting::planMountEntry(withBind, QStringLiteral("/srv/data"), standard, current);
        QVERIFY(!p.ok());
        QVERIFY(p.errors.join(QString()).contains(QLatin1String("Mount /mnt/data first")));
        QList<mounts::Mount> driveMounted = current;
        driveMounted.append(mnt("/dev/sdc1", "/mnt/data", "ext4"));
        QVERIFY(mounting::planMountEntry(withBind, QStringLiteral("/srv/data"), standard, driveMounted).ok());
        const QByteArray hardDep = fstabData + "/mnt/data /srv/hard none bind,nofail,x-systemd.requires=/mnt/data 0 0\n";
        QVERIFY(!mounting::planMountEntry(hardDep, QStringLiteral("/srv/hard"), standard, current).ok());
        QVERIFY(mounting::planMountEntry(hardDep, QStringLiteral("/srv/hard"), standard, driveMounted).ok());
    }

    void temporaryMountRules_data()
    {
        QTest::addColumn<QString>("spec");
        QTest::addColumn<QString>("mountPoint");
        QTest::addColumn<QString>("type");
        QTest::addColumn<QStringList>("opts");
        QTest::addColumn<bool>("sudo");
        QTest::addColumn<bool>("confirm");
        QTest::addColumn<bool>("ok");
        const QStringList none;
        QTest::newRow("uuid in /mnt") << "UUID=1234-ABCD" << "/mnt/stick" << "vfat" << QStringList{QStringLiteral("uid=1000")} << false << false << true;
        QTest::newRow("device path") << "/dev/sdb1" << "/srv/games" << "ext4" << none << false << false << true;
        QTest::newRow("smb share") << "//nas.local/Photos" << "/mnt/photos" << "cifs" << QStringList{QStringLiteral("guest")} << false << false << true;
        QTest::newRow("nfs share") << "nas:/export" << "/mnt/export" << "nfs4" << none << false << false << true;
        QTest::newRow("override zone") << "LABEL=x" << "/opt/x" << "ext4" << none << false << false << true;
        QTest::newRow("protected, no confirm") << "LABEL=x" << "/etc/ssh/x" << "ext4" << none << true << false << false;
        QTest::newRow("protected, confirmed") << "LABEL=x" << "/etc/ssh/x" << "ext4" << none << true << true << true;
        QTest::newRow("protected, driveadmin") << "LABEL=x" << "/etc/ssh/x" << "ext4" << none << false << true << false;
        QTest::newRow("fstab mount point") << "LABEL=y" << "/mnt/data" << "ext4" << none << false << false << false;
        QTest::newRow("already mounted") << "LABEL=y" << "/srv/x" << "ext4" << none << false << false << false;
        QTest::newRow("bind") << "/dev/sdb1" << "/mnt/b" << "ext4" << QStringList{QStringLiteral("bind")} << false << false << false;
        QTest::newRow("helper") << "/dev/sdb1" << "/mnt/b" << "ext4" << QStringList{QStringLiteral("helper=x")} << false << false << false;
        QTest::newRow("X-mount") << "/dev/sdb1" << "/mnt/b" << "ext4" << QStringList{QStringLiteral("X-mount.mkdir")} << false << false << false;
        QTest::newRow("file as source") << "/home/anna/disk.img" << "/mnt/b" << "ext4" << none << false << false << false;
        QTest::newRow("option-looking source") << "-oremount" << "/mnt/b" << "ext4" << none << false << false << false;
        QTest::newRow("share with option") << "-x:/e" << "/mnt/b" << "nfs4" << none << false << false << false;
        QTest::newRow("swap") << "/dev/sdb2" << "/mnt/b" << "swap" << none << false << false << false;
        QTest::newRow("newline") << "LABEL=a\nb" << "/mnt/b" << "ext4" << none << false << false << false;
        QTest::newRow("relative") << "LABEL=a" << "mnt/b" << "ext4" << none << false << false << false;
    }
    void temporaryMountRules()
    {
        QFETCH(QString, spec);
        QFETCH(QString, mountPoint);
        QFETCH(QString, type);
        QFETCH(QStringList, opts);
        QFETCH(bool, sudo);
        QFETCH(bool, confirm);
        QFETCH(bool, ok);
        const fstab::Entry e{spec, mountPoint, type, opts, 0, 0};
        const auto p = mounting::planTemporaryMount(e, sudo ? sudoer : driveadmin, confirm, fstabData, current);
        QVERIFY2(p.ok() == ok, qPrintable(p.errors.join(QLatin1Char(' '))));
        // Never for standard users.
        QVERIFY(!mounting::planTemporaryMount(e, standard, confirm, fstabData, current).ok());
    }

    void unmountRules()
    {
        // fstab entry, not system: anyone who passed polkit.
        QList<mounts::Mount> now = current;
        now.append(mnt("/dev/sdc1", "/mnt/data", "ext4"));
        auto p = mounting::planUnmount(fstabData, now, QStringLiteral("/mnt/data"), standard, false);
        QVERIFY(p.ok());
        QVERIFY(!p.system);
        // System entries: full access and a typed confirmation.
        QVERIFY(!mounting::planUnmount(fstabData, now, QStringLiteral("/home"), standard, true).ok());
        QVERIFY(!mounting::planUnmount(fstabData, now, QStringLiteral("/home"), driveadmin, false).ok());
        p = mounting::planUnmount(fstabData, now, QStringLiteral("/home"), driveadmin, true);
        QVERIFY(p.ok() && p.system);
        QVERIFY(mounting::planUnmount(fstabData, now, QStringLiteral("/"), sudoer, true).ok());
        // Not in fstab: full access only.
        QVERIFY(!mounting::planUnmount(fstabData, now, QStringLiteral("/run/media/anna/Stick"), standard, false).ok());
        QVERIFY(mounting::planUnmount(fstabData, now, QStringLiteral("/run/media/anna/Stick"), driveadmin, false).ok());
        QVERIFY(mounting::planUnmount(fstabData, now, QStringLiteral("/srv/x"), driveadmin, false).ok()); // network share
        QVERIFY(mounting::planUnmount(fstabData, now, QStringLiteral("/mnt/win"), driveadmin, false).ok()); // FUSE drive
        // Pseudo file systems: never.
        QVERIFY(!mounting::planUnmount(fstabData, now, QStringLiteral("/proc"), sudoer, true).ok());
        QVERIFY(!mounting::planUnmount(fstabData, now, QStringLiteral("/run/user/1000"), sudoer, true).ok());
        // Nothing there (an automount placeholder doesn't count).
        QVERIFY(!mounting::planUnmount(fstabData, now, QStringLiteral("/mnt/auto"), sudoer, false).ok());
        now.append(mnt("/dev/sdd1", "/mnt/auto", "ext4"));
        p = mounting::planUnmount(fstabData, now, QStringLiteral("/mnt/auto"), standard, false);
        QVERIFY(p.ok());
        QCOMPARE(p.method, Method::Systemd);
    }

    // ---- the service, with a fake command runner ---------------------------------------

    void serviceRunsTheRightCommands()
    {
        QTemporaryDir tmp;
        service::Paths paths;
        paths.fstabDir = tmp.path() + QStringLiteral("/etc");
        paths.fstabName = QStringLiteral("fstab");
        paths.configDir = tmp.path() + QStringLiteral("/etc/anchorage");
        paths.backupDir = paths.configDir + QStringLiteral("/backups");
        paths.mountRoot = tmp.path() + QStringLiteral("/root");
        paths.mountInfo = tmp.path() + QStringLiteral("/mountinfo");
        paths.procRoot = tmp.path() + QStringLiteral("/proc"); // empty: nobody uses anything
        QVERIFY(QDir().mkpath(paths.fstabDir) && QDir().mkpath(paths.mountRoot + QStringLiteral("/mnt")) && QDir().mkpath(paths.procRoot));
        const auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        write(paths.fstabDir + QStringLiteral("/fstab"), fstabData);
        const QByteArray mountinfoBase = "1 0 8:1 / / rw - btrfs /dev/nvme0n1p2 rw\n"
                                         "2 1 0:5 / /mnt/auto rw - autofs systemd-1 rw\n";
        write(paths.mountInfo, mountinfoBase);

        QList<QStringList> calls;
        service::CommandResult next{0, QString()};
        bool reallyMount = true; // a successful mount shows up in the mount table
        const service::Runner fake = [&](const QString &program, const QStringList &args) {
            calls.append(QStringList{program} + args);
            if (next.exitCode == 0 && reallyMount && (program.endsWith(QLatin1String("/mount")) || args.value(0) == QLatin1String("start"))) {
                const QString target = program.endsWith(QLatin1String("/mount")) ? args.last() : QStringLiteral("/mnt/auto");
                QFile f(paths.mountInfo);
                if (f.open(QIODevice::Append)) {
                    f.write(QStringLiteral("9 1 8:33 / %1 rw - ext4 /dev/sdz1 rw\n").arg(target).toUtf8());
                }
            }
            return next;
        };

        // fstab entry: mount --fstab <the fstab> <mount point>; folder created.
        auto o = service::mountEntry(paths, standard, QStringLiteral("/mnt/data"), fake);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char(' '))));
        QCOMPARE(calls.last(), QStringList({QStringLiteral("/usr/bin/mount"), QStringLiteral("--fstab"), paths.fstabDir + QStringLiteral("/fstab"),
                                            QStringLiteral("/mnt/data")}));
        QVERIFY(QFileInfo(paths.mountRoot + QStringLiteral("/mnt/data")).isDir());

        // mount says "done" but nothing is mounted (nofail and a missing device): an error.
        write(paths.mountInfo, mountinfoBase);
        reallyMount = false;
        o = service::mountEntry(paths, standard, QStringLiteral("/mnt/data"), fake);
        QVERIFY(!o.ok());
        QVERIFY2(o.errors.join(QString()).contains(QLatin1String("isn't connected")), qPrintable(o.errors.join(QLatin1Char(' '))));
        reallyMount = true;

        // Lists (a drive and its binds): in order, stopping at the first failure, which is named.
        write(paths.mountInfo, mountinfoBase);
        const int beforeList = int(calls.size());
        o = service::mountEntries(paths, standard, {QStringLiteral("/mnt/data"), QStringLiteral("/mnt/not-in-fstab"), QStringLiteral("/mnt/auto")}, fake);
        QVERIFY(!o.ok());
        QCOMPARE(o.done, QStringList{QStringLiteral("/mnt/data")});
        QCOMPARE(o.failed, QStringLiteral("/mnt/not-in-fstab"));
        QCOMPARE(int(calls.size()), beforeList + 1); // /mnt/auto was never tried
        auto u = service::unmountAll(paths, standard, {QStringLiteral("/mnt/not-mounted"), QStringLiteral("/mnt/data")}, false, fake);
        QVERIFY(!u.ok());
        QVERIFY(u.done.isEmpty());
        QCOMPARE(u.failed, QStringLiteral("/mnt/not-mounted"));
        QFile info(paths.mountInfo);
        QVERIFY(info.open(QIODevice::ReadOnly));
        QVERIFY(mounting::mountedAt(mounts::parseMountInfo(info.readAll()), QStringLiteral("/mnt/data"))); // left alone
        write(paths.mountInfo, mountinfoBase);

        // Automount entry: systemctl; on failure the journal's reason is shown.
        next = {1, QStringLiteral("Job failed")};
        o = service::mountEntry(paths, standard, QStringLiteral("/mnt/auto"), fake);
        QVERIFY(!o.ok());
        QCOMPARE(calls.at(calls.size() - 2), QStringList({QStringLiteral("/usr/bin/systemctl"), QStringLiteral("start"), QStringLiteral("mnt-auto.mount")}));
        QCOMPARE(calls.last().first(), QStringLiteral("/usr/bin/journalctl"));
        QVERIFY(calls.last().contains(QStringLiteral("mnt-auto.mount")));

        // Temporary mount: mount -t <type> -o <options> <device> <folder>.
        next = {0, QString()};
        write(paths.mountInfo, mountinfoBase);
        const fstab::Entry t{QStringLiteral("UUID=1234-ABCD"), QStringLiteral("/mnt/stick"), QStringLiteral("vfat"),
                             {QStringLiteral("uid=1000"), QStringLiteral("umask=077")}, 0, 0};
        o = service::mountTemporary(paths, driveadmin, t, false, fake);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char(' '))));
        QCOMPARE(calls.last(), QStringList({QStringLiteral("/usr/bin/mount"), QStringLiteral("-t"), QStringLiteral("vfat"), QStringLiteral("-o"),
                                            QStringLiteral("uid=1000,umask=077"), QStringLiteral("UUID=1234-ABCD"), QStringLiteral("/mnt/stick")}));
        const int before = int(calls.size());
        QVERIFY(!service::mountTemporary(paths, standard, t, false, fake).ok());
        QCOMPARE(calls.size(), before); // refused before running anything

        // Override zone: the folder must be empty.
        QVERIFY(QDir().mkpath(paths.mountRoot + QStringLiteral("/opt/full")));
        write(paths.mountRoot + QStringLiteral("/opt/full/file"), "x");
        fstab::Entry full = t;
        full.mountPoint = QStringLiteral("/opt/full");
        QVERIFY(!service::mountTemporary(paths, driveadmin, full, false, fake).ok());
        QCOMPARE(calls.size(), before);

        // Unmount: umount <folder>; "still mounted" and busy reported.
        write(paths.mountInfo, mountinfoBase + "3 1 8:17 / /mnt/data rw - ext4 /dev/sdc1 rw\n");
        o = service::unmount(paths, standard, QStringLiteral("/mnt/data"), false, fake);
        QVERIFY(!o.ok()); // the fake doesn't really unmount
        QCOMPARE(calls.last(), QStringList({QStringLiteral("/usr/bin/umount"), QStringLiteral("/mnt/data")}));
        QVERIFY(o.errors.join(QString()).contains(QLatin1String("still mounted")));
        next = {32, QStringLiteral("umount: /mnt/data: target is busy.")};
        o = service::unmount(paths, standard, QStringLiteral("/mnt/data"), false, fake);
        QVERIFY(o.errors.join(QString()).contains(QLatin1String("in use")));
        write(paths.mountInfo, mountinfoBase);
        next = {0, QString()};
        QVERIFY(!service::unmount(paths, standard, QStringLiteral("/mnt/data"), false, fake).ok()); // nothing mounted
    }

    void findsProgramsUsingAFolder()
    {
        // This test's own working directory is on the file system holding the temp folder.
        QTemporaryDir tmp;
        const QString old = QDir::currentPath();
        QVERIFY(QDir::setCurrent(tmp.path()));
        const QStringList names = service::processesUsing(tmp.path(), QStringLiteral("/proc"));
        QDir::setCurrent(old);
        QVERIFY2(names.contains(QStringLiteral("mountplantest")), qPrintable(names.join(QLatin1Char(','))));
        QVERIFY(service::processesUsing(QStringLiteral("/does/not/exist"), QStringLiteral("/proc")).isEmpty());
    }

    void requestsRoundTrip()
    {
        const requests::TemporaryMount t{{QStringLiteral("UUID=1"), QStringLiteral("/mnt/x"), QStringLiteral("ext4"), {QStringLiteral("ro")}, 0, 0}, true};
        const auto back = requests::decodeTemporaryMount(requests::encodeTemporaryMount(t));
        QVERIFY(back.ok());
        QCOMPARE(back->entry.spec, t.entry.spec);
        QCOMPARE(back->entry.options, t.entry.options);
        QVERIFY(back->confirmProtected);
        QVariantMap extra = requests::encodeTemporaryMount(t);
        extra.insert(QStringLiteral("program"), QStringLiteral("/bin/sh"));
        QVERIFY(!requests::decodeTemporaryMount(extra).ok());
        QVariantMap wrong = requests::encodeTemporaryMount(t);
        wrong[QStringLiteral("options")] = QStringLiteral("ro");
        QVERIFY(!requests::decodeTemporaryMount(wrong).ok());

        const requests::Unmount u{{QStringLiteral("/mnt/x/b"), QStringLiteral("/mnt/x")}, true};
        QCOMPARE(requests::decodeUnmount(requests::encodeUnmount(u))->confirmSystem, true);
        QCOMPARE(requests::decodeUnmount(requests::encodeUnmount(u))->mountPoints, u.mountPoints);
        // Lists: 1 to kMaxMountPoints, no repeats, strings only.
        QCOMPARE(*requests::decodeMountPoints(requests::encodeMountPoints({QStringLiteral("/a"), QStringLiteral("/b")})),
                 QStringList({QStringLiteral("/a"), QStringLiteral("/b")}));
        QVERIFY(!requests::decodeMountPoints(requests::encodeMountPoints({})).ok());
        QVERIFY(!requests::decodeMountPoints(requests::encodeMountPoints({QStringLiteral("/a"), QStringLiteral("/a")})).ok());
        QStringList many;
        for (int i = 0; i <= requests::kMaxMountPoints; ++i) {
            many.append(QStringLiteral("/mnt/%1").arg(i));
        }
        QVERIFY(!requests::decodeMountPoints(requests::encodeMountPoints(many)).ok());
        QVERIFY(!requests::decodeMountPoints({{QStringLiteral("mountPoints"), QStringLiteral("/a")}}).ok()); // a string, not a list
        QVERIFY(!requests::decodeMountPoints({{QStringLiteral("mountPoint"), QStringLiteral("/a")}}).ok()); // the old key
        QVERIFY(!requests::decodeUnmount({{QStringLiteral("mountPoints"), QVariantList{QStringLiteral("/a")}}, {QStringLiteral("confirmSystem"), false}}).ok());
        QVariantMap badU = requests::encodeUnmount(u);
        badU[QStringLiteral("confirmSystem")] = 1;
        QVERIFY(!requests::decodeUnmount(badU).ok());
    }
};

QTEST_GUILESS_MAIN(MountPlanTest)
#include "mountplantest.moc"
