// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <functional>

// The programs SMB and NFS mounts need (mount.cifs, mount.nfs), and how to install them on
// this distribution (SPEC §11). Pure apart from the `Exists` callback, so it is unit-tested
// with sample /etc/os-release files.
namespace anchorage::clienttools
{

enum class Protocol { Smb, Nfs };
enum class Manager { Apt, Dnf, Zypper, Pacman, Unknown };

struct Distro {
    QString id; // "ubuntu"
    QStringList idLike; // {"debian"}
    QString name; // "Ubuntu 26.04.1 LTS"
};

using Exists = std::function<bool(const QString &path)>;

Distro parseOsRelease(QByteArrayView data);

// The distribution's own package manager: by family (ID, then ID_LIKE), confirmed by the
// tool being present; otherwise whichever known tool is present.
Manager detectManager(const Distro &distro, const Exists &exists);

QString packageName(Manager manager, Protocol protocol); // "cifs-utils", "nfs-common", ...
// "sudo apt install cifs-utils"; for Unknown: a sentence naming the package.
QString installCommand(Manager manager, Protocol protocol);

// The mount helper program (/usr/sbin/mount.cifs or /sbin/mount.cifs, same for nfs).
bool isInstalled(Protocol protocol, const Exists &exists);

// Smb for cifs/smb3, Nfs for nfs/nfs4; false for anything else.
bool protocolFor(const QString &fsType, Protocol *protocol);

} // namespace anchorage::clienttools
