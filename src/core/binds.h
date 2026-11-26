// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fstab.h"

#include <QList>
#include <QString>

#include <utility>

// "Also show it at…" (SPEC §8): extra locations for a drive, each its own bind entry:
//   /mnt/Lingames  /run/media/oarko/Lingames  none
//       bind,nofail,x-systemd.requires=/mnt/Lingames,x-systemd.requires-mounts-for=/mnt/Lingames  0 0
// x-systemd.requires (Requires= + After= on the drive's mount unit) makes systemd refuse
// the bind when the drive isn't mounted or has no fstab line at all; requires-mounts-for
// alone only waits for a mount unit that exists, and otherwise binds the empty folder
// (found in the VM, 2026-11-26).
namespace anchorage::binds
{

struct Bind {
    QString subfolder; // folder on the drive, relative ("" = the whole drive)
    QString target; // where it is shown too
    bool operator==(const Bind &) const = default;
};

bool isBind(const fstab::Entry &entry);

// Empty if `subfolder` is usable: relative, no "." or "..", no doubled slashes.
QString checkSubfolder(const QString &subfolder);

// The bind entry for `drive`. It follows the drive's "when it mounts" choice.
fstab::Entry makeEntry(const fstab::Entry &drive, const Bind &bind);

// Bind entries in `file` that show (part of) the drive mounted at `mountPoint`: line
// index and the bind, in file order.
QList<std::pair<int, Bind>> findFor(const fstab::File &file, const QString &mountPoint);

// Makes the file's binds for a drive match `binds`: existing ones (found under
// `oldMountPoint`) are changed in place, extra ones removed, new ones added at the end of
// the file. Returns false (file unchanged) if an entry is refused.
bool apply(fstab::File &file, const QString &oldMountPoint, const fstab::Entry &drive, const QList<Bind> &binds, QString *error = nullptr);

// Folders a bind entry depends on (x-systemd.requires=/path and requires-mounts-for=).
QStringList dependsOn(const fstab::Entry &bind);

// Bind entries whose drive is gone: they depend on a mount point that no entry in the file
// mounts. Line indexes, in file order.
QList<int> orphaned(const fstab::File &file);

} // namespace anchorage::binds
