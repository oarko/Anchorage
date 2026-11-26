// SPDX-License-Identifier: GPL-3.0-or-later
// Mount-point suggestions (src/core/locations.cpp), bind entries (src/core/binds.cpp) and
// the bind-source rule in policy::planFstabWrite.
#include "binds.h"
#include "locations.h"
#include "mountpolicy.h"

#include <QTest>

using namespace anchorage;
using binds::Bind;

namespace
{
Caller makeCaller(bool full)
{
    Caller c;
    c.uid = 1000;
    c.name = QStringLiteral("oarko");
    c.home = QStringLiteral("/home/oarko");
    c.driveAdmin = full;
    return c;
}

QByteArray lines(const fstab::File &f)
{
    return f.serialize();
}
} // namespace

class BindTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void personalBaseIsDetected()
    {
        const auto none = [](const QString &) { return false; };
        const auto onlyMedia = [](const QString &p) { return p == QLatin1String("/media/oarko"); };
        const auto both = [](const QString &) { return true; };
        const QList<mounts::Mount> runMedia = {{QStringLiteral("/dev/sdb1"), QStringLiteral("/run/media/oarko/Lingames"), QStringLiteral("btrfs"), {}, {}}};
        const QList<mounts::Mount> media = {{QStringLiteral("/dev/sdb1"), QStringLiteral("/media/oarko/Stick"), QStringLiteral("vfat"), {}, {}}};
        // What is mounted now wins (this machine has both folders).
        QCOMPARE(locations::personalBase(QStringLiteral("oarko"), runMedia, both), QStringLiteral("/run/media/oarko"));
        QCOMPARE(locations::personalBase(QStringLiteral("oarko"), media, both), QStringLiteral("/media/oarko"));
        // Then which folder exists, then UDisks' default.
        QCOMPARE(locations::personalBase(QStringLiteral("oarko"), {}, onlyMedia), QStringLiteral("/media/oarko"));
        QCOMPARE(locations::personalBase(QStringLiteral("oarko"), {}, none), QStringLiteral("/run/media/oarko"));
    }

    void suggestionsStartWhereTheDriveIs()
    {
        drives::Volume v;
        v.name = QStringLiteral("sdb1");
        v.path = QStringLiteral("/dev/sdb1");
        v.label = QStringLiteral("Lingames");
        v.fsType = QStringLiteral("btrfs");
        v.mountPoints = {QStringLiteral("/run/media/oarko/Lingames")};
        const auto exists = [](const QString &p) { return p == QLatin1String("/run/media/oarko"); };
        QCOMPARE(locations::suggestions(v, QStringLiteral("oarko"), {}, exists),
                 QStringList({QStringLiteral("/run/media/oarko/Lingames"), QStringLiteral("/mnt/Lingames"), QStringLiteral("/media/Lingames")}));
        v.mountPoints.clear();
        v.label.clear();
        QCOMPARE(locations::suggestions(v, QStringLiteral("oarko"), {}, exists).first(), QStringLiteral("/mnt/sdb1"));
        // System locations are never suggested as "where it is now".
        v.mountPoints = {QStringLiteral("/home")};
        QVERIFY(!locations::suggestions(v, QStringLiteral("oarko"), {}, exists).contains(QStringLiteral("/home")));
        QCOMPARE(locations::folderName(QStringLiteral("My Games!")), QStringLiteral("My-Games-"));
        QCOMPARE(locations::folderName(QStringLiteral("..")), QStringLiteral("drive"));
    }

    void bindEntriesFollowTheDrive()
    {
        const fstab::Entry drive{QStringLiteral("UUID=1"), QStringLiteral("/mnt/Lingames"), QStringLiteral("btrfs"), {QStringLiteral("nofail")}, 0, 0};
        auto e = binds::makeEntry(drive, {QString(), QStringLiteral("/run/media/oarko/Lingames")});
        QCOMPARE(e.spec, QStringLiteral("/mnt/Lingames"));
        QCOMPARE(e.type, QStringLiteral("none"));
        QCOMPARE(e.options, QStringList({QStringLiteral("bind"), QStringLiteral("nofail"), QStringLiteral("x-systemd.requires-mounts-for=/mnt/Lingames")}));
        fstab::Entry automount = drive;
        automount.options = {QStringLiteral("nofail"), QStringLiteral("noauto"), QStringLiteral("x-systemd.automount")};
        e = binds::makeEntry(automount, {QStringLiteral("SteamLibrary"), QStringLiteral("/home/oarko/Games")});
        QCOMPARE(e.spec, QStringLiteral("/mnt/Lingames/SteamLibrary"));
        QVERIFY(e.options.contains(QStringLiteral("x-systemd.automount")) && e.options.contains(QStringLiteral("noauto")));

        QVERIFY(binds::checkSubfolder(QString()).isEmpty());
        QVERIFY(binds::checkSubfolder(QStringLiteral("Games/Steam")).isEmpty());
        for (const char *bad : {"/abs", "a/../b", "a//b", "a/", "."}) {
            QVERIFY2(!binds::checkSubfolder(QString::fromLatin1(bad)).isEmpty(), bad);
        }
    }

    void applyChangesBindsInPlace()
    {
        fstab::File f = fstab::File::parse("# comment\n"
                                           "UUID=1  /run/media/oarko/Lingames  btrfs  nofail  0 0\n"
                                           "/run/media/oarko/Lingames/Old  /srv/old  none  bind,nofail  0 0\n"
                                           "tmpfs  /tmp  tmpfs  defaults  0 0\n");
        QCOMPARE(binds::findFor(f, QStringLiteral("/run/media/oarko/Lingames")).size(), 1);
        // The drive moves to /mnt/Lingames; its old place is kept as a bind, the old bind follows.
        const fstab::Entry drive{QStringLiteral("UUID=1"), QStringLiteral("/mnt/Lingames"), QStringLiteral("btrfs"), {QStringLiteral("nofail")}, 0, 0};
        QVERIFY(f.replace(1, drive));
        QString error;
        QVERIFY2(binds::apply(f, QStringLiteral("/run/media/oarko/Lingames"), drive,
                              {{QStringLiteral("Old"), QStringLiteral("/srv/old")}, {QString(), QStringLiteral("/run/media/oarko/Lingames")}}, &error),
                 qPrintable(error));
        const QByteArray out = lines(f);
        // The existing bind keeps its line and columns; the new one is added at the end.
        QVERIFY2(out.contains("\n/mnt/Lingames/Old              /srv/old  none  bind,nofail,x-systemd.requires-mounts-for=/mnt/Lingames 0 0\ntmpfs"),
                 out.constData());
        QVERIFY(out.endsWith("/mnt/Lingames /run/media/oarko/Lingames none bind,nofail,x-systemd.requires-mounts-for=/mnt/Lingames 0 0\n"));
        QVERIFY(out.startsWith("# comment\n")); // the rest of the file is untouched
        QVERIFY(out.contains("tmpfs  /tmp  tmpfs  defaults  0 0\n"));
        QCOMPARE(binds::findFor(f, QStringLiteral("/mnt/Lingames")).size(), 2);
        // Removing all binds removes their lines only.
        QVERIFY(binds::apply(f, QStringLiteral("/mnt/Lingames"), drive, {}, &error));
        QCOMPARE(binds::findFor(f, QStringLiteral("/mnt/Lingames")).size(), 0);
        QCOMPARE(f.entryLines().size(), 2u);
        // A bad target refuses everything.
        const QByteArray before = lines(f);
        QVERIFY(!binds::apply(f, QStringLiteral("/mnt/Lingames"), drive, {{QString(), QStringLiteral("relative")}}, &error));
        QCOMPARE(lines(f), before);
    }

    void bindSourcesAreChecked_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<bool>("standardOk");
        QTest::newRow("drive in /mnt") << QByteArray("/mnt/Lingames /run/media/oarko/Lingames none bind,nofail 0 0") << true;
        QTest::newRow("personal folder") << QByteArray("/home/oarko/Games /mnt/games none bind,nofail 0 0") << true;
        QTest::newRow("inside an entry's drive") << QByteArray("/opt/data/photos /mnt/photos none bind,nofail 0 0") << true;
        QTest::newRow("system folder") << QByteArray("/etc /mnt/etc none bind,nofail 0 0") << false;
        QTest::newRow("root's home") << QByteArray("/root /mnt/r none rbind 0 0") << false;
        QTest::newRow("the whole system") << QByteArray("/ /mnt/all none bind 0 0") << false;
        QTest::newRow("bind option on another type") << QByteArray("/usr /mnt/u ext4 bind 0 0") << false;
    }
    void bindSourcesAreChecked()
    {
        QFETCH(QByteArray, line);
        QFETCH(bool, standardOk);
        const QByteArray current = "UUID=r / btrfs defaults 0 0\nLABEL=data /opt/data ext4 nofail 0 0\n";
        const QByteArray proposed = current + line + "\n";
        const auto standard = policy::planFstabWrite(current, proposed, makeCaller(false), false);
        QVERIFY2(standard.ok() == standardOk, qPrintable(standard.errors.join(QLatin1Char(' '))));
        // driveadmins and administrators may show any folder elsewhere.
        const auto full = policy::planFstabWrite(current, proposed, makeCaller(true), false);
        QVERIFY2(full.ok(), qPrintable(full.errors.join(QLatin1Char(' '))));
    }
};

QTEST_GUILESS_MAIN(BindTest)
#include "bindtest.moc"
