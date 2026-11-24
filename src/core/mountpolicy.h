// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "caller.h"
#include "fstab.h"

#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

// What a caller may change in fstab, and where mount points may go (SPEC §4.2, §7).
// Everything here is pure (no file system access), so it can be unit-tested and fuzzed.
namespace anchorage::policy
{

enum class Zone {
    Safe, // /mnt, /media, /srv, /home/<user>/..., /run/media/<user>/...
    Override, // anywhere else that isn't protected; driveadmins/sudo, must be empty or new
    Blocked, // protected system locations; sudo only, after typed confirmation
    Invalid, // not a usable path at all
};

struct ZoneResult {
    Zone zone = Zone::Invalid;
    QString reason;
};

// Empty string when `path` is an absolute, normalised path without control characters,
// '#', "." or ".." components, doubled/trailing slashes, or over-long components.
QString checkPathSyntax(const QString &path);

// `extraBlocked` comes from the root-owned blocked-paths file; it can only add protection.
ZoneResult classifyMountPoint(const QString &path, const QStringList &extraBlocked = {});

// Entries the computer needs to start: /, /boot..., /home, /usr..., /var..., /tmp, swap.
bool isSystemEntry(const fstab::Entry &entry);

bool isNetworkType(const QString &type); // cifs, smb3, nfs, nfs4

// ---- Whole-file writes (helper action "writefstab") ---------------------------------

struct Plan {
    QStringList errors;
    QList<fstab::Entry> added; // new or changed entries
    QList<fstab::Entry> removed; // entries that disappear (or the old side of a change)
    QStringList newMountPoints; // mount points that weren't in the file before
    QStringList overrideMountPoints; // subset of newMountPoints in the override zone
    bool ok() const { return errors.isEmpty(); }
};

// Compares the current file with the proposed one and checks every difference against
// the caller's rights. Unchanged lines (including pre-existing broken ones) are fine.
Plan planFstabWrite(QByteArrayView current,
                    QByteArrayView proposed,
                    const Caller &caller,
                    bool confirmProtected,
                    const QStringList &extraBlocked = {});

// ---- Shared network mounts in /mnt (helper actions for the netmount group) ----------

struct SharedMountPlan {
    QStringList errors;
    fstab::Entry entry; // the entry as it will be written (forced options added)
    bool ok() const { return errors.isEmpty(); }
};

// Checks a shared mount request: network type, /mnt/<name>, allow-listed options only,
// uid/gid only the caller's own, credentials only from the caller's own files.
// Forces nosuid, nodev, nofail and _netdev.
SharedMountPlan planSharedMount(const fstab::Entry &requested,
                                const Caller &caller,
                                QByteArrayView current,
                                const QString &credentialsDir);

QStringList checkSharedMountRemoval(QByteArrayView current, const QString &mountPoint, const Caller &caller);

} // namespace anchorage::policy
