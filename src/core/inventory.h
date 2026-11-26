// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "drives.h"
#include "fstab.h"
#include "mounts.h"

#include <QList>
#include <QString>

#include <optional>

// Everything Anchorage shows: fstab entries joined with the drives they refer to and what
// is mounted right now, plus drives and network shares that aren't in fstab.
namespace anchorage::inventory
{

enum class Kind {
    Drive,
    Network,
    Swap,
};

enum class Startup {
    NotInFstab,
    AtBoot, // mounted while the computer starts
    OnFirstAccess, // noauto + x-systemd.automount
    WhenOpened, // noauto: mounted when someone opens it (or by hand)
};

struct Item {
    QString id; // stable while the data doesn't change: "fstab:<line>", "vol:<path>", "mnt:<target>"
    Kind kind = Kind::Drive;
    QString title;
    QString mountPoint; // configured (fstab) or current mount point
    QString source; // fstab spec, device path or share
    QString fsType; // from fstab, the device, or the current mount

    std::optional<fstab::Entry> entry; // set when it's in fstab
    int fstabLine = -1;
    QString fstabText; // the line exactly as written in the file
    std::optional<drives::Volume> volume; // set when the device is present

    bool system = false; // needed to start the computer (policy::isSystemEntry)
    bool mounted = false;
    QString mountedAt; // where it is mounted right now (may differ from mountPoint)
    bool deviceMissing = false; // fstab names a local device that isn't connected
    Startup startup = Startup::NotInFstab;
    bool userMountable = false; // fstab options user/users/owner
    bool orphanedBind = false; // a bind whose drive is no longer in fstab (binds::orphaned)
    qint64 size = -1; // file system size, if known
    qint64 used = -1;
};

// True if an fstab device field (UUID=..., LABEL=..., /dev/disk/by-*/..., /dev/sdb1)
// refers to `volume`.
bool specMatches(const QString &spec, const drives::Volume &volume);

QList<Item> build(const fstab::File &fstab, const QList<drives::Volume> &volumes, const QList<mounts::Mount> &mounts);

// "1.8 TB", "512 MB" (decimal units, like disk vendors and Dolphin's default).
QString formatSize(qint64 bytes);

} // namespace anchorage::inventory
