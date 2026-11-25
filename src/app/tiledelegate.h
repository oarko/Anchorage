// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStyledItemDelegate>

namespace anchorage::inventory
{
struct Item;
}

// Paints a tile: icon, name, kind, mount point, usage bar and status chips.
class TileDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QSize tileSize();
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    struct Chip {
        QString text;
        enum Tone { Neutral, Positive, Warning, Negative } tone = Neutral;
    };
    static QList<Chip> chips(const anchorage::inventory::Item &item);
};
