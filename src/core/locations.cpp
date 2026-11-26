// SPDX-License-Identifier: GPL-3.0-or-later
#include "locations.h"

#include "mountpolicy.h"

#include <QRegularExpression>

namespace anchorage::locations
{

QString personalBase(const QString &user, const QList<mounts::Mount> &current, const Exists &exists)
{
    QString run = QStringLiteral("/run/media/") + user;
    QString media = QStringLiteral("/media/") + user;
    for (const auto &m : current) {
        if (m.target.startsWith(run + QLatin1Char('/'))) {
            return run;
        }
        if (m.target.startsWith(media + QLatin1Char('/'))) {
            return media;
        }
    }
    if (exists(run)) {
        return run;
    }
    return exists(media) ? media : run;
}

QString folderName(const QString &labelOrName)
{
    QString base = labelOrName;
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9._-]+"));
    base.replace(bad, QStringLiteral("-"));
    while (base.startsWith(QLatin1Char('.')) || base.startsWith(QLatin1Char('-'))) {
        base.remove(0, 1);
    }
    return base.isEmpty() ? QStringLiteral("drive") : base;
}

QStringList suggestions(const drives::Volume &v, const QString &user, const QList<mounts::Mount> &current, const Exists &exists)
{
    QStringList out;
    for (const QString &mp : v.mountPoints) {
        // Where it is now, unless that is part of the running system ("/", "/home", ...).
        if (!mp.isEmpty() && !policy::isSystemEntry({v.path, mp, v.fsType, {}, 0, 0}) && policy::checkPathSyntax(mp).isEmpty()) {
            out.append(mp);
        }
    }
    const QString name = folderName(v.label.isEmpty() ? v.name : v.label);
    const QStringList usual = {QStringLiteral("/mnt/") + name, QStringLiteral("/media/") + name, personalBase(user, current, exists) + QLatin1Char('/') + name};
    for (const QString &p : usual) {
        if (!out.contains(p)) {
            out.append(p);
        }
    }
    return out;
}

} // namespace anchorage::locations
