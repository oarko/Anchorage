// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings.h"

#include <QDialog>

class QComboBox;
class QSpinBox;

// Anchorage's system-wide settings (/etc/anchorage/config). Only driveadmins and
// administrators can change them; everyone else sees them read-only.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(const anchorage::settings::Settings &current, bool canChange, QWidget *parent = nullptr);
    anchorage::settings::Settings settings() const;

private:
    QSpinBox *m_keep;
    QComboBox *m_ntfs;
};
