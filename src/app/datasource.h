// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "caller.h"
#include "drives.h"
#include "helperclient.h"
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
    QList<anchorage::verify::Finding> verify(const QByteArray &content) override;

private:
    void watchFiles();
    QTimer m_debounce;
    QFileSystemWatcher *m_watcher = nullptr;
    int m_mountInfoFd = -1;
    QSocketNotifier *m_mountNotifier = nullptr;
};
