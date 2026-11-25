// SPDX-License-Identifier: GPL-3.0-or-later
#include "backupspage.h"

#include "datasource.h"

#include <KLocalizedString>

#include <QFontDatabase>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QVBoxLayout>

BackupsPage::BackupsPage(DataSource *source, QWidget *parent)
    : QWidget(parent)
    , m_source(source)
{
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(i18n("A copy of /etc/fstab is saved in /etc/anchorage/backups before every change Anchorage makes. "
                                  "Restoring a backup comes with editing in the next version."),
                             this);
    intro->setWordWrap(true);
    m_empty = new QLabel(i18n("No backups yet. The first one is made the first time Anchorage changes /etc/fstab."), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setForegroundRole(QPalette::PlaceholderText);

    auto *split = new QSplitter(Qt::Horizontal, this);
    m_list = new QListWidget(split);
    m_list->setObjectName(QStringLiteral("backups.list"));
    m_content = new QPlainTextEdit(split);
    m_content->setReadOnly(true);
    m_content->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_content->setLineWrapMode(QPlainTextEdit::NoWrap);
    split->setStretchFactor(1, 3);

    layout->addWidget(intro);
    layout->addWidget(m_empty, 1);
    layout->addWidget(split, 1);

    connect(m_list, &QListWidget::currentTextChanged, this, [this](const QString &name) {
        m_content->setPlainText(name.isEmpty() ? QString() : QString::fromUtf8(m_source->readBackup(name)));
    });
    setBackups({});
}

void BackupsPage::setBackups(const QStringList &names)
{
    const QString current = m_list->currentItem() ? m_list->currentItem()->text() : QString();
    m_list->clear();
    m_list->addItems(names);
    const bool any = !names.isEmpty();
    m_empty->setVisible(!any);
    m_list->parentWidget()->setVisible(any);
    const auto found = m_list->findItems(current, Qt::MatchExactly);
    m_list->setCurrentRow(found.isEmpty() ? 0 : m_list->row(found.first()));
}
