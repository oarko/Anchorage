// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <sys/types.h>

// Symlink-safe file operations for the root helper (CLAUDE.md security rule 6).
//
// Every path is walked one folder at a time from "/" with O_NOFOLLOW, so a symlink
// anywhere in the path makes the operation fail instead of being followed. Paths must
// already be absolute and normalised (see policy::checkPathSyntax).
namespace anchorage::safefs
{

// RAII file descriptor.
class Fd
{
public:
    Fd() = default;
    explicit Fd(int fd)
        : m_fd(fd)
    {
    }
    ~Fd();
    Fd(Fd &&other) noexcept;
    Fd &operator=(Fd &&other) noexcept;
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }

private:
    int m_fd = -1;
};

// Opens an existing directory without following symlinks anywhere in the path.
Result<Fd> openDirectory(const QString &path);

struct Owner {
    uid_t uid = 0;
    gid_t gid = 0;
};

// Creates `path` and any missing parent folders with `mode`. Folders that are created
// get `owner` when running as root. Existing folders are left untouched.
// `created` receives the folders that were made, outermost first.
Status makeDirectories(const QString &path, mode_t mode, Owner owner, QStringList *created = nullptr);

// true if `path` is an existing, empty directory; false if it has entries.
Result<bool> isEmptyDirectory(const QString &path);
bool exists(const QString &path); // without following symlinks

// Reads a regular file `name` inside `dir` (no symlinks), at most `maxSize` bytes.
Result<QByteArray> readFile(const QString &dir, const QString &name, qsizetype maxSize);

// Atomically replaces `dir`/`name` with `data`: temp file in the same folder, fsync,
// rename over the old file, fsync the folder. When running as root the file gets
// `owner`. `mode` is applied exactly (not affected by umask).
Status writeFileAtomic(const QString &dir, const QString &name, const QByteArray &data, mode_t mode, Owner owner);

Result<QStringList> listDirectory(const QString &dir); // names only, no "." or ".."
Status removeFile(const QString &dir, const QString &name);

// Exclusive lock on `dir`/`name` (created if missing), held until the Fd is destroyed.
Result<Fd> lock(const QString &dir, const QString &name);

} // namespace anchorage::safefs
