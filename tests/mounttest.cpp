// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real mounts of what the entry editor produces. Runs only inside the throwaway
// privileged container started by tests/docker/run-mount-tests.sh (root, loop devices,
// ANCHORAGE_MOUNT_TESTS=1); everywhere else it skips.
//
// For every file system the catalogue can create, it formats a loop image, then:
//  * accessChoices: every combination of the Access tab, written as a proxy fstab with
//    fstab::File, mounted with `mount -T <proxy> <target>`, checked in mountinfo and with
//    stat(), and run through systemd-fstab-generator to check the units systemd makes;
//  * optionSweep: every catalogue option for the type (unless marked "skip"), one at a
//    time, so a typo or an option the kernel doesn't know fails the test.
#include "access.h"
#include "fstabservice.h"
#include "fstab.h"
#include "options.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace anchorage;

namespace
{
struct Run {
    int exit = -1;
    QString out;
};

Run run(const QString &program, const QStringList &args)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(10000)) {
        return {-1, QStringLiteral("could not start %1").arg(program)};
    }
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished();
        return {-1, QStringLiteral("timeout")};
    }
    return {p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1, QString::fromUtf8(p.readAll()).trimmed()};
}

struct Image {
    QString type;
    QString spec; // UUID=... (or the source for tmpfs / bind, or a share)
    QString loop;
    QStringList base; // options every mount of this type needs (guest for the SMB test share)
};

// Per-mount and super-block options of whatever is mounted at `target`, or nothing.
std::optional<QStringList> mountedOptions(const QString &target)
{
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    std::optional<QStringList> found;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 10 || QString::fromUtf8(fields.at(4)) != target) {
            continue;
        }
        const qsizetype dash = fields.indexOf("-");
        QStringList opts = QString::fromUtf8(fields.at(5)).split(QLatin1Char(','));
        if (dash > 0 && dash + 3 < fields.size()) {
            opts += QString::fromUtf8(fields.at(dash + 3)).split(QLatin1Char(','));
        }
        found = opts; // the last one is on top
    }
    return found;
}

QString unitName(const QString &path)
{
    return run(QStringLiteral("/usr/bin/systemd-escape"), {QStringLiteral("--path"), path}).out;
}
} // namespace

class MountTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir{QStringLiteral("/tmp/anchorage-mounttest-XXXXXX")};
    QList<Image> m_images;
    QString m_fstab;
    bool m_network = false;
    const options::Catalogue &m_cat = options::Catalogue::builtIn();

    // Writes a proxy fstab with one entry and mounts it by its mount point.
    Run mountEntry(const fstab::Entry &e)
    {
        fstab::File file = fstab::File::parse(QByteArray("# proxy fstab written by mounttest\n"));
        QList<fstab::Issue> issues;
        if (file.append(e, &issues) < 0) {
            return {-1, QStringLiteral("fstab::File refused the entry: %1").arg(issues.isEmpty() ? QString() : issues.first().message)};
        }
        QFile f(m_fstab);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(file.serialize()) < 0) {
            return {-1, QStringLiteral("cannot write proxy fstab")};
        }
        f.close();
        QDir().mkpath(e.mountPoint);
        return run(QStringLiteral("/usr/bin/mount"), {QStringLiteral("-T"), m_fstab, e.mountPoint});
    }

    void unmount(const QString &target)
    {
        for (int i = 0; i < 5 && mountedOptions(target); ++i) {
            run(QStringLiteral("/usr/bin/umount"), {target});
        }
    }

    const Image *image(const QString &type) const
    {
        for (const Image &i : m_images) {
            if (i.type == type) {
                return &i;
            }
        }
        return nullptr;
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (geteuid() != 0 || qEnvironmentVariable("ANCHORAGE_MOUNT_TESTS") != QLatin1String("1")) {
            QSKIP("needs root in the privileged test container (tests/docker/run-mount-tests.sh)");
        }
        QVERIFY(m_dir.isValid());
        m_fstab = m_dir.filePath(QStringLiteral("fstab"));

        // mkfs command for each type the catalogue marks testable.
        const QHash<QString, QStringList> mkfs = {
            {QStringLiteral("ext4"), {QStringLiteral("/usr/sbin/mkfs.ext4"), QStringLiteral("-q"), QStringLiteral("-F")}},
            {QStringLiteral("ext3"), {QStringLiteral("/usr/sbin/mkfs.ext3"), QStringLiteral("-q"), QStringLiteral("-F")}},
            {QStringLiteral("ext2"), {QStringLiteral("/usr/sbin/mkfs.ext2"), QStringLiteral("-q"), QStringLiteral("-F")}},
            {QStringLiteral("btrfs"), {QStringLiteral("/usr/sbin/mkfs.btrfs"), QStringLiteral("-q"), QStringLiteral("-f")}},
            {QStringLiteral("xfs"), {QStringLiteral("/usr/sbin/mkfs.xfs"), QStringLiteral("-q"), QStringLiteral("-f")}},
            {QStringLiteral("f2fs"), {QStringLiteral("/usr/sbin/mkfs.f2fs"), QStringLiteral("-q"), QStringLiteral("-f")}},
            {QStringLiteral("vfat"), {QStringLiteral("/usr/sbin/mkfs.vfat")}},
            {QStringLiteral("exfat"), {QStringLiteral("/usr/sbin/mkfs.exfat")}},
            {QStringLiteral("ntfs3"), {QStringLiteral("/usr/sbin/mkntfs"), QStringLiteral("-Q"), QStringLiteral("-F"), QStringLiteral("-q")}},
            {QStringLiteral("udf"), {QStringLiteral("/usr/sbin/mkudffs")}},
        };
        // ANCHORAGE_MOUNT_TYPES=cifs,nfs4 limits the run to those types.
        const QStringList only = qEnvironmentVariable("ANCHORAGE_MOUNT_TYPES").split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const auto &fs : m_cat.fileSystems()) {
            if (!fs.testable || fs.family == options::Family::Network || fs.family == options::Family::Swap
                || (!only.isEmpty() && !only.contains(fs.type))) {
                continue;
            }
            if (fs.type == QLatin1String("tmpfs")) {
                m_images.append({fs.type, QStringLiteral("tmpfs"), QString(), {}});
                continue;
            }
            if (fs.type == QLatin1String("none")) {
                const QString src = m_dir.filePath(QStringLiteral("bind-source"));
                QVERIFY(QDir().mkpath(src));
                m_images.append({fs.type, src, QString(), {}});
                continue;
            }
            // ntfs and ntfs-3g (FUSE) share the ntfs3 image's format but get their own.
            const QString maker = (fs.type == QLatin1String("ntfs") || fs.type == QLatin1String("ntfs-3g")) ? QStringLiteral("ntfs3") : fs.type;
            QVERIFY2(mkfs.contains(maker), qPrintable(QStringLiteral("no mkfs for testable type ") + fs.type));
            const QString img = m_dir.filePath(fs.type + QStringLiteral(".img"));
            QFile f(img);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(qint64(320) * 1024 * 1024)); // sparse; XFS needs 300 MB
            f.close();
            QStringList cmd = mkfs.value(maker);
            const QString program = cmd.takeFirst();
            const Run made = run(program, cmd << img);
            QVERIFY2(made.exit == 0, qPrintable(fs.type + QStringLiteral(": ") + made.out));
            const Run loop = run(QStringLiteral("/usr/sbin/losetup"), {QStringLiteral("--find"), QStringLiteral("--show"), img});
            QVERIFY2(loop.exit == 0, qPrintable(loop.out));
            const Run uuid = run(QStringLiteral("/usr/sbin/blkid"), {QStringLiteral("-o"), QStringLiteral("value"), QStringLiteral("-s"), QStringLiteral("UUID"), loop.out});
            QVERIFY2(uuid.exit == 0 && !uuid.out.isEmpty(), qPrintable(fs.type + QStringLiteral(" has no UUID: ") + uuid.out));
            m_images.append({fs.type, QStringLiteral("UUID=") + uuid.out, loop.out, {}});
        }
        // Network shares, when a test server runs on this machine (tests/vm/network.sh sets
        // it up in the VM: Samba share "anchorage" for guests, NFS export /srv/anchorage-nfs).
        if (qEnvironmentVariable("ANCHORAGE_NETWORK_TESTS") == QLatin1String("1")) {
            m_network = true;
            for (const QString &t : {QStringLiteral("cifs"), QStringLiteral("smb3")}) {
                if (!only.isEmpty() && !only.contains(t)) {
                    continue;
                }
                m_images.append({t, QStringLiteral("//127.0.0.1/anchorage"), QString(), {QStringLiteral("guest")}});
            }
            for (const QString &t : {QStringLiteral("nfs"), QStringLiteral("nfs4")}) {
                if (!only.isEmpty() && !only.contains(t)) {
                    continue;
                }
                m_images.append({t, QStringLiteral("127.0.0.1:/srv/anchorage-nfs"), QString(), {}});
            }
        }
        qInfo("Testing %lld file systems", static_cast<long long>(m_images.size()));
    }

    // A failed check returns early; never let a mount leak into the next test.
    void cleanup()
    {
        for (const Image &i : std::as_const(m_images)) {
            unmount(QStringLiteral("/mnt/anchorage-test/") + i.type);
        }
        unmount(QStringLiteral("/mnt/anchorage-svc/bind"));
        unmount(QStringLiteral("/mnt/anchorage-svc/entry"));
        unmount(QStringLiteral("/mnt/anchorage-svc/temporary"));
    }

    void cleanupTestCase()
    {
        for (const Image &i : std::as_const(m_images)) {
            unmount(QStringLiteral("/mnt/anchorage-test/") + i.type);
            if (!i.loop.isEmpty()) {
                run(QStringLiteral("/usr/sbin/losetup"), {QStringLiteral("-d"), i.loop});
            }
        }
    }

    void accessChoices_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<int>("start");
        QTest::addColumn<int>("who");
        QTest::addColumn<int>("sharing");
        for (const Image &i : std::as_const(m_images)) {
            const bool owner = access::applicable(i.type).owner;
            for (int start = 0; start < 3; ++start) {
                for (int who = 0; who < 3; ++who) {
                    for (int sharing = 0; sharing < (owner ? 4 : 1); ++sharing) {
                        QTest::addRow("%s start%d who%d sharing%d", qPrintable(i.type), start, who, sharing) << i.type << start << who << sharing;
                    }
                }
            }
        }
    }
    void accessChoices()
    {
        QFETCH(QString, type);
        QFETCH(int, start);
        QFETCH(int, who);
        QFETCH(int, sharing);
        const Image *img = image(type);
        QVERIFY(img);

        access::Settings s = access::defaultsFor(type);
        s.start = access::Start(start);
        s.who = access::Who(who);
        const bool owner = access::applicable(type).owner;
        if (owner) {
            s.uid = 1234;
            s.gid = 2345;
            s.sharing = access::Sharing(sharing);
        }
        fstab::Entry e;
        e.spec = img->spec;
        e.mountPoint = QStringLiteral("/mnt/anchorage-test/") + type;
        e.type = type;
        e.options = access::apply({}, s, type);
        if (type == QLatin1String("none")) {
            e.options.prepend(QStringLiteral("bind"));
        }
        e.options = img->base + e.options;
        QCOMPARE(access::read(e.options, type).start, s.start);

        const Run m = mountEntry(e);
        QVERIFY2(m.exit == 0, qPrintable(e.options.join(QLatin1Char(',')) + QStringLiteral(": ") + m.out));
        const auto opts = mountedOptions(e.mountPoint);
        QVERIFY(opts);
        if (s.who != access::Who::Admins) {
            // user/users imply nosuid,nodev,noexec.
            for (const char *o : {"nosuid", "nodev", "noexec"}) {
                QVERIFY2(opts->contains(QLatin1String(o)), qPrintable(opts->join(QLatin1Char(','))));
            }
        }
        if (owner && type != QLatin1String("udf")) {
            struct stat st{};
            QCOMPARE(::stat(QFile::encodeName(e.mountPoint).constData(), &st), 0);
            QCOMPARE(st.st_uid, uid_t(1234));
            QCOMPARE(st.st_gid, gid_t(2345));
            const QHash<int, mode_t> modes = {{int(access::Sharing::OwnerOnly), 0700}, {int(access::Sharing::OthersRead), 0755}, {int(access::Sharing::Everyone), 0777}};
            if (type == QLatin1String("ntfs3")) {
                // Known kernel behaviour (7.0): ntfs3 shows the top folder as 0777 whatever
                // the umask, so every user can create files there. The editor warns about
                // it; this check notices if a kernel update changes it.
                QCOMPARE(st.st_mode & 0777, mode_t(0777));
            } else if (modes.contains(sharing)) {
                QCOMPARE(st.st_mode & 0777, modes.value(sharing));
            }
        }
        unmount(e.mountPoint);

        // What systemd makes of it.
        const QString out = m_dir.filePath(QStringLiteral("units"));
        QDir(out).removeRecursively();
        QVERIFY(QDir().mkpath(out));
        QProcess gen;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SYSTEMD_FSTAB"), m_fstab);
        gen.setProcessEnvironment(env);
        gen.start(QStringLiteral("/usr/lib/systemd/system-generators/systemd-fstab-generator"), {out, out, out});
        QVERIFY(gen.waitForFinished(30000));
        const QString unit = unitName(e.mountPoint);
        QVERIFY(QFile::exists(out + QLatin1Char('/') + unit + QStringLiteral(".mount")));
        // Network shares belong to remote-fs.target, everything else to local-fs.target.
        const QString target = m_cat.family(type) == options::Family::Network ? QStringLiteral("/remote-fs.target") : QStringLiteral("/local-fs.target");
        const QString wants = out + target + QStringLiteral(".wants/");
        const QString requires_ = out + target + QStringLiteral(".requires/");
        const bool mountPulled = QFile::exists(wants + unit + QStringLiteral(".mount")) || QFile::exists(requires_ + unit + QStringLiteral(".mount"));
        const bool automountPulled = QFile::exists(wants + unit + QStringLiteral(".automount")) || QFile::exists(requires_ + unit + QStringLiteral(".automount"));
        switch (s.start) {
        case access::Start::AtBoot:
            QVERIFY(mountPulled && !automountPulled);
            QVERIFY(QFile::exists(wants + unit + QStringLiteral(".mount"))); // nofail: wanted, not required
            break;
        case access::Start::OnRequest:
            QVERIFY(!mountPulled && !automountPulled);
            break;
        case access::Start::WhenOpened:
            QVERIFY(!mountPulled && automountPulled);
            break;
        }
    }

    // The helper's mount actions (src/core/fstabservice.cpp) with the real commands, as
    // root: an fstab entry, a temporary mount, "in use", and the refusals.
    void serviceMountsForReal()
    {
        const Image *img = image(QStringLiteral("ext4"));
        if (!img) {
            QSKIP("no ext4 image in this run");
        }
        service::Paths paths;
        paths.fstabDir = m_dir.path();
        paths.fstabName = QStringLiteral("fstab");
        paths.configDir = m_dir.filePath(QStringLiteral("anchorage"));
        paths.backupDir = m_dir.filePath(QStringLiteral("anchorage/backups"));
        Caller admin;
        admin.driveAdmin = true;
        Caller standard;
        const QString mp = QStringLiteral("/mnt/anchorage-svc/entry");
        const QString tmp = QStringLiteral("/mnt/anchorage-svc/temporary");
        QFile f(m_fstab);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QStringLiteral("%1 %2 ext4 nofail 0 0\n").arg(img->spec, mp).toUtf8());
        f.close();
        const auto run = service::systemRunner();

        // fstab entry, by a standard user (who passed polkit); the folder is created.
        auto o = service::mountEntry(paths, standard, mp, run);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char(' '))));
        QVERIFY(mountedOptions(mp));
        QCOMPARE(o.createdDirs.isEmpty(), false);
        QVERIFY(!service::mountEntry(paths, standard, mp, run).ok()); // already mounted

        // In use: a program with its working directory there is named, and nothing changes.
        QProcess holder;
        holder.setWorkingDirectory(mp);
        holder.start(QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")});
        QVERIFY(holder.waitForStarted());
        o = service::unmount(paths, standard, mp, false, run);
        QVERIFY(!o.ok());
        QVERIFY2(o.processes.contains(QStringLiteral("sleep")), qPrintable(o.processes.join(QLatin1Char(','))));
        QVERIFY(mountedOptions(mp));
        holder.kill();
        holder.waitForFinished();
        o = service::unmount(paths, standard, mp, false, run);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char(' '))));
        QVERIFY(!mountedOptions(mp));

        // A bind of a folder on the drive: refused while the drive isn't mounted (mount would
        // show the empty folder), then shows the drive's files.
        const QString bindTarget = QStringLiteral("/mnt/anchorage-svc/bind");
        QFile fb(m_fstab);
        QVERIFY(fb.open(QIODevice::WriteOnly | QIODevice::Truncate));
        fb.write(QStringLiteral("%1 %2 ext4 nofail 0 0\n%2/shared %3 none bind,nofail,x-systemd.requires-mounts-for=%2 0 0\n")
                     .arg(img->spec, mp, bindTarget)
                     .toUtf8());
        fb.close();
        o = service::mountEntry(paths, standard, bindTarget, run);
        QVERIFY(!o.ok());
        QVERIFY2(o.errors.join(QString()).contains(QLatin1String("first")), qPrintable(o.errors.join(QLatin1Char(' '))));
        QVERIFY(!mountedOptions(bindTarget));
        QVERIFY(service::mountEntry(paths, standard, mp, run).ok());
        QVERIFY(QDir().mkpath(mp + QStringLiteral("/shared")));
        QFile marker(mp + QStringLiteral("/shared/marker"));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.write("on the drive");
        marker.close();
        o = service::mountEntry(paths, standard, bindTarget, run);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char(' '))));
        QFile seen(bindTarget + QStringLiteral("/marker"));
        QVERIFY(seen.open(QIODevice::ReadOnly));
        QCOMPARE(seen.readAll(), QByteArray("on the drive"));
        seen.close();
        QVERIFY(service::unmount(paths, standard, bindTarget, false, run).ok());
        QVERIFY(service::unmount(paths, standard, mp, false, run).ok());

        // Temporary mount of the same drive elsewhere: driveadmins only.
        const fstab::Entry t{img->spec, tmp, QStringLiteral("ext4"), {QStringLiteral("ro"), QStringLiteral("noatime")}, 0, 0};
        QVERIFY(!service::mountTemporary(paths, standard, t, false, run).ok());
        o = service::mountTemporary(paths, admin, t, false, run);
        QVERIFY2(o.ok(), qPrintable(o.errors.join(QLatin1Char(' '))));
        const auto opts = mountedOptions(tmp);
        QVERIFY(opts && opts->contains(QStringLiteral("ro")) && opts->contains(QStringLiteral("noatime")));
        QVERIFY(!service::unmount(paths, standard, tmp, false, run).ok()); // not in fstab: driveadmins only
        QVERIFY(service::unmount(paths, admin, tmp, false, run).ok());
        QVERIFY(!mountedOptions(tmp));

        // A wrong file system type: mount's own message comes back.
        fstab::Entry wrong = t;
        wrong.type = QStringLiteral("vfat");
        o = service::mountTemporary(paths, admin, wrong, false, run);
        QVERIFY(!o.ok());
        QVERIFY2(o.errors.join(QString()).contains(QLatin1String("wrong fs type")) || o.errors.join(QString()).contains(QLatin1String("bad superblock")),
                 qPrintable(o.errors.join(QLatin1Char(' '))));
        QVERIFY(!mountedOptions(tmp));

        // Pseudo file systems are refused even for administrators.
        Caller root;
        root.sudo = true;
        QVERIFY(!service::unmount(paths, root, QStringLiteral("/proc"), true, run).ok());
    }

    void optionSweep_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QStringList>("opts");
        for (const Image &i : std::as_const(m_images)) {
            for (const auto *o : m_cat.optionsFor(i.type)) {
                const bool networkOnly = o->testSkip == QLatin1String("network");
                if (!o->testSkip.isEmpty() && !(networkOnly && m_network)) {
                    continue;
                }
                if (networkOnly && m_cat.family(i.type) != options::Family::Network) {
                    continue;
                }
                std::optional<QString> value;
                if (o->takesValue()) {
                    const QString v = !o->testValue.isEmpty() ? o->testValue : !o->example.isEmpty() ? o->example : o->choices.value(0);
                    if (!v.isEmpty() || o->kind == options::Kind::Value) {
                        value = v;
                    }
                }
                QStringList opts = i.base + o->testWith;
                opts.append(options::join(o->name, value));
                if (i.type == QLatin1String("none") && o->exclusive != QLatin1String("bind")) {
                    opts.prepend(QStringLiteral("bind"));
                }
                QTest::addRow("%s %s", qPrintable(i.type), qPrintable(opts.join(QLatin1Char(',')))) << i.type << opts;
                // Flags that may take a value are tried both ways.
                if (o->kind == options::Kind::FlagOrValue && value) {
                    QStringList bare = i.base + o->testWith;
                    bare.append(o->name);
                    if (i.type == QLatin1String("none")) {
                        bare.prepend(QStringLiteral("bind"));
                    }
                    QTest::addRow("%s %s (no value)", qPrintable(i.type), qPrintable(o->name)) << i.type << bare;
                }
            }
        }
    }
    void optionSweep()
    {
        QFETCH(QString, type);
        QFETCH(QStringList, opts);
        const Image *img = image(type);
        QVERIFY(img);
        QVERIFY2(!fstab::hasErrors(options::check(opts, type)), qPrintable(opts.join(QLatin1Char(','))));
        fstab::Entry e{img->spec, QStringLiteral("/mnt/anchorage-test/") + type, type, opts, 0, 0};
        const Run m = mountEntry(e);
        unmount(e.mountPoint);
        QVERIFY2(m.exit == 0, qPrintable(QStringLiteral("mount -o %1 (%2): %3").arg(opts.join(QLatin1Char(',')), type, m.out)));
    }
};

QTEST_GUILESS_MAIN(MountTest)
#include "mounttest.moc"
