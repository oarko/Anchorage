// SPDX-License-Identifier: GPL-3.0-or-later
#include "safefs.h"

#include <QFile>
#include <QRandomGenerator>

#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace anchorage::safefs
{

namespace
{
QString errnoText(const QString &what, int err)
{
    return QStringLiteral("%1: %2").arg(what, QString::fromLocal8Bit(strerror(err)));
}

bool runningAsRoot()
{
    return geteuid() == 0;
}

QStringList components(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

// Rejects names that could escape the folder they're used in.
bool isPlainName(const QString &name)
{
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..") && !name.contains(QLatin1Char('/'))
        && !name.contains(QChar(0));
}

constexpr int dirFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

// Explains why openat(parent, name, dirFlags) failed. With O_DIRECTORY|O_NOFOLLOW Linux
// may report a symlink as ENOTDIR, so look at what is actually there.
QString openFailure(int parent, const QByteArray &name, const QString &walked, int err)
{
    struct stat st {
    };
    if (::fstatat(parent, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(st.st_mode)) {
            return QStringLiteral("%1 is a symbolic link; refusing to follow it.").arg(walked);
        }
        if (!S_ISDIR(st.st_mode)) {
            return QStringLiteral("%1 exists but is not a folder.").arg(walked);
        }
    }
    return errnoText(QStringLiteral("Cannot open %1").arg(walked), err);
}
} // namespace

Fd::~Fd()
{
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

Fd::Fd(Fd &&other) noexcept
    : m_fd(other.m_fd)
{
    other.m_fd = -1;
}

Fd &Fd::operator=(Fd &&other) noexcept
{
    if (this != &other) {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = other.m_fd;
        other.m_fd = -1;
    }
    return *this;
}

Result<Fd> openDirectory(const QString &path)
{
    if (!path.startsWith(QLatin1Char('/'))) {
        return Result<Fd>::failure(QStringLiteral("%1 is not an absolute path.").arg(path));
    }
    Fd fd(::open("/", dirFlags));
    if (!fd.valid()) {
        return Result<Fd>::failure(errnoText(QStringLiteral("Cannot open /"), errno));
    }
    QString walked;
    for (const QString &part : components(path)) {
        if (!isPlainName(part)) {
            return Result<Fd>::failure(QStringLiteral("%1 is not a normalised path.").arg(path));
        }
        walked += QLatin1Char('/') + part;
        const QByteArray name = QFile::encodeName(part);
        Fd next(::openat(fd.get(), name.constData(), dirFlags));
        if (!next.valid()) {
            return Result<Fd>::failure(openFailure(fd.get(), name, walked, errno));
        }
        fd = std::move(next);
    }
    return fd;
}

Status makeDirectories(const QString &path, mode_t mode, Owner owner, QStringList *created)
{
    if (!path.startsWith(QLatin1Char('/'))) {
        return Status::failure(QStringLiteral("%1 is not an absolute path.").arg(path));
    }
    Fd fd(::open("/", dirFlags));
    if (!fd.valid()) {
        return Status::failure(errnoText(QStringLiteral("Cannot open /"), errno));
    }
    QString walked;
    for (const QString &part : components(path)) {
        if (!isPlainName(part)) {
            return Status::failure(QStringLiteral("%1 is not a normalised path.").arg(path));
        }
        walked += QLatin1Char('/') + part;
        const QByteArray name = QFile::encodeName(part);
        bool madeHere = false;
        if (::mkdirat(fd.get(), name.constData(), 0700) == 0) {
            madeHere = true; // start private; final mode/owner set below once opened safely
        } else if (errno != EEXIST) {
            return Status::failure(errnoText(QStringLiteral("Cannot create %1").arg(walked), errno));
        }
        Fd next(::openat(fd.get(), name.constData(), dirFlags));
        if (!next.valid()) {
            return Status::failure(openFailure(fd.get(), name, walked, errno));
        }
        if (madeHere) {
            if (runningAsRoot() && ::fchown(next.get(), owner.uid, owner.gid) != 0) {
                return Status::failure(errnoText(QStringLiteral("Cannot set the owner of %1").arg(walked), errno));
            }
            if (::fchmod(next.get(), mode) != 0) {
                return Status::failure(errnoText(QStringLiteral("Cannot set permissions of %1").arg(walked), errno));
            }
            if (created) {
                created->append(walked);
            }
        }
        fd = std::move(next);
    }
    return Status::success();
}

Result<bool> isEmptyDirectory(const QString &path)
{
    auto dir = listDirectory(path);
    if (!dir) {
        return Result<bool>::failure(dir.error());
    }
    return dir->isEmpty();
}

bool exists(const QString &path)
{
    struct stat st {
    };
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0;
}

Result<QByteArray> readFile(const QString &dir, const QString &name, qsizetype maxSize)
{
    if (!isPlainName(name)) {
        return Result<QByteArray>::failure(QStringLiteral("Invalid file name."));
    }
    auto dirFd = openDirectory(dir);
    if (!dirFd) {
        return Result<QByteArray>::failure(dirFd.error());
    }
    Fd fd(::openat(dirFd->get(), QFile::encodeName(name).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY));
    if (!fd.valid()) {
        return Result<QByteArray>::failure(errnoText(QStringLiteral("Cannot open %1/%2").arg(dir, name), errno));
    }
    struct stat st {
    };
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return Result<QByteArray>::failure(QStringLiteral("%1/%2 is not a regular file.").arg(dir, name));
    }
    QByteArray data;
    std::array<char, 8192> buf{};
    while (true) {
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result<QByteArray>::failure(errnoText(QStringLiteral("Cannot read %1/%2").arg(dir, name), errno));
        }
        if (n == 0) {
            break;
        }
        data.append(buf.data(), n);
        if (data.size() > maxSize) {
            return Result<QByteArray>::failure(QStringLiteral("%1/%2 is too large.").arg(dir, name));
        }
    }
    return data;
}

Status writeFileAtomic(const QString &dir, const QString &name, const QByteArray &data, mode_t mode, Owner owner)
{
    if (!isPlainName(name)) {
        return Status::failure(QStringLiteral("Invalid file name."));
    }
    auto dirFd = openDirectory(dir);
    if (!dirFd) {
        return Status::failure(dirFd.error());
    }

    // Unpredictable temp name, created exclusively, never through a symlink.
    QByteArray tmpName;
    Fd fd;
    for (int attempt = 0; attempt < 8 && !fd.valid(); ++attempt) {
        tmpName = QFile::encodeName(QStringLiteral(".%1.anchorage-%2").arg(name).arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0')));
        fd = Fd(::openat(dirFd->get(), tmpName.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!fd.valid() && errno != EEXIST) {
            return Status::failure(errnoText(QStringLiteral("Cannot create a temporary file in %1").arg(dir), errno));
        }
    }
    if (!fd.valid()) {
        return Status::failure(QStringLiteral("Cannot create a temporary file in %1.").arg(dir));
    }

    const auto abandon = [&](const QString &why) {
        ::unlinkat(dirFd->get(), tmpName.constData(), 0);
        return Status::failure(why);
    };

    qsizetype written = 0;
    while (written < data.size()) {
        const QByteArrayView remaining = QByteArrayView(data).sliced(written);
        const ssize_t n = ::write(fd.get(), remaining.data(), static_cast<size_t>(remaining.size()));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return abandon(errnoText(QStringLiteral("Cannot write %1/%2").arg(dir, name), errno));
        }
        written += n;
    }
    if (runningAsRoot() && ::fchown(fd.get(), owner.uid, owner.gid) != 0) {
        return abandon(errnoText(QStringLiteral("Cannot set the owner of %1/%2").arg(dir, name), errno));
    }
    if (::fchmod(fd.get(), mode) != 0) {
        return abandon(errnoText(QStringLiteral("Cannot set permissions of %1/%2").arg(dir, name), errno));
    }
    if (::fsync(fd.get()) != 0) {
        return abandon(errnoText(QStringLiteral("Cannot flush %1/%2 to disk").arg(dir, name), errno));
    }
    fd = Fd();
    if (::renameat(dirFd->get(), tmpName.constData(), dirFd->get(), QFile::encodeName(name).constData()) != 0) {
        return abandon(errnoText(QStringLiteral("Cannot replace %1/%2").arg(dir, name), errno));
    }
    ::fsync(dirFd->get()); // best effort: the rename itself already happened
    return Status::success();
}

Result<QStringList> listDirectory(const QString &dir)
{
    auto dirFd = openDirectory(dir);
    if (!dirFd) {
        return Result<QStringList>::failure(dirFd.error());
    }
    const int dupFd = ::fcntl(dirFd->get(), F_DUPFD_CLOEXEC, 0);
    if (dupFd < 0) {
        return Result<QStringList>::failure(errnoText(QStringLiteral("Cannot read %1").arg(dir), errno));
    }
    DIR *d = ::fdopendir(dupFd);
    if (!d) {
        ::close(dupFd);
        return Result<QStringList>::failure(errnoText(QStringLiteral("Cannot read %1").arg(dir), errno));
    }
    QStringList names;
    while (const dirent *ent = ::readdir(d)) {
        const QString n = QFile::decodeName(ent->d_name);
        if (n != QLatin1String(".") && n != QLatin1String("..")) {
            names.append(n);
        }
    }
    ::closedir(d);
    return names;
}

Status removeFile(const QString &dir, const QString &name)
{
    if (!isPlainName(name)) {
        return Status::failure(QStringLiteral("Invalid file name."));
    }
    auto dirFd = openDirectory(dir);
    if (!dirFd) {
        return Status::failure(dirFd.error());
    }
    if (::unlinkat(dirFd->get(), QFile::encodeName(name).constData(), 0) != 0) {
        return Status::failure(errnoText(QStringLiteral("Cannot remove %1/%2").arg(dir, name), errno));
    }
    return Status::success();
}

Result<Fd> lock(const QString &dir, const QString &name)
{
    if (!isPlainName(name)) {
        return Result<Fd>::failure(QStringLiteral("Invalid file name."));
    }
    auto dirFd = openDirectory(dir);
    if (!dirFd) {
        return Result<Fd>::failure(dirFd.error());
    }
    Fd fd(::openat(dirFd->get(), QFile::encodeName(name).constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!fd.valid()) {
        return Result<Fd>::failure(errnoText(QStringLiteral("Cannot open lock file %1/%2").arg(dir, name), errno));
    }
    while (::flock(fd.get(), LOCK_EX) != 0) {
        if (errno != EINTR) {
            return Result<Fd>::failure(errnoText(QStringLiteral("Cannot lock %1/%2").arg(dir, name), errno));
        }
    }
    return fd;
}

} // namespace anchorage::safefs
