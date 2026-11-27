// SPDX-License-Identifier: GPL-3.0-or-later
// Passwords typed into fstab (src/core/secrets.cpp).
#include "secrets.h"

#include <QTest>

using namespace anchorage;

class SecretsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void findsTypedPasswords()
    {
        const fstab::File f = fstab::File::parse("//nas/a /mnt/a cifs username=anna,password=s3cret,domain=WG,nofail 0 0\n"
                                                 "//nas/b /mnt/b cifs user,credentials=/etc/anchorage/credentials/x 0 0\n" // "user" flag only
                                                 "//nas/c /mnt/c smb3 user=bob,pass=hunter2 0 0\n"
                                                 "nas:/e /mnt/e nfs4 defaults 0 0\n"
                                                 "//nas/d /mnt/d cifs username=x,password=removed-by-anchorage 0 0\n");
        const auto found = secrets::find(f);
        QCOMPARE(found.size(), 2);
        QCOMPARE(found[0].line, 0);
        QCOMPARE(found[0].server, QStringLiteral("nas"));
        QCOMPARE(found[0].user, QStringLiteral("anna"));
        QCOMPARE(found[0].password, QStringLiteral("s3cret"));
        QCOMPARE(found[0].domain, QStringLiteral("WG"));
        QCOMPARE(found[1].user, QStringLiteral("bob"));
        QCOMPARE(found[1].password, QStringLiteral("hunter2"));
    }

    void movesTheLoginOut()
    {
        const fstab::Entry e{QStringLiteral("//nas/a"), QStringLiteral("/mnt/a"), QStringLiteral("cifs"),
                             {QStringLiteral("nofail"), QStringLiteral("username=anna"), QStringLiteral("password=s3cret"), QStringLiteral("domain=WG"),
                              QStringLiteral("user"), QStringLiteral("vers=3.1.1")},
                             0, 0};
        const auto moved = secrets::withCredentials(e, QStringLiteral("/etc/anchorage/credentials/root-nas-anna"));
        QCOMPARE(moved.options, QStringList({QStringLiteral("nofail"), QStringLiteral("credentials=/etc/anchorage/credentials/root-nas-anna"),
                                             QStringLiteral("user"), QStringLiteral("vers=3.1.1")}));
    }

    void redactsOnlyThePassword()
    {
        const QByteArray before = "# keep me\n//nas/a   /mnt/a   cifs   username=anna,password=s3cret,nofail   0 0\n"
                                  "//nas/c /mnt/c cifs pass=other 0 0\ntmpfs /tmp tmpfs defaults 0 0\n";
        const QByteArray after = "# keep me\n//nas/a   /mnt/a   cifs   credentials=/etc/anchorage/credentials/r-nas-anna,nofail   0 0\n"
                                 "//nas/c /mnt/c cifs pass=other 0 0\ntmpfs /tmp tmpfs defaults 0 0\n";
        const QStringList gone = secrets::removedPasswords(before, after);
        QCOMPARE(gone, QStringList{QStringLiteral("s3cret")}); // "other" is still in use
        const QByteArray clean = secrets::redact(before, gone);
        QVERIFY(!clean.contains("s3cret"));
        QVERIFY(clean.contains("password=removed-by-anchorage"));
        QVERIFY(clean.contains("pass=other")); // still in fstab: not touched
        QVERIFY(clean.startsWith("# keep me\n//nas/a   /mnt/a   cifs   username=anna,password=removed-by-anchorage,nofail"));
        QVERIFY(clean.endsWith("tmpfs /tmp tmpfs defaults 0 0\n"));
        QCOMPARE(secrets::redact(before, {}), before);
        // A line the fstab editor would refuse to rewrite (a control character, found by
        // fuzzing) is scrubbed too, byte for byte.
        const QByteArray odd = "\x18//nas/a /mnt/a cifs username=x,password=s3cret,nofail 0 0\n";
        const QByteArray scrubbed = secrets::redact(odd, {QStringLiteral("s3cret")});
        QCOMPARE(scrubbed, QByteArray("\x18//nas/a /mnt/a cifs username=x,password=removed-by-anchorage,nofail 0 0\n"));
        // Only whole option values: "s3cret" inside a longer password is not touched.
        QCOMPARE(secrets::redact("//n/a /m cifs password=s3cretly 0 0\n", {QStringLiteral("s3cret")}),
                 QByteArray("//n/a /m cifs password=s3cretly 0 0\n"));
    }
};

QTEST_GUILESS_MAIN(SecretsTest)
#include "secretstest.moc"
