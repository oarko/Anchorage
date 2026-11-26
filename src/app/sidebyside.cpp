// SPDX-License-Identifier: GPL-3.0-or-later
#include "sidebyside.h"

#include "linediff.h"

#include <KColorScheme>

#include <QFontDatabase>
#include <QGridLayout>
#include <QLabel>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>

using namespace anchorage;

namespace
{
QTextEdit *makeView(QWidget *parent, const QString &name)
{
    auto *v = new QTextEdit(parent);
    v->setObjectName(name);
    v->setReadOnly(true);
    v->setLineWrapMode(QTextEdit::NoWrap);
    v->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->setUndoRedoEnabled(false);
    return v;
}

struct Colours {
    QColor removedBg, addedBg, changedBg, fillerFg;
    QColor removedFg, addedFg, faint;
};

// Writes one side of one row as a text block.
void writeSide(QTextCursor &c, const linediff::Side &side, linediff::Row::Kind kind, bool isLeft, int numberWidth, const Colours &col, bool first)
{
    QTextBlockFormat block;
    using K = linediff::Row::Kind;
    if (!side.present) {
        block.setBackground(QBrush(col.fillerFg, Qt::BDiagPattern));
    } else if (kind == K::Removed) {
        block.setBackground(col.removedBg);
    } else if (kind == K::Added) {
        block.setBackground(col.addedBg);
    } else if (kind == K::Changed) {
        block.setBackground(col.changedBg);
    }
    if (first) {
        c.setBlockFormat(block);
    } else {
        c.insertBlock(block);
    }
    QTextCharFormat number;
    number.setForeground(col.faint);
    if (!side.present) {
        // Filler: no line number, no ↵. A space keeps the block's height.
        c.insertText(QStringLiteral(" "), number);
        return;
    }
    c.insertText(QStringLiteral("%1  ").arg(side.line, numberWidth), number);
    for (const auto &seg : side.segments) {
        QTextCharFormat f;
        if (seg.changed) {
            f.setFontUnderline(true);
            f.setForeground(isLeft ? col.removedFg : col.addedFg);
            f.setFontWeight(QFont::Bold);
        }
        // Make tabs and trailing spaces visible enough to compare.
        QString t = seg.text;
        t.replace(QLatin1Char('\t'), QStringLiteral("→   "));
        c.insertText(t, f);
    }
    QTextCharFormat eol;
    eol.setForeground(col.faint);
    c.insertText(QStringLiteral("↵"), eol);
}
} // namespace

SideBySideView::SideBySideView(QWidget *parent)
    : QWidget(parent)
{
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    m_leftTitle = new QLabel(this);
    m_rightTitle = new QLabel(this);
    m_leftTitle->setObjectName(QStringLiteral("diff.leftTitle"));
    m_rightTitle->setObjectName(QStringLiteral("diff.rightTitle"));
    m_left = makeView(this, QStringLiteral("diff.left"));
    m_right = makeView(this, QStringLiteral("diff.right"));
    grid->addWidget(m_leftTitle, 0, 0);
    grid->addWidget(m_rightTitle, 0, 1);
    grid->addWidget(m_left, 1, 0);
    grid->addWidget(m_right, 1, 1);

    // Scroll together.
    for (auto [a, b] : {std::pair{m_left, m_right}, std::pair{m_right, m_left}}) {
        connect(a->verticalScrollBar(), &QScrollBar::valueChanged, b->verticalScrollBar(), &QScrollBar::setValue);
        connect(a->horizontalScrollBar(), &QScrollBar::valueChanged, b->horizontalScrollBar(), &QScrollBar::setValue);
    }
}

void SideBySideView::setTitles(const QString &left, const QString &right)
{
    m_leftTitle->setText(left);
    m_rightTitle->setText(right);
}

void SideBySideView::setTexts(const QByteArray &left, const QByteArray &right)
{
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    Colours col;
    // A light tint of the status colour over the normal background: the text stays
    // readable with light and dark colour schemes.
    const QColor base = m_left->palette().color(QPalette::Base); // what the text is drawn on
    const auto tint = [&base](const QColor &c, double amount) {
        return QColor::fromRgbF(float(base.redF() * (1 - amount) + c.redF() * amount), float(base.greenF() * (1 - amount) + c.greenF() * amount),
                                float(base.blueF() * (1 - amount) + c.blueF() * amount));
    };
    col.removedBg = tint(scheme.foreground(KColorScheme::NegativeText).color(), 0.22);
    col.addedBg = tint(scheme.foreground(KColorScheme::PositiveText).color(), 0.22);
    col.changedBg = tint(QColor(0xf2, 0xc8, 0x00), 0.30); // yellow (Breeze's "neutral" is orange)
    col.removedFg = scheme.foreground(KColorScheme::NegativeText).color();
    col.addedFg = scheme.foreground(KColorScheme::PositiveText).color();
    col.faint = scheme.foreground(KColorScheme::InactiveText).color();
    col.fillerFg = col.faint;
    col.fillerFg.setAlpha(90);

    const QList<linediff::Row> rows = linediff::sideBySide(left, right);
    int maxLine = 1;
    m_differences = 0;
    for (const auto &r : rows) {
        maxLine = std::max({maxLine, r.left.line, r.right.line});
        m_differences += r.kind == linediff::Row::Kind::Same ? 0 : 1;
    }
    const int width = int(QString::number(maxLine).size());
    for (auto [view, isLeft] : {std::pair{m_left, true}, std::pair{m_right, false}}) {
        const int scroll = view->verticalScrollBar()->value();
        view->clear();
        QTextCursor c(view->document());
        bool first = true;
        for (const auto &r : rows) {
            writeSide(c, isLeft ? r.left : r.right, r.kind, isLeft, width, col, first);
            first = false;
        }
        view->verticalScrollBar()->setValue(scroll);
    }
}
