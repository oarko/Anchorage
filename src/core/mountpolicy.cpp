// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountpolicy.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace anchorage::policy
{

namespace
{
using fstab::Entry;

// Mounting directly on these would hide or replace part of the operating system.
const QStringList &blockedExact()
{
    static const QStringList list = {
        QStringLiteral("/"),     QStringLiteral("/bin"),   QStringLiteral("/boot"),     QStringLiteral("/dev"),
        QStringLiteral("/etc"),  QStringLiteral("/home"),  QStringLiteral("/lib"),      QStringLiteral("/lib32"),
        QStringLiteral("/lib64"), QStringLiteral("/libx32"), QStringLiteral("/media"),  QStringLiteral("/mnt"),
        QStringLiteral("/opt"),  QStringLiteral("/proc"),  QStringLiteral("/root"),     QStringLiteral("/run"),
        QStringLiteral("/run/media"), QStringLiteral("/sbin"), QStringLiteral("/snap"), QStringLiteral("/srv"),
        QStringLiteral("/sys"),  QStringLiteral("/tmp"),   QStringLiteral("/usr"),      QStringLiteral("/var"),
        QStringLiteral("/var/lib"), QStringLiteral("/var/log"),
    };
    return list;
}

// Whole subtrees where a mount would hand control of the computer to whoever controls
// the mounted file system (SPEC §7).
const QStringList &blockedTrees()
{
    static const QStringList list = {
        QStringLiteral("/bin"),          QStringLiteral("/boot"),        QStringLiteral("/dev"),
        QStringLiteral("/lib"),          QStringLiteral("/lib32"),       QStringLiteral("/lib64"),
        QStringLiteral("/libx32"),       QStringLiteral("/proc"),        QStringLiteral("/root"),
        QStringLiteral("/sbin"),         QStringLiteral("/snap"),        QStringLiteral("/sys"),
        QStringLiteral("/usr"),          QStringLiteral("/etc/anchorage"), QStringLiteral("/etc/apparmor.d"),
        QStringLiteral("/etc/exports.d"), QStringLiteral("/etc/pam.d"),  QStringLiteral("/etc/polkit-1"),
        QStringLiteral("/etc/samba"),    QStringLiteral("/etc/security"), QStringLiteral("/etc/ssh"),
        QStringLiteral("/etc/sudoers.d"), QStringLiteral("/etc/systemd"), QStringLiteral("/var/lib/apt"),
        QStringLiteral("/var/lib/dpkg"), QStringLiteral("/var/lib/polkit-1"), QStringLiteral("/var/lib/sudo"),
    };
    return list;
}

bool isUnder(const QString &path, const QString &tree)
{
    return path == tree || path.startsWith(tree + QLatin1Char('/'));
}

int depth(const QString &path)
{
    return path == QLatin1String("/") ? 0 : static_cast<int>(path.count(QLatin1Char('/')));
}

QString optionName(const QString &option)
{
    const qsizetype eq = option.indexOf(QLatin1Char('='));
    return eq < 0 ? option : option.left(eq);
}

QString optionValue(const QString &option)
{
    const qsizetype eq = option.indexOf(QLatin1Char('='));
    return eq < 0 ? QString() : option.mid(eq + 1);
}

bool hasOption(const Entry &e, const QString &name)
{
    return std::any_of(e.options.cbegin(), e.options.cend(), [&](const QString &o) {
        return optionName(o) == name;
    });
}

bool isSwapMountPoint(const Entry &e)
{
    return e.type == QLatin1String("swap") && (e.mountPoint == QLatin1String("none") || e.mountPoint == QLatin1String("swap"));
}

// Removes each element of `remove` once from `from` (multiset difference).
QList<Entry> difference(QList<Entry> from, const QList<Entry> &remove)
{
    for (const Entry &r : remove) {
        const auto it = std::find(from.begin(), from.end(), r);
        if (it != from.end()) {
            from.erase(it);
        }
    }
    return from;
}

QList<Entry> entriesOf(const fstab::File &f)
{
    QList<Entry> out;
    for (const int i : f.entryLines()) {
        if (const auto &entry = f.lines().at(static_cast<std::size_t>(i)).entry) {
            out.append(*entry);
        }
    }
    return out;
}

const QRegularExpression &sharedMountPointRe()
{
    // /mnt/<one folder>: letters, digits, space, . _ - ; not "." or "..".
    static const QRegularExpression re(QStringLiteral("^/mnt/(?!\\.{1,2}$)[\\p{L}\\p{N} ._-]{1,64}$"));
    return re;
}
} // namespace

QString checkPathSyntax(const QString &path)
{
    if (path.isEmpty()) {
        return QStringLiteral("The path is empty.");
    }
    if (!path.startsWith(QLatin1Char('/'))) {
        return QStringLiteral("Use a full path starting with /.");
    }
    if (path.toUtf8().size() > 4095) {
        return QStringLiteral("The path is too long.");
    }
    for (const QChar c : path) {
        const auto cat = c.category();
        if (cat == QChar::Other_Control || cat == QChar::Separator_Line || cat == QChar::Separator_Paragraph) {
            return QStringLiteral("The path contains a control character or line break.");
        }
    }
    if (path.contains(QLatin1Char('#'))) {
        return QStringLiteral("The path must not contain '#'.");
    }
    if (path != QLatin1String("/")) {
        if (path.endsWith(QLatin1Char('/'))) {
            return QStringLiteral("Remove the trailing slash.");
        }
        const QStringList parts = path.mid(1).split(QLatin1Char('/'));
        for (const QString &p : parts) {
            if (p.isEmpty()) {
                return QStringLiteral("Remove the doubled slash.");
            }
            if (p == QLatin1String(".") || p == QLatin1String("..")) {
                return QStringLiteral("\".\" and \"..\" are not allowed in a path.");
            }
            if (p.toUtf8().size() > 255) {
                return QStringLiteral("A folder name in the path is too long.");
            }
        }
    }
    return {};
}

ZoneResult classifyMountPoint(const QString &path, const QStringList &extraBlocked)
{
    if (const QString err = checkPathSyntax(path); !err.isEmpty()) {
        return {Zone::Invalid, err};
    }

    // Dolphin/UDisks per-user location: /run/media/<user>/<name>
    if (path.startsWith(QLatin1String("/run/media/")) && depth(path) >= 4) {
        return {Zone::Safe, QStringLiteral("Personal mount location.")};
    }
    if (blockedExact().contains(path)) {
        return {Zone::Blocked, QStringLiteral("%1 is a protected system folder.").arg(path)};
    }
    for (const QString &tree : blockedTrees()) {
        if (isUnder(path, tree)) {
            return {Zone::Blocked, QStringLiteral("%1 is inside the protected location %2.").arg(path, tree)};
        }
    }
    if (isUnder(path, QStringLiteral("/run"))) {
        return {Zone::Blocked, QStringLiteral("/run is managed by the system.")};
    }
    for (const QString &tree : extraBlocked) {
        if (checkPathSyntax(tree).isEmpty() && isUnder(path, tree)) {
            return {Zone::Blocked, QStringLiteral("%1 is protected by this computer's administrator.").arg(tree)};
        }
    }
    if (path.startsWith(QLatin1String("/home/")) && depth(path) == 2) {
        return {Zone::Override, QStringLiteral("This would cover a whole home folder.")};
    }
    for (const auto *prefix : {"/mnt/", "/media/", "/srv/", "/home/"}) {
        if (path.startsWith(QLatin1String(prefix))) {
            return {Zone::Safe, QStringLiteral("Usual place for mounts.")};
        }
    }
    return {Zone::Override, QStringLiteral("Outside the usual mount locations.")};
}

bool isSystemEntry(const Entry &entry)
{
    if (entry.type == QLatin1String("swap")) {
        return true;
    }
    const QString &mp = entry.mountPoint;
    if (blockedExact().contains(mp)) {
        return true;
    }
    for (const auto *tree : {"/boot", "/usr", "/var"}) {
        if (isUnder(mp, QLatin1String(tree))) {
            return true;
        }
    }
    return false;
}

bool isNetworkType(const QString &type)
{
    return type == QLatin1String("cifs") || type == QLatin1String("smb3") || type == QLatin1String("nfs") || type == QLatin1String("nfs4");
}

Plan planFstabWrite(QByteArrayView current, QByteArrayView proposed, const Caller &caller, bool confirmProtected, const QStringList &extraBlocked)
{
    Plan plan;
    if (proposed.size() > fstab::maxFileSize) {
        plan.errors.append(QStringLiteral("The new file is too large."));
        return plan;
    }

    const fstab::File oldFile = fstab::File::parse(current);
    const fstab::File newFile = fstab::File::parse(proposed);

    // New lines that don't parse are refused; broken lines that were already there may stay.
    QList<QByteArray> oldRaw;
    for (const auto &l : oldFile.lines()) {
        oldRaw.append(l.raw);
    }
    int lineNo = 0;
    for (const auto &l : newFile.lines()) {
        ++lineNo;
        if (l.kind == fstab::LineKind::Invalid && !oldRaw.contains(l.raw)) {
            plan.errors.append(QStringLiteral("Line %1 can't be read: %2").arg(lineNo).arg(l.error));
        }
        if (l.raw.size() > fstab::maxLineLength && !oldRaw.contains(l.raw)) {
            plan.errors.append(QStringLiteral("Line %1 is too long.").arg(lineNo));
        }
    }

    const QList<Entry> oldEntries = entriesOf(oldFile);
    const QList<Entry> newEntries = entriesOf(newFile);
    plan.added = difference(newEntries, oldEntries);
    plan.removed = difference(oldEntries, newEntries);

    QSet<QString> oldMountPoints;
    for (const Entry &e : oldEntries) {
        oldMountPoints.insert(e.mountPoint);
    }

    for (const Entry &e : plan.removed) {
        if (isSystemEntry(e) && !caller.fullAccess()) {
            plan.errors.append(QStringLiteral("Only driveadmins or administrators can change the system entry for %1.").arg(e.mountPoint));
        }
    }

    for (const Entry &e : plan.added) {
        for (const fstab::Issue &issue : fstab::validate(e)) {
            if (issue.severity == fstab::Issue::Severity::Error) {
                plan.errors.append(QStringLiteral("%1: %2").arg(e.mountPoint, issue.message));
            }
        }
        if (isSystemEntry(e) && !caller.fullAccess()) {
            plan.errors.append(QStringLiteral("Only driveadmins or administrators can add or change the system entry for %1.").arg(e.mountPoint));
            continue;
        }
        if (isSwapMountPoint(e) || oldMountPoints.contains(e.mountPoint)) {
            continue; // editing an existing mount point, or swap: no new location to check
        }

        // A mount point that is new to this file.
        const qsizetype sameMp = std::count_if(newEntries.cbegin(), newEntries.cend(), [&](const Entry &n) {
            return n.mountPoint == e.mountPoint;
        });
        if (sameMp > 1) {
            plan.errors.append(QStringLiteral("%1 is used by more than one entry.").arg(e.mountPoint));
        }
        const ZoneResult zone = classifyMountPoint(e.mountPoint, extraBlocked);
        switch (zone.zone) {
        case Zone::Invalid:
            plan.errors.append(QStringLiteral("%1: %2").arg(e.mountPoint, zone.reason));
            break;
        case Zone::Blocked:
            if (!caller.sudo) {
                plan.errors.append(QStringLiteral("%1 Only administrators can mount there.").arg(zone.reason));
            } else if (!confirmProtected) {
                plan.errors.append(QStringLiteral("%1 Confirm by typing the path to mount there.").arg(zone.reason));
            } else {
                plan.newMountPoints.append(e.mountPoint);
            }
            break;
        case Zone::Override:
            if (!caller.fullAccess()) {
                plan.errors.append(QStringLiteral("%1 Only driveadmins or administrators can mount there.").arg(zone.reason));
            } else {
                plan.newMountPoints.append(e.mountPoint);
                plan.overrideMountPoints.append(e.mountPoint);
            }
            break;
        case Zone::Safe:
            plan.newMountPoints.append(e.mountPoint);
            break;
        }
    }
    return plan;
}

SharedMountPlan planSharedMount(const Entry &requested, const Caller &caller, QByteArrayView current, const QString &credentialsDir)
{
    SharedMountPlan plan;
    plan.entry = requested;
    const auto fail = [&plan](const QString &msg) {
        plan.errors.append(msg);
    };

    if (!caller.netMount && !caller.fullAccess()) {
        fail(QStringLiteral("Only members of the netmount group can create shared network mounts."));
        return plan;
    }
    if (!isNetworkType(requested.type)) {
        fail(QStringLiteral("Shared mounts must be SMB (cifs) or NFS shares."));
        return plan;
    }
    if (!sharedMountPointRe().match(requested.mountPoint).hasMatch()) {
        fail(QStringLiteral("Shared mounts go directly inside /mnt, e.g. /mnt/games."));
    }

    static const QRegularExpression smbSpec(QStringLiteral("^//[^/\\s]+/[^\\s]+$"));
    static const QRegularExpression nfsSpec(QStringLiteral("^([^:/\\s\\[\\]]+|\\[[0-9A-Fa-f:.]+\\]):/[^\\s]*$"));
    const bool smb = requested.type == QLatin1String("cifs") || requested.type == QLatin1String("smb3");
    // Shares may contain spaces; check the shape with spaces removed.
    QString shape = requested.spec;
    shape.remove(QLatin1Char(' '));
    if (!(smb ? smbSpec : nfsSpec).match(shape).hasMatch()) {
        fail(smb ? QStringLiteral("An SMB share looks like //server/share.") : QStringLiteral("An NFS share looks like server:/path."));
    }

    // Only options known to be harmless for an unprivileged requester (SPEC §4.2).
    static const QSet<QString> generic = {
        QStringLiteral("ro"), QStringLiteral("rw"), QStringLiteral("noexec"), QStringLiteral("nosuid"), QStringLiteral("nodev"),
        QStringLiteral("noatime"), QStringLiteral("relatime"), QStringLiteral("nofail"), QStringLiteral("_netdev"),
        QStringLiteral("noauto"), QStringLiteral("auto"), QStringLiteral("user"), QStringLiteral("users"),
        QStringLiteral("x-systemd.automount"), QStringLiteral("x-systemd.idle-timeout"), QStringLiteral("x-systemd.mount-timeout"),
        QStringLiteral("x-systemd.device-timeout"),
    };
    static const QSet<QString> smbOptions = {
        QStringLiteral("vers"), QStringLiteral("sec"), QStringLiteral("seal"), QStringLiteral("cache"), QStringLiteral("actimeo"),
        QStringLiteral("uid"), QStringLiteral("gid"), QStringLiteral("file_mode"), QStringLiteral("dir_mode"),
        QStringLiteral("iocharset"), QStringLiteral("mfsymlinks"), QStringLiteral("nobrl"), QStringLiteral("noserverino"),
        QStringLiteral("nounix"), QStringLiteral("hard"), QStringLiteral("soft"), QStringLiteral("echo_interval"),
        QStringLiteral("rsize"), QStringLiteral("wsize"), QStringLiteral("guest"), QStringLiteral("username"),
        QStringLiteral("domain"), QStringLiteral("credentials"), QStringLiteral("multichannel"), QStringLiteral("max_channels"),
        QStringLiteral("port"), QStringLiteral("nosharesock"), QStringLiteral("noperm"),
    };
    static const QSet<QString> nfsOptions = {
        QStringLiteral("vers"), QStringLiteral("nfsvers"), QStringLiteral("proto"), QStringLiteral("sec"), QStringLiteral("hard"),
        QStringLiteral("soft"), QStringLiteral("timeo"), QStringLiteral("retrans"), QStringLiteral("rsize"), QStringLiteral("wsize"),
        QStringLiteral("nconnect"), QStringLiteral("noac"), QStringLiteral("actimeo"), QStringLiteral("lookupcache"),
        QStringLiteral("nolock"), QStringLiteral("port"),
    };
    static const QRegularExpression simpleValue(QStringLiteral("^[A-Za-z0-9._:+-]{0,64}$"));

    QStringList options;
    for (const QString &opt : requested.options) {
        const QString name = optionName(opt);
        const QString value = optionValue(opt);
        if (!generic.contains(name) && !(smb ? smbOptions : nfsOptions).contains(name)) {
            fail(QStringLiteral("The option \"%1\" isn't allowed for shared mounts.").arg(name));
            continue;
        }
        if (name == QLatin1String("uid") && value != QString::number(caller.uid)) {
            fail(QStringLiteral("uid= can only be your own user ID (%1).").arg(caller.uid));
            continue;
        }
        if (name == QLatin1String("gid") && value != QString::number(caller.gid)) {
            fail(QStringLiteral("gid= can only be your own group ID (%1).").arg(caller.gid));
            continue;
        }
        if (name == QLatin1String("credentials")) {
            // Only the caller's own credentials files, so nobody can send another user's
            // saved password to a server they control.
            const QString prefix = credentialsDir + QLatin1Char('/') + caller.name + QLatin1Char('-');
            const QString rest = value.mid(prefix.size());
            if (!value.startsWith(prefix) || rest.isEmpty() || rest.contains(QLatin1Char('/')) || !checkPathSyntax(value).isEmpty()) {
                fail(QStringLiteral("credentials= must be one of your own saved logins."));
                continue;
            }
        } else if (name != QLatin1String("username") && name != QLatin1String("domain") && !simpleValue.match(value).hasMatch()) {
            fail(QStringLiteral("The value of \"%1\" contains characters that aren't allowed.").arg(name));
            continue;
        }
        if (!options.contains(opt)) {
            options.append(opt);
        }
    }
    for (const auto *forced : {"nosuid", "nodev", "nofail", "_netdev"}) {
        const QString f = QLatin1String(forced);
        if (!options.contains(f)) {
            options.append(f);
        }
    }
    plan.entry.options = options;
    plan.entry.dump = 0;
    plan.entry.pass = 0;

    for (const fstab::Issue &issue : fstab::validate(plan.entry)) {
        if (issue.severity == fstab::Issue::Severity::Error) {
            fail(issue.message);
        }
    }
    const fstab::File file = fstab::File::parse(current);
    if (file.findMountPoint(requested.mountPoint) >= 0) {
        fail(QStringLiteral("%1 is already used by another entry.").arg(requested.mountPoint));
    }
    return plan;
}

QStringList checkSharedMountRemoval(QByteArrayView current, const QString &mountPoint, const Caller &caller)
{
    if (!caller.netMount && !caller.fullAccess()) {
        return {QStringLiteral("Only members of the netmount group can remove shared network mounts.")};
    }
    const fstab::File file = fstab::File::parse(current);
    const int line = file.findMountPoint(mountPoint);
    if (line < 0) {
        return {QStringLiteral("There is no entry for %1.").arg(mountPoint)};
    }
    const auto &found = file.lines().at(static_cast<std::size_t>(line)).entry;
    if (!found) {
        return {QStringLiteral("There is no entry for %1.").arg(mountPoint)};
    }
    const Entry &e = *found;
    if (!isNetworkType(e.type) || !sharedMountPointRe().match(e.mountPoint).hasMatch() || !hasOption(e, QStringLiteral("nosuid"))
        || !hasOption(e, QStringLiteral("nodev"))) {
        return {QStringLiteral("%1 isn't a shared network mount.").arg(mountPoint)};
    }
    return {};
}

} // namespace anchorage::policy
