// SPDX-License-Identifier: GPL-3.0-or-later
#include "caller.h"

#include "config-anchorage.h"

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <vector>

namespace anchorage
{

GroupNames defaultGroupNames()
{
    return {
        QStringLiteral(ANCHORAGE_DRIVEADMIN_GROUP),
        QStringLiteral(ANCHORAGE_NETMOUNT_GROUP),
        {QStringLiteral("sudo"), QStringLiteral("admin"), QStringLiteral("wheel")},
    };
}

Result<Caller> resolveCaller(uid_t uid, const GroupNames &groups)
{
    long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufSize <= 0) {
        bufSize = 16384;
    }
    std::vector<char> buf(static_cast<std::size_t>(bufSize));
    passwd pw{};
    passwd *found = nullptr;
    if (getpwuid_r(uid, &pw, buf.data(), buf.size(), &found) != 0 || !found) {
        return Result<Caller>::failure(QStringLiteral("Unknown user ID %1.").arg(uid));
    }

    Caller c;
    c.uid = uid;
    c.gid = pw.pw_gid;
    c.name = QString::fromLocal8Bit(pw.pw_name);
    c.home = QString::fromLocal8Bit(pw.pw_dir);

    // All groups the user belongs to, including the primary group.
    int count = 32;
    std::vector<gid_t> gids;
    for (int attempt = 0; attempt < 4; ++attempt) {
        gids.resize(static_cast<std::size_t>(count));
        int n = count;
        if (getgrouplist(pw.pw_name, pw.pw_gid, gids.data(), &n) >= 0) {
            gids.resize(static_cast<std::size_t>(n));
            break;
        }
        count = n > count ? n : count * 2;
        gids.clear();
    }

    long grSize = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (grSize <= 0) {
        grSize = 16384;
    }
    std::vector<char> grBuf(static_cast<std::size_t>(grSize));
    for (const gid_t g : gids) {
        group gr{};
        group *grFound = nullptr;
        if (getgrgid_r(g, &gr, grBuf.data(), grBuf.size(), &grFound) != 0 || !grFound) {
            continue;
        }
        const QString name = QString::fromLocal8Bit(gr.gr_name);
        if (groups.sudo.contains(name)) {
            c.sudo = true;
        }
        if (name == groups.driveAdmin) {
            c.driveAdmin = true;
        }
        if (name == groups.netMount) {
            c.netMount = true;
        }
    }
    if (uid == 0) {
        c.sudo = true;
    }
    return c;
}

} // namespace anchorage
