// SPDX-License-Identifier: GPL-3.0-or-later
#include "drives.h"
#include "inventory.h"
#include "mounts.h"

#include <QFile>
#include <QTest>

using namespace anchorage;
using namespace anchorage::inventory;

namespace
{
QByteArray readData(const QString &name)
{
    QFile f(QStringLiteral(TEST_DATA_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("cannot open test data %s", qPrintable(name));
    }
    return f.readAll();
}

const Item *byMountPoint(const QList<Item> &items, const QString &mp)
{
    for (const Item &i : items) {
        if (i.mountPoint == mp) {
            return &i;
        }
    }
    return nullptr;
}

const Item *byTitle(const QList<Item> &items, const QString &title)
{
    for (const Item &i : items) {
        if (i.title == title) {
            return &i;
        }
    }
    return nullptr;
}
} // namespace

class InventoryTest : public QObject
{
    Q_OBJECT

    QList<drives::Volume> m_volumes;
    QList<mounts::Mount> m_mounts;
    QList<Item> m_items;

private Q_SLOTS:
    void initTestCase()
    {
        auto v = drives::parseLsblk(readData(QStringLiteral("lsblk-desktop.json")));
        QVERIFY2(v.ok(), qPrintable(v.error()));
        m_volumes = *v;
        m_mounts = mounts::parseMountInfo(readData(QStringLiteral("mountinfo-desktop.txt")));
        m_items = build(fstab::File::parse(readData(QStringLiteral("kubuntu-default.fstab"))), m_volumes, m_mounts);
    }

    // ---- lsblk ----
    void parsesDrives()
    {
        const auto lin = std::find_if(m_volumes.cbegin(), m_volumes.cend(), [](const auto &v) { return v.label == QLatin1String("Lingames"); });
        QVERIFY(lin != m_volumes.cend());
        QCOMPARE(lin->path, QStringLiteral("/dev/sdb1"));
        QCOMPARE(lin->fsType, QStringLiteral("btrfs"));
        QCOMPARE(lin->diskPath, QStringLiteral("/dev/sdb"));
        QCOMPARE(lin->diskModel, QStringLiteral("Samsung SSD 870 QVO 2TB"));
        QCOMPARE(lin->transport, QStringLiteral("sata"));
        QCOMPARE(lin->mountPoints, QStringList{QStringLiteral("/run/media/oarko/Lingames")});
        QVERIFY(lin->fsUsed > 0 && lin->fsSize > lin->fsUsed);

        const auto root = std::find_if(m_volumes.cbegin(), m_volumes.cend(), [](const auto &v) { return v.path == QLatin1String("/dev/nvme1n1p1"); });
        QCOMPARE(root->mountPoints.size(), 3); // /, /home, /swap subvolumes
    }

    void hidesWhatPeopleDontThinkOfAsDrives()
    {
        for (const auto &v : m_volumes) {
            if (v.fsType == QLatin1String("squashfs") || v.fsType.isEmpty()) { // snaps, whole disks, MSR
                QVERIFY2(!drives::isUserVisible(v), qPrintable(v.path));
            }
        }
        drives::Volume image;
        image.type = QStringLiteral("loop");
        image.fsType = QStringLiteral("ext4");
        QVERIFY(drives::isUserVisible(image)); // an attached disk image is a drive
    }

    void malformedJsonIsAnError()
    {
        QVERIFY(!drives::parseLsblk("{not json").ok());
        QVERIFY(drives::parseLsblk("{}")->isEmpty());
        QVERIFY(drives::parseLsblk(R"({"blockdevices":[{"name":"x","size":"12","rm":"1"}]})")->first().removable); // old lsblk: strings
    }

    // ---- mountinfo ----
    void parsesMounts()
    {
        const auto games = std::find_if(m_mounts.cbegin(), m_mounts.cend(), [](const auto &m) { return m.fsType == QLatin1String("cifs"); });
        QCOMPARE(games->target, QStringLiteral("/run/media/oarko/My Games"));
        QCOMPARE(games->source, QStringLiteral("//nas.local/My Games"));
        const auto home = std::find_if(m_mounts.cbegin(), m_mounts.cend(), [](const auto &m) { return m.target == QLatin1String("/home"); });
        QCOMPARE(home->root, QStringLiteral("/@home"));
        QVERIFY(mounts::parseMountInfo("garbage\n\n1 2 3\n").isEmpty());
    }

    // ---- inventory ----
    void fstabEntriesAreJoinedWithDrivesAndMounts()
    {
        const Item *root = byMountPoint(m_items, QStringLiteral("/"));
        QVERIFY(root);
        QCOMPARE(root->title, QStringLiteral("System"));
        QVERIFY(root->system);
        QVERIFY(root->mounted);
        QVERIFY(root->volume);
        QCOMPARE(root->volume->label, QStringLiteral("kubuntu_2510"));
        QVERIFY(root->used > 0);
        QCOMPARE(root->startup, Startup::AtBoot);

        const Item *efi = byMountPoint(m_items, QStringLiteral("/boot/efi"));
        QVERIFY(efi && efi->system && efi->mounted && efi->volume);

        const Item *swap = byTitle(m_items, QStringLiteral("Swap file"));
        QVERIFY(swap && swap->kind == Kind::Swap && swap->system);

        // /swap shares the root file system (btrfs subvolume): system, named after its folder.
        const Item *swapDir = byMountPoint(m_items, QStringLiteral("/swap"));
        QVERIFY(swapDir && swapDir->system);
        QCOMPARE(swapDir->title, QStringLiteral("swap"));

        const Item *tmp = byMountPoint(m_items, QStringLiteral("/tmp"));
        QVERIFY(tmp && tmp->mounted && !tmp->deviceMissing); // tmpfs isn't a device
    }

