// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

namespace anchorage::settings
{

Settings parse(QByteArrayView data)
{
    Settings s;
    if (data.size() > maxFileSize) {
        return s;
    }
    for (const QByteArray &raw : data.toByteArray().split('\n')) {
        QByteArray line = raw;
        if (const qsizetype hash = line.indexOf('#'); hash >= 0) {
            line.truncate(hash);
        }
        const qsizetype eq = line.indexOf('=');
        if (eq < 0) {
            continue;
        }
        const QByteArray key = line.left(eq).trimmed();
        const QByteArray value = line.mid(eq + 1).trimmed();
        if (key == "keep-backups") {
            bool ok = false;
            const int n = value.toInt(&ok);
            if (ok && n >= minKeepBackups && n <= maxKeepBackups) {
                s.keepBackups = n;
            }
        } else if (key == "ntfs-driver") {
            if (value == "ntfs3" || value == "ntfs") {
                s.ntfsDriver = QString::fromLatin1(value);
            }
        }
    }
    return s;
}

QStringList validate(const Settings &s)
{
    QStringList errors;
    if (s.keepBackups < minKeepBackups || s.keepBackups > maxKeepBackups) {
        errors.append(QStringLiteral("The number of backups must be between %1 and %2.").arg(minKeepBackups).arg(maxKeepBackups));
    }
    if (s.ntfsDriver != QLatin1String("ntfs3") && s.ntfsDriver != QLatin1String("ntfs")) {
        errors.append(QStringLiteral("The NTFS driver must be ntfs3 or ntfs."));
    }
    return errors;
}

QByteArray serialize(const Settings &s)
{
    QByteArray out;
    out += "# Anchorage settings. Change them in Anchorage (Settings > Configure Anchorage),\n";
    out += "# which needs driveadmins or administrator rights.\n\n";
    out += "# How many backups of /etc/fstab to keep (" + QByteArray::number(minKeepBackups) + "-" + QByteArray::number(maxKeepBackups) + ").\n";
    out += "keep-backups = " + QByteArray::number(s.keepBackups) + "\n\n";
    out += "# File system suggested for new NTFS entries: ntfs3 (kernel, fast) or ntfs (ntfs-3g).\n";
    out += "ntfs-driver = " + s.ntfsDriver.toLatin1() + "\n";
    return out;
}

} // namespace anchorage::settings
