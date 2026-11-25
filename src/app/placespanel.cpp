// SPDX-License-Identifier: GPL-3.0-or-later
#include "placespanel.h"

#include <KLocalizedString>

namespace
{
constexpr int PlaceRole = Qt::UserRole + 1;
}

PlacesPanel::PlacesPanel(QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("places"));
    setFrameShape(QFrame::NoFrame);
    setIconSize(QSize(16, 16));
    setSpacing(1);
    viewport()->setAutoFillBackground(false);

    addHeader(i18n("Views"));
    addPlace(Place::All, QStringLiteral("view-list-icons"));
    addPlace(Place::Drives, QStringLiteral("drive-harddisk"));
    addPlace(Place::Network, QStringLiteral("folder-network"));
    addPlace(Place::System, QStringLiteral("object-locked"));
    addHeader(i18n("Admin"));
    addPlace(Place::Backups, QStringLiteral("document-revert"));

    connect(this, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *now) {
        if (now && now->data(PlaceRole).isValid()) {
            Q_EMIT placeChanged(static_cast<Place>(now->data(PlaceRole).toInt()));
        }
    });
    select(Place::All);
    setMinimumWidth(190);
}

QString PlacesPanel::name(Place place)
{
    switch (place) {
    case Place::All:
        return i18n("All Mounts");
    case Place::Drives:
        return i18n("Drives");
    case Place::Network:
        return i18n("Network Shares");
    case Place::System:
        return i18n("System");
    case Place::Backups:
        return i18n("Backups");
    }
    return {};
}

QListWidgetItem *PlacesPanel::addHeader(const QString &text)
{
    auto *item = new QListWidgetItem(text.toUpper(), this);
    item->setFlags(Qt::NoItemFlags);
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 0.85);
    item->setFont(f);
    item->setSizeHint(QSize(0, fontMetrics().height() + 14));
    return item;
}

QListWidgetItem *PlacesPanel::addPlace(Place place, const QString &icon)
{
    auto *item = new QListWidgetItem(QIcon::fromTheme(icon), name(place), this);
    item->setData(PlaceRole, static_cast<int>(place));
    item->setSizeHint(QSize(0, fontMetrics().height() + 10));
    return item;
}

QListWidgetItem *PlacesPanel::itemFor(Place place) const
{
    for (int i = 0; i < count(); ++i) {
        if (item(i)->data(PlaceRole).isValid() && item(i)->data(PlaceRole).toInt() == static_cast<int>(place)) {
            return item(i);
        }
    }
    return nullptr;
}

void PlacesPanel::setShowSystem(bool show)
{
    if (auto *it = itemFor(Place::System)) {
        it->setHidden(!show);
        if (!show && current() == Place::System) {
            select(Place::All);
        }
    }
}

void PlacesPanel::setCounts(int all, int drives, int network, int system, int backups)
{
    const QList<std::pair<Place, int>> counts = {{Place::All, all}, {Place::Drives, drives}, {Place::Network, network},
                                                 {Place::System, system}, {Place::Backups, backups}};
    for (const auto &[place, n] : counts) {
        if (auto *it = itemFor(place)) {
            it->setText(QStringLiteral("%1  (%2)").arg(name(place)).arg(n));
        }
    }
}

void PlacesPanel::select(Place place)
{
    if (auto *it = itemFor(place)) {
        setCurrentItem(it);
    }
}

PlacesPanel::Place PlacesPanel::current() const
{
    const QListWidgetItem *it = currentItem();
    return it && it->data(PlaceRole).isValid() ? static_cast<Place>(it->data(PlaceRole).toInt()) : Place::All;
}
