// SPDX-License-Identifier: GPL-3.0-or-later
#include "tiledelegate.h"

#include "inventorymodel.h"

#include <KColorScheme>
#include <KLocalizedString>

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QSortFilterProxyModel>

using namespace anchorage::inventory;

namespace
{
constexpr int margin = 10;
constexpr int iconSize = 40;

const Item *itemAt(const QModelIndex &index)
{
    const auto *proxy = qobject_cast<const QSortFilterProxyModel *>(index.model());
    if (!proxy) {
        return nullptr;
    }
    const auto *model = qobject_cast<const InventoryModel *>(proxy->sourceModel());
    return model ? model->item(proxy->mapToSource(index)) : nullptr;
}
} // namespace

QSize TileDelegate::tileSize()
{
    const QFontMetrics fm(QApplication::font());
    return {std::max(250, fm.averageCharWidth() * 34), margin * 2 + iconSize + fm.height() * 3 + 20};
}

QSize TileDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return tileSize();
}

QList<TileDelegate::Chip> TileDelegate::chips(const Item &it)
{
    QList<Chip> out;
    if (it.deviceMissing) {
        out.append({i18n("Device not found"), Chip::Negative});
    }
    if (it.orphanedBind) {
        out.append({i18n("Drive not in fstab"), Chip::Negative});
    }
    if (it.mounted) {
        out.append({i18n("Mounted"), Chip::Positive});
    }
    if (it.entry && !it.orphanedBind) { // an orphaned bind isn't mounted at start-up whatever it says
        out.append({InventoryModel::startupText(it.startup), Chip::Neutral});
    } else if (it.kind != Kind::Swap) {
        out.append({i18n("Not in fstab"), Chip::Neutral});
    }
    if (it.userMountable) {
        out.append({i18n("Users may mount"), Chip::Neutral});
    }
    return out;
}

void TileDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Item *it = itemAt(index);
    if (!it) {
        return;
    }
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    const QPalette &pal = option.palette;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const QRectF box = QRectF(option.rect).adjusted(3.5, 3.5, -3.5, -3.5);

    // Card
    QColor fill = pal.color(QPalette::Base);
    QColor border = pal.color(QPalette::Mid);
    if (selected) {
        fill = pal.color(QPalette::Highlight);
        fill.setAlphaF(0.18F);
        border = pal.color(QPalette::Highlight);
    } else if (hovered) {
        border = pal.color(QPalette::Highlight);
    }
    p->setPen(QPen(border, selected ? 2.0 : 1.0));
    p->setBrush(fill);
    p->drawRoundedRect(box, 6, 6);

    const QRect inner = option.rect.adjusted(margin, margin, -margin, -margin);
    const QFontMetrics fm(option.font);

    // Icon and lock for system entries
    const QIcon icon = InventoryModel::icon(*it);
    icon.paint(p, QRect(inner.topLeft(), QSize(iconSize, iconSize)), Qt::AlignCenter, it->mounted ? QIcon::Normal : QIcon::Disabled);
    if (it->system) {
        QIcon::fromTheme(QStringLiteral("object-locked")).paint(p, QRect(inner.left() + iconSize - 16, inner.top() + iconSize - 16, 16, 16));
    }

    // Title and subtitle
    const int textX = inner.left() + iconSize + 10;
    const int textW = inner.right() - textX;
    QFont bold = option.font;
    bold.setBold(true);
    p->setFont(bold);
    p->setPen(pal.color(QPalette::Text));
    const QFontMetrics bfm(bold);
    p->drawText(QRect(textX, inner.top() + 2, textW, bfm.height()), Qt::AlignLeft | Qt::AlignVCenter, bfm.elidedText(it->title, Qt::ElideRight, textW));
    p->setFont(option.font);
    QColor dim = pal.color(QPalette::Text);
    dim.setAlphaF(0.72F);
    p->setPen(dim);
    p->drawText(QRect(textX, inner.top() + 2 + bfm.height(), textW, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                fm.elidedText(InventoryModel::subtitle(*it), Qt::ElideRight, textW));

    // Mount point
    int y = inner.top() + iconSize + 6;
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(option.font.pointSizeF() * 0.92);
    p->setFont(mono);
    const QFontMetrics mfm(mono);
    const QString where = it->mountPoint.isEmpty() ? i18n("not mounted") : it->mountPoint;
    p->setPen(it->mountPoint.isEmpty() ? dim : pal.color(QPalette::Text));
    p->drawText(QRect(inner.left(), y, inner.width(), mfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                mfm.elidedText(where, Qt::ElideMiddle, inner.width()));
    y += mfm.height() + 5;

    // Usage bar
    const QRectF track(inner.left(), y, inner.width(), 5);
    QColor trackColor = pal.color(QPalette::Text);
    trackColor.setAlphaF(0.12F);
    p->setPen(Qt::NoPen);
    p->setBrush(trackColor);
    p->drawRoundedRect(track, 2.5, 2.5);
    if (it->size > 0 && it->used >= 0) {
        const double frac = std::clamp(static_cast<double>(it->used) / static_cast<double>(it->size), 0.0, 1.0);
        QColor barColor = it->system ? dim : pal.color(QPalette::Highlight);
        if (frac > 0.9) {
            barColor = KColorScheme(QPalette::Active, KColorScheme::View).foreground(KColorScheme::NegativeText).color();
        }
        p->setBrush(barColor);
        p->drawRoundedRect(QRectF(track.left(), track.top(), track.width() * frac, track.height()), 2.5, 2.5);
    }
    y += 11;

    // Status chips
    QFont small = option.font;
    small.setPointSizeF(option.font.pointSizeF() * 0.88);
    p->setFont(small);
    const QFontMetrics sfm(small);
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    int x = inner.left();
    for (const Chip &c : chips(*it)) {
        const int w = sfm.horizontalAdvance(c.text) + 14;
        if (x + w > inner.right()) {
            break;
        }
        QColor bg;
        QColor fg;
        switch (c.tone) {
        case Chip::Positive:
            bg = scheme.background(KColorScheme::PositiveBackground).color();
            fg = scheme.foreground(KColorScheme::PositiveText).color();
            break;
        case Chip::Warning:
            bg = scheme.background(KColorScheme::NeutralBackground).color();
            fg = scheme.foreground(KColorScheme::NeutralText).color();
            break;
        case Chip::Negative:
            bg = scheme.background(KColorScheme::NegativeBackground).color();
            fg = scheme.foreground(KColorScheme::NegativeText).color();
            break;
        case Chip::Neutral:
            bg = pal.color(QPalette::Text);
            bg.setAlphaF(0.09F);
            fg = pal.color(QPalette::Text);
            break;
        }
        const QRectF chip(x, y, w, sfm.height() + 4);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(chip, chip.height() / 2, chip.height() / 2);
        p->setPen(fg);
        p->drawText(chip, Qt::AlignCenter, c.text);
        x += w + 5;
    }
    p->restore();
}
