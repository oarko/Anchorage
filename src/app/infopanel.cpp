// SPDX-License-Identifier: GPL-3.0-or-later
#include "infopanel.h"

#include "inventorymodel.h"

#include <KLocalizedString>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

using namespace anchorage::inventory;

InfoPanel::InfoPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("info"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    m_icon = new QLabel(this);
    m_icon->setAlignment(Qt::AlignCenter);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("info.title"));
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    QFont tf = m_title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    tf.setBold(true);
    m_title->setFont(tf);
    m_subtitle = new QLabel(this);
    m_subtitle->setAlignment(Qt::AlignCenter);
    m_subtitle->setWordWrap(true);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);

    m_form = new QFormLayout;
    m_form->setLabelAlignment(Qt::AlignRight);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_fstabBox = new QWidget(this);
    auto *fstabLayout = new QVBoxLayout(m_fstabBox);
    fstabLayout->setContentsMargins(0, 6, 0, 0);
    auto *fstabTitle = new QLabel(i18n("Line in /etc/fstab"), m_fstabBox);
    QFont bold = fstabTitle->font();
    bold.setBold(true);
    fstabTitle->setFont(bold);
    m_fstabLine = new QLabel(m_fstabBox);
    m_fstabLine->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_fstabLine->setWordWrap(true);
    m_fstabLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fstabLine->setFrameShape(QFrame::StyledPanel);
    m_fstabLine->setMargin(6);
    fstabLayout->addWidget(fstabTitle);
    fstabLayout->addWidget(m_fstabLine);

    m_open = new QPushButton(QIcon::fromTheme(QStringLiteral("system-file-manager")), i18n("Open in File Manager"), this);
    m_copy = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy fstab Line"), this);
    connect(m_open, &QPushButton::clicked, this, [this] {
        if (m_item && m_item->mounted) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_item->mountedAt));
        }
    });
    connect(m_copy, &QPushButton::clicked, this, [this] {
        if (m_item && m_item->entry) {
            QApplication::clipboard()->setText(m_item->fstabText.trimmed());
        }
    });

    m_adminBox = new QWidget(this);
    auto *adminLayout = new QVBoxLayout(m_adminBox);
    adminLayout->setContentsMargins(0, 6, 0, 0);
    auto *adminTitle = new QLabel(i18n("Administrator"), m_adminBox);
    adminTitle->setFont(bold);
    m_edit = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), QString(), m_adminBox);
    m_edit->setObjectName(QStringLiteral("info.edit"));
    m_remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove from fstab"), m_adminBox);
    m_remove->setObjectName(QStringLiteral("info.remove"));
    adminLayout->addWidget(adminTitle);
    adminLayout->addWidget(m_edit);
    adminLayout->addWidget(m_remove);
    connect(m_edit, &QPushButton::clicked, this, &InfoPanel::editRequested);
    connect(m_remove, &QPushButton::clicked, this, &InfoPanel::removeRequested);
    m_adminBox->hide();

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("info.note"));
    m_note->setWordWrap(true);
    m_note->setForegroundRole(QPalette::PlaceholderText);

    layout->addWidget(m_icon);
    layout->addWidget(m_title);
    layout->addWidget(m_subtitle);
    layout->addSpacing(6);
    layout->addLayout(m_form);
    layout->addWidget(m_fstabBox);
    layout->addSpacing(6);
    layout->addWidget(m_open);
    layout->addWidget(m_copy);
    layout->addWidget(m_adminBox);
    layout->addStretch(1);
    layout->addWidget(m_note);

    setMinimumWidth(280);
    showSummary(0, 0, QString());
}

void InfoPanel::setActions(const QString &editText, bool canRemove, const QString &hint)
{
    m_adminBox->setVisible(!editText.isEmpty() || canRemove);
    m_edit->setVisible(!editText.isEmpty());
    m_edit->setText(editText);
    m_remove->setVisible(canRemove);
    m_note->setText(hint);
}

