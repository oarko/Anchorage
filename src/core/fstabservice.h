// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "caller.h"
#include "fstab.h"
#include "settings.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

// The privileged operations behind the helper's actions.
//
// Paths are parameters so tests can run against a temporary folder. The shipped helper
// only ever passes systemPaths(), built from the compile-time constants in paths.h.
namespace anchorage::service
{

struct Paths {
    QString fstabDir; // folder holding the fstab file
    QString fstabName; // "fstab"
    QString configDir; // holds the lock file and blocked-paths file
    QString backupDir;
    QString credentialsDir;
    // Prepended to mount points when creating or inspecting their folders. Empty on a real
    // system; tests point it at a temporary folder so /mnt/x becomes <tmp>/mnt/x.
    QString mountRoot;
    QString configName = QStringLiteral("config"); // settings file inside configDir
    QString mountInfo = QStringLiteral("/proc/self/mountinfo"); // what is mounted now
    QString procRoot = QStringLiteral("/proc"); // for finding programs using a drive
};
Paths systemPaths();

struct Outcome {
    QStringList errors;
    QString backupName; // backup made before writing, if any
    QStringList createdDirs; // mount-point folders that were created
    bool ok() const { return errors.isEmpty(); }
};

// SHA-256 (hex) of the current file, so the GUI can prove which version it edited.
QByteArray fingerprint(const QByteArray &content);

// Reads the fstab file (no symlinks, size-limited).
Result<QByteArray> readFstab(const Paths &paths);

// Replaces the whole file. Refused if the file changed since the GUI read it
// (`baselineFingerprint` mismatch) or if any difference breaks the caller's rights.
Outcome writeFstab(const Paths &paths,
                   const Caller &caller,
                   const QByteArray &baselineFingerprint,
                   const QByteArray &proposed,
                   bool confirmProtected,
                   bool createMountPoints);

// Adds or removes one shared network mount in /mnt (netmount group).
Outcome addSharedMount(const Paths &paths, const Caller &caller, const fstab::Entry &entry);
Outcome removeSharedMount(const Paths &paths, const Caller &caller, const QString &mountPoint);

// Settings (/etc/anchorage/config). Reading needs no rights; anyone may read the file.
settings::Settings readSettings(const Paths &paths);
// Only driveadmins and administrators may change them (SPEC §4.2).
Outcome writeSettings(const Paths &paths, const Caller &caller, const settings::Settings &settings);

// ---- Mounting (SPEC §6.1) ------------------------------------------------------------

// Runs one program (absolute path, argument list, never a shell). Tests pass a fake.
struct CommandResult {
    int exitCode = -1;
    QString output; // stdout and stderr together
};
using Runner = std::function<CommandResult(const QString &program, const QStringList &args)>;
Runner systemRunner(); // QProcess, 2 minute limit

struct MountOutcome {
    QStringList errors;
    QStringList createdDirs;
    QStringList processes; // when unmounting failed because the drive is in use
    bool ok() const { return errors.isEmpty(); }
};

MountOutcome mountEntry(const Paths &paths, const Caller &caller, const QString &mountPoint, const Runner &run);
MountOutcome mountTemporary(const Paths &paths, const Caller &caller, const fstab::Entry &entry, bool confirmProtected, const Runner &run);
MountOutcome unmount(const Paths &paths, const Caller &caller, const QString &mountPoint, bool confirmSystem, const Runner &run);

// Names of the programs with a file, folder or working directory on the file system
// mounted at `path` (sorted, no duplicates). Reads `procRoot` (/proc).
QStringList processesUsing(const QString &path, const QString &procRoot);

// Backups: names look like fstab-2026-11-24T23-10-05Z (optionally with -N).
bool isBackupName(const QString &name);
Result<QStringList> listBackups(const Paths &paths); // newest first
// Restoring goes through the same checks as writeFstab.
Outcome restoreBackup(const Paths &paths, const Caller &caller, const QString &backupName, bool confirmProtected);

} // namespace anchorage::service
