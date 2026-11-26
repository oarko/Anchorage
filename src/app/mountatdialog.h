// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "caller.h"
#include "datasource.h"
#include "fstab.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

// "Mount At…" (SPEC §6.1): driveadmins and administrators mount a drive or share at a
// folder they choose, without changing /etc/fstab (gone after a restart).
class MountAtDialog : public QDialog
{
    Q_OBJECT

public:
    // `initial`: device or share, suggested folder and type.
    MountAtDialog(const anchorage::fstab::Entry &initial, const QString &title, const Snapshot &snapshot, QWidget *parent = nullptr);

    anchorage::fstab::Entry entry() const;
    QStringList errors() const; // what stops mounting (OK is disabled while there are any)

    // "/mnt/<label>" or "/mnt/<device name>" (sda2, nvme1n1p3, ...).
    static QString suggestedFolder(const anchorage::drives::Volume &volume);

private:
    void refresh();

    anchorage::fstab::Entry m_initial;
    const Snapshot &m_snapshot;
    QLineEdit *m_mountPoint;
    QComboBox *m_type;
    QLineEdit *m_options;
    QCheckBox *m_readOnly;
    QCheckBox *m_ownFiles;
    QLabel *m_issues;
    QDialogButtonBox *m_buttons;
};
