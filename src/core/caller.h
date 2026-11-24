// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "result.h"

#include <QString>
#include <QStringList>

#include <sys/types.h>

// Who is asking the helper to do something. The helper works this out itself from the
// caller's user ID (from KAuth); nothing the GUI says about the user is trusted.
namespace anchorage
{

struct GroupNames {
    QString driveAdmin; // default "driveadmins"
    QString netMount; // default "netmount"
    QStringList sudo; // groups that count as system administrators
};
GroupNames defaultGroupNames();

struct Caller {
    uid_t uid = 0;
    gid_t gid = 0;
    QString name;
    QString home;
    bool sudo = false; // member of an administrator group, or root
    bool driveAdmin = false;
    bool netMount = false;

    // driveadmins and administrators may change anything in fstab (SPEC §4.2).
    bool fullAccess() const { return sudo || driveAdmin; }
};

// Looks the user up in the system user/group databases.
Result<Caller> resolveCaller(uid_t uid, const GroupNames &groups);

} // namespace anchorage
