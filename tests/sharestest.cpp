// SPDX-License-Identifier: GPL-3.0-or-later
// Listing SMB and NFS shares (src/client/shares.cpp).
//
// The live part runs only with ANCHORAGE_NAS_ENV=<file> pointing at a private file with
// NAS_HOST, NAS_USER and NAS_PASSWORD (mode 600). It only lists; it never mounts or writes,
// and never prints the password.
#include "shares.h"

#include <QFile>
#include <QTest>

using namespace anchorage::shares;

namespace
{
QHash<QString, QString> nasDetails()
{
    QHash<QString, QString> out;
    const QString path = qEnvironmentVariable("ANCHORAGE_NAS_ENV");
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        return out;
    }
    for (const QByteArray &line : f.readAll().split('\n')) {
        const qsizetype eq = line.indexOf('=');
        const QByteArray key = line.left(eq).trimmed();
        if (eq > 0 && key.startsWith("NAS_")) {
            QByteArray value = line.mid(eq + 1).trimmed();
            if (value.size() >= 2 && (value.startsWith('"') || value.startsWith('\'')) && value.endsWith(value.at(0))) {
                value = value.mid(1, value.size() - 2);
            }
            out.insert(QString::fromUtf8(key), QString::fromUtf8(value));
        }
    }
    return out;
}
} // namespace

class SharesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesAddresses_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("host");
        QTest::addColumn<int>("protocol");
        QTest::addColumn<bool>("given");
        QTest::newRow("plain") << "nas.local" << "nas.local" << int(Protocol::Smb) << false;
        QTest::newRow("smb url") << "smb://nas.local/Photos" << "nas.local" << int(Protocol::Smb) << true;
        QTest::newRow("nfs url") << "nfs://10.0.0.5" << "10.0.0.5" << int(Protocol::Nfs) << true;
        QTest::newRow("windows") << "\\\\NAS\\Share" << "NAS" << int(Protocol::Smb) << true;
        QTest::newRow("unix share") << "//nas/x" << "nas" << int(Protocol::Smb) << true;
        QTest::newRow("nfs export") << "nas:/srv/media" << "nas" << int(Protocol::Nfs) << true;
        QTest::newRow("ipv6") << "smb://[fe80::1]" << "[fe80::1]" << int(Protocol::Smb) << true;
        QTest::newRow("spaces around") << "  nas.example.com  " << "nas.example.com" << int(Protocol::Smb) << false;
    }
    void parsesAddresses()
    {
        QFETCH(QString, text);
        QFETCH(QString, host);
        QFETCH(int, protocol);
        QFETCH(bool, given);
        const auto a = parseAddress(text);
        QVERIFY2(a.ok(), qPrintable(a.error()));
        QCOMPARE(a->host, host);
        QCOMPARE(int(a->protocol), protocol);
        QCOMPARE(a->protocolGiven, given);
    }

    void refusesBadAddresses()
    {
        for (const char *bad : {"", "-oProxyCommand=x", "nas local", "nas#1", "a,b", "smb://", "nas\n.local", ".nas", "nas-"}) {
            QVERIFY2(!parseAddress(QString::fromLatin1(bad)).ok(), bad);
        }
    }

    void parsesShowmount()
    {
        const auto s = parseShowmount("/mnt/NAS_Pool/TVshows              *\n/srv/with space  10.0.0.0/24\nclnt_create: RPC: timed out\n");
        QCOMPARE(s.size(), 2);
        QCOMPARE(s[0].name, QStringLiteral("/mnt/NAS_Pool/TVshows"));
        QCOMPARE(s[0].comment, QStringLiteral("*"));
        QCOMPARE(s[1].name, QStringLiteral("/srv/with space"));
        QCOMPARE(deviceFor(QStringLiteral("nas"), Protocol::Nfs, s[0]), QStringLiteral("nas:/mnt/NAS_Pool/TVshows"));
        QCOMPARE(deviceFor(QStringLiteral("nas"), Protocol::Smb, {QStringLiteral("test_share"), {}}), QStringLiteral("//nas/test_share"));
    }

    void parsesRpcinfo()
    {
        const QByteArray nas = "   program version(s) netid(s)                         service     owner\n"
                               "    100005  3         tcp6,udp6,tcp,udp                mountd      superuser\n"
                               "    100003  4,3       tcp6,tcp                         nfs         superuser\n";
        QCOMPARE(parseRpcinfo(nas), QList<int>({4, 3}));
        QCOMPARE(parseRpcinfo("    100003  3 tcp nfs superuser\n"), QList<int>{3});
        QVERIFY(parseRpcinfo("rpcinfo: can't contact rpcbind").isEmpty());
    }

    void listsTheNas()
    {
        const auto nas = nasDetails();
        if (!nas.contains(QStringLiteral("NAS_HOST"))) {
            QSKIP("set ANCHORAGE_NAS_ENV to a private file with NAS_HOST, NAS_USER, NAS_PASSWORD");
        }
        const QString host = nas.value(QStringLiteral("NAS_HOST"));
        // SMB without a login: refused, and recognised as "needs a login".
        const SmbList anonymous = listSmb(host, {});
        QVERIFY2(!anonymous.ok(), "the NAS listed shares without a login");
        // With the wrong password: refused.
        const SmbList wrong = listSmb(host, {nas.value(QStringLiteral("NAS_USER")), QStringLiteral("definitely-not-it"), {}, false});
        QVERIFY(!wrong.ok());
        QCOMPARE(int(wrong.error), int(SmbError::LoginRefused));
        // With the login: test_share is there.
        const SmbList smb = listSmb(host, {nas.value(QStringLiteral("NAS_USER")), nas.value(QStringLiteral("NAS_PASSWORD")), {}, false});
        QVERIFY2(smb.ok(), qPrintable(smb.message));
        QStringList names;
        for (const auto &s : smb.shares) {
            names.append(s.name);
        }
        QVERIFY2(names.contains(QStringLiteral("test_share")), qPrintable(names.join(QLatin1Char(','))));
        QVERIFY(!names.contains(QStringLiteral("IPC$")));
        // NFS exports (no login involved).
        const auto nfs = listNfs(host);
        QVERIFY2(nfs.ok(), qPrintable(nfs.error()));
        bool tv = false;
        for (const auto &s : *nfs) {
            tv = tv || s.name == QLatin1String("/mnt/NAS_Pool/TVshows");
        }
        QVERIFY(tv);
        QCOMPARE(nfsVersions(host), QList<int>({4, 3})); // NFSv4 was switched on for this NAS
        // A host that doesn't exist.
        QCOMPARE(int(listSmb(QStringLiteral("no-such-host.invalid"), {}).error), int(SmbError::Unreachable));
        QVERIFY(!listNfs(QStringLiteral("no-such-host.invalid")).ok());
    }
};

QTEST_GUILESS_MAIN(SharesTest)
#include "sharestest.moc"
