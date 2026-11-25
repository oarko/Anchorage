// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QListWidget>

// Left-hand Places panel (layout C): Views and Admin sections, like Dolphin's Places.
class PlacesPanel : public QListWidget
{
    Q_OBJECT

public:
    enum class Place { All, Drives, Network, System, Backups };

    explicit PlacesPanel(QWidget *parent = nullptr);

    void setShowSystem(bool show);
    void setCounts(int all, int drives, int network, int system, int backups);
    void select(Place place);
    Place current() const;
    static QString name(Place place);

Q_SIGNALS:
    void placeChanged(PlacesPanel::Place place);

private:
    QListWidgetItem *addHeader(const QString &text);
    QListWidgetItem *addPlace(Place place, const QString &icon);
    QListWidgetItem *itemFor(Place place) const;
};
