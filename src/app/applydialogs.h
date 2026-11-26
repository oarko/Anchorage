// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QStringList>

class QLineEdit;
class QPushButton;

// "Preview changes" (SPEC §9): the old and new /etc/fstab, line by line.
class PreviewDialog : public QDialog
{
    Q_OBJECT

public:
    // With `forApply`, the dialog offers Apply instead of just Close.
    PreviewDialog(const QByteArray &oldText, const QByteArray &newText, bool forApply, QWidget *parent = nullptr);
    static QString diffHtml(const QByteArray &oldText, const QByteArray &newText);
};

// Typed confirmation for mount points in protected system locations (SPEC §7):
// the OK button only works once every path has been typed exactly.
class ProtectedConfirmDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProtectedConfirmDialog(const QStringList &paths, QWidget *parent = nullptr);

private:
    QStringList m_paths;
    QList<QLineEdit *> m_edits;
    QPushButton *m_ok;
};
