// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "inventory.h"

#include <KCategorizedSortFilterProxyModel>

#include <QAbstractListModel>
#include <QIcon>

// One row per inventory::Item (a tile).
class InventoryModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        SubtitleRole,
        CategoryRole, // "Local drives", "Network shares", "System"
        CategoryOrderRole,
    };

    using QAbstractListModel::QAbstractListModel;

    void setItems(const QList<anchorage::inventory::Item> &items);
    const anchorage::inventory::Item *item(const QModelIndex &index) const;
    QModelIndex indexOf(const QString &id) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // Text helpers shared with the delegate and the info panel.
    static QString subtitle(const anchorage::inventory::Item &item);
    static QIcon icon(const anchorage::inventory::Item &item);
    static QString category(const anchorage::inventory::Item &item);
    static QString startupText(anchorage::inventory::Startup s);

private:
    QList<anchorage::inventory::Item> m_items;
};

// Filters by Places view and search text, groups tiles into categories.
class InventoryFilter : public KCategorizedSortFilterProxyModel
{
    Q_OBJECT

public:
    enum class View { All, Drives, Network, System };

    explicit InventoryFilter(QObject *parent = nullptr);

    void setView(View view);
    View view() const { return m_view; }
    void setSearch(const QString &text);
    // Standard users don't see system entries (cosmetic: /etc/fstab is world-readable).
    void setShowSystem(bool show);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool subSortLessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    View m_view = View::All;
    QString m_search;
    bool m_showSystem = false;
};
