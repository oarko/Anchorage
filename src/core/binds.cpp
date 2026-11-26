// SPDX-License-Identifier: GPL-3.0-or-later
#include "binds.h"

#include "access.h"

#include <algorithm>

namespace anchorage::binds
{

namespace
{
const QString kRequiresMountsFor = QStringLiteral("x-systemd.requires-mounts-for=");
const QString kRequires = QStringLiteral("x-systemd.requires=");
}

bool isBind(const fstab::Entry &e)
{
    return e.options.contains(QStringLiteral("bind")) || e.options.contains(QStringLiteral("rbind"));
}

QString checkSubfolder(const QString &sub)
{
    if (sub.isEmpty()) {
        return {};
    }
    if (sub.startsWith(QLatin1Char('/')) || sub.endsWith(QLatin1Char('/'))) {
        return QStringLiteral("Write the folder relative to the drive, e.g. Games/Steam.");
    }
    for (const QString &part : sub.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) {
            return QStringLiteral("The folder on the drive must not contain \".\", \"..\" or \"//\".");
        }
    }
    return {};
}

fstab::Entry makeEntry(const fstab::Entry &drive, const Bind &b)
{
    fstab::Entry e;
    e.spec = b.subfolder.isEmpty() ? drive.mountPoint : drive.mountPoint + QLatin1Char('/') + b.subfolder;
    e.mountPoint = b.target;
    e.type = QStringLiteral("none");
    e.options = access::startOptions(access::read(drive.options, drive.type).start);
    e.options << QStringLiteral("bind") << QStringLiteral("nofail") << kRequires + drive.mountPoint << kRequiresMountsFor + drive.mountPoint;
    return e;
}

QList<std::pair<int, Bind>> findFor(const fstab::File &file, const QString &mountPoint)
{
    QList<std::pair<int, Bind>> out;
    if (mountPoint.isEmpty()) {
        return out;
    }
    for (const int i : file.entryLines()) {
        const auto &e = file.lines().at(static_cast<std::size_t>(i)).entry;
        if (!e || !isBind(*e)) {
            continue;
        }
        const bool requires_ = e->options.contains(kRequires + mountPoint) || e->options.contains(kRequiresMountsFor + mountPoint);
        if (e->spec == mountPoint) {
            out.append({i, {QString(), e->mountPoint}});
        } else if (e->spec.startsWith(mountPoint + QLatin1Char('/')) && (requires_ || mountPoint != QLatin1String("/"))) {
            out.append({i, {e->spec.mid(mountPoint.size() + 1), e->mountPoint}});
        }
    }
    return out;
}

bool apply(fstab::File &file, const QString &oldMountPoint, const fstab::Entry &drive, const QList<Bind> &binds, QString *error)
{
    fstab::File work = file;
    const auto existing = findFor(work, oldMountPoint);
    QList<int> lines;
    for (const auto &[line, b] : existing) {
        lines.append(line);
    }
    // Change in place as far as possible, keeping each bind's line and column layout.
    const qsizetype common = std::min(lines.size(), binds.size());
    for (qsizetype k = 0; k < common; ++k) {
        QList<fstab::Issue> issues;
        if (!work.replace(lines.at(k), makeEntry(drive, binds.at(k)), &issues)) {
            if (error) {
                *error = issues.isEmpty() ? QStringLiteral("The bind entry was refused.") : issues.first().message;
            }
            return false;
        }
    }
    // Remove the extra ones, last first so the indexes stay valid.
    for (qsizetype k = lines.size() - 1; k >= common; --k) {
        work.remove(lines.at(k));
    }
    for (qsizetype k = common; k < binds.size(); ++k) {
        QList<fstab::Issue> issues;
        if (work.append(makeEntry(drive, binds.at(k)), &issues) < 0) {
            if (error) {
                *error = issues.isEmpty() ? QStringLiteral("The bind entry was refused.") : issues.first().message;
            }
            return false;
        }
    }
    file = work;
    return true;
}

QStringList dependsOn(const fstab::Entry &bind)
{
    QStringList out;
    for (const QString &o : bind.options) {
        for (const QString &key : {kRequires, kRequiresMountsFor}) {
            if (o.startsWith(key) && o.mid(key.size()).startsWith(QLatin1Char('/')) && !out.contains(o.mid(key.size()))) {
                out.append(o.mid(key.size()));
            }
        }
    }
    return out;
}

QList<int> orphaned(const fstab::File &file)
{
    QStringList mounted; // mount points of entries that mount something themselves
    for (const int i : file.entryLines()) {
        const auto &e = file.lines().at(static_cast<std::size_t>(i)).entry;
        if (e && !isBind(*e)) {
            mounted.append(e->mountPoint);
        }
    }
    QList<int> out;
    for (const int i : file.entryLines()) {
        const auto &e = file.lines().at(static_cast<std::size_t>(i)).entry;
        if (!e || !isBind(*e)) {
            continue;
        }
        for (const QString &dep : dependsOn(*e)) {
            if (!mounted.contains(dep)) {
                out.append(i);
                break;
            }
        }
    }
    return out;
}

} // namespace anchorage::binds
