// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "drives.h"
#include "mounts.h"

#include <QString>
#include <QStringList>

#include <functional>

// Where drives are usually mounted (SPEC §5, §8): suggestions for the mount-point list.
namespace anchorage::locations
{

using Exists = std::function<bool(const QString &path)>;

// The per-user folder UDisks (Dolphin) mounts into: /run/media/<user> (UDisks' default;
// Fedora, Arch, openSUSE, Kubuntu 26.04) or /media/<user> (Debian, Mint, older Ubuntu).
// Detected from what is mounted now, then from which folder exists.
QString personalBase(const QString &user, const QList<mounts::Mount> &current, const Exists &exists);

// A folder name made from a label or device name: "My Games" -> "My-Games", "" -> "drive".
QString folderName(const QString &labelOrName);

// Mount-point suggestions for a drive, best first, without duplicates: where it is
// mounted now (if anywhere outside the system folders), /mnt/<name>, /media/<name>, and
// the personal UDisks location.
QStringList suggestions(const drives::Volume &volume, const QString &user, const QList<mounts::Mount> &current, const Exists &exists);

} // namespace anchorage::locations
