// SPDX-License-Identifier: GPL-3.0-or-later
#include "mounterrors.h"

namespace anchorage::mounterrors
{

QString explain(const QString &fsType, const QString &message)
{
    const QString m = message.toLower();
    const bool nfs = fsType.startsWith(QLatin1String("nfs"));
    const bool smb = fsType == QLatin1String("cifs") || fsType == QLatin1String("smb3");
    if (nfs && m.contains(QLatin1String("protocol not supported"))) {
        return QStringLiteral("The server doesn't offer this NFS version. Choose \"Automatic\" (or NFS 3) as the version, or switch NFSv4 on in "
                              "the server's NFS settings.");
    }
    if (nfs && m.contains(QLatin1String("access denied"))) {
        return QStringLiteral("The server refused this computer. Check that the export allows this computer's address. Many servers (such as "
                              "TrueNAS) also only accept connections from a privileged port unless \"allow non-root mount\" (insecure) is on.");
    }
    if (nfs && m.contains(QLatin1String("operation not permitted"))) {
        return QStringLiteral("The server refused the connection. Servers that require a privileged port refuse clients behind NAT or in "
                              "virtual machines unless \"allow non-root mount\" (insecure) is switched on for the export.");
    }
    if (nfs && (m.contains(QLatin1String("no route to host")) || m.contains(QLatin1String("connection refused")) || m.contains(QLatin1String("timed out")))) {
        return QStringLiteral("The NFS server can't be reached. Check the address, that the server is on, and that its NFS service is running.");
    }
    if (smb && (m.contains(QLatin1String("permission denied")) || m.contains(QLatin1String("error(13)")))) {
        return QStringLiteral("The server refused the login. Check the user name and password (and the domain or workgroup, if the server uses one).");
    }
    if (smb && (m.contains(QLatin1String("error(2)")) || m.contains(QLatin1String("no such file")))) {
        return QStringLiteral("The server doesn't have a share with this name. Use Connect to Server to see the shares it offers.");
    }
    if (smb && (m.contains(QLatin1String("error(112)")) || m.contains(QLatin1String("host is down")) || m.contains(QLatin1String("error(115)")))) {
        return QStringLiteral("The SMB server can't be reached. Check the address and that the server is on.");
    }
    if (smb && (m.contains(QLatin1String("error(95)")) || m.contains(QLatin1String("operation not supported")))) {
        return QStringLiteral("The server and this computer don't agree on an SMB version. Try another \"vers=\" on the Advanced tab (e.g. 3.0).");
    }
    if (m.contains(QLatin1String("bad option")) && (smb || nfs)) {
        return QStringLiteral("The client tools for this kind of share may be missing (cifs-utils for SMB, the NFS client for NFS), or an option "
                              "isn't understood.");
    }
    return {};
}

} // namespace anchorage::mounterrors
