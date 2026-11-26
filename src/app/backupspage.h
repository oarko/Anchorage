// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QWidget>

class DataSource;
class QLabel;
class QListWidget;
class QPushButton;
class SideBySideView;

// Backups of /etc/fstab (SPEC §9): the list of saved versions, the current file next to
// the selected backup, and "Restore This Version…".
class BackupsPage : public QWidget
{
    Q_OBJECT

public:
    BackupsPage(DataSource *source, QWidget *parent = nullptr);
    // `current` is /etc/fstab as it is on disk now. `keep` is how many are kept.
    void setBackups(const QStringList &names, const QByteArray &current, int keep);
    void setRestoreEnabled(bool enabled, const QString &whyNot = {});
    QString selected() const;
    void select(const QString &name);

    // "fstab-2026-11-25T23-57-45Z" -> "Wed 25 Nov 2026, 23:57:45" in local time.
    static QString displayName(const QString &backupName);

Q_SIGNALS:
    void restoreRequested(const QString &name);

private:
    void showSelected();

    DataSource *m_source;
    QLabel *m_intro;
    QListWidget *m_list;
    SideBySideView *m_diff;
    QLabel *m_summary;
    QPushButton *m_restore;
    QLabel *m_empty;
    QWidget *m_body;
    QByteArray m_current;
    bool m_restoreAllowed = true;
    QString m_whyNot;
};
