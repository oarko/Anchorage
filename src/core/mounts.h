// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "result.h"

#include <QByteArrayView>
#include <QList>
#include <QString>

// What is mounted right now, from /proc/self/mountinfo (see proc_pid_mountinfo(5)).
namespace anchorage::mounts
{

struct Mount {
    QString source; // /dev/sdb1, //nas/share, nas:/export
    QString target; // where it is mounted
    QString fsType;
    QString root; // subtree of the file system mounted here (btrfs subvolumes: /@home)
    QString options; // per-mount options (rw,noatime,...)
};

QList<Mount> parseMountInfo(QByteArrayView data);
Result<QList<Mount>> readCurrent(); // /proc/self/mountinfo

} // namespace anchorage::mounts
