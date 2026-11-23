// SPDX-License-Identifier: GPL-3.0-or-later
#include "paths.h"

#include <QDir>
#include <QTest>

using namespace anchorage;

// Guards the fixed paths the root helper writes to (CLAUDE.md security rule 2).
class PathsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fixedPathsAreAbsoluteAndClean_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("fstab") << QString::fromUtf8(paths::fstab);
        QTest::newRow("configDir") << QString::fromUtf8(paths::configDir);
        QTest::newRow("backupDir") << QString::fromUtf8(paths::backupDir);
        QTest::newRow("credentialsDir") << QString::fromUtf8(paths::credentialsDir);
        QTest::newRow("configFile") << QString::fromUtf8(paths::configFile);
        QTest::newRow("blockedPathsFile") << QString::fromUtf8(paths::blockedPathsFile);
        QTest::newRow("polkitRulesFile") << QString::fromUtf8(paths::polkitRulesFile);
    }

    void fixedPathsAreAbsoluteAndClean()
    {
        QFETCH(QString, path);
        QVERIFY(path.startsWith(QLatin1Char('/')));
        // cleanPath removes "..", ".", doubled and trailing slashes.
        QCOMPARE(QDir::cleanPath(path), path);
    }

    void fstabIsTheSystemFile()
    {
        QCOMPARE(QString::fromUtf8(paths::fstab), QStringLiteral("/etc/fstab"));
    }

    void appFilesStayInsideConfigDir()
    {
        const QString dir = QString::fromUtf8(paths::configDir) + QLatin1Char('/');
        QVERIFY(QString::fromUtf8(paths::backupDir).startsWith(dir));
        QVERIFY(QString::fromUtf8(paths::credentialsDir).startsWith(dir));
        QVERIFY(QString::fromUtf8(paths::configFile).startsWith(dir));
        QVERIFY(QString::fromUtf8(paths::blockedPathsFile).startsWith(dir));
    }
};

QTEST_GUILESS_MAIN(PathsTest)
#include "pathstest.moc"
