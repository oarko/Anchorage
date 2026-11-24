// SPDX-License-Identifier: GPL-3.0-or-later
#include "safefs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <sys/stat.h>

using namespace anchorage;
using namespace anchorage::safefs;

namespace
{
void writePlain(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray readPlain(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}

mode_t modeOf(const QString &path)
{
    struct stat st {
    };
    ::lstat(QFile::encodeName(path).constData(), &st);
    return st.st_mode & 07777;
}
} // namespace

class SafeFsTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_tmpDir;
    QString path() const { return m_tmpDir->path(); }
    QString t(const QString &rel) const { return path() + QLatin1Char('/') + rel; }

private Q_SLOTS:
    // A fresh folder for every test.
    void init()
    {
        m_tmpDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_tmpDir->isValid());
    }
    void cleanup() { m_tmpDir.reset(); }

    void makeDirectoriesCreatesNestedWithExactMode()
    {
        const mode_t old = ::umask(077); // must not affect the result
        QStringList created;
        const Status s = makeDirectories(t(QStringLiteral("mk/a/b")), 0755, {}, &created);
        ::umask(old);
        QVERIFY2(s.ok(), qPrintable(s.error));
        QCOMPARE(created, (QStringList{t(QStringLiteral("mk")), t(QStringLiteral("mk/a")), t(QStringLiteral("mk/a/b"))}));
        QCOMPARE(modeOf(t(QStringLiteral("mk/a/b"))), mode_t(0755));
        // Existing folders are fine and not reported as created.
        created.clear();
        QVERIFY(makeDirectories(t(QStringLiteral("mk/a/b")), 0755, {}, &created).ok());
        QVERIFY(created.isEmpty());
    }

    void makeDirectoriesRefusesSymlinkInPath()
    {
        QVERIFY(QDir().mkpath(t(QStringLiteral("real"))));
        QVERIFY(QFile::link(t(QStringLiteral("real")), t(QStringLiteral("link"))));
        const Status s = makeDirectories(t(QStringLiteral("link/x")), 0755, {});
        QVERIFY(!s.ok());
        QVERIFY(s.error.contains(QStringLiteral("symbolic link")));
        QVERIFY(!QFileInfo::exists(t(QStringLiteral("real/x"))));
    }

    void makeDirectoriesRefusesSymlinkAsFinalFolder()
    {
        QVERIFY(QFile::link(QStringLiteral("/etc"), t(QStringLiteral("evil"))));
        QVERIFY(!makeDirectories(t(QStringLiteral("evil")), 0755, {}).ok());
    }

    void makeDirectoriesRefusesFileInPath()
    {
        writePlain(t(QStringLiteral("afile")), "x");
        QVERIFY(!makeDirectories(t(QStringLiteral("afile/sub")), 0755, {}).ok());
    }

    void openDirectoryRejectsNonNormalisedPaths()
    {
        QVERIFY(!openDirectory(QStringLiteral("relative/path")).ok());
        QVERIFY(!openDirectory(t(QStringLiteral("a/../b"))).ok());
        QVERIFY(openDirectory(path()).ok());
    }

    void writeFileAtomicWritesAndReplaces()
    {
        QVERIFY(writeFileAtomic(path(), QStringLiteral("f"), "one", 0644, {}).ok());
        QCOMPARE(readPlain(t(QStringLiteral("f"))), QByteArray("one"));
        QCOMPARE(modeOf(t(QStringLiteral("f"))), mode_t(0644));
        QVERIFY(writeFileAtomic(path(), QStringLiteral("f"), "two", 0600, {}).ok());
        QCOMPARE(readPlain(t(QStringLiteral("f"))), QByteArray("two"));
        QCOMPARE(modeOf(t(QStringLiteral("f"))), mode_t(0600));
        // No temporary files left behind.
        const QStringList names = QDir(path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(names, QStringList{QStringLiteral("f")});
    }

    void writeFileAtomicReplacesSymlinkInsteadOfFollowingIt()
    {
        writePlain(t(QStringLiteral("victim")), "precious");
        QVERIFY(QFile::link(t(QStringLiteral("victim")), t(QStringLiteral("target"))));
        QVERIFY(writeFileAtomic(path(), QStringLiteral("target"), "new", 0644, {}).ok());
        QCOMPARE(readPlain(t(QStringLiteral("victim"))), QByteArray("precious"));
        QVERIFY(!QFileInfo(t(QStringLiteral("target"))).isSymLink());
        QCOMPARE(readPlain(t(QStringLiteral("target"))), QByteArray("new"));
    }

    void writeFileAtomicRefusesSymlinkedFolder()
    {
        QVERIFY(QDir().mkpath(t(QStringLiteral("realdir"))));
        QVERIFY(QFile::link(t(QStringLiteral("realdir")), t(QStringLiteral("linkdir"))));
        QVERIFY(!writeFileAtomic(t(QStringLiteral("linkdir")), QStringLiteral("f"), "x", 0644, {}).ok());
        QVERIFY(!QFileInfo::exists(t(QStringLiteral("realdir/f"))));
    }

    void namesCannotEscapeTheFolder()
    {
        for (const QString &bad : {QStringLiteral(".."), QStringLiteral("."), QStringLiteral("a/b"), QString(), QStringLiteral("../x")}) {
            QVERIFY(!writeFileAtomic(path(), bad, "x", 0644, {}).ok());
            QVERIFY(!readFile(path(), bad, 100).ok());
            QVERIFY(!removeFile(path(), bad).ok());
            QVERIFY(!lock(path(), bad).ok());
        }
    }

    void readFileRefusesSymlinksAndFoldersAndBigFiles()
    {
        writePlain(t(QStringLiteral("small")), "hello");
        QCOMPARE(*readFile(path(), QStringLiteral("small"), 100), QByteArray("hello"));
        QVERIFY(!readFile(path(), QStringLiteral("small"), 3).ok());
        QVERIFY(QFile::link(t(QStringLiteral("small")), t(QStringLiteral("small-link"))));
        QVERIFY(!readFile(path(), QStringLiteral("small-link"), 100).ok());
        QVERIFY(QDir().mkpath(t(QStringLiteral("adir"))));
        QVERIFY(!readFile(path(), QStringLiteral("adir"), 100).ok());
    }

    void emptyDirectoryCheck()
    {
        QVERIFY(QDir().mkpath(t(QStringLiteral("empty"))));
        QVERIFY(QDir().mkpath(t(QStringLiteral("full"))));
        writePlain(t(QStringLiteral("full/.hidden")), "x");
        QCOMPARE(*isEmptyDirectory(t(QStringLiteral("empty"))), true);
        QCOMPARE(*isEmptyDirectory(t(QStringLiteral("full"))), false);
        QVERIFY(!isEmptyDirectory(t(QStringLiteral("missing"))).ok());
    }

    void lockWorks()
    {
        auto l = lock(path(), QStringLiteral("x.lock"));
        QVERIFY(l.ok());
        QVERIFY(l->valid());
    }
};

QTEST_GUILESS_MAIN(SafeFsTest)
#include "safefstest.moc"
