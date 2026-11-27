// SPDX-License-Identifier: GPL-3.0-or-later
// Beta logging (SPEC §12a): which lines a build shows, and that passwords never reach a log.
#include "config-anchorage.h"
#include "logging.h"
#include "shares.h"

#include <QTest>

using namespace anchorage;

namespace
{
QStringList captured;
void capture(QtMsgType, const QMessageLogContext &, const QString &message)
{
    captured.append(message);
}
} // namespace

class LoggingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Runs first, before any filter rules are set below.
    void infoLinesOnlyInBetaBuilds()
    {
        if (qEnvironmentVariableIsSet("QT_LOGGING_RULES") || qEnvironmentVariableIsSet("QT_LOGGING_CONF")) {
            QSKIP("QT_LOGGING_RULES/QT_LOGGING_CONF override the build's defaults");
        }
        QCOMPARE(logging::betaBuild(), bool(ANCHORAGE_BETA));
        QCOMPARE(logging::network().isInfoEnabled(), logging::betaBuild());
        QCOMPARE(logging::helper().isInfoEnabled(), logging::betaBuild());
        QVERIFY(logging::mount().isWarningEnabled());
        QVERIFY(!logging::fstab().isDebugEnabled());
    }

    void versionSuffixTurnsBetaOn()
    {
        const QString version = QStringLiteral(ANCHORAGE_VERSION);
        if (version.contains(QLatin1String("~beta")) || version.contains(QLatin1String("~rc"))) {
            QVERIFY(logging::betaBuild());
        }
    }

    void scrubRemovesPasswords_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("secrets");
        QTest::addColumn<QString>("expected");
        QTest::newRow("password=") << QStringLiteral("//nas/a /mnt/a cifs username=bob,password=hunter2,uid=1000 0 0") << QStringList()
                                   << QStringLiteral("//nas/a /mnt/a cifs username=bob,password=…,uid=1000 0 0");
        QTest::newRow("pass= at end") << QStringLiteral("options: rw,pass=s3cr3t") << QStringList() << QStringLiteral("options: rw,pass=…");
        QTest::newRow("upper case") << QStringLiteral("PASSWORD=x y") << QStringList() << QStringLiteral("PASSWORD=… y");
        QTest::newRow("known secret") << QStringLiteral("mount error: bad login Tr0ub4dor for bob") << QStringList{QStringLiteral("Tr0ub4dor")}
                                      << QStringLiteral("mount error: bad login … for bob");
        QTest::newRow("empty secret ignored") << QStringLiteral("nothing here") << QStringList{QString()} << QStringLiteral("nothing here");
        QTest::newRow("passwd= kept") << QStringLiteral("/etc/passwd=fine") << QStringList() << QStringLiteral("/etc/passwd=fine");
    }
    void scrubRemovesPasswords()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, secrets);
        QFETCH(QString, expected);
        QCOMPARE(logging::scrub(text, secrets), expected);
    }

    // A scan with a login logs the scan, but neither the password nor anything containing it.
    void smbScanNeverLogsThePassword()
    {
        QLoggingCategory::setFilterRules(QStringLiteral("anchorage.*.info=true"));
        captured.clear();
        const QtMessageHandler previous = qInstallMessageHandler(capture);
        const QString password = QStringLiteral("Zq9-never-in-a-log");
        const shares::SmbList list = shares::listSmb(QStringLiteral("127.0.0.1"), {QStringLiteral("bob"), password, QStringLiteral("WORKGROUP"), false});
        qInstallMessageHandler(previous);
        QLoggingCategory::setFilterRules(QString());
        Q_UNUSED(list);
        QVERIFY2(!captured.isEmpty(), "the scan should have logged something");
        QVERIFY(captured.first().contains(QLatin1String("Starting network scan of 127.0.0.1")));
        for (const QString &line : std::as_const(captured)) {
            QVERIFY2(!line.contains(password), qPrintable(line));
        }
    }
};

QTEST_GUILESS_MAIN(LoggingTest)
#include "loggingtest.moc"
