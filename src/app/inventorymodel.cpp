// SPDX-License-Identifier: GPL-3.0-or-later
#include "inventorymodel.h"

#include <KLocalizedString>

using namespace anchorage::inventory;

void InventoryModel::setItems(const QList<Item> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
}

const Item *InventoryModel::item(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return nullptr;
    }
    return &m_items.at(index.row());
}

QModelIndex InventoryModel::indexOf(const QString &id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).id == id) {
            return index(i);
        }
    }
    return {};
}

int InventoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_items.size());
}

QVariant InventoryModel::data(const QModelIndex &index, int role) const
{
    const Item *it = item(index);
    if (!it) {
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return it->title;
    case Qt::DecorationRole:
        return icon(*it);
    case Qt::ToolTipRole:
        return it->mountPoint.isEmpty() ? it->source : it->mountPoint;
    case ItemIdRole:
        return it->id;
    case SubtitleRole:
        return subtitle(*it);
    case CategoryRole:
    case KCategorizedSortFilterProxyModel::CategoryDisplayRole:
        return category(*it);
    case CategoryOrderRole:
    case KCategorizedSortFilterProxyModel::CategorySortRole:
        return it->system ? 2 : it->kind == Kind::Network ? 1 : 0;
    default:
        return {};
    }
}

QString InventoryModel::category(const Item &it)
{
    if (it.system) {
        return i18n("System");
    }
    return it.kind == Kind::Network ? i18n("Network shares") : i18n("Local drives");
}

QString InventoryModel::subtitle(const Item &it)
{
    if (it.kind == Kind::Swap) {
        return i18n("Swap");
    }
    if (it.kind == Kind::Network) {
        const QString &type = it.fsType;
        const QString proto = type.startsWith(QLatin1String("nfs")) ? QStringLiteral("NFS")
            : (type == QLatin1String("cifs") || type == QLatin1String("smb3")) ? QStringLiteral("SMB")
                                                                               : type.toUpper();
        const QString server = it.source.startsWith(QLatin1String("//")) ? it.source.mid(2).section(QLatin1Char('/'), 0, 0)
                                                                         : it.source.section(QLatin1Char(':'), 0, 0);
        return proto.isEmpty() ? server : i18nc("protocol · server", "%1 · %2", proto, server);
    }
    const QString fs = it.volume ? it.volume->fsType : (it.entry ? it.entry->type : QString());
    QString what;
    if (it.system) {
        what = i18n("System");
    } else if (it.volume && it.volume->type == QLatin1String("loop")) {
        what = i18n("Disk image");
    } else if (it.volume && (it.volume->removable || it.volume->transport == QLatin1String("usb"))) {
        what = i18n("USB drive");
    } else {
        what = i18n("Local drive");
    }
    return fs.isEmpty() ? what : i18nc("kind · file system", "%1 · %2", what, fs);
}

QIcon InventoryModel::icon(const Item &it)
{
    const auto themed = [](std::initializer_list<const char *> names) {
        for (const char *n : names) {
            const QIcon i = QIcon::fromTheme(QLatin1String(n));
            if (!i.isNull()) {
                return i;
            }
        }
        return QIcon();
    };
    if (it.kind == Kind::Network) {
        return themed({"folder-network", "network-server"});
    }
    if (it.kind == Kind::Swap) {
        return themed({"media-flash", "drive-harddisk"});
    }
    if (it.volume && (it.volume->removable || it.volume->transport == QLatin1String("usb"))) {
        return themed({"drive-removable-media-usb", "drive-removable-media", "drive-harddisk"});
    }
    if (it.mountPoint == QLatin1String("/")) {
        return themed({"drive-harddisk-root", "drive-harddisk"});
    }
    if (it.volume && it.volume->transport == QLatin1String("nvme")) {
        return themed({"drive-harddisk-solidstate", "drive-harddisk"});
    }
    return themed({"drive-harddisk"});
}

QString InventoryModel::startupText(Startup s)
{
    switch (s) {
    case Startup::AtBoot:
        return i18n("At startup");
    case Startup::OnFirstAccess:
        return i18n("On first access");
    case Startup::WhenOpened:
        return i18n("When opened");
    case Startup::NotInFstab:
        return i18n("Not in fstab");
    }
    return {};
}

// ---- filter ---------------------------------------------------------------------------

InventoryFilter::InventoryFilter(QObject *parent)
    : KCategorizedSortFilterProxyModel(parent)
{
    setCategorizedModel(true);
    setSortRole(Qt::DisplayRole);
    sort(0);
}

void InventoryFilter::setView(View view)
{
    m_view = view;
    // A full re-layout instead of row removals: KCategorizedView (KF 6.24) crashes on
    // rowsAboutToBeRemoved when filtering removes rows across categories.
    invalidate();
}

void InventoryFilter::setSearch(const QString &text)
{
    m_search = text.trimmed();
    // A full re-layout instead of row removals: KCategorizedView (KF 6.24) crashes on
    // rowsAboutToBeRemoved when filtering removes rows across categories.
    invalidate();
}

void InventoryFilter::setShowSystem(bool show)
{
    m_showSystem = show;
    // A full re-layout instead of row removals: KCategorizedView (KF 6.24) crashes on
    // rowsAboutToBeRemoved when filtering removes rows across categories.
    invalidate();
}

bool InventoryFilter::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const auto *model = static_cast<const InventoryModel *>(sourceModel());
    const Item *it = model->item(model->index(sourceRow, 0, sourceParent));
    if (!it) {
        return false;
    }
    if (it->system && !m_showSystem) {
        return false;
    }
    switch (m_view) {
    case View::All:
        break;
    case View::Drives:
        if (it->kind == Kind::Network) {
            return false;
        }
        break;
    case View::Network:
        if (it->kind != Kind::Network) {
            return false;
        }
        break;
    case View::System:
        if (!it->system) {
            return false;
        }
        break;
    }
    if (m_search.isEmpty()) {
        return true;
    }
    const QString haystack = it->title + QLatin1Char(' ') + it->mountPoint + QLatin1Char(' ') + it->source + QLatin1Char(' ')
        + InventoryModel::subtitle(*it) + QLatin1Char(' ') + (it->volume ? it->volume->label : QString());
    return haystack.contains(m_search, Qt::CaseInsensitive);
}

bool InventoryFilter::subSortLessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const auto *model = static_cast<const InventoryModel *>(sourceModel());
    const Item *a = model->item(left);
    const Item *b = model->item(right);
    if (!a || !b) {
        return false;
    }
    // Things in fstab first, then by mount point, then by name.
    if (a->entry.has_value() != b->entry.has_value()) {
        return a->entry.has_value();
    }
    const QString ka = a->mountPoint.isEmpty() ? QStringLiteral("~") + a->title : a->mountPoint;
    const QString kb = b->mountPoint.isEmpty() ? QStringLiteral("~") + b->title : b->mountPoint;
    return QString::localeAwareCompare(ka, kb) < 0;
}
