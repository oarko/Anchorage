// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "datasource.h"

#include <KXmlGuiWindow>

class BackupsPage;
class InfoPanel;
class InventoryFilter;
class InventoryModel;
class KCategorizedView;
class KMessageWidget;
class PlacesPanel;
class QLabel;
class QLineEdit;
class QStackedWidget;

// Main window, layout C (Dolphin-like): Places | tiles | info panel. See docs/SPEC.md §15.
// This version is read-only: it shows /etc/fstab, drives and mounts, and changes nothing.
class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    // Takes ownership of `source`.
    explicit MainWindow(DataSource *source, QWidget *parent = nullptr);

    void reload();

private:
    void setupActions();
    void updateSelection();
    void updateHeader();
    void showContextMenu(const QPoint &pos);

    DataSource *m_source;
    Snapshot m_snapshot;
    InventoryModel *m_model;
    InventoryFilter *m_filter;
    PlacesPanel *m_places;
    KCategorizedView *m_tiles;
    InfoPanel *m_info;
    BackupsPage *m_backups;
    QStackedWidget *m_pages;
    KMessageWidget *m_banner;
    QLabel *m_breadcrumb;
    QLineEdit *m_search;
    QLabel *m_statusLeft;
    QLabel *m_statusRight;
    QString m_selectedId;
};
