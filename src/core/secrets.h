// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fstab.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

// Passwords typed straight into /etc/fstab (SPEC §11). The file is readable by every user,
// so these are moved into root-only saved logins, and removed from backups afterwards.
namespace anchorage::secrets
{

inline constexpr const char *redactedValue = "removed-by-anchorage";

struct Plaintext {
    int line = -1;
    QString server; // from //server/share
    QString user; // username= / user= (may be empty)
    QString password;
    QString domain; // domain= / dom= / workgroup=
};

// SMB entries (cifs, smb3) with password= or pass=, in file order. Already redacted ones
// are not reported.
QList<Plaintext> find(const fstab::File &file);

// The entry with its login options (username/user=, password/pass, domain/dom/workgroup)
// replaced by credentials=<path>, at the place of the first one.
fstab::Entry withCredentials(const fstab::Entry &entry, const QString &credentialsPath);

// Passwords that appear in `before` but no longer anywhere in `after`.
QStringList removedPasswords(QByteArrayView before, QByteArrayView after);

// `content` with password=/pass= options whose value is one of `passwords` replaced by
// redactedValue; every other byte unchanged. Returns `content` unchanged if none matched.
QByteArray redact(QByteArrayView content, const QStringList &passwords);

} // namespace anchorage::secrets
