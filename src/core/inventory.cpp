// SPDX-License-Identifier: GPL-3.0-or-later
#include "inventory.h"

#include "mountpolicy.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <array>

namespace anchorage::inventory
{

namespace
{
using drives::Volume;

// udev's /dev/disk/by-label names escape special characters as \xNN.
QString unescapeUdev(QString s)
{
    for (qsizetype i = s.indexOf(QStringLiteral("\\x")); i >= 0 && i + 3 < s.size(); i = s.indexOf(QStringLiteral("\\x"), i + 1)) {
        bool ok = false;
        const int code = s.mid(i + 2, 2).toInt(&ok, 16);
        if (ok) {
            s.replace(i, 4, QChar(code));
        }
    }
    return s;
}

bool isNetworkFs(const QString &type)
{
    return policy::isNetworkType(type) || type == QLatin1String("smbfs") || type.startsWith(QLatin1String("fuse.sshfs"));
}

QString lastComponent(const QString &s)
{
    const QString trimmed = s.endsWith(QLatin1Char('/')) ? s.chopped(1) : s;
    const qsizetype slash = trimmed.lastIndexOf(QLatin1Char('/'));
    const qsizetype colon = trimmed.lastIndexOf(QLatin1Char(':'));
    return trimmed.mid(std::max(slash, colon) + 1);
}

QString titleForEntry(const fstab::Entry &e, const std::optional<Volume> &v, bool sharedVolume)
{
    if (e.type == QLatin1String("swap")) {
        return e.spec.startsWith(QLatin1Char('/')) && !e.spec.startsWith(QLatin1String("/dev/")) ? QStringLiteral("Swap file")
                                                                                                 : QStringLiteral("Swap partition");
    }
    static const QList<std::pair<QString, QString>> known = {
        {QStringLiteral("/"), QStringLiteral("System")},
        {QStringLiteral("/home"), QStringLiteral("Home")},
        {QStringLiteral("/boot"), QStringLiteral("Boot")},
        {QStringLiteral("/boot/efi"), QStringLiteral("EFI system partition")},
        {QStringLiteral("/tmp"), QStringLiteral("Temporary files")},
        {QStringLiteral("/var"), QStringLiteral("Variable data")},
    };
    for (const auto &[mp, name] : known) {
        if (e.mountPoint == mp) {
            return name;
        }
    }
    if (isNetworkFs(e.type)) {
        return lastComponent(e.spec);
    }
    if (v && !v->label.isEmpty() && !sharedVolume) {
        return v->label;
    }
    return lastComponent(e.mountPoint);
}

QString titleForVolume(const Volume &v)
{
    if (!v.label.isEmpty()) {
        return v.label;
    }
    if (v.partTypeName == QLatin1String("EFI System")) {
        return QStringLiteral("EFI system partition");
    }
    if (v.partTypeName == QLatin1String("Windows recovery environment")) {
        return QStringLiteral("Windows recovery partition");
    }
    return QStringLiteral("%1 %2 partition").arg(formatSize(v.size), v.fsType.toUpper());
}

Startup startupOf(const fstab::Entry &e)
{
    const bool noauto = e.options.contains(QStringLiteral("noauto"));
    if (e.options.contains(QStringLiteral("x-systemd.automount"))) {
        return Startup::OnFirstAccess;
    }
    return noauto ? Startup::WhenOpened : Startup::AtBoot;
}

bool isLocalDeviceSpec(const QString &spec)
{
    for (const auto *p : {"UUID=", "LABEL=", "PARTUUID=", "PARTLABEL=", "/dev/"}) {
        if (spec.startsWith(QLatin1String(p))) {
            return true;
        }
    }
    return false;
}
} // namespace

bool specMatches(const QString &spec, const Volume &v)
{
    const auto value = [&](const char *prefix) -> std::optional<QString> {
        const QLatin1String p(prefix);
        if (spec.startsWith(p)) {
            QString val = spec.mid(p.size());
            if (val.size() >= 2 && val.startsWith(QLatin1Char('"')) && val.endsWith(QLatin1Char('"'))) {
                val = val.mid(1, val.size() - 2);
            }
            return val;
        }
        return std::nullopt;
    };
    const auto same = [](const QString &a, const QString &b) {
        return !a.isEmpty() && a.compare(b, Qt::CaseInsensitive) == 0;
    };
    if (auto u = value("UUID=")) {
        return same(*u, v.uuid);
    }
    if (auto u = value("/dev/disk/by-uuid/")) {
        return same(*u, v.uuid);
    }
    if (auto l = value("LABEL=")) {
        return !v.label.isEmpty() && *l == v.label;
    }
    if (auto l = value("/dev/disk/by-label/")) {
        return !v.label.isEmpty() && unescapeUdev(*l) == v.label;
    }
    if (auto p = value("PARTUUID=")) {
        return same(*p, v.partUuid);
    }
    if (auto p = value("/dev/disk/by-partuuid/")) {
        return same(*p, v.partUuid);
    }
    if (auto p = value("PARTLABEL=")) {
        return !v.partLabel.isEmpty() && *p == v.partLabel;
    }
    return !v.path.isEmpty() && spec == v.path;
}

QList<Item> build(const fstab::File &fstab, const QList<Volume> &volumes, const QList<mounts::Mount> &mounts)
{
    QList<Item> items;
    QSet<QString> usedVolumes;
    QSet<QString> coveredTargets;

    const auto mountAt = [&](const QString &target) -> const mounts::Mount * {
        for (const auto &m : mounts) {
            if (m.target == target) {
                return &m;
            }
        }
        return nullptr;
    };

    // Specs of the file system mounted at "/": its other subvolumes (/home, /swap, ...) are
    // part of the installed system too.
    QStringList rootSpecs;
    for (const int line : fstab.entryLines()) {
        const auto &e = fstab.lines().at(static_cast<std::size_t>(line)).entry;
        if (e && e->mountPoint == QLatin1String("/")) {
            rootSpecs.append(e->spec);
        }
    }
    // How many entries use each spec (btrfs subvolumes share one).
    QHash<QString, int> specUse;
    for (const int line : fstab.entryLines()) {
        if (const auto &e = fstab.lines().at(static_cast<std::size_t>(line)).entry) {
            ++specUse[e->spec];
        }
    }

    // 1. Every fstab entry.
    for (const int line : fstab.entryLines()) {
        const auto &entryOpt = fstab.lines().at(static_cast<std::size_t>(line)).entry;
        if (!entryOpt) {
            continue;
        }
        const fstab::Entry &e = *entryOpt;
        Item it;
        it.id = QStringLiteral("fstab:%1").arg(line);
        it.entry = e;
        it.fstabLine = line;
        it.fstabText = QString::fromUtf8(fstab.lines().at(static_cast<std::size_t>(line)).raw);
        it.source = e.spec;
        it.mountPoint = e.mountPoint;
        it.fsType = e.type;
        it.kind = e.type == QLatin1String("swap") ? Kind::Swap : isNetworkFs(e.type) ? Kind::Network : Kind::Drive;
        it.system = policy::isSystemEntry(e, rootSpecs);
        it.startup = startupOf(e);
        it.userMountable = e.options.contains(QStringLiteral("user")) || e.options.contains(QStringLiteral("users"))
            || e.options.contains(QStringLiteral("owner"));
        for (const Volume &v : volumes) {
            if (specMatches(e.spec, v)) {
                it.volume = v;
                usedVolumes.insert(v.path);
                break;
            }
        }
        it.deviceMissing = !it.volume && isLocalDeviceSpec(e.spec);
        if (it.kind != Kind::Swap) {
            if (const auto *m = mountAt(e.mountPoint)) {
                it.mounted = true;
                it.mountedAt = m->target;
                coveredTargets.insert(m->target);
            }
        }
        if (it.mounted && it.volume) {
            it.size = it.volume->fsSize;
            it.used = it.volume->fsUsed;
        }
        it.title = titleForEntry(e, it.volume, specUse.value(e.spec) > 1);
        items.append(it);
    }

    // 2. Drives with a file system that fstab doesn't mention (e.g. Lingames, USB sticks).
    for (const Volume &v : volumes) {
        if (usedVolumes.contains(v.path) || !drives::isUserVisible(v)) {
            continue;
        }
        Item it;
        it.id = QStringLiteral("vol:%1").arg(v.path);
        it.kind = Kind::Drive;
        it.volume = v;
        it.source = v.path;
        it.fsType = v.fsType;
        it.title = titleForVolume(v);
        if (!v.mountPoints.isEmpty()) {
            it.mounted = true;
            it.mountedAt = v.mountPoints.first();
            it.mountPoint = it.mountedAt;
            it.size = v.fsSize;
            it.used = v.fsUsed;
            for (const QString &mp : v.mountPoints) {
                coveredTargets.insert(mp);
            }
        }
        items.append(it);
    }

    // 3. Mounted network shares that aren't in fstab (mounted by hand or by another tool).
    for (const auto &m : mounts) {
        if (!isNetworkFs(m.fsType) || coveredTargets.contains(m.target)) {
            continue;
        }
        Item it;
        it.id = QStringLiteral("mnt:%1").arg(m.target);
        it.kind = Kind::Network;
        it.source = m.source;
        it.mountPoint = m.target;
        it.fsType = m.fsType;
        it.mounted = true;
        it.mountedAt = m.target;
        it.title = lastComponent(m.source);
        coveredTargets.insert(m.target);
        items.append(it);
    }
    return items;
}

QString formatSize(qint64 bytes)
{
    if (bytes < 0) {
        return QString();
    }
    static constexpr std::array<const char *, 6> units = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1000.0 && unit < 5) {
        value /= 1000.0;
        ++unit;
    }
    const int decimals = (unit == 0 || value >= 100.0) ? 0 : 1;
    return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(QLatin1String(units.at(static_cast<std::size_t>(unit))));
}

} // namespace anchorage::inventory
