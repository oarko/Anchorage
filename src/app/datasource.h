// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "caller.h"
#include "drives.h"
#include "mounts.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

class QFileSystemWatcher;
class QSocketNotifier;

// Everything the window shows, read without any privileges.
struct Snapshot {
    QByteArray fstab;
    QString fstabError; // empty if /etc/fstab was read
    QList<anchorage::drives::Volume> volumes;
    QString drivesError;
    QList<anchorage::mounts::Mount> mounts;
    QStringList backups; // newest first
    anchorage::Caller me;
};

// Where the data comes from: the real system, or fixtures in tests.
class DataSource : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    virtual Snapshot load() = 0;
    virtual QByteArray readBackup(const QString &name) = 0;

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

private:
    void watchFiles();
    QTimer m_debounce;
    QFileSystemWatcher *m_watcher = nullptr;
    int m_mountInfoFd = -1;
    QSocketNotifier *m_mountNotifier = nullptr;
};
