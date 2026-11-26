// SPDX-License-Identifier: GPL-3.0-or-later
#include "fstabservice.h"

#include "mountplan.h"
#include "mountpolicy.h"
#include "mounts.h"
#include "paths.h"
#include "safefs.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <sys/stat.h>

namespace anchorage::service
{

namespace
{
const QString lockName = QStringLiteral("fstab.lock");
const QString blockedPathsName = QStringLiteral("blocked-paths");

Outcome failed(const QString &error)
{
    Outcome o;
    o.errors.append(error);
    return o;
}

Outcome failed(const QStringList &errors)
{
    Outcome o;
    o.errors = errors;
    return o;
}

// Extra protected locations, one absolute path per line; '#' starts a comment.
QStringList extraBlocked(const Paths &paths)
{
    auto data = safefs::readFile(paths.configDir, blockedPathsName, qsizetype(64) * 1024);
    if (!data) {
        return {};
    }
    QStringList out;
    for (const QByteArray &line : data->split('\n')) {
        const QString t = QString::fromUtf8(line).trimmed();
        if (!t.isEmpty() && !t.startsWith(QLatin1Char('#'))) {
            out.append(t);
        }
    }
    return out;
}

Status ensureDir(const QString &path, mode_t mode)
{
    return safefs::makeDirectories(path, mode, safefs::Owner{0, 0});
}

// Copies the current file into the backup folder and drops the oldest extra backups.
Result<QString> backup(const Paths &paths, const QByteArray &current)
{
    if (Status s = ensureDir(paths.backupDir, 0755); !s) {
        return Result<QString>::failure(s.error);
    }
    const QString base = QStringLiteral("fstab-") + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH-mm-ss")) + QLatin1Char('Z');
    auto existing = safefs::listDirectory(paths.backupDir);
    if (!existing) {
        return Result<QString>::failure(existing.error());
    }
    // Several backups in the same second get increasing "-N" suffixes, so the newest
    // always sorts first even after older ones were pruned.
    int highest = -1;
    for (const QString &n : *existing) {
        if (n == base) {
            highest = std::max(highest, 0);
        } else if (n.startsWith(base + QLatin1Char('-')) && isBackupName(n)) {
            highest = std::max(highest, n.mid(base.size() + 1).toInt());
        }
    }
    const QString name = highest < 0 ? base : base + QLatin1Char('-') + QString::number(highest + 1);
    if (Status s = safefs::writeFileAtomic(paths.backupDir, name, current, 0644, {0, 0}); !s) {
        return Result<QString>::failure(s.error);
    }
    // The number to keep comes from the settings file; lowering it takes effect here, at
    // the next write, never straight away (SPEC §9).
    const int keep = readSettings(paths).keepBackups;
    if (auto all = listBackups(paths); all && all->size() > keep) {
        for (qsizetype i = keep; i < all->size(); ++i) {
            (void)safefs::removeFile(paths.backupDir, all->at(i));
        }
    }
    return name;
}

// The owner new mount-point folders get: the home folder's owner inside /home/<user>/,
// otherwise root.
safefs::Owner ownerFor(const Paths &paths, const QString &mountPoint)
{
    static const QRegularExpression home(QStringLiteral("^(/home/[^/]+)/"));
    const auto m = home.match(mountPoint);
    if (m.hasMatch()) {
        struct stat st {
        };
        auto fd = safefs::openDirectory(paths.mountRoot + m.captured(1));
        if (fd && ::fstat(fd->get(), &st) == 0) {
            return {st.st_uid, st.st_gid};
        }
    }
    return {0, 0};
}

// Writes `proposed` after the checks every write shares. Caller holds the lock.
Outcome commit(const Paths &paths, const QByteArray &current, const QByteArray &proposed, const QStringList &newMountPoints,
               const QStringList &overrideMountPoints, bool createMountPoints)
{
    Outcome out;
    // Override-zone mount points must be empty folders, or not exist yet (SPEC §7).
    for (const QString &mp : overrideMountPoints) {
        if (safefs::exists(paths.mountRoot + mp)) {
            auto empty = safefs::isEmptyDirectory(paths.mountRoot + mp);
            if (!empty) {
                out.errors.append(empty.error());
            } else if (!*empty) {
                out.errors.append(QStringLiteral("%1 is not empty. Outside the usual mount locations, only empty folders can be used.").arg(mp));
            }
        }
    }
    if (!out.ok()) {
        return out;
    }

    auto name = backup(paths, current);
    if (!name) {
        return failed(QStringLiteral("Could not back up the current file, so nothing was changed: %1").arg(name.error()));
    }
    out.backupName = *name;

    if (createMountPoints) {
        for (const QString &mp : newMountPoints) {
            if (Status s = safefs::makeDirectories(paths.mountRoot + mp, 0755, ownerFor(paths, mp), &out.createdDirs); !s) {
                out.errors.append(s.error);
                return out;
            }
        }
    }
    if (Status s = safefs::writeFileAtomic(paths.fstabDir, paths.fstabName, proposed, 0644, {0, 0}); !s) {
        out.errors.append(s.error);
    }
    return out;
}
} // namespace

Paths systemPaths()
{
    const QFileInfo fstabFile(QString::fromUtf8(paths::fstab));
    Paths p;
    p.fstabDir = fstabFile.path();
    p.fstabName = fstabFile.fileName();
    p.configDir = QString::fromUtf8(paths::configDir);
    p.backupDir = QString::fromUtf8(paths::backupDir);
    p.credentialsDir = QString::fromUtf8(paths::credentialsDir);
    p.configName = QFileInfo(QString::fromUtf8(paths::configFile)).fileName();
    return p;
}

QByteArray fingerprint(const QByteArray &content)
{
    return QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
}

Result<QByteArray> readFstab(const Paths &paths)
{
    return safefs::readFile(paths.fstabDir, paths.fstabName, fstab::maxFileSize);
}

Outcome writeFstab(const Paths &paths, const Caller &caller, const QByteArray &baselineFingerprint, const QByteArray &proposed,
                   bool confirmProtected, bool createMountPoints)
{
    if (Status s = ensureDir(paths.configDir, 0755); !s) {
        return failed(s.error);
    }
    auto guard = safefs::lock(paths.configDir, lockName);
    if (!guard) {
        return failed(guard.error());
    }
    auto current = readFstab(paths);
    if (!current) {
        return failed(current.error());
    }
    if (fingerprint(*current) != baselineFingerprint) {
        return failed(QStringLiteral("/etc/fstab was changed by something else since it was opened. Reload and try again."));
    }
    if (*current == proposed) {
        return {};
    }
    const policy::Plan plan = policy::planFstabWrite(*current, proposed, caller, confirmProtected, extraBlocked(paths));
    if (!plan.ok()) {
        return failed(plan.errors);
    }
    return commit(paths, *current, proposed, plan.newMountPoints, plan.overrideMountPoints, createMountPoints);
}

Outcome addSharedMount(const Paths &paths, const Caller &caller, const fstab::Entry &entry)
{
    if (Status s = ensureDir(paths.configDir, 0755); !s) {
        return failed(s.error);
    }
    auto guard = safefs::lock(paths.configDir, lockName);
    if (!guard) {
        return failed(guard.error());
    }
    auto current = readFstab(paths);
    if (!current) {
        return failed(current.error());
    }
    const policy::SharedMountPlan plan = policy::planSharedMount(entry, caller, *current, paths.credentialsDir);
    if (!plan.ok()) {
        return failed(plan.errors);
    }
    fstab::File file = fstab::File::parse(*current);
    QList<fstab::Issue> issues;
    if (file.append(plan.entry, &issues) < 0) {
        QStringList errors;
        for (const auto &i : issues) {
            errors.append(i.message);
        }
        return failed(errors.isEmpty() ? QStringList{QStringLiteral("The entry could not be added.")} : errors);
    }
    // The finished file must still pass the general rules too (defence in depth).
    const QByteArray proposed = file.serialize();
    Caller limited = caller;
    limited.sudo = false;
    limited.driveAdmin = false;
    const policy::Plan check = policy::planFstabWrite(*current, proposed, limited, false, extraBlocked(paths));
    if (!check.ok()) {
        return failed(check.errors);
    }
    return commit(paths, *current, proposed, {plan.entry.mountPoint}, {}, true);
}

Outcome removeSharedMount(const Paths &paths, const Caller &caller, const QString &mountPoint)
{
    if (Status s = ensureDir(paths.configDir, 0755); !s) {
        return failed(s.error);
    }
    auto guard = safefs::lock(paths.configDir, lockName);
    if (!guard) {
        return failed(guard.error());
    }
    auto current = readFstab(paths);
    if (!current) {
        return failed(current.error());
    }
    if (const QStringList errors = policy::checkSharedMountRemoval(*current, mountPoint, caller); !errors.isEmpty()) {
        return failed(errors);
    }
    fstab::File file = fstab::File::parse(*current);
    if (!file.remove(file.findMountPoint(mountPoint))) {
        return failed(QStringLiteral("The entry could not be removed."));
    }
    return commit(paths, *current, file.serialize(), {}, {}, false);
}

settings::Settings readSettings(const Paths &paths)
{
    auto data = safefs::readFile(paths.configDir, paths.configName, settings::maxFileSize);
    return data ? settings::parse(*data) : settings::Settings{};
}

Outcome writeSettings(const Paths &paths, const Caller &caller, const settings::Settings &s)
{
    if (!caller.fullAccess()) {
        return failed(QStringLiteral("Only driveadmins and administrators can change Anchorage's settings."));
    }
    if (const QStringList errors = settings::validate(s); !errors.isEmpty()) {
        return failed(errors);
    }
    if (Status st = ensureDir(paths.configDir, 0755); !st) {
        return failed(st.error);
    }
    auto guard = safefs::lock(paths.configDir, lockName);
    if (!guard) {
        return failed(guard.error());
    }
    if (Status st = safefs::writeFileAtomic(paths.configDir, paths.configName, settings::serialize(s), 0644, {0, 0}); !st) {
        return failed(st.error);
    }
    return {};
}

Runner systemRunner()
{
    return [](const QString &program, const QStringList &args) {
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        // A fixed, minimal environment: nothing from the caller reaches mount's helpers.
        QProcessEnvironment env;
        env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/sbin:/usr/bin:/sbin:/bin"));
        env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        p.setProcessEnvironment(env);
        p.start(program, args);
        CommandResult r;
        if (!p.waitForStarted(10000)) {
            r.output = QStringLiteral("Could not start %1.").arg(program);
            return r;
        }
        if (!p.waitForFinished(120000)) {
            p.kill();
            p.waitForFinished();
            r.output = QStringLiteral("%1 did not finish within 2 minutes.").arg(program);
            return r;
        }
        r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        r.output = QString::fromUtf8(p.readAll()).trimmed();
        return r;
    };
}

namespace
{
const QString kMount = QStringLiteral("/usr/bin/mount");
const QString kUmount = QStringLiteral("/usr/bin/umount");
const QString kSystemctl = QStringLiteral("/usr/bin/systemctl");
const QString kJournalctl = QStringLiteral("/usr/bin/journalctl");

QList<mounts::Mount> currentMounts(const Paths &paths)
{
    auto m = mounts::readCurrent(paths.mountInfo);
    return m ? *m : QList<mounts::Mount>{};
}

// Creates a missing mount-point folder with the rules used when saving (SPEC §7).
Status ensureMountPoint(const Paths &paths, const Caller &caller, const QString &mountPoint, bool mustBeEmpty, QStringList *created)
{
    const QString real = paths.mountRoot + mountPoint;
    if (safefs::exists(real)) {
        if (mustBeEmpty) {
            auto empty = safefs::isEmptyDirectory(real);
            if (!empty) {
                return Status::failure(empty.error());
            }
            if (!*empty) {
                return Status::failure(QStringLiteral("%1 is not empty. Outside the usual mount locations, only empty folders can be used.").arg(mountPoint));
            }
        }
        return safefs::openDirectory(real) ? Status::success() : Status::failure(QStringLiteral("%1 is not a folder, or is reached through a link.").arg(mountPoint));
    }
    const auto zone = policy::classifyMountPoint(mountPoint, extraBlocked(paths)).zone;
    const bool allowed = zone == policy::Zone::Safe || (zone == policy::Zone::Override && caller.fullAccess()) || (zone == policy::Zone::Blocked && caller.sudo);
    if (!allowed) {
        return Status::failure(QStringLiteral("The folder %1 doesn't exist, and you can't create folders there.").arg(mountPoint));
    }
    return safefs::makeDirectories(real, 0755, ownerFor(paths, mountPoint), created);
}

// systemd only says "see journalctl": fetch the unit's last messages for the real reason.
QString journalReason(const Runner &run, const QString &unit)
{
    const CommandResult j = run(kJournalctl, {QStringLiteral("--no-pager"), QStringLiteral("-o"), QStringLiteral("cat"), QStringLiteral("-b"),
                                              QStringLiteral("-n"), QStringLiteral("8"), QStringLiteral("-u"), unit});
    return j.output.trimmed();
}
} // namespace

MountOutcome mountEntry(const Paths &paths, const Caller &caller, const QString &mountPoint, const Runner &run)
{
    MountOutcome out;
    auto fstabData = readFstab(paths);
    if (!fstabData) {
        out.errors.append(fstabData.error());
        return out;
    }
    const mounting::MountPlan plan = mounting::planMountEntry(*fstabData, mountPoint, caller, currentMounts(paths));
    if (!plan.ok()) {
        out.errors = plan.errors;
        return out;
    }
    if (Status s = ensureMountPoint(paths, caller, mountPoint, false, &out.createdDirs); !s) {
        out.errors.append(s.error);
        return out;
    }
    if (plan.method == mounting::Method::Systemd) {
        const QString unit = mounting::unitName(mountPoint) + QStringLiteral(".mount");
        const CommandResult r = run(kSystemctl, {QStringLiteral("start"), unit});
        if (r.exitCode != 0) {
            const QString why = journalReason(run, unit);
            out.errors.append(QStringLiteral("Mounting %1 failed.%2").arg(mountPoint, why.isEmpty() ? QString() : QLatin1Char('\n') + why));
            return out;
        }
    } else {
        const QString fstabPath = paths.fstabDir + QLatin1Char('/') + paths.fstabName;
        const CommandResult r = run(kMount, {QStringLiteral("--fstab"), fstabPath, mountPoint});
        if (r.exitCode != 0) {
            out.errors.append(r.output.isEmpty() ? QStringLiteral("Mounting %1 failed.").arg(mountPoint) : r.output);
            return out;
        }
    }
    // With nofail, mount reports success for a device that isn't there and mounts
    // nothing (found by tests/vm/editor_scenarios.py). Check what really happened.
    if (!mounting::mountedAt(currentMounts(paths), mountPoint)) {
        out.errors.append(plan.entry.options.contains(QStringLiteral("nofail"))
                              ? QStringLiteral("Nothing was mounted at %1: the device %2 isn't connected (\"start even if missing\" hides the error).")
                                    .arg(mountPoint, plan.entry.spec)
                              : QStringLiteral("Nothing was mounted at %1.").arg(mountPoint));
    }
    return out;
}

MountOutcome mountTemporary(const Paths &paths, const Caller &caller, const fstab::Entry &entry, bool confirmProtected, const Runner &run)
{
    MountOutcome out;
    auto fstabData = readFstab(paths);
    if (!fstabData) {
        out.errors.append(fstabData.error());
        return out;
    }
    const mounting::TemporaryPlan plan =
        mounting::planTemporaryMount(entry, caller, confirmProtected, *fstabData, currentMounts(paths), extraBlocked(paths));
    if (!plan.ok()) {
        out.errors = plan.errors;
        return out;
    }
    if (Status s = ensureMountPoint(paths, caller, entry.mountPoint, plan.overrideZone, &out.createdDirs); !s) {
        out.errors.append(s.error);
        return out;
    }
    QStringList args = {QStringLiteral("-t"), entry.type};
    if (!entry.options.isEmpty()) {
        args << QStringLiteral("-o") << entry.options.join(QLatin1Char(','));
    }
    args << entry.spec << entry.mountPoint;
    const CommandResult r = run(kMount, args);
    if (r.exitCode != 0) {
        out.errors.append(r.output.isEmpty() ? QStringLiteral("Mounting %1 failed.").arg(entry.mountPoint) : r.output);
    } else if (!mounting::mountedAt(currentMounts(paths), entry.mountPoint)) {
        out.errors.append(QStringLiteral("Nothing was mounted at %1.").arg(entry.mountPoint));
    }
    return out;
}

MountOutcome unmount(const Paths &paths, const Caller &caller, const QString &mountPoint, bool confirmSystem, const Runner &run)
{
    MountOutcome out;
    auto fstabData = readFstab(paths);
    if (!fstabData) {
        out.errors.append(fstabData.error());
        return out;
    }
    const mounting::UnmountPlan plan = mounting::planUnmount(*fstabData, currentMounts(paths), mountPoint, caller, confirmSystem);
    if (!plan.ok()) {
        out.errors = plan.errors;
        return out;
    }
    CommandResult r;
    if (plan.method == mounting::Method::Systemd) {
        const QString unit = mounting::unitName(mountPoint) + QStringLiteral(".mount");
        r = run(kSystemctl, {QStringLiteral("stop"), unit});
        if (r.exitCode != 0) {
            r.output = journalReason(run, unit);
        }
    } else {
        r = run(kUmount, {mountPoint});
    }
    if (r.exitCode == 0 && !mounting::mountedAt(currentMounts(paths), mountPoint)) {
        return out;
    }
    if (r.exitCode == 0) {
        r.output = QStringLiteral("%1 is still mounted.").arg(mountPoint);
    }
    out.processes = processesUsing(paths.mountRoot + mountPoint, paths.procRoot);
    if (!out.processes.isEmpty() || r.output.contains(QLatin1String("busy"))) {
        out.errors.append(out.processes.isEmpty() ? QStringLiteral("%1 is in use.").arg(mountPoint)
                                                  : QStringLiteral("%1 is in use by: %2.").arg(mountPoint, out.processes.join(QStringLiteral(", "))));
    } else {
        out.errors.append(r.output.isEmpty() ? QStringLiteral("Unmounting %1 failed.").arg(mountPoint) : r.output);
    }
    return out;
}

QStringList processesUsing(const QString &path, const QString &procRoot)
{
    struct stat target {
    };
    if (::stat(QFile::encodeName(path).constData(), &target) != 0) {
        return {};
    }
    QSet<QString> names;
    const auto onTarget = [&target](const QString &p) {
        struct stat st {
        };
        return ::stat(QFile::encodeName(p).constData(), &st) == 0 && st.st_dev == target.st_dev;
    };
    const QDir proc(procRoot);
    for (const QString &pid : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool isNumber = false;
        pid.toInt(&isNumber);
        if (!isNumber) {
            continue;
        }
        const QString base = procRoot + QLatin1Char('/') + pid;
        bool uses = onTarget(base + QStringLiteral("/cwd")) || onTarget(base + QStringLiteral("/root"));
        if (!uses) {
            QDirIterator fds(base + QStringLiteral("/fd"), QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
            while (!uses && fds.hasNext()) {
                uses = onTarget(fds.next());
            }
        }
        if (uses) {
            QFile comm(base + QStringLiteral("/comm"));
            if (comm.open(QIODevice::ReadOnly)) {
                const QString name = QString::fromUtf8(comm.read(64)).trimmed();
                if (!name.isEmpty()) {
                    names.insert(name);
                }
            }
        }
    }
    QStringList out(names.cbegin(), names.cend());
    out.sort();
    return out;
}

bool isBackupName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^fstab-\\d{4}-\\d{2}-\\d{2}T\\d{2}-\\d{2}-\\d{2}Z(-\\d{1,4})?$"));
    return re.match(name).hasMatch();
}

Result<QStringList> listBackups(const Paths &paths)
{
    auto names = safefs::listDirectory(paths.backupDir);
    if (!names) {
        return Result<QStringList>::failure(names.error());
    }
    QStringList out;
    for (const QString &n : *names) {
        if (isBackupName(n)) {
            out.append(n);
        }
    }
    // Newest first. "-N" suffixes sort after the plain name of the same second.
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        const auto key = [](const QString &s) {
            const qsizetype z = s.indexOf(QLatin1Char('Z'));
            return std::pair(s.left(z), s.mid(z + 2).toInt());
        };
        return key(a) > key(b);
    });
    return out;
}

Outcome restoreBackup(const Paths &paths, const Caller &caller, const QString &backupName, bool confirmProtected)
{
    if (!isBackupName(backupName)) {
        return failed(QStringLiteral("That is not a backup name."));
    }
    auto data = safefs::readFile(paths.backupDir, backupName, fstab::maxFileSize);
    if (!data) {
        return failed(data.error());
    }
    auto current = readFstab(paths);
    if (!current) {
        return failed(current.error());
    }
    // Same checks as any other write, against the file as it is right now.
    return writeFstab(paths, caller, fingerprint(*current), *data, confirmProtected, false);
}

} // namespace anchorage::service