void InfoPanel::clearRows()
{
    while (m_form->rowCount() > 0) {
        m_form->removeRow(0);
    }
}

void InfoPanel::addRow(const QString &label, const QString &value, bool mono)
{
    if (value.isEmpty()) {
        return;
    }
    auto *v = new QLabel(value, this);
    v->setWordWrap(true);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (mono) {
        v->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }
    m_form->addRow(label, v);
}

void InfoPanel::showSummary(int total, int mounted, const QString &viewName)
{
    m_item.reset();
    clearRows();
    m_icon->setPixmap(QIcon::fromTheme(QStringLiteral("drive-multidisk"), QIcon::fromTheme(QStringLiteral("drive-harddisk"))).pixmap(64, 64));
    m_title->setText(viewName);
    m_subtitle->setText(i18np("%1 item, %2 mounted", "%1 items, %2 mounted", total, mounted));
    m_fstabBox->hide();
    m_open->hide();
    m_copy->hide();
    m_adminBox->hide();
    m_note->clear();
}

void InfoPanel::showMessage(const QString &title, const QString &text, const QString &iconName)
{
    m_item.reset();
    clearRows();
    m_icon->setPixmap(QIcon::fromTheme(iconName).pixmap(64, 64));
    m_title->setText(title);
    m_subtitle->setText(text);
    m_fstabBox->hide();
    m_open->hide();
    m_copy->hide();
    m_adminBox->hide();
    m_note->clear();
}

void InfoPanel::showItem(const std::optional<Item> &item)
{
    if (!item) {
        return;
    }
    m_item = item;
    const Item &it = *item;
    clearRows();
    m_icon->setPixmap(InventoryModel::icon(it).pixmap(64, 64));
    m_title->setText(it.title);
    m_subtitle->setText(InventoryModel::subtitle(it));

    if (it.volume) {
        const auto &v = *it.volume;
        addRow(i18n("Device:"), v.path, true);
        addRow(i18n("Disk:"), v.diskModel.isEmpty() ? v.diskPath : QStringLiteral("%1 (%2)").arg(v.diskModel, v.transport.toUpper()));
        addRow(i18n("Label:"), v.label);
        addRow(i18n("File system:"), v.fsVersion.isEmpty() ? v.fsType : QStringLiteral("%1 %2").arg(v.fsType, v.fsVersion));
        addRow(i18n("UUID:"), v.uuid, true);
        addRow(i18n("Partition size:"), formatSize(v.size));
    } else {
        addRow(i18n("Source:"), it.source, true);
    }
    if (it.size > 0 && it.used >= 0) {
        addRow(i18n("Used:"), i18n("%1 of %2 (%3%)", formatSize(it.used), formatSize(it.size), static_cast<int>(100.0 * static_cast<double>(it.used) / static_cast<double>(it.size))));
    }
    if (it.entry) {
        addRow(i18n("Mount point:"), it.entry->mountPoint, true);
        addRow(i18n("Mounts:"), InventoryModel::startupText(it.startup));
        addRow(i18n("Options:"), it.entry->options.isEmpty() ? QStringLiteral("defaults") : it.entry->options.join(QLatin1Char(',')), true);
    }
    addRow(i18n("Status:"), it.deviceMissing ? i18n("The device isn't connected") : it.mounted ? i18n("Mounted") : i18n("Not mounted"));
    if (it.mounted && (it.mountedAt != it.mountPoint || !it.entry)) {
        addRow(i18n("Mounted at:"), it.mountedAt, true);
    }
    if (it.system) {
        addRow(i18n("Protected:"), i18n("Needed to start the computer; only driveadmins and administrators can change it."));
    }

    if (it.entry) {
        m_fstabLine->setText(it.fstabText.trimmed());
        m_fstabBox->show();
        m_copy->show();
    } else {
        m_fstabBox->hide();
        m_copy->hide();
    }
    m_open->setVisible(it.mounted && it.kind != Kind::Swap);
}
