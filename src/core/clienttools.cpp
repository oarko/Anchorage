// SPDX-License-Identifier: GPL-3.0-or-later
#include "clienttools.h"

namespace anchorage::clienttools
{

namespace
{
QString unquote(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2 && (v.startsWith(QLatin1Char('"')) || v.startsWith(QLatin1Char('\''))) && v.endsWith(v.at(0))) {
        v = v.mid(1, v.size() - 2);
    }
    return v;
}

struct Family {
    Manager manager;
    QString tool;
    QStringList ids;
};

const QList<Family> &families()
{
    static const QList<Family> list = {
        {Manager::Apt, QStringLiteral("/usr/bin/apt"), {QStringLiteral("debian"), QStringLiteral("ubuntu"), QStringLiteral("linuxmint"), QStringLiteral("pop"), QStringLiteral("neon")}},
        {Manager::Dnf, QStringLiteral("/usr/bin/dnf"), {QStringLiteral("fedora"), QStringLiteral("rhel"), QStringLiteral("centos"), QStringLiteral("rocky"), QStringLiteral("almalinux"), QStringLiteral("nobara")}},
        {Manager::Zypper, QStringLiteral("/usr/bin/zypper"), {QStringLiteral("opensuse"), QStringLiteral("suse"), QStringLiteral("opensuse-tumbleweed"), QStringLiteral("opensuse-leap"), QStringLiteral("sles")}},
        {Manager::Pacman, QStringLiteral("/usr/bin/pacman"), {QStringLiteral("arch"), QStringLiteral("manjaro"), QStringLiteral("endeavouros"), QStringLiteral("cachyos"), QStringLiteral("garuda")}},
    };
    return list;
}
} // namespace

Distro parseOsRelease(QByteArrayView data)
{
    Distro d;
    for (const QByteArray &raw : data.toByteArray().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        const QString key = line.left(eq);
        const QString value = unquote(line.mid(eq + 1));
        if (key == QLatin1String("ID")) {
            d.id = value.toLower();
        } else if (key == QLatin1String("ID_LIKE")) {
            d.idLike = value.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        } else if (key == QLatin1String("PRETTY_NAME")) {
            d.name = value;
        }
    }
    return d;
}

Manager detectManager(const Distro &distro, const Exists &exists)
{
    const QStringList order = QStringList{distro.id} + distro.idLike;
    for (const QString &id : order) {
        for (const Family &f : families()) {
            // "opensuse-tumbleweed" and similar variants count by prefix too.
            const bool matches = f.ids.contains(id) || (id.startsWith(QLatin1String("opensuse")) && f.manager == Manager::Zypper);
            if (matches && exists(f.tool)) {
                return f.manager;
            }
        }
    }
    for (const Family &f : families()) {
        if (exists(f.tool)) {
            return f.manager;
        }
    }
    return Manager::Unknown;
}

QString packageName(Manager manager, Protocol protocol)
{
    if (protocol == Protocol::Smb) {
        return QStringLiteral("cifs-utils");
    }
    switch (manager) {
    case Manager::Apt:
        return QStringLiteral("nfs-common");
    case Manager::Zypper:
        return QStringLiteral("nfs-client");
    case Manager::Dnf:
    case Manager::Pacman:
    case Manager::Unknown:
        return QStringLiteral("nfs-utils");
    }
    return {};
}

QString installCommand(Manager manager, Protocol protocol)
{
    const QString pkg = packageName(manager, protocol);
    switch (manager) {
    case Manager::Apt:
        return QStringLiteral("sudo apt install ") + pkg;
    case Manager::Dnf:
        return QStringLiteral("sudo dnf install ") + pkg;
    case Manager::Zypper:
        return QStringLiteral("sudo zypper install ") + pkg;
    case Manager::Pacman:
        return QStringLiteral("sudo pacman -S ") + pkg;
    case Manager::Unknown:
        break;
    }
    return QStringLiteral("Install the package that provides mount.%1 (usually \"%2\") with your distribution's package manager.")
        .arg(protocol == Protocol::Smb ? QStringLiteral("cifs") : QStringLiteral("nfs"), pkg);
}

bool isInstalled(Protocol protocol, const Exists &exists)
{
    const QString name = protocol == Protocol::Smb ? QStringLiteral("mount.cifs") : QStringLiteral("mount.nfs");
    for (const char *dir : {"/usr/sbin/", "/sbin/", "/usr/bin/"}) {
        if (exists(QLatin1String(dir) + name)) {
            return true;
        }
    }
    return false;
}

bool protocolFor(const QString &fsType, Protocol *protocol)
{
    if (fsType == QLatin1String("cifs") || fsType == QLatin1String("smb3")) {
        *protocol = Protocol::Smb;
        return true;
    }
    if (fsType == QLatin1String("nfs") || fsType == QLatin1String("nfs4")) {
        *protocol = Protocol::Nfs;
        return true;
    }
    return false;
}

} // namespace anchorage::clienttools
