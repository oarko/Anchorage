// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"

#include "backupspage.h"
#include "infopanel.h"
#include "inventorymodel.h"
#include "placespanel.h"
#include "tiledelegate.h"

#include <KActionCollection>
#include <KCategorizedView>
#include <KCategoryDrawer>
#include <KLocalizedString>
#include <KMessageWidget>
#include <KStandardAction>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

using namespace anchorage;

MainWindow::MainWindow(DataSource *source, QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_source(source)
{
    m_source->setParent(this);
    m_model = new InventoryModel(this);
    m_filter = new InventoryFilter(this);
    m_filter->setSourceModel(m_model);

    // Top row: breadcrumb and search, like Dolphin's location bar.
    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *top = new QWidget(central);
    auto *topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(8, 6, 8, 6);
    m_breadcrumb = new QLabel(top);
    m_search = new QLineEdit(top);
    m_search->setObjectName(QStringLiteral("search"));
    m_search->setPlaceholderText(i18n("Search…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(280);
    topLayout->addWidget(m_breadcrumb, 1);
    topLayout->addWidget(m_search);
    outer->addWidget(top);

    auto *split = new QSplitter(Qt::Horizontal, central);
    m_places = new PlacesPanel(split);

    // Centre: banner + tiles, or the backups page.
    m_pages = new QStackedWidget(split);
    auto *tilesPage = new QWidget(m_pages);
    auto *tilesLayout = new QVBoxLayout(tilesPage);
    tilesLayout->setContentsMargins(0, 0, 0, 0);
    m_banner = new KMessageWidget(tilesPage);
    m_banner->setCloseButtonVisible(false);
    m_banner->setWordWrap(true);
    m_tiles = new KCategorizedView(tilesPage);
    m_tiles->setObjectName(QStringLiteral("tiles"));
    m_tiles->setCategoryDrawer(new KCategoryDrawer(m_tiles));
    m_tiles->setModel(m_filter);
    m_tiles->setItemDelegate(new TileDelegate(m_tiles));
    m_tiles->setViewMode(QListView::IconMode);
    m_tiles->setResizeMode(QListView::Adjust);
    m_tiles->setMovement(QListView::Static);
    m_tiles->setUniformItemSizes(true);
    m_tiles->setGridSize(TileDelegate::tileSize() + QSize(8, 8));
    m_tiles->setSpacing(4);
    m_tiles->setMouseTracking(true);
    m_tiles->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tiles->setContextMenuPolicy(Qt::CustomContextMenu);
    tilesLayout->addWidget(m_banner);
    tilesLayout->addWidget(m_tiles, 1);
    m_backups = new BackupsPage(m_source, m_pages);
    m_pages->addWidget(tilesPage);
    m_pages->addWidget(m_backups);

    m_info = new InfoPanel(split);
    split->addWidget(m_places);
    split->addWidget(m_pages);
    split->addWidget(m_info);
    split->setStretchFactor(1, 1);
    split->setSizes({200, 760, 300});
    outer->addWidget(split, 1);
    setCentralWidget(central);

    m_statusLeft = new QLabel(this);
    m_statusRight = new QLabel(this);
    statusBar()->addWidget(m_statusLeft, 1);
    statusBar()->addPermanentWidget(m_statusRight);

    connect(m_places, &PlacesPanel::placeChanged, this, [this](PlacesPanel::Place place) {
        if (place == PlacesPanel::Place::Backups) {
            m_pages->setCurrentWidget(m_backups);
        } else {
            m_pages->setCurrentIndex(0);
            const QHash<PlacesPanel::Place, InventoryFilter::View> views = {{PlacesPanel::Place::All, InventoryFilter::View::All},
                                                                           {PlacesPanel::Place::Drives, InventoryFilter::View::Drives},
                                                                           {PlacesPanel::Place::Network, InventoryFilter::View::Network},
                                                                           {PlacesPanel::Place::System, InventoryFilter::View::System}};
            m_filter->setView(views.value(place));
        }
        updateHeader();
        updateSelection();
    });
    connect(m_search, &QLineEdit::textChanged, m_filter, &InventoryFilter::setSearch);
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::updateSelection);
    connect(m_tiles->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateSelection);
    connect(m_tiles, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(m_tiles, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto *it = m_model->item(m_filter->mapToSource(index));
        if (it && it->mounted && it->kind != inventory::Kind::Swap) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(it->mountedAt));
        }
    });
    connect(m_source, &DataSource::changed, this, &MainWindow::reload);

    setupActions();
    setupGUI(QSize(1280, 780), Default, QStringLiteral("anchorageui.rc"));
    reload();
}

void MainWindow::setupActions()
{
    KStandardAction::quit(qApp, &QApplication::closeAllWindows, actionCollection());
    KStandardAction::redisplay(this, &MainWindow::reload, actionCollection());
    KStandardAction::find(this, [this] { m_search->setFocus(); m_search->selectAll(); }, actionCollection());
}

