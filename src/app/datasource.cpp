// SPDX-License-Identifier: GPL-3.0-or-later
#include "datasource.h"

#include "fstabservice.h"
#include "paths.h"
#include "safefs.h"

#include <Solid/DeviceNotifier>

#include <QFile>
#include <QFileSystemWatcher>
#include <QSocketNotifier>
#include <QWidget>

#include <algorithm>

#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

using namespace anchorage;

namespace
{
QList<Account> regularAccounts()
{
    QList<Account> users;
    setpwent();
    while (const passwd *pw = getpwent()) {
        const QString shell = QString::fromLocal8Bit(pw->pw_shell);
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000 || shell.endsWith(QLatin1String("nologin")) || shell.endsWith(QLatin1String("false"))) {
            continue;
        }
        users.append({pw->pw_uid, pw->pw_gid, QString::fromLocal8Bit(pw->pw_name), QString::fromLocal8Bit(pw->pw_gecos).section(QLatin1Char(','), 0, 0)});
    }
    endpwent();
    std::sort(users.begin(), users.end(), [](const Account &a, const Account &b) {
        return a.uid < b.uid;
    });
    return users;
}
} // namespace

SystemDataSource::SystemDataSource(QObject *parent)
    : DataSource(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(400);
    connect(&m_debounce, &QTimer::timeout, this, &DataSource::changed);

    // Drives plugged in or removed.
    auto *notifier = Solid::DeviceNotifier::instance();
    connect(notifier, &Solid::DeviceNotifier::deviceAdded, &m_debounce, qOverload<>(&QTimer::start));
    connect(notifier, &Solid::DeviceNotifier::deviceRemoved, &m_debounce, qOverload<>(&QTimer::start));

    // Anything mounted or unmounted: the kernel flags /proc/self/mountinfo (POLLPRI).
    m_mountInfoFd = ::open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (m_mountInfoFd >= 0) {
        m_mountNotifier = new QSocketNotifier(m_mountInfoFd, QSocketNotifier::Exception, this);
        connect(m_mountNotifier, &QSocketNotifier::activated, this, [this] {
            char buf[4096];
            ::lseek(m_mountInfoFd, 0, SEEK_SET);
            while (::read(m_mountInfoFd, buf, sizeof(buf)) > 0) {
            }
            m_debounce.start();
        });
    }

    // /etc/fstab is replaced atomically (new inode), so watch the folder too.
    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(this, &DataSource::changed, this, &SystemDataSource::watchFiles);
    watchFiles();
}

SystemDataSource::~SystemDataSource()
{
    if (m_mountInfoFd >= 0) {
        ::close(m_mountInfoFd);
    }
}

void SystemDataSource::watchFiles()
{
    const QString fstab = QString::fromUtf8(paths::fstab);
    const QString backups = QString::fromUtf8(paths::backupDir);
    const QString config = QString::fromUtf8(paths::configFile);
    for (const QString &p : {fstab, backups, config}) {
        if (QFile::exists(p) && !m_watcher->files().contains(p) && !m_watcher->directories().contains(p)) {
            m_watcher->addPath(p);
        }
    }
}

Snapshot SystemDataSource::load()
{
    Snapshot s;
    QFile f(QString::fromUtf8(paths::fstab));
    if (f.open(QIODevice::ReadOnly)) {
        s.fstab = f.read(fstab::maxFileSize + 1);
        if (s.fstab.size() > fstab::maxFileSize) {
            s.fstab.clear();
            s.fstabError = QStringLiteral("/etc/fstab is unusually large and was not read.");
        }
    } else {
        s.fstabError = f.errorString();
    }

    if (auto v = drives::scan()) {
        s.volumes = *v;
    } else {
        s.drivesError = v.error();
    }
    if (auto m = mounts::readCurrent()) {
        s.mounts = *m;
    }
    if (auto b = service::listBackups(service::systemPaths())) {
        s.backups = *b;
    }
    if (auto me = resolveCaller(getuid(), defaultGroupNames())) {
        s.me = *me;
    }
    s.accounts = regularAccounts();
    s.settings = service::readSettings(service::systemPaths());
    return s;
}

QByteArray SystemDataSource::readBackup(const QString &name)
{
    if (!service::isBackupName(name)) {
        return {};
    }
    auto data = safefs::readFile(QString::fromUtf8(paths::backupDir), name, fstab::maxFileSize);
    return data ? *data : QByteArray();
}

void SystemDataSource::writeFstab(const requests::WriteFstab &request, QWidget *window, Done done)
{
    client::run(client::Action::WriteFstab, requests::encodeWriteFstab(request), window ? window->window()->windowHandle() : nullptr, this,
                std::move(done));
}

QList<verify::Finding> SystemDataSource::verify(const QByteArray &content)
{
    auto f = verify::findmnt(content);
    return f ? *f : QList<verify::Finding>{};
}

void SystemDataSource::restoreBackup(const requests::Restore &request, QWidget *window, Done done)
{
    client::run(client::Action::RestoreBackup, requests::encodeRestore(request), window ? window->window()->windowHandle() : nullptr, this,
                std::move(done));
}

void SystemDataSource::writeSettings(const settings::Settings &s, QWidget *window, Done done)
{
    client::run(client::Action::WriteSettings, requests::encodeSettings(s), window ? window->window()->windowHandle() : nullptr, this,
                std::move(done));
}
