// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "caller.h"
#include "drives.h"
#include "clienttools.h"
#include "helperclient.h"
#include "shares.h"
#include "requests.h"
#include "settings.h"
#include "verify.h"
#include "mounts.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

#include <memory>

class QFileSystemWatcher;
class QSocketNotifier;
class QWidget;

// A person who can own files (Access tab, "Who owns the files").
struct Account {
    quint32 uid = 0;
    quint32 gid = 0;
    QString name;
    QString fullName;
};

// Everything the window shows, read without any privileges.
struct Snapshot {
    QByteArray fstab;
    QString fstabError; // empty if /etc/fstab was read
    QList<anchorage::drives::Volume> volumes;
    QString drivesError;
    QList<anchorage::mounts::Mount> mounts;
    QStringList backups; // newest first
    anchorage::Caller me;
    QList<Account> accounts; // regular user accounts (UID 1000-59999 with a login shell)
    anchorage::settings::Settings settings; // /etc/anchorage/config
};

// What a server offers (Connect to Server, SPEC §11).
struct Browse {
    anchorage::shares::SmbList smb; // shares, or why not
    bool smbTried = false;
    QList<anchorage::shares::Share> nfs;
    QString nfsError;
    bool nfsTried = false;
    QList<int> nfsVersions; // e.g. {4, 3}; empty if unknown
};

// Where the data comes from: the real system, or fixtures in tests.
class DataSource : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    virtual Snapshot load() = 0;
    virtual QByteArray readBackup(const QString &name) = 0;
    // Asks the helper to replace /etc/fstab (password prompt included). `done` runs once.
    using Done = std::function<void(const anchorage::client::Reply &)>;
    virtual void writeFstab(const anchorage::requests::WriteFstab &request, QWidget *window, Done done) = 0;
    virtual void restoreBackup(const anchorage::requests::Restore &request, QWidget *window, Done done) = 0;
    virtual void writeSettings(const anchorage::settings::Settings &settings, QWidget *window, Done done) = 0;
    // Mounting through the helper (SPEC §6.1). Reply data: "processes" when in use.
    virtual void mountEntry(const QString &mountPoint, QWidget *window, Done done) = 0;
    virtual void mountTemporary(const anchorage::requests::TemporaryMount &request, QWidget *window, Done done) = 0;
    virtual void unmount(const anchorage::requests::Unmount &request, QWidget *window, Done done) = 0;
    // Saves an SMB login as a root-only file (SPEC §11). Reply data: "path".
    virtual void writeCredentials(const anchorage::requests::Credentials &request, QWidget *window, Done done) = 0;
    // Lists what `host` offers over SMB and/or NFS, off the GUI thread. `done` runs once.
    using BrowseDone = std::function<void(const Browse &)>;
    virtual void browse(const QString &host, bool smb, bool nfs, const anchorage::shares::Login &login, BrowseDone done) = 0;
    // Client tools for mounting shares (mount.cifs, mount.nfs).
    virtual bool toolInstalled(anchorage::clienttools::Protocol protocol) = 0;
    virtual QString installCommand(anchorage::clienttools::Protocol protocol) = 0;
    virtual bool canInstall() = 0; // PackageKit available
    using InstallDone = std::function<void(bool ok, bool cancelled, const QString &message)>;
    virtual void installTool(anchorage::clienttools::Protocol protocol, InstallDone done) = 0;
    // Entries marked "any user": /usr/bin/mount and /usr/bin/umount run as the person.
    virtual void userMount(const QString &mountPoint, bool mount, Done done) = 0;
    // findmnt --verify on a proposed file (SPEC §9 "Validate before writing").
    virtual QList<anchorage::verify::Finding> verify(const QByteArray &content) = 0;

Q_SIGNALS:
    // Something changed (fstab edited, drive plugged in, something mounted).
    void changed();
};

// The real system: /etc/fstab, lsblk, /proc/self/mountinfo, /etc/anchorage/backups.
// Watches all of them and emits changed() (debounced) when anything moves.
class SystemDataSource : public DataSource
{
    Q_OBJECT

public:
    explicit SystemDataSource(QObject *parent = nullptr);
    ~SystemDataSource() override;

    Snapshot load() override;
    QByteArray readBackup(const QString &name) override;
    void writeFstab(const anchorage::requests::WriteFstab &request, QWidget *window, Done done) override;
    void restoreBackup(const anchorage::requests::Restore &request, QWidget *window, Done done) override;
    void writeSettings(const anchorage::settings::Settings &settings, QWidget *window, Done done) override;
    void mountEntry(const QString &mountPoint, QWidget *window, Done done) override;
    void mountTemporary(const anchorage::requests::TemporaryMount &request, QWidget *window, Done done) override;
    void unmount(const anchorage::requests::Unmount &request, QWidget *window, Done done) override;
    void userMount(const QString &mountPoint, bool mount, Done done) override;
    void writeCredentials(const anchorage::requests::Credentials &request, QWidget *window, Done done) override;
    void browse(const QString &host, bool smb, bool nfs, const anchorage::shares::Login &login, BrowseDone done) override;
    bool toolInstalled(anchorage::clienttools::Protocol protocol) override;
    QString installCommand(anchorage::clienttools::Protocol protocol) override;
    bool canInstall() override;
    void installTool(anchorage::clienttools::Protocol protocol, InstallDone done) override;
    QList<anchorage::verify::Finding> verify(const QByteArray &content) override;

private:
    void watchFiles();
    QTimer m_debounce;
    QFileSystemWatcher *m_watcher = nullptr;
    int m_mountInfoFd = -1;
    QSocketNotifier *m_mountNotifier = nullptr;
};
