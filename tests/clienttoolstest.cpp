// SPDX-License-Identifier: GPL-3.0-or-later
// Package manager detection and install commands for SMB/NFS client tools.
#include "clienttools.h"

#include <QTest>

using namespace anchorage::clienttools;

namespace
{
Exists only(const QStringList &paths)
{
    return [paths](const QString &p) {
        return paths.contains(p);
    };
}
} // namespace

class ClientToolsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void detectsTheDistribution_data()
    {
        QTest::addColumn<QByteArray>("osRelease");
        QTest::addColumn<QStringList>("tools");
        QTest::addColumn<int>("manager");
        QTest::addColumn<QString>("nfsCommand");
        const QByteArray kubuntu = "PRETTY_NAME=\"Ubuntu 26.04.1 LTS\"\nNAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\n";
        const QByteArray mint = "NAME=\"Linux Mint\"\nID=linuxmint\nID_LIKE=\"ubuntu debian\"\n";
        const QByteArray fedora = "NAME=\"Fedora Linux\"\nID=fedora\n";
        const QByteArray nobara = "NAME=\"Nobara Linux\"\nID=nobara\nID_LIKE=\"rhel centos fedora\"\n";
        const QByteArray tumbleweed = "NAME=\"openSUSE Tumbleweed\"\nID=\"opensuse-tumbleweed\"\nID_LIKE=\"opensuse suse\"\n";
        const QByteArray endeavour = "NAME='EndeavourOS'\nID=endeavouros\nID_LIKE=arch\n";
        const QByteArray unknown = "NAME=Solus\nID=solus\n";
        const QString apt = QStringLiteral("/usr/bin/apt");
        QTest::newRow("Kubuntu") << kubuntu << QStringList{apt} << int(Manager::Apt) << "sudo apt install nfs-common";
        QTest::newRow("Mint") << mint << QStringList{apt} << int(Manager::Apt) << "sudo apt install nfs-common";
        QTest::newRow("Fedora") << fedora << QStringList{QStringLiteral("/usr/bin/dnf")} << int(Manager::Dnf) << "sudo dnf install nfs-utils";
        QTest::newRow("Nobara") << nobara << QStringList{QStringLiteral("/usr/bin/dnf")} << int(Manager::Dnf) << "sudo dnf install nfs-utils";
        QTest::newRow("Tumbleweed") << tumbleweed << QStringList{QStringLiteral("/usr/bin/zypper")} << int(Manager::Zypper)
                                    << "sudo zypper install nfs-client";
        QTest::newRow("EndeavourOS") << endeavour << QStringList{QStringLiteral("/usr/bin/pacman")} << int(Manager::Pacman)
                                     << "sudo pacman -S nfs-utils";
        // The family's tool missing (e.g. apt removed): fall back to what is there.
        QTest::newRow("tool decides") << kubuntu << QStringList{QStringLiteral("/usr/bin/pacman")} << int(Manager::Pacman)
                                      << "sudo pacman -S nfs-utils";
        QTest::newRow("unknown") << unknown << QStringList{} << int(Manager::Unknown)
                                 << "Install the package that provides mount.nfs (usually \"nfs-utils\") with your distribution's package manager.";
    }
    void detectsTheDistribution()
    {
        QFETCH(QByteArray, osRelease);
        QFETCH(QStringList, tools);
        QFETCH(int, manager);
        QFETCH(QString, nfsCommand);
        const Manager m = detectManager(parseOsRelease(osRelease), only(tools));
        QCOMPARE(int(m), manager);
        QCOMPARE(installCommand(m, Protocol::Nfs), nfsCommand);
        QCOMPARE(packageName(m, Protocol::Smb), QStringLiteral("cifs-utils"));
    }

    void parsesOsRelease()
    {
        const Distro d = parseOsRelease("# comment\nPRETTY_NAME=\"Ubuntu 26.04.1 LTS\"\nID=Ubuntu\nID_LIKE=\"debian\"\nbroken line\n");
        QCOMPARE(d.id, QStringLiteral("ubuntu"));
        QCOMPARE(d.idLike, QStringList{QStringLiteral("debian")});
        QCOMPARE(d.name, QStringLiteral("Ubuntu 26.04.1 LTS"));
    }

    void findsTheMountHelpers()
    {
        QVERIFY(isInstalled(Protocol::Smb, only({QStringLiteral("/usr/sbin/mount.cifs")})));
        QVERIFY(isInstalled(Protocol::Nfs, only({QStringLiteral("/sbin/mount.nfs")})));
        QVERIFY(!isInstalled(Protocol::Smb, only({QStringLiteral("/usr/sbin/mount.nfs")})));
        Protocol p = Protocol::Nfs;
        QVERIFY(protocolFor(QStringLiteral("smb3"), &p) && p == Protocol::Smb);
        QVERIFY(protocolFor(QStringLiteral("nfs4"), &p) && p == Protocol::Nfs);
        QVERIFY(!protocolFor(QStringLiteral("ext4"), &p));
    }
};

QTEST_GUILESS_MAIN(ClientToolsTest)
#include "clienttoolstest.moc"
