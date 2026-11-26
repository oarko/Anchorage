// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountplan.h"

#include "mountpolicy.h"

#include <QRegularExpression>

namespace anchorage::mounting
{

namespace
{
QStringList rootSpecs(const fstab::File &file)
{
    QStringList specs;
    for (const int i : file.entryLines()) {
        const auto &e = file.lines().at(static_cast<std::size_t>(i)).entry;
        if (e && e->mountPoint == QLatin1String("/")) {
            specs.append(e->spec);
        }
    }
    return specs;
}

const fstab::Entry *entryAt(const fstab::File &file, const QString &mountPoint)
{
    const int i = file.findMountPoint(mountPoint);
    if (i < 0) {
        return nullptr;
    }
    const auto &e = file.lines().at(static_cast<std::size_t>(i)).entry;
    return e ? &*e : nullptr;
}

bool isAutomount(const fstab::Entry &e)
{
    return e.options.contains(QStringLiteral("x-systemd.automount"));
}

QString optionName(const QString &o)
{
    return o.section(QLatin1Char('='), 0, 0);
}

// Device or share forms a temporary mount may name.
bool isAcceptableSource(const QString &spec, bool network)
{
    if (network) {
        // //server/share[/path] or server:/path; no leading '-' anywhere it could be an option.
        static const QRegularExpression smb(QStringLiteral("^//[A-Za-z0-9._\\[\\]:-]+/[^\\s]+$"));
        static const QRegularExpression nfs(QStringLiteral("^[A-Za-z0-9._\\[\\]:-]*[A-Za-z0-9\\]]:/[^\\s]*$"));
        return !spec.startsWith(QLatin1Char('-')) && (smb.match(spec).hasMatch() || nfs.match(spec).hasMatch());
    }
    static const QRegularExpression tag(QStringLiteral("^(UUID|LABEL|PARTUUID|PARTLABEL)=[^\\s]+$"));
    if (tag.match(spec).hasMatch()) {
        return true;
    }
    return spec.startsWith(QLatin1String("/dev/")) && policy::checkPathSyntax(spec).isEmpty();
}
} // namespace

QString unitName(const QString &mountPoint)
{
    // systemd's unit_name_from_path() + unit_name_path_escape(): strip the slashes at both
    // ends, "/" becomes "-", and everything except [A-Za-z0-9:_.] (and a leading '.')
    // becomes \xNN, byte by byte.
    QString path = mountPoint;
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (path == QLatin1String("/")) {
        return QStringLiteral("-");
    }
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    const QByteArray bytes = path.toUtf8();
    QString out;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char c = bytes.at(i);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '_' || (c == '.' && i > 0);
        if (c == '/') {
            out += QLatin1Char('-');
        } else if (plain) {
            out += QLatin1Char(c);
        } else {
            out += QStringLiteral("\\x%1").arg(static_cast<unsigned char>(c), 2, 16, QLatin1Char('0'));
        }
    }
    return out;
}

const mounts::Mount *mountedAt(const QList<mounts::Mount> &current, const QString &target)
{
    const mounts::Mount *found = nullptr;
    for (const auto &m : current) {
        if (m.target == target && m.fsType != QLatin1String("autofs")) {
            found = &m; // later lines are mounted on top
        }
    }
    return found;
}

MountPlan planMountEntry(QByteArrayView fstabData, const QString &mountPoint, const Caller &caller, const QList<mounts::Mount> &current)
{
    MountPlan plan;
    if (const QString e = policy::checkPathSyntax(mountPoint); !e.isEmpty()) {
        plan.errors.append(e);
        return plan;
    }
    const fstab::File file = fstab::File::parse(fstabData);
    const fstab::Entry *entry = entryAt(file, mountPoint);
    if (!entry) {
        plan.errors.append(QStringLiteral("%1 is not in /etc/fstab.").arg(mountPoint));
        return plan;
    }
    plan.entry = *entry;
    if (entry->type == QLatin1String("swap")) {
        plan.errors.append(QStringLiteral("Swap is switched on with swapon, not mounted."));
        return plan;
    }
    if (policy::isSystemEntry(*entry, rootSpecs(file)) && !caller.fullAccess()) {
        plan.errors.append(QStringLiteral("Only driveadmins or administrators can mount the system entry %1.").arg(mountPoint));
        return plan;
    }
    if (mountedAt(current, mountPoint)) {
        plan.errors.append(QStringLiteral("Something is already mounted at %1.").arg(mountPoint));
        return plan;
    }
    plan.method = isAutomount(*entry) ? Method::Systemd : Method::MountCommand;
    // mount ignores x-systemd.requires-mounts-for: without this check a bind entry would
    // show the drive's empty folder when the drive isn't mounted. (systemd checks it itself.)
    if (plan.method == Method::MountCommand) {
        static const QString requires_ = QStringLiteral("x-systemd.requires-mounts-for=");
        for (const QString &o : entry->options) {
            if (o.startsWith(requires_) && !mountedAt(current, o.mid(requires_.size()))) {
                plan.errors.append(QStringLiteral("Mount %1 first: %2 shows what is on it.").arg(o.mid(requires_.size()), mountPoint));
            }
        }
    }
    return plan;
}

