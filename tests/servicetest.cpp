// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the helper's operations against a temporary folder that stands in for /etc and
// for the mount-point locations. Nothing on the real system is touched.
#include "fstabservice.h"
#include "requests.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace anchorage;
using namespace anchorage::service;

namespace
{
Caller makeCaller(bool sudo, bool driveAdmin, bool netMount)
{
    Caller c;
    c.uid = getuid();
    c.gid = getgid();
    c.name = QStringLiteral("tester");
    c.home = QStringLiteral("/home/tester");
    c.sudo = sudo;
    c.driveAdmin = driveAdmin;
    c.netMount = netMount;
    return c;
}
const Caller standard = makeCaller(false, false, false);
const Caller netmount = makeCaller(false, false, true);
const Caller driveadmin = makeCaller(false, true, false);
const Caller sudoer = makeCaller(true, false, false);

const QByteArray initial =
    "# /etc/fstab test copy\n"
    "UUID=root  /      ext4  errors=remount-ro  0 1\n"
    "UUID=home  /home  ext4  defaults           0 2\n";

struct FileInfo {
    uid_t uid = 0;
    gid_t gid = 0;
    mode_t mode = 0;
};
FileInfo statOf(const QString &path)
{
    struct stat st {
    };
    ::lstat(QFile::encodeName(path).constData(), &st);
    return {st.st_uid, st.st_gid, static_cast<mode_t>(st.st_mode & 07777)};
}

// An arbitrary non-root owner for "someone's home folder" (doesn't need to exist).
constexpr uid_t homeOwner = 4242;

QByteArray readPlain(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}
} // namespace

class ServiceTest : public QObject
{
    Q_OBJECT

    QTemporaryDir *m_tmp = nullptr;
    Paths m_paths;

    QString fstabPath() const { return m_paths.fstabDir + QStringLiteral("/fstab"); }
    QByteArray current() const { return readPlain(fstabPath()); }
    Outcome write(const Caller &c, const QByteArray &content, bool confirm = false, bool create = true)
    {
        return writeFstab(m_paths, c, fingerprint(current()), content, confirm, create);
    }

private Q_SLOTS:
    void init()
    {
        m_tmp = new QTemporaryDir;
        QVERIFY(m_tmp->isValid());
        const QString root = m_tmp->path();
        m_paths.fstabDir = root + QStringLiteral("/etc");
        m_paths.fstabName = QStringLiteral("fstab");
        m_paths.configDir = root + QStringLiteral("/etc/anchorage");
        m_paths.backupDir = root + QStringLiteral("/etc/anchorage/backups");
        m_paths.credentialsDir = QStringLiteral("/etc/anchorage/credentials");
        m_paths.mountRoot = root + QStringLiteral("/fsroot");
        m_paths.keepBackups = 3;
        QVERIFY(QDir().mkpath(m_paths.fstabDir));
        QVERIFY(QDir().mkpath(m_paths.mountRoot + QStringLiteral("/mnt")));
        QFile f(fstabPath());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(initial);
    }
    void cleanup()
    {
        delete m_tmp;
        m_tmp = nullptr;
    }

    void systemPathsAreTheRealOnes()
    {
        const Paths p = systemPaths();
        QCOMPARE(p.fstabDir + QLatin1Char('/') + p.fstabName, QStringLiteral("/etc/fstab"));
        QVERIFY(p.mountRoot.isEmpty()); // the shipped helper never redirects mount points
    }

