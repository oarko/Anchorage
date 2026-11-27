// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fstab.h"
#include "settings.h"
#include "result.h"

#include <QVariantMap>

// Decoding of the argument maps the GUI sends to the helper. Every field is checked for
// presence and exact type; unknown keys are refused, so a request means one thing only.
namespace anchorage::requests
{

struct WriteFstab {
    QByteArray baselineFingerprint; // 64 hex characters
    QByteArray content;
    bool confirmProtected = false;
    bool createMountPoints = false;
};

struct TemporaryMount {
    fstab::Entry entry; // dump and pass are ignored
    bool confirmProtected = false;
};

// mountentry and unmount take a list, handled in order and stopping at the first failure,
// so a drive and its binds need one request (and one password prompt). SPEC §8.
inline constexpr int kMaxMountPoints = 64;

struct Unmount {
    QStringList mountPoints;
    bool confirmSystem = false;
};

struct Restore {
    QString backupName;
    bool confirmProtected = false;
};

Result<WriteFstab> decodeWriteFstab(const QVariantMap &args);
Result<fstab::Entry> decodeEntry(const QVariantMap &args); // addsharedmount
Result<QString> decodeMountPoint(const QVariantMap &args); // removesharedmount
Result<Restore> decodeRestore(const QVariantMap &args);
Result<settings::Settings> decodeSettings(const QVariantMap &args); // writesettings
Result<TemporaryMount> decodeTemporaryMount(const QVariantMap &args); // mounttemporary
Result<Unmount> decodeUnmount(const QVariantMap &args); // unmount
Result<QStringList> decodeMountPoints(const QVariantMap &args); // mountentry

struct Credentials {
    QString server;
    QString user;
    QString password;
    QString domain;
};
Result<Credentials> decodeCredentials(const QVariantMap &args); // writecredentials

// Builders used by the GUI side (and tests), so both ends agree on the keys.
QVariantMap encodeWriteFstab(const WriteFstab &request);
QVariantMap encodeEntry(const fstab::Entry &entry);
QVariantMap encodeMountPoint(const QString &mountPoint);
QVariantMap encodeRestore(const Restore &request);
QVariantMap encodeSettings(const settings::Settings &settings);
QVariantMap encodeTemporaryMount(const TemporaryMount &request);
QVariantMap encodeUnmount(const Unmount &request);
QVariantMap encodeMountPoints(const QStringList &mountPoints);
QVariantMap encodeCredentials(const Credentials &request);

} // namespace anchorage::requests
