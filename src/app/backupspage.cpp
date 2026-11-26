// SPDX-License-Identifier: GPL-3.0-or-later
#include "backupspage.h"

#include "datasource.h"
#include "sidebyside.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QVBoxLayout>

BackupsPage::BackupsPage(DataSource *source, QWidget *parent)
    : QWidget(parent)
    , m_source(source)
{
    setObjectName(QStringLiteral("backups"));
    auto *layout = new QVBoxLayout(this);
    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    m_empty = new QLabel(i18n("No backups yet. The first one is made the first time Anchorage changes /etc/fstab."), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setForegroundRole(QPalette::PlaceholderText);

    m_body = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    auto *split = new QSplitter(Qt::Horizontal, m_body);
    m_list = new QListWidget(split);
    m_list->setObjectName(QStringLiteral("backups.list"));
    m_diff = new SideBySideView(split);
    split->addWidget(m_list);
    split->addWidget(m_diff);
    split->setStretchFactor(1, 4);
    split->setSizes({220, 900});
    auto *bottom = new QHBoxLayout;
    m_summary = new QLabel(m_body);
    m_summary->setObjectName(QStringLiteral("backups.summary"));
    m_summary->setWordWrap(true);
    m_restore = new QPushButton(QIcon::fromTheme(QStringLiteral("document-revert")), i18n("Restore This Version…"), m_body);
    m_restore->setObjectName(QStringLiteral("backups.restore"));
    bottom->addWidget(m_summary, 1);
    bottom->addWidget(m_restore);
    bodyLayout->addWidget(split, 1);
    bodyLayout->addLayout(bottom);

    layout->addWidget(m_intro);
    layout->addWidget(m_empty, 1);
    layout->addWidget(m_body, 1);

    connect(m_list, &QListWidget::currentRowChanged, this, &BackupsPage::showSelected);
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (!selected().isEmpty()) {
            Q_EMIT restoreRequested(selected());
        }
    });
    setBackups({}, {}, 10);
}

QString BackupsPage::displayName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^fstab-(\\d{4}-\\d{2}-\\d{2}T\\d{2}-\\d{2}-\\d{2})Z(?:-(\\d+))?$"));
    const auto m = re.match(name);
    if (!m.hasMatch()) {
        return name;
    }
    QDateTime t = QDateTime::fromString(m.captured(1), QStringLiteral("yyyy-MM-ddTHH-mm-ss"));
    t.setTimeZone(QTimeZone::UTC);
    QString text = QLocale().toString(t.toLocalTime(), QStringLiteral("ddd d MMM yyyy, HH:mm:ss"));
    if (!m.captured(2).isEmpty()) {
        text += QStringLiteral(" (%1)").arg(m.captured(2).toInt() + 1);
    }
    return text;
}

void BackupsPage::setBackups(const QStringList &names, const QByteArray &current, int keep)
{
    m_current = current;
    m_intro->setText(i18np("A copy of /etc/fstab is saved before every change Anchorage makes; the newest %1 is kept. Choose a version to "
                           "compare it with the file as it is now: <b>left</b> is now, <b>right</b> is the backup.",
                           "A copy of /etc/fstab is saved before every change Anchorage makes; the newest %1 are kept. Choose a version to "
                           "compare it with the file as it is now: <b>left</b> is now, <b>right</b> is the backup.",
                           keep));
    const QString was = selected();
    {
        const QSignalBlocker block(m_list);
        m_list->clear();
        for (const QString &n : names) {
            auto *item = new QListWidgetItem(displayName(n), m_list);
            item->setData(Qt::UserRole, n);
            item->setToolTip(n);
        }
    }
    const bool any = !names.isEmpty();
    m_empty->setVisible(!any);
    m_body->setVisible(any);
    int row = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == was) {
            row = i;
        }
    }
    if (any) {
        m_list->setCurrentRow(row);
    }
    showSelected();
}

void BackupsPage::setRestoreEnabled(bool enabled, const QString &whyNot)
{
    m_restoreAllowed = enabled;
    m_whyNot = whyNot;
    showSelected();
}

QString BackupsPage::selected() const
{
    return m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
}

void BackupsPage::select(const QString &name)
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == name) {
            m_list->setCurrentRow(i);
        }
    }
}

void BackupsPage::showSelected()
{
    const QString name = selected();
    if (name.isEmpty()) {
        m_diff->setTexts({}, {});
        m_restore->setEnabled(false);
        m_summary->clear();
        return;
    }
    const QByteArray backup = m_source->readBackup(name);
    m_diff->setTitles(i18n("<b>Now</b> (/etc/fstab)"), i18n("<b>Backup</b> from %1", displayName(name)));
    m_diff->setTexts(m_current, backup);
    const int n = m_diff->differences();
    if (n == 0) {
        m_summary->setText(i18n("This backup is the same as the current file."));
    } else {
        m_summary->setText(i18np("%1 line differs. Restoring saves a backup of the current file first, so it can be undone.",
                                 "%1 lines differ. Restoring saves a backup of the current file first, so it can be undone.", n)
                           + (m_restoreAllowed || m_whyNot.isEmpty() ? QString() : QStringLiteral("\n") + m_whyNot));
    }
    m_restore->setEnabled(n > 0 && m_restoreAllowed);
}
