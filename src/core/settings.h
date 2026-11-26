// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>

// System-wide settings in /etc/anchorage/config (SPEC §4.2: driveadmins and
// administrators change them through the helper's "writesettings" action).
//
// File format, one "key = value" per line, '#' starts a comment:
//   keep-backups = 10      how many backups of /etc/fstab to keep (1-100)
//   ntfs-driver = ntfs3    file system suggested for new NTFS entries (ntfs3 or ntfs)
//
// Reading is forgiving (unknown keys and bad values fall back to the defaults, so a
// hand-edited file never stops the helper); writing is strict.
namespace anchorage::settings
{

inline constexpr int minKeepBackups = 1;
inline constexpr int maxKeepBackups = 100;
inline constexpr int defaultKeepBackups = 10;
inline constexpr qsizetype maxFileSize = qsizetype(16) * 1024;

struct Settings {
    int keepBackups = defaultKeepBackups;
    QString ntfsDriver = QStringLiteral("ntfs3"); // "ntfs3" (kernel) or "ntfs" (ntfs-3g)

    bool operator==(const Settings &) const = default;
};

Settings parse(QByteArrayView data);
// Empty if the settings may be written.
QStringList validate(const Settings &settings);
// The file as Anchorage writes it (comments explain each key).
QByteArray serialize(const Settings &settings);

} // namespace anchorage::settings