TemporaryPlan planTemporaryMount(const fstab::Entry &requested,
                                 const Caller &caller,
                                 bool confirmProtected,
                                 QByteArrayView fstabData,
                                 const QList<mounts::Mount> &current,
                                 const QStringList &extraBlocked)
{
    TemporaryPlan plan;
    plan.entry = requested;
    const auto fail = [&plan](const QString &m) {
        plan.errors.append(m);
    };
    if (!caller.fullAccess()) {
        fail(QStringLiteral("Only driveadmins and administrators can mount drives at a chosen folder."));
        return plan;
    }
    for (const fstab::Issue &i : fstab::validate(requested)) {
        if (i.severity == fstab::Issue::Severity::Error) {
            fail(i.message);
        }
    }
    if (!plan.ok()) {
        return plan;
    }
    if (requested.type == QLatin1String("swap") || requested.type == QLatin1String("none") || requested.type == QLatin1String("auto")) {
        fail(QStringLiteral("Choose the drive's file system (not %1).").arg(requested.type));
    }
    if (!isAcceptableSource(requested.spec, policy::isNetworkType(requested.type))) {
        fail(policy::isNetworkType(requested.type) ? QStringLiteral("Write the share as //server/share or server:/path.")
                                                   : QStringLiteral("Name the drive as UUID=, LABEL=, PARTUUID=, PARTLABEL= or /dev/…"));
    }
    static const QStringList refused = {QStringLiteral("bind"), QStringLiteral("rbind"), QStringLiteral("move"), QStringLiteral("remount"),
                                        QStringLiteral("helper")};
    for (const QString &o : requested.options) {
        const QString name = optionName(o);
        if (refused.contains(name) || name.startsWith(QLatin1String("X-mount."), Qt::CaseInsensitive)) {
            fail(QStringLiteral("The option %1 can't be used for a temporary mount.").arg(name));
        }
    }
    const fstab::File file = fstab::File::parse(fstabData);
    if (entryAt(file, requested.mountPoint)) {
        fail(QStringLiteral("%1 belongs to an entry in /etc/fstab; mount that entry instead.").arg(requested.mountPoint));
    }
    if (mountedAt(current, requested.mountPoint)) {
        fail(QStringLiteral("Something is already mounted at %1.").arg(requested.mountPoint));
    }
    const policy::ZoneResult zone = policy::classifyMountPoint(requested.mountPoint, extraBlocked);
    switch (zone.zone) {
    case policy::Zone::Invalid:
        fail(zone.reason);
        break;
    case policy::Zone::Blocked:
        if (!caller.sudo) {
            fail(QStringLiteral("%1 Only administrators can mount there.").arg(zone.reason));
        } else if (!confirmProtected) {
            fail(QStringLiteral("%1 Confirm by typing the path to mount there.").arg(zone.reason));
        }
        break;
    case policy::Zone::Override:
        plan.overrideZone = true;
        break;
    case policy::Zone::Safe:
        break;
    }
    return plan;
}

UnmountPlan planUnmount(QByteArrayView fstabData, const QList<mounts::Mount> &current, const QString &mountPoint, const Caller &caller, bool confirmSystem)
{
    UnmountPlan plan;
    if (const QString e = policy::checkPathSyntax(mountPoint); !e.isEmpty()) {
        plan.errors.append(e);
        return plan;
    }
    const mounts::Mount *m = mountedAt(current, mountPoint);
    if (!m) {
        plan.errors.append(QStringLiteral("Nothing is mounted at %1.").arg(mountPoint));
        return plan;
    }
    const fstab::File file = fstab::File::parse(fstabData);
    const fstab::Entry *entry = entryAt(file, mountPoint);
    const bool realDevice = m->source.startsWith(QLatin1String("/dev/")) || m->fsType.startsWith(QLatin1String("fuse"));
    if (!entry && !realDevice && !policy::isNetworkType(m->fsType)) {
        plan.errors.append(QStringLiteral("%1 (%2) is managed by the system and can't be unmounted here.").arg(mountPoint, m->fsType));
        return plan;
    }
    const fstab::Entry asEntry = entry ? *entry : fstab::Entry{m->source, mountPoint, m->fsType, {}, 0, 0};
    plan.system = policy::isSystemEntry(asEntry, rootSpecs(file)) || mountPoint == QLatin1String("/");
    if ((plan.system || !entry) && !caller.fullAccess()) {
        plan.errors.append(plan.system ? QStringLiteral("Only driveadmins or administrators can unmount the system entry %1.").arg(mountPoint)
                                       : QStringLiteral("Only driveadmins or administrators can unmount %1 (it is not in /etc/fstab).").arg(mountPoint));
        return plan;
    }
    if (plan.system && !confirmSystem) {
        plan.errors.append(QStringLiteral("%1 is needed to start the computer. Confirm by typing the path to unmount it.").arg(mountPoint));
        return plan;
    }
    plan.method = entry && isAutomount(*entry) ? Method::Systemd : Method::MountCommand;
    return plan;
}

} // namespace anchorage::mounting