void MainWindow::reload()
{
    m_snapshot = m_source->load();
    const fstab::File file = fstab::File::parse(m_snapshot.fstab);
    m_model->setItems(inventory::build(file, m_snapshot.volumes, m_snapshot.mounts));

    const bool fullAccess = m_snapshot.me.fullAccess();
    m_filter->setShowSystem(fullAccess);
    m_places->setShowSystem(fullAccess);
    m_backups->setBackups(m_snapshot.backups);

    int all = 0;
    int drives = 0;
    int network = 0;
    int system = 0;
    int mounted = 0;
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const auto *it = m_model->item(m_model->index(i));
        if (it->system) {
            ++system;
            if (!fullAccess) {
                continue;
            }
        }
        ++all;
        mounted += it->mounted ? 1 : 0;
        (it->kind == inventory::Kind::Network ? network : drives) += 1;
    }
    m_places->setCounts(all, drives, network, system, static_cast<int>(m_snapshot.backups.size()));

    // Banner: problems first, otherwise what this version can do.
    QStringList problems;
    if (!m_snapshot.fstabError.isEmpty()) {
        problems.append(i18n("Could not read /etc/fstab: %1", m_snapshot.fstabError));
    }
    if (!m_snapshot.drivesError.isEmpty()) {
        problems.append(m_snapshot.drivesError);
    }
    int broken = 0;
    for (const auto &l : file.lines()) {
        broken += l.kind == fstab::LineKind::Invalid ? 1 : 0;
    }
    if (broken > 0) {
        problems.append(i18np("/etc/fstab has %1 line that mount can't read.", "/etc/fstab has %1 lines that mount can't read.", broken));
    }
    if (!problems.isEmpty()) {
        m_banner->setMessageType(KMessageWidget::Warning);
        m_banner->setText(problems.join(QLatin1Char('\n')));
    } else {
        m_banner->setMessageType(KMessageWidget::Information);
        m_banner->setText(fullAccess ? i18n("Read-only preview. System entries (shown with a lock) can only be changed by driveadmins and "
                                            "administrators. Editing comes in the next version.")
                                     : i18n("Read-only preview. Editing comes in the next version."));
    }
    m_banner->show();

    m_statusLeft->setText(i18np("%1 item, %2 mounted", "%1 items, %2 mounted", all, mounted));
    const QStringList groups = [&] {
        QStringList g;
        if (m_snapshot.me.sudo) {
            g.append(QStringLiteral("sudo"));
        }
        if (m_snapshot.me.driveAdmin) {
            g.append(QStringLiteral("driveadmins"));
        }
        if (m_snapshot.me.netMount) {
            g.append(QStringLiteral("netmount"));
        }
        return g;
    }();
    m_statusRight->setText(groups.isEmpty() ? m_snapshot.me.name : QStringLiteral("%1 · %2").arg(m_snapshot.me.name, groups.join(QStringLiteral(", "))));

    // Keep the selection across reloads.
    const QModelIndex again = m_filter->mapFromSource(m_model->indexOf(m_selectedId));
    if (again.isValid()) {
        m_tiles->setCurrentIndex(again);
    }
    updateHeader();
    updateSelection();
}

void MainWindow::updateHeader()
{
    const QString place = PlacesPanel::name(m_places->current());
    m_breadcrumb->setText(QStringLiteral("Anchorage  ›  <b>%1</b>").arg(place.toHtmlEscaped()));
    setCaption(place);
}

void MainWindow::updateSelection()
{
    if (m_places->current() == PlacesPanel::Place::Backups) {
        const int n = static_cast<int>(m_snapshot.backups.size());
        m_info->showMessage(PlacesPanel::name(PlacesPanel::Place::Backups),
                            n == 0 ? i18n("No backups yet") : i18np("%1 backup of /etc/fstab", "%1 backups of /etc/fstab", n),
                            QStringLiteral("document-revert"));
        return;
    }
    const QModelIndexList sel = m_tiles->selectionModel()->selectedIndexes();
    const auto *it = sel.isEmpty() ? nullptr : m_model->item(m_filter->mapToSource(sel.first()));
    if (it) {
        m_selectedId = it->id;
        m_info->showItem(*it);
        return;
    }
    m_selectedId.clear();
    int mounted = 0;
    for (int i = 0; i < m_filter->rowCount(); ++i) {
        const auto *x = m_model->item(m_filter->mapToSource(m_filter->index(i, 0)));
        mounted += x && x->mounted ? 1 : 0;
    }
    m_info->showSummary(m_filter->rowCount(), mounted, PlacesPanel::name(m_places->current()));
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_tiles->indexAt(pos);
    const auto *it = m_model->item(m_filter->mapToSource(index));
    if (!it) {
        return;
    }
    m_tiles->setCurrentIndex(index);
    const inventory::Item item = *it;
    QMenu menu(this);
    QAction *open = menu.addAction(QIcon::fromTheme(QStringLiteral("system-file-manager")), i18n("Open in File Manager"));
    open->setEnabled(item.mounted && item.kind != inventory::Kind::Swap);
    connect(open, &QAction::triggered, this, [item] { QDesktopServices::openUrl(QUrl::fromLocalFile(item.mountedAt)); });
    menu.addSeparator();
    QAction *copyMp = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Mount Point"));
    copyMp->setEnabled(!item.mountPoint.isEmpty());
    connect(copyMp, &QAction::triggered, this, [item] { QApplication::clipboard()->setText(item.mountPoint); });
    QAction *copyLine = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy fstab Line"));
    copyLine->setEnabled(item.entry.has_value());
    connect(copyLine, &QAction::triggered, this, [item] { QApplication::clipboard()->setText(item.fstabText.trimmed()); });
    if (item.volume && !item.volume->uuid.isEmpty()) {
        QAction *copyUuid = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy UUID"));
        connect(copyUuid, &QAction::triggered, this, [item] { QApplication::clipboard()->setText(item.volume->uuid); });
    }
    menu.addSeparator();
    QAction *edit = menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Edit… (next version)"));
    edit->setEnabled(false);
    menu.exec(m_tiles->viewport()->mapToGlobal(pos));
}
