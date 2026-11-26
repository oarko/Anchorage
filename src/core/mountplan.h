// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "caller.h"
#include "fstab.h"
#include "mounts.h"

#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

// Who may mount or unmount what (SPEC §6.1). Pure functions, so they can be unit-tested
// and fuzzed; src/core/mountservice.* runs the commands.
namespace anchorage::mounting
{

enum class Method {
    MountCommand, // /usr/bin/mount, /usr/bin/umount
    Systemd, // systemctl start/stop <unit>.mount (entries mounted when the folder is opened)
};

// systemd's unit name for a mount point, like `systemd-escape --path` ("/mnt/my disk" ->
// "mnt-my\x20disk"); add ".mount" or ".automount".
QString unitName(const QString &mountPoint);

// Mounted right now at `target` (the topmost mount, ignoring an automount placeholder).
const mounts::Mount *mountedAt(const QList<mounts::Mount> &current, const QString &target);

struct MountPlan {
    QStringList errors;
    fstab::Entry entry;
    Method method = Method::MountCommand;
    bool ok() const { return errors.isEmpty(); }
};

// mountentry: an fstab entry, named by its mount point.
MountPlan planMountEntry(QByteArrayView fstab, const QString &mountPoint, const Caller &caller, const QList<mounts::Mount> &current);

struct TemporaryPlan {
    QStringList errors;
    fstab::Entry entry;
    bool overrideZone = false; // the folder must be empty or new
    bool ok() const { return errors.isEmpty(); }
};

// mounttemporary: driveadmins/sudo mount a drive or share somewhere without fstab.
TemporaryPlan planTemporaryMount(const fstab::Entry &requested,
                                 const Caller &caller,
                                 bool confirmProtected,
                                 QByteArrayView fstab,
                                 const QList<mounts::Mount> &current,
                                 const QStringList &extraBlocked = {});

struct UnmountPlan {
    QStringList errors;
    Method method = Method::MountCommand;
    bool system = false; // needed to start the computer
    bool ok() const { return errors.isEmpty(); }
};

UnmountPlan planUnmount(QByteArrayView fstab, const QList<mounts::Mount> &current, const QString &mountPoint, const Caller &caller, bool confirmSystem);

} // namespace anchorage::mounting