    void addEntryWritesBacksUpAndCreatesFolder()
    {
        const QByteArray proposed = initial + "LABEL=Lingames  /mnt/lingames  btrfs  nofail  0 0\n";
        const Outcome o = write(standard, proposed);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char('\n'))));
        QCOMPARE(current(), proposed);
        QVERIFY(isBackupName(o.backupName));
        QCOMPARE(readPlain(m_paths.backupDir + QLatin1Char('/') + o.backupName), initial);
        QVERIFY(QFileInfo(m_paths.mountRoot + QStringLiteral("/mnt/lingames")).isDir());
        QCOMPARE(o.createdDirs, QStringList{m_paths.mountRoot + QStringLiteral("/mnt/lingames")});
    }

    void unchangedWriteDoesNothing()
    {
        const Outcome o = write(standard, initial);
        QVERIFY(o.ok());
        QVERIFY(o.backupName.isEmpty());
        QVERIFY(!QFileInfo::exists(m_paths.backupDir));
    }

    void staleBaselineIsRefused()
    {
        const QByteArray stale = fingerprint("something else\n");
        const Outcome o = writeFstab(m_paths, standard, stale, initial + "a /mnt/a ext4 defaults 0 0\n", false, false);
        QVERIFY(!o.ok());
        QCOMPARE(current(), initial);
    }

    void systemEntryRules()
    {
        QByteArray changed = initial;
        changed.replace("defaults           0 2", "defaults,noatime   0 2");
        QVERIFY(!write(standard, changed).ok());
        QCOMPARE(current(), initial);
        QVERIFY(write(driveadmin, changed).ok());
        QCOMPARE(current(), changed);
    }

    void overrideZoneNeedsEmptyFolder()
    {
        const QByteArray proposed = initial + "nas:/cfg  /opt/app  nfs4  nofail  0 0\n";
        QVERIFY(!write(standard, proposed).ok());

        QVERIFY(QDir().mkpath(m_paths.mountRoot + QStringLiteral("/opt/app")));
        QFile f(m_paths.mountRoot + QStringLiteral("/opt/app/settings.conf"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const Outcome notEmpty = write(driveadmin, proposed);
        QVERIFY(!notEmpty.ok());
        QVERIFY(notEmpty.errors.join(QString()).contains(QStringLiteral("not empty")));
        QCOMPARE(current(), initial);

        QVERIFY(QFile::remove(m_paths.mountRoot + QStringLiteral("/opt/app/settings.conf")));
        QVERIFY(write(driveadmin, proposed).ok());
    }

    void blockedZone()
    {
        const QByteArray proposed = initial + "nas:/k  /etc/ssh/keys  nfs4  nofail  0 0\n";
        QVERIFY(!write(driveadmin, proposed, true).ok());
        QVERIFY(!write(sudoer, proposed, false).ok());
        QVERIFY(write(sudoer, proposed, true, false).ok());
    }

    void newBrokenLineRefused()
    {
        QVERIFY(!write(sudoer, initial + "two fields\n", true).ok());
        QCOMPARE(current(), initial);
    }

    void symlinkedFstabIsRefused()
    {
        QVERIFY(QFile::remove(fstabPath()));
        QFile victim(m_tmp->path() + QStringLiteral("/victim"));
        QVERIFY(victim.open(QIODevice::WriteOnly));
        victim.write(initial);
        victim.close();
        QVERIFY(QFile::link(victim.fileName(), fstabPath()));
        QVERIFY(!readFstab(m_paths).ok());
        QVERIFY(!writeFstab(m_paths, sudoer, fingerprint(initial), initial + "a /mnt/a ext4 defaults 0 0\n", false, false).ok());
        QCOMPARE(readPlain(victim.fileName()), initial);
    }

    void symlinkedMountPointParentIsRefused()
    {
        // /mnt/evil -> somewhere else: creating /mnt/evil/x must not follow it.
        QVERIFY(QDir().mkpath(m_tmp->path() + QStringLiteral("/elsewhere")));
        QVERIFY(QFile::link(m_tmp->path() + QStringLiteral("/elsewhere"), m_paths.mountRoot + QStringLiteral("/mnt/evil")));
        const Outcome o = write(standard, initial + "LABEL=x  /mnt/evil/x  ext4  nofail  0 0\n");
        QVERIFY(!o.ok());
        QVERIFY(!QFileInfo::exists(m_tmp->path() + QStringLiteral("/elsewhere/x")));
        QCOMPARE(current(), initial);
    }

    void backupsArePruned()
    {
        QByteArray content = initial;
        for (int i = 0; i < 5; ++i) {
            content += "LABEL=d" + QByteArray::number(i) + "  /mnt/d" + QByteArray::number(i) + "  ext4  nofail  0 0\n";
            QVERIFY(write(standard, content, false, false).ok());
        }
        auto list = listBackups(m_paths);
        QVERIFY(list.ok());
        QCOMPARE(list->size(), 3);
        // The newest backup holds the file as it was before the last write.
        QVERIFY(readPlain(m_paths.backupDir + QLatin1Char('/') + list->first()).contains("/mnt/d3"));
        QVERIFY(!readPlain(m_paths.backupDir + QLatin1Char('/') + list->first()).contains("/mnt/d4"));
    }

    void restoreBackup()
    {
        const Outcome first = write(standard, initial + "LABEL=a  /mnt/a  ext4  nofail  0 0\n", false, false);
        QVERIFY(first.ok());
        const Outcome restored = service::restoreBackup(m_paths, standard, first.backupName, false);
        QVERIFY2(restored.ok(), qPrintable(restored.errors.join(QLatin1Char('\n'))));
        QCOMPARE(current(), initial);
        QVERIFY(!restored.backupName.isEmpty()); // restoring also backs up first

        for (const QString &bad : {QStringLiteral("../fstab"), QStringLiteral("fstab-../../etc/shadow"), QStringLiteral("passwd"),
                                   QStringLiteral("fstab-2026-11-24T23-10-05Z/../../x")}) {
            QVERIFY(!service::restoreBackup(m_paths, sudoer, bad, true).ok());
        }
    }

    void restoreIsCheckedLikeAWrite()
    {
        // A backup with a changed system entry can't be restored by a standard user.
        QByteArray changed = initial;
        changed.replace("errors=remount-ro", "defaults");
        QVERIFY(write(driveadmin, changed).ok()); // backup holds `initial`
        const QString name = listBackups(m_paths)->first();
        QVERIFY(!service::restoreBackup(m_paths, standard, name, false).ok());
        QCOMPARE(current(), changed);
        QVERIFY(service::restoreBackup(m_paths, driveadmin, name, false).ok());
        QCOMPARE(current(), initial);
    }

    void sharedMountAddAndRemove()
    {
        const fstab::Entry e{QStringLiteral("//nas.local/Games"), QStringLiteral("/mnt/games"), QStringLiteral("cifs"),
                             {QStringLiteral("vers=3.1.1"), QStringLiteral("guest")}, 0, 0};
        QVERIFY(!addSharedMount(m_paths, standard, e).ok());
        const Outcome o = addSharedMount(m_paths, netmount, e);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char('\n'))));
        QVERIFY(current().contains("//nas.local/Games"));
        QVERIFY(current().contains("nosuid,nodev,nofail,_netdev"));
        QVERIFY(QFileInfo(m_paths.mountRoot + QStringLiteral("/mnt/games")).isDir());

        QVERIFY(!removeSharedMount(m_paths, netmount, QStringLiteral("/home")).ok());
        QVERIFY(removeSharedMount(m_paths, netmount, QStringLiteral("/mnt/games")).ok());
        QCOMPARE(current(), initial);
    }

    void requestRoundTrip()
    {
        const requests::WriteFstab w{fingerprint(initial), initial + "x /mnt/x ext4 defaults 0 0\n", true, false};
        const auto decoded = requests::decodeWriteFstab(requests::encodeWriteFstab(w));
        QVERIFY(decoded.ok());
        QCOMPARE(decoded->content, w.content);
        QCOMPARE(decoded->confirmProtected, true);

        QVariantMap extra = requests::encodeWriteFstab(w);
        extra.insert(QStringLiteral("path"), QStringLiteral("/etc/shadow"));
        QVERIFY(!requests::decodeWriteFstab(extra).ok());

        QVariantMap wrongType = requests::encodeWriteFstab(w);
        wrongType[QStringLiteral("content")] = QString::fromLatin1(w.content);
        QVERIFY(!requests::decodeWriteFstab(wrongType).ok());

        QVariantMap badHash = requests::encodeWriteFstab(w);
        badHash[QStringLiteral("baseline")] = QByteArray("zz");
        QVERIFY(!requests::decodeWriteFstab(badHash).ok());

        QVariantMap missing = requests::encodeWriteFstab(w);
        missing.remove(QStringLiteral("confirmProtected"));
        QVERIFY(!requests::decodeWriteFstab(missing).ok());

        const fstab::Entry e{QStringLiteral("//nas/G"), QStringLiteral("/mnt/g"), QStringLiteral("cifs"), {QStringLiteral("guest")}, 0, 0};
        QCOMPARE(*requests::decodeEntry(requests::encodeEntry(e)), e);
        QVariantMap badEntry = requests::encodeEntry(e);
        badEntry[QStringLiteral("pass")] = QStringLiteral("0");
        QVERIFY(!requests::decodeEntry(badEntry).ok());
    }

    // ---- Only meaningful as root (run tests/docker/run-root-tests.sh) ----------------
    void rootWritesRootOwnedFiles()
    {
        if (geteuid() != 0) {
            QSKIP("needs root: run tests/docker/run-root-tests.sh");
        }
        // Start from a file owned by someone else, to prove the helper resets ownership.
        QVERIFY(::chown(QFile::encodeName(fstabPath()).constData(), homeOwner, homeOwner) == 0);
        const Outcome o = write(standard, initial + "LABEL=a  /mnt/a  ext4  nofail  0 0\n");
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char('\n'))));
        const FileInfo f = statOf(fstabPath());
        QCOMPARE(f.uid, uid_t(0));
        QCOMPARE(f.gid, gid_t(0));
        QCOMPARE(f.mode, mode_t(0644));
        const FileInfo b = statOf(m_paths.backupDir + QLatin1Char('/') + o.backupName);
        QCOMPARE(b.uid, uid_t(0));
        QCOMPARE(b.mode, mode_t(0644));
        const FileInfo mp = statOf(m_paths.mountRoot + QStringLiteral("/mnt/a"));
        QCOMPARE(mp.uid, uid_t(0));
        QCOMPARE(mp.gid, gid_t(0));
        QCOMPARE(mp.mode, mode_t(0755));
    }

    void rootGivesHomeMountPointsToTheHomeOwner()
    {
        if (geteuid() != 0) {
            QSKIP("needs root: run tests/docker/run-root-tests.sh");
        }
        const QString home = m_paths.mountRoot + QStringLiteral("/home/alice");
        QVERIFY(QDir().mkpath(home));
        QVERIFY(::chown(QFile::encodeName(home).constData(), homeOwner, homeOwner) == 0);
        const Outcome o = write(standard, initial + "nas:/x  /home/alice/NAS/games  nfs4  nofail  0 0\n");
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char('\n'))));
        for (const QString &dir : QStringList{home + QStringLiteral("/NAS"), home + QStringLiteral("/NAS/games")}) {
            const FileInfo d = statOf(dir);
            QCOMPARE(d.uid, homeOwner);
            QCOMPARE(d.gid, gid_t(homeOwner));
            QCOMPARE(d.mode, mode_t(0755));
        }
        QCOMPARE(statOf(home).uid, homeOwner); // existing folders are left alone
    }

    void rootStillRefusesSymlinks()
    {
        if (geteuid() != 0) {
            QSKIP("needs root: run tests/docker/run-root-tests.sh");
        }
        // Root bypasses permission checks, but must not follow a planted symlink.
        const QString target = m_tmp->path() + QStringLiteral("/root-owned-target");
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QFile::link(target, m_paths.mountRoot + QStringLiteral("/mnt/trap")));
        QVERIFY(!write(sudoer, initial + "LABEL=a  /mnt/trap/x  ext4  nofail  0 0\n").ok());
        QVERIFY(!QFileInfo::exists(target + QStringLiteral("/x")));

        QVERIFY(QFile::remove(fstabPath()));
        QVERIFY(QFile::link(target, fstabPath()));
        QVERIFY(!readFstab(m_paths).ok());
    }

    void rootCallerIsAdministrator()
    {
        const auto root = resolveCaller(0, defaultGroupNames());
        QVERIFY(root.ok());
        QVERIFY(root->sudo);
        QVERIFY(root->fullAccess());
    }

    void callerLookup()
    {
        const auto me = resolveCaller(getuid(), defaultGroupNames());
        QVERIFY(me.ok());
        QCOMPARE(me->uid, getuid());
        QVERIFY(!me->name.isEmpty());
        QVERIFY(!resolveCaller(uid_t(987654321), defaultGroupNames()).ok());
    }
};

QTEST_GUILESS_MAIN(ServiceTest)
#include "servicetest.moc"