    void drivesNotInFstabAreShown()
    {
        const Item *lin = byTitle(m_items, QStringLiteral("Lingames"));
        QVERIFY(lin);
        QVERIFY(!lin->entry);
        QCOMPARE(lin->startup, Startup::NotInFstab);
        QVERIFY(lin->mounted);
        QCOMPARE(lin->mountPoint, QStringLiteral("/run/media/oarko/Lingames"));
        QVERIFY(!lin->system);

        // Two NTFS partitions and the Windows EFI partition, not mounted.
        int ntfs = 0;
        for (const Item &i : m_items) {
            if (i.volume && i.volume->fsType == QLatin1String("ntfs")) {
                ++ntfs;
                QVERIFY(!i.mounted);
                QVERIFY(i.title.endsWith(QStringLiteral("NTFS partition")) || i.title == QLatin1String("Windows recovery partition"));
            }
        }
        QCOMPARE(ntfs, 2);
        QVERIFY(byTitle(m_items, QStringLiteral("Windows recovery partition")));
        QVERIFY(byTitle(m_items, QStringLiteral("EFI system partition"))); // Windows' EFI partition (not in fstab)
        // Snaps, whole disks and the partition without a file system never appear.
        for (const Item &i : m_items) {
            QVERIFY(!i.volume || drives::isUserVisible(*i.volume) || i.entry);
        }
    }

    void mountedNetworkSharesNotInFstabAreShown()
    {
        const Item *media = byMountPoint(m_items, QStringLiteral("/mnt/media"));
        QVERIFY(media && media->kind == Kind::Network && media->mounted && !media->entry);
        QCOMPARE(media->title, QStringLiteral("media"));
        QCOMPARE(media->fsType, QStringLiteral("nfs4"));
        const Item *games = byMountPoint(m_items, QStringLiteral("/run/media/oarko/My Games"));
        QVERIFY(games && games->kind == Kind::Network);
        QCOMPARE(games->title, QStringLiteral("My Games"));
    }

    void missingDeviceIsFlagged()
    {
        const auto file = fstab::File::parse("UUID=dead-beef /mnt/gone ext4 nofail 0 0\nnas:/x /mnt/nas nfs4 noauto,x-systemd.automount 0 0\n");
        const QList<Item> items = build(file, m_volumes, m_mounts);
        const Item *gone = byMountPoint(items, QStringLiteral("/mnt/gone"));
        QVERIFY(gone && gone->deviceMissing && !gone->mounted);
        const Item *nas = byMountPoint(items, QStringLiteral("/mnt/nas"));
        QVERIFY(nas && !nas->deviceMissing && nas->kind == Kind::Network);
        QCOMPARE(nas->startup, Startup::OnFirstAccess);
    }

    void specMatching_data()
    {
        QTest::addColumn<QString>("spec");
        QTest::addColumn<bool>("matches");
        QTest::newRow("uuid") << QStringLiteral("UUID=3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e") << true;
        QTest::newRow("uuid case") << QStringLiteral("UUID=3F2A7D10-88C4-4B6E-A0D9-5E7B1C2F9C1E") << true;
        QTest::newRow("uuid quoted") << QStringLiteral("UUID=\"3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e\"") << true;
        QTest::newRow("by-uuid") << QStringLiteral("/dev/disk/by-uuid/3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e") << true;
        QTest::newRow("label") << QStringLiteral("LABEL=Lingames") << true;
        QTest::newRow("label case matters") << QStringLiteral("LABEL=lingames") << false;
        QTest::newRow("by-label") << QStringLiteral("/dev/disk/by-label/Lingames") << true;
        QTest::newRow("path") << QStringLiteral("/dev/sdb1") << true;
        QTest::newRow("other path") << QStringLiteral("/dev/sdb") << false;
        QTest::newRow("other uuid") << QStringLiteral("UUID=1111") << false;
        QTest::newRow("empty uuid") << QStringLiteral("UUID=") << false;
    }
    void specMatching()
    {
        QFETCH(QString, spec);
        QFETCH(bool, matches);
        const auto lin = std::find_if(m_volumes.cbegin(), m_volumes.cend(), [](const auto &v) { return v.label == QLatin1String("Lingames"); });
        QCOMPARE(specMatches(spec, *lin), matches);
    }

    void byLabelUnescapesUdevNames()
    {
        drives::Volume v;
        v.label = QStringLiteral("My USB");
        QVERIFY(specMatches(QStringLiteral("/dev/disk/by-label/My\\x20USB"), v));
    }

    void sizes()
    {
        QCOMPARE(formatSize(2000394649600), QStringLiteral("2.0 TB"));
        QCOMPARE(formatSize(209715200), QStringLiteral("210 MB"));
        QCOMPARE(formatSize(512), QStringLiteral("512 B"));
        QCOMPARE(formatSize(-1), QString());
    }

    // The real machine running the tests: must parse without errors.
    void liveScanWorks()
    {
        if (!QFile::exists(QStringLiteral("/usr/bin/lsblk"))) {
            QSKIP("lsblk not installed");
        }
        const auto v = drives::scan();
        QVERIFY2(v.ok(), qPrintable(v.error()));
        const auto m = mounts::readCurrent();
        QVERIFY2(m.ok(), qPrintable(m.error()));
        QVERIFY(std::any_of(m->cbegin(), m->cend(), [](const auto &x) { return x.target == QLatin1String("/"); }));
    }
};

QTEST_GUILESS_MAIN(InventoryTest)
#include "inventorytest.moc"
