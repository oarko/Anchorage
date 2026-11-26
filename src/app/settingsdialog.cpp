// SPDX-License-Identifier: GPL-3.0-or-later
#include "settingsdialog.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace anchorage;

namespace
{
QLabel *note(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}
} // namespace

SettingsDialog::SettingsDialog(const settings::Settings &current, bool canChange, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("settings"));
    setWindowTitle(i18n("Configure Anchorage"));
    auto *v = new QVBoxLayout(this);

    auto *backups = new QGroupBox(i18n("Backups of /etc/fstab"), this);
    auto *bf = new QFormLayout(backups);
    m_keep = new QSpinBox(backups);
    m_keep->setObjectName(QStringLiteral("settings.keepBackups"));
    m_keep->setRange(settings::minKeepBackups, settings::maxKeepBackups);
    m_keep->setValue(current.keepBackups);
    bf->addRow(i18n("Versions to keep:"), m_keep);
    bf->addRow(note(i18n("A backup is saved before every change. When there are more than this, the oldest are deleted the next time "
                         "/etc/fstab is saved, never straight away."),
                    backups));
    v->addWidget(backups);

    auto *ntfs = new QGroupBox(i18n("New NTFS entries"), this);
    auto *nf = new QFormLayout(ntfs);
    m_ntfs = new QComboBox(ntfs);
    m_ntfs->setObjectName(QStringLiteral("settings.ntfsDriver"));
    m_ntfs->addItem(i18n("ntfs3 (in the kernel, fast)"), QStringLiteral("ntfs3"));
    m_ntfs->addItem(i18n("ntfs-3g (slower, honours permissions on the top folder)"), QStringLiteral("ntfs"));
    m_ntfs->setCurrentIndex(m_ntfs->findData(current.ntfsDriver));
    nf->addRow(i18n("Suggested driver:"), m_ntfs);
    nf->addRow(note(i18n("Used when a Windows (NTFS) drive is added to fstab. Each entry can still use either driver."), ntfs));
    v->addWidget(ntfs);

    if (!canChange) {
        m_keep->setEnabled(false);
        m_ntfs->setEnabled(false);
        v->addWidget(note(i18n("Only members of driveadmins and administrators can change these settings."), this));
    } else {
        v->addWidget(note(i18n("These settings apply to everyone on this computer. Saving them asks for your password."), this));
    }
    v->addStretch(1);
    auto *buttons = new QDialogButtonBox(canChange ? QDialogButtonBox::Save | QDialogButtonBox::Cancel : QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
    resize(520, 360);
}

settings::Settings SettingsDialog::settings() const
{
    settings::Settings s;
    s.keepBackups = m_keep->value();
    s.ntfsDriver = m_ntfs->currentData().toString();
    return s;
}
