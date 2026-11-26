// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "options.h"

#include <QString>
#include <QStringList>

#include <optional>

// The entry editor's Access tab (SPEC §8): plain-language choices that map to mount
// options. read() works out the choices from an existing option list; apply() changes
// only the options those choices own and keeps every other option where it was.
namespace anchorage::access
{

enum class Start {
    AtBoot, // (no option)
    OnRequest, // noauto
    WhenOpened, // noauto,x-systemd.automount
};

enum class Who {
    Admins, // (no option; nouser is the default)
    AnyUser, // user
    AnyUserAnyoneUnmounts, // users
};

// Permissions for file systems without Linux owners (FAT, exFAT, NTFS, SMB).
enum class Sharing {
    Unset, // nothing written: the driver default (others can read)
    OwnerOnly, // umask=077 / file_mode=0600,dir_mode=0700
    OthersRead, // umask=022 / file_mode=0644,dir_mode=0755
    Everyone, // umask=000 / file_mode=0666,dir_mode=0777
    Custom, // dmask/fmask or other values: left as they are
};

struct Settings {
    Start start = Start::AtBoot;
    Who who = Who::Admins;
    std::optional<quint32> uid; // owner of the files (Mask and Cifs models only)
    std::optional<quint32> gid;
    Sharing sharing = Sharing::Unset;
    bool nofail = false;
    bool netdev = false;

    bool operator==(const Settings &) const = default;
};

// Which choices make sense for a file system type.
struct Applicable {
    bool start = true;
    bool automount = true; // swap can't be automounted
    bool who = true;
    bool owner = false; // uid/gid/sharing
    bool netdev = true;
};
Applicable applicable(const QString &type, const options::Catalogue &catalogue = options::Catalogue::builtIn());

Settings read(const QStringList &options, const QString &type, const options::Catalogue &catalogue = options::Catalogue::builtIn());
QStringList apply(const QStringList &options,
                  const Settings &settings,
                  const QString &type,
                  const options::Catalogue &catalogue = options::Catalogue::builtIn());

// The options a single choice produces, for showing next to it ("noauto").
QStringList startOptions(Start start);
QStringList whoOptions(Who who);
QStringList sharingOptions(Sharing sharing, options::OwnerModel model);

// Settings the editor suggests for a new entry of this type: nofail for everything that
// isn't needed to start; _netdev and automount for network shares.
Settings defaultsFor(const QString &type, const options::Catalogue &catalogue = options::Catalogue::builtIn());

} // namespace anchorage::access
