// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "result.h"

#include <QByteArrayView>
#include <QList>
#include <QString>

// Listing the shares of an SMB or NFS server (SPEC §11). Runs as the person using the app,
// never as root: libsmbclient for SMB, /usr/sbin/showmount for NFS. Both block, so the GUI
// calls them from a worker thread.
namespace anchorage::shares
{

enum class Protocol { Smb, Nfs };

struct Address {
    QString host; // name or IP address, without protocol or path
    Protocol protocol = Protocol::Smb;
    bool protocolGiven = false; // smb:// or nfs:// was typed
};

// Accepts "nas.local", "smb://nas.local", "nfs://nas", "//nas", "\\\\nas", "smb://nas/share"
// (the share is ignored), IPv4 and [IPv6] addresses. Refuses anything that could be an
// option or contains spaces, slashes in the host, control characters, '#' or ','.
Result<Address> parseAddress(const QString &text);

struct Share {
    QString name; // SMB share name, or the NFS export path
    QString comment; // SMB comment, or the NFS clients allowed ("*", "10.0.0.0/24")
};

struct Login {
    QString user;
    QString password;
    QString domain;
    bool guest = false;
};

// `showmount -e --no-headers` output: "<export> <clients>" per line.
QList<Share> parseShowmount(QByteArrayView output);
Result<QList<Share>> listNfs(const QString &host);

enum class SmbError { None, NeedsLogin, LoginRefused, Unreachable, Other };
struct SmbList {
    QList<Share> shares;
    SmbError error = SmbError::None;
    QString message;
    bool ok() const { return error == SmbError::None; }
};
// Disk shares only (no IPC$, no printers). Hidden shares (ending in $) are included.
SmbList listSmb(const QString &host, const Login &login);

// NFS versions a server offers, from `rpcinfo -s` (rpcbind; no root needed), e.g. {4, 3}.
// Empty if rpcbind didn't answer (some NFSv4-only servers don't run it).
QList<int> parseRpcinfo(QByteArrayView output);
QList<int> nfsVersions(const QString &host);

// The fstab device field: "//host/share" or "host:/export".
QString deviceFor(const QString &host, Protocol protocol, const Share &share);

} // namespace anchorage::shares
