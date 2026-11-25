// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class DataSource;
class QListWidget;
class QPlainTextEdit;
class QLabel;

// Backups of /etc/fstab made before every change (read-only in this version).
class BackupsPage : public QWidget
{
    Q_OBJECT

public:
    BackupsPage(DataSource *source, QWidget *parent = nullptr);
    void setBackups(const QStringList &names);

private:
    DataSource *m_source;
    QListWidget *m_list;
    QPlainTextEdit *m_content;
    QLabel *m_empty;
};
