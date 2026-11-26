// SPDX-License-Identifier: GPL-3.0-or-later
#include "applydialogs.h"

#include "linediff.h"

#include <KColorScheme>
#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace anchorage;

QString PreviewDialog::diffHtml(const QByteArray &oldText, const QByteArray &newText)
{
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    // Coloured text (not backgrounds) stays readable with light and dark colour schemes.
    const QString removed = scheme.foreground(KColorScheme::NegativeText).color().name();
    const QString added = scheme.foreground(KColorScheme::PositiveText).color().name();
    QString html = QStringLiteral("<pre style=\"margin:0\">");
    for (const auto &l : linediff::diff(oldText, newText)) {
        const QString text = QString::fromUtf8(l.text).toHtmlEscaped();
        switch (l.kind) {
        case linediff::Line::Kind::Same:
            html += QStringLiteral("  %1\n").arg(text);
            break;
        case linediff::Line::Kind::Removed:
            html += QStringLiteral("<span style=\"color:%1\"><b>- %2</b></span>\n").arg(removed, text);
            break;
        case linediff::Line::Kind::Added:
            html += QStringLiteral("<span style=\"color:%1\"><b>+ %2</b></span>\n").arg(added, text);
            break;
        }
    }
    return html + QStringLiteral("</pre>");
}

PreviewDialog::PreviewDialog(const QByteArray &oldText, const QByteArray &newText, bool forApply, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("preview"));
    setWindowTitle(i18n("Changes to /etc/fstab"));
    auto *v = new QVBoxLayout(this);
    const int n = linediff::changeCount(linediff::diff(oldText, newText));
    v->addWidget(new QLabel(i18np("%1 line changes. Removed lines are marked -, new lines +.", "%1 lines change. Removed lines are marked -, new lines +.", n), this));
    auto *view = new QTextBrowser(this);
    view->setObjectName(QStringLiteral("preview.diff"));
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setLineWrapMode(QTextEdit::NoWrap);
    view->setHtml(diffHtml(oldText, newText));
    v->addWidget(view, 1);
    auto *buttons = new QDialogButtonBox(forApply ? QDialogButtonBox::Apply | QDialogButtonBox::Cancel : QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (forApply) {
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &QDialog::accept);
    }
    v->addWidget(buttons);
    resize(900, 520);
}

ProtectedConfirmDialog::ProtectedConfirmDialog(const QStringList &paths, QWidget *parent)
    : ProtectedConfirmDialog(paths,
                             i18n("Mount in a Protected Location?"),
                             i18n("These mount points are inside protected system folders. Mounting something there can hide files the system "
                                  "needs, or let the mounted drive change how the computer behaves."),
                             i18n("Mount There"),
                             parent)
{
}

ProtectedConfirmDialog::ProtectedConfirmDialog(const QStringList &paths, const QString &title, const QString &explanation, const QString &okText, QWidget *parent)
    : QDialog(parent)
    , m_paths(paths)
{
    setObjectName(QStringLiteral("confirmProtected"));
    setWindowTitle(title);
    auto *v = new QVBoxLayout(this);
    auto *text = new QLabel(explanation + QStringLiteral("<br><br>") + i18n("Type each path exactly to confirm:"), this);
    text->setWordWrap(true);
    v->addWidget(text);
    auto *form = new QFormLayout;
    for (const QString &p : paths) {
        auto *e = new QLineEdit(this);
        e->setObjectName(QStringLiteral("confirm.path"));
        e->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        auto *label = new QLabel(p, this);
        label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        form->addRow(label, e);
        m_edits.append(e);
    }
    v->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(okText);
    m_ok->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (auto *e : std::as_const(m_edits)) {
        connect(e, &QLineEdit::textChanged, this, [this] {
            bool all = true;
            for (int i = 0; i < m_edits.size(); ++i) {
                all = all && m_edits.at(i)->text() == m_paths.at(i);
            }
            m_ok->setEnabled(all);
        });
    }
    v->addWidget(buttons);
}
