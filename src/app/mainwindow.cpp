// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"

#include "applydialogs.h"
#include "backupspage.h"
#include "entryeditor.h"
#include "fstabservice.h"
#include "infopanel.h"
#include "inventorymodel.h"
#include "linediff.h"
#include "mountpolicy.h"
#include "placespanel.h"
#include "settingsdialog.h"
#include "mountatdialog.h"
#include "mountplan.h"
#include "access.h"
#include "binds.h"
#include "secrets.h"
#include "clienttools.h"
#include "connectdialog.h"
#include "mounterrors.h"
#include "options.h"
#include "tiledelegate.h"

#include <KActionCollection>
#include <KCategorizedView>
#include <KCategoryDrawer>
#include <KLocalizedString>
#include <KMessageBox>
#include <KMessageWidget>
#include <KStandardAction>
#include <KToggleAction>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollArea>
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

    // Apply bar and the system-editing warning sit above everything, like Dolphin's.
    m_applyBar = new KMessageWidget(central);
    m_applyBar->setObjectName(QStringLiteral("applyBar"));
    m_applyBar->setCloseButtonVisible(false);
    m_applyBar->setWordWrap(true);
    m_applyBar->hide();
    m_systemBanner = new KMessageWidget(central);
    m_systemBanner->setObjectName(QStringLiteral("systemBanner"));
    m_systemBanner->setMessageType(KMessageWidget::Error);
    m_systemBanner->setCloseButtonVisible(false);
    m_systemBanner->setWordWrap(true);
    m_systemBanner->setText(i18n("Editing system entries is switched on. A mistake in these entries can stop the computer from starting."));
    m_systemBanner->hide();
    outer->addWidget(m_applyBar);
    outer->addWidget(m_systemBanner);
    m_remountBar = new KMessageWidget(central);
    m_remountBar->setObjectName(QStringLiteral("remountBar"));
    m_remountBar->setMessageType(KMessageWidget::Information);
    m_remountBar->setWordWrap(true);
    m_remountBar->hide();
    outer->addWidget(m_remountBar);

    auto *split = new QSplitter(Qt::Horizontal, central);
    m_places = new PlacesPanel(split);

    // Centre: banner + tiles, or the backups page.
    m_pages = new QStackedWidget(split);
    auto *tilesPage = new QWidget(m_pages);
    auto *tilesLayout = new QVBoxLayout(tilesPage);
    tilesLayout->setContentsMargins(0, 0, 0, 0);
    m_banner = new KMessageWidget(tilesPage);
    m_banner->setObjectName(QStringLiteral("banner"));
    m_banner->setCloseButtonVisible(false);
    m_banner->setWordWrap(true);
    m_removeOrphansAction = new QAction(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove Them"), this);
    m_removeOrphansAction->setObjectName(QStringLiteral("removeOrphans"));
    m_removeOrphansAction->setVisible(false);
    m_banner->addAction(m_removeOrphansAction);
    m_movePasswordsAction = new QAction(QIcon::fromTheme(QStringLiteral("security-high")), i18n("Move to a Safe File"), this);
    m_movePasswordsAction->setObjectName(QStringLiteral("movePasswords"));
    m_movePasswordsAction->setVisible(false);
    m_banner->addAction(m_movePasswordsAction);
    connect(m_movePasswordsAction, &QAction::triggered, this, &MainWindow::movePasswords);
    connect(m_removeOrphansAction, &QAction::triggered, this, [this] {
        fstab::File file = fstab::File::parse(currentContent());
        QList<int> lines = binds::orphaned(file);
        std::sort(lines.begin(), lines.end(), std::greater<int>());
        for (const int line : std::as_const(lines)) {
            file.remove(line);
        }
        setPending(file.serialize());
        statusBar()->showMessage(i18n("Removed the places whose drive is gone. Click Apply to save, or Discard to undo."), 8000);
    });
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

    // The info panel scrolls when the window is short (e.g. while the Apply bar shows).
    auto *infoScroll = new QScrollArea(split);
    m_infoScroll = infoScroll;
    infoScroll->setWidgetResizable(true);
    infoScroll->setFrameShape(QFrame::NoFrame);
    infoScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_info = new InfoPanel(infoScroll);
    infoScroll->setWidget(m_info);
    infoScroll->setMinimumWidth(m_info->minimumWidth());
    split->addWidget(m_places);
    split->addWidget(m_pages);
    split->addWidget(infoScroll);
    split->setStretchFactor(1, 1);
    split->setSizes({200, 760, 300});
    outer->addWidget(split, 1);
    setCentralWidget(central);

    m_statusLeft = new QLabel(this);
    m_statusRight = new QLabel(this);
    statusBar()->addWidget(m_statusLeft, 1);
    statusBar()->addPermanentWidget(m_statusRight);

    connect(m_places, &PlacesPanel::placeChanged, this, [this](PlacesPanel::Place place) {
        // The Backups page needs the width for its two columns.
        m_infoScroll->setVisible(place != PlacesPanel::Place::Backups);
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
    connect(m_backups, &BackupsPage::restoreRequested, this, &MainWindow::restoreBackup);
    connect(m_source, &DataSource::changed, this, &MainWindow::reload);

    setupActions();
    setupGUI(QSize(1280, 780), Default, QStringLiteral("anchorageui.rc"));
    reload();
}

void MainWindow::setupActions()
{
    KActionCollection *ac = actionCollection();
    KStandardAction::quit(this, &QWidget::close, ac);
    KStandardAction::redisplay(this, &MainWindow::reload, ac);
    KStandardAction::find(this, [this] { m_search->setFocus(); m_search->selectAll(); }, ac);
    KStandardAction::preferences(this, &MainWindow::showSettings, ac);

    m_newAction = ac->addAction(QStringLiteral("entry_new"), this, &MainWindow::newEntry);
    m_newAction->setText(i18n("New fstab Entry…"));
    m_newAction->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    ac->setDefaultShortcut(m_newAction, QKeySequence::New);

    m_editAction = ac->addAction(QStringLiteral("entry_edit"), this, &MainWindow::editSelected);
    m_editAction->setText(i18n("Edit…"));
    m_editAction->setIcon(QIcon::fromTheme(QStringLiteral("document-edit")));
    ac->setDefaultShortcut(m_editAction, Qt::Key_F2);

    m_removeAction = ac->addAction(QStringLiteral("entry_remove"), this, &MainWindow::removeSelected);
    m_removeAction->setText(i18n("Remove from fstab"));
    m_removeAction->setIcon(QIcon::fromTheme(QStringLiteral("list-remove")));
    ac->setDefaultShortcut(m_removeAction, QKeySequence::Delete);

    m_applyAction = ac->addAction(QStringLiteral("changes_apply"), this, &MainWindow::applyChanges);
    m_applyAction->setText(i18n("Apply"));
    m_applyAction->setIcon(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")));
    ac->setDefaultShortcut(m_applyAction, QKeySequence::Save);

    m_discardAction = ac->addAction(QStringLiteral("changes_discard"), this, &MainWindow::discardChanges);
    m_discardAction->setText(i18n("Discard"));
    m_discardAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete-remove")));

    m_previewAction = ac->addAction(QStringLiteral("changes_preview"), this, &MainWindow::previewChanges);
    m_previewAction->setText(i18n("Preview…"));
    m_previewAction->setIcon(QIcon::fromTheme(QStringLiteral("document-preview")));

    m_mountAction = ac->addAction(QStringLiteral("mount"), this, &MainWindow::mountSelected);
    m_mountAction->setText(i18n("Mount"));
    m_mountAction->setIcon(QIcon::fromTheme(QStringLiteral("media-mount")));
    m_unmountAction = ac->addAction(QStringLiteral("unmount"), this, &MainWindow::unmountSelected);
    m_unmountAction->setText(i18n("Unmount"));
    m_unmountAction->setIcon(QIcon::fromTheme(QStringLiteral("media-eject")));
    m_connectAction = ac->addAction(QStringLiteral("connect_server"), this, &MainWindow::connectToServer);
    m_connectAction->setText(i18n("Connect to Server…"));
    m_connectAction->setIcon(QIcon::fromTheme(QStringLiteral("network-connect")));
    m_mountAtAction = ac->addAction(QStringLiteral("mount_at"), this, &MainWindow::mountAtSelected);
    m_mountAtAction->setText(i18n("Mount At…"));
    m_mountAtAction->setIcon(QIcon::fromTheme(QStringLiteral("media-mount")));
    auto *remount = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Remount"), this);
    remount->setObjectName(QStringLiteral("remount"));
    connect(remount, &QAction::triggered, this, &MainWindow::remountChanged);
    m_remountBar->addAction(remount);

    m_systemAction = new KToggleAction(QIcon::fromTheme(QStringLiteral("object-unlocked")), i18n("Allow Editing System Entries"), this);
    ac->addAction(QStringLiteral("allow_system_editing"), m_systemAction);
    connect(m_systemAction, &QAction::toggled, this, &MainWindow::setSystemEditing);

    m_applyBar->addAction(m_discardAction);
    m_applyBar->addAction(m_previewAction);
    m_applyBar->addAction(m_applyAction);
    auto *lockAgain = new QAction(QIcon::fromTheme(QStringLiteral("object-locked")), i18n("Lock Again"), this);
    connect(lockAgain, &QAction::triggered, this, [this] { m_systemAction->setChecked(false); });
    m_systemBanner->addAction(lockAgain);
}

QByteArray MainWindow::currentContent() const
{
    return m_pending.value_or(m_snapshot.fstab);
}

void MainWindow::reload()
{
    m_snapshot = m_source->load();
    rebuild();
}

void MainWindow::rebuild()
{
    const fstab::File file = fstab::File::parse(currentContent());
    m_model->setItems(inventory::build(file, m_snapshot.volumes, m_snapshot.mounts));

    const bool fullAccess = m_snapshot.me.fullAccess();
    m_filter->setShowSystem(fullAccess);
    m_places->setShowSystem(fullAccess);
    // The Backups page compares with the file on disk, not with pending changes.
    m_backups->setBackups(m_snapshot.backups, m_snapshot.fstab, m_snapshot.settings.keepBackups);

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
    updateBanners();
    updateHeader();
    updateSelection();
}

void MainWindow::updateBanners()
{
    const fstab::File file = fstab::File::parse(currentContent());
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
    const QList<int> orphans = binds::orphaned(file);
    if (!orphans.isEmpty()) {
        problems.append(i18np("%1 place shows a drive that is no longer in /etc/fstab. It isn't mounted at start-up.",
                              "%1 places show a drive that is no longer in /etc/fstab. They aren't mounted at start-up.", int(orphans.size())));
    }
    // Only those who can fix it are told; standard users aren't pointed at the password.
    const int typed = m_snapshot.me.fullAccess() ? int(secrets::find(file).size()) : 0;
    if (typed > 0) {
        problems.append(i18np("%1 entry stores a password in /etc/fstab, which every user can read.",
                              "%1 entries store a password in /etc/fstab, which every user can read.", typed));
    }
    m_banner->setMessageType(KMessageWidget::Warning);
    m_banner->setText(problems.join(QLatin1Char('\n')));
    m_banner->setVisible(!problems.isEmpty());
    m_removeOrphansAction->setVisible(!orphans.isEmpty());
    m_movePasswordsAction->setVisible(typed > 0 && !m_applying);

    if (m_pending) {
        const int n = linediff::changeCount(linediff::diff(m_baseline, *m_pending));
        if (m_snapshot.fstab != m_baseline) {
            m_applyBar->setMessageType(KMessageWidget::Warning);
            m_applyBar->setText(i18n("/etc/fstab was changed by another program while you were editing. Discard your changes to see the new "
                                     "version, then make them again."));
        } else {
            m_applyBar->setMessageType(KMessageWidget::Information);
            m_applyBar->setText(i18np("%1 changed line in /etc/fstab is not saved yet.", "%1 changed lines in /etc/fstab are not saved yet.", n));
        }
        m_applyBar->show();
    } else {
        m_applyBar->hide();
    }
    m_systemBanner->setVisible(m_systemUnlocked);
    updateActions();
}

void MainWindow::updateActions()
{
    const bool fullAccess = m_snapshot.me.fullAccess();
    m_systemAction->setVisible(fullAccess);
    const auto *it = selectedItem();
    QString why;
    const bool editable = it && canEdit(*it, &why);
    m_editAction->setEnabled(editable && !m_applying);
    m_editAction->setText(it && !it->entry ? i18n("Add to fstab…") : i18n("Edit…"));
    m_removeAction->setEnabled(editable && it->entry && !m_applying);
    m_newAction->setEnabled(!m_applying && m_snapshot.fstabError.isEmpty());
    const bool pending = m_pending.has_value();
    m_applyAction->setEnabled(pending && !m_applying && m_snapshot.fstab == m_baseline);
    m_discardAction->setEnabled(pending && !m_applying);
    m_previewAction->setEnabled(pending);
    const MountChoices mc = it ? mountChoices(*it) : MountChoices{};
    m_mountAction->setEnabled(mc.mount != Route::None && !m_applying);
    m_mountAction->setVisible(!it || mc.mount != Route::None || (it->entry && !it->mounted));
    m_unmountAction->setEnabled(mc.unmount != Route::None && !m_applying);
    m_unmountAction->setVisible(!it || it->mounted);
    m_mountAtAction->setEnabled(mc.mountAt && !m_applying);
    m_mountAtAction->setVisible(!it || mc.mountAt);
    m_backups->setRestoreEnabled(!m_applying && m_snapshot.fstabError.isEmpty(),
                                 m_applying ? i18n("Wait until saving has finished.") : QString());
}

void MainWindow::updateHeader()
{
    const QString place = PlacesPanel::name(m_places->current());
    m_breadcrumb->setText(QStringLiteral("Anchorage  ›  <b>%1</b>").arg(place.toHtmlEscaped()));
    setCaption(place, m_pending.has_value());
}

const inventory::Item *MainWindow::selectedItem() const
{
    if (m_places->current() == PlacesPanel::Place::Backups) {
        return nullptr;
    }
    const QModelIndexList sel = m_tiles->selectionModel()->selectedIndexes();
    return sel.isEmpty() ? nullptr : m_model->item(m_filter->mapToSource(sel.first()));
}

void MainWindow::updateSelection()
{
    if (m_places->current() == PlacesPanel::Place::Backups) {
        const int n = static_cast<int>(m_snapshot.backups.size());
        m_info->showMessage(PlacesPanel::name(PlacesPanel::Place::Backups),
                            n == 0 ? i18n("No backups yet") : i18np("%1 backup of /etc/fstab", "%1 backups of /etc/fstab", n),
                            QStringLiteral("document-revert"));
        updateActions();
        return;
    }
    const auto *it = selectedItem();
    if (it) {
        m_selectedId = it->id;
        m_info->showItem(*it);
        QString why;
        canEdit(*it, &why);
        updateActions();
        const MountChoices mc = mountChoices(*it);
        const QString hint = mc.hint.isEmpty() ? why : mc.hint;
        m_info->setActions({m_mountAction, m_unmountAction, m_mountAtAction, m_editAction, m_removeAction}, hint);
        return;
    }
    m_selectedId.clear();
    int mounted = 0;
    for (int i = 0; i < m_filter->rowCount(); ++i) {
        const auto *x = m_model->item(m_filter->mapToSource(m_filter->index(i, 0)));
        mounted += x && x->mounted ? 1 : 0;
    }
    m_info->showSummary(m_filter->rowCount(), mounted, PlacesPanel::name(m_places->current()));
    updateActions();
}

bool MainWindow::canEdit(const inventory::Item &item, QString *why) const
{
    if (!m_snapshot.fstabError.isEmpty()) {
        *why = i18n("/etc/fstab could not be read, so it can't be changed.");
        return false;
    }
    if (item.kind == inventory::Kind::Swap && !item.entry && !item.volume) {
        return false;
    }
    if (item.system) {
        if (!m_snapshot.me.fullAccess()) {
            return false;
        }
        if (!m_systemUnlocked) {
            *why = i18n("Needed to start the computer. Switch on \"Allow Editing System Entries\" to change it.");
            return false;
        }
    }
    if (!m_snapshot.me.fullAccess()) {
        *why = i18n("Saving changes to /etc/fstab asks for an administrator's password.");
    }
    return true;
}

EditorContext MainWindow::editorContext(int editedLine) const
{
    EditorContext ctx;
    ctx.volumes = m_snapshot.volumes;
    ctx.me = m_snapshot.me;
    ctx.systemUnlocked = m_systemUnlocked;
    ctx.users = m_snapshot.accounts;
    ctx.ntfsDriver = m_snapshot.settings.ntfsDriver;
    ctx.mounts = m_snapshot.mounts;
    const fstab::File file = fstab::File::parse(currentContent());
    for (const int i : file.entryLines()) {
        const auto &entry = file.lines().at(static_cast<std::size_t>(i)).entry;
        if (!entry) {
            continue;
        }
        const auto &e = *entry;
        if (e.mountPoint == QLatin1String("/")) {
            ctx.rootSpecs.append(e.spec);
        }
        if (i != editedLine && e.type != QLatin1String("swap")) {
            ctx.otherMountPoints.append(e.mountPoint);
        }
    }
    if (editedLine >= 0) {
        if (const auto &edited = file.lines().at(static_cast<std::size_t>(editedLine)).entry) {
            for (const auto &[line, b] : binds::findFor(file, edited->mountPoint)) {
                ctx.binds.append(b);
            }
        }
    }
    return ctx;
}

void MainWindow::setPending(const QByteArray &content)
{
    if (!m_pending) {
        m_baseline = m_snapshot.fstab;
    }
    if (content == m_baseline) {
        m_pending.reset();
    } else {
        m_pending = content;
    }
    rebuild();
}

void MainWindow::newEntry()
{
    if (m_applying) {
        return;
    }
    auto *editor = new EntryEditor(fstab::Entry{}, true, editorContext(-1), nullptr, this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &QDialog::accepted, this, [this, editor] {
        fstab::File file = fstab::File::parse(currentContent());
        const int line = file.append(editor->entry());
        if (line >= 0 && binds::apply(file, QString(), editor->entry(), editor->binds())) {
            m_selectedId = QStringLiteral("fstab:%1").arg(line);
            setPending(file.serialize());
        }
    });
    Q_EMIT editorOpened(editor);
    editor->open();
}

void MainWindow::editSelected()
{
    if (const auto *it = selectedItem()) {
        const inventory::Item item = *it; // the model is rebuilt while editing
        editItem(item);
    }
}

void MainWindow::editItem(const inventory::Item &item)
{
    QString why;
    if (m_applying || !canEdit(item, &why)) {
        if (!why.isEmpty() && item.system) {
            KMessageBox::information(this, why);
        }
        return;
    }
    const fstab::File file = fstab::File::parse(currentContent());
    EntryEditor *editor = nullptr;
    if (item.entry) {
        const auto &line = file.lines().at(static_cast<std::size_t>(item.fstabLine));
        editor = new EntryEditor(*item.entry, false, editorContext(item.fstabLine), &line, this);
    } else {
        fstab::Entry e;
        if (item.kind == inventory::Kind::Network) {
            // A share mounted some other way: start from what is mounted now.
            e.spec = item.source;
            e.type = item.fsType;
            e.mountPoint = item.mountPoint;
            e.options = access::apply({}, access::defaultsFor(e.type), e.type);
        }
        editor = new EntryEditor(e, true, editorContext(-1), nullptr, this);
        if (item.volume) {
            editor->useVolume(*item.volume);
        }
    }
    editor->setAttribute(Qt::WA_DeleteOnClose);
    const int lineIndex = item.fstabLine;
    const bool isNew = !item.entry;
    const QString oldMountPoint = item.entry ? item.entry->mountPoint : QString();
    connect(editor, &QDialog::accepted, this, [this, editor, lineIndex, isNew, oldMountPoint] {
        fstab::File f = fstab::File::parse(currentContent());
        if (isNew) {
            const int line = f.append(editor->entry());
            if (line < 0) {
                return;
            }
            m_selectedId = QStringLiteral("fstab:%1").arg(line);
        } else if (!f.replace(lineIndex, editor->entry())) {
            return;
        }
        // The drive's "also show it at" entries follow it (and move with its mount point).
        if (!binds::isBind(editor->entry()) && !binds::apply(f, oldMountPoint, editor->entry(), editor->binds())) {
            return;
        }
        if (const auto login = editor->pendingLogin()) {
            m_pendingLogins.insert(editor->entry().mountPoint, *login);
        }
        setPending(f.serialize());
    });
    Q_EMIT editorOpened(editor);
    editor->open();
}

void MainWindow::removeSelected()
{
    const auto *it = selectedItem();
    QString why;
    if (!it || !it->entry || m_applying || !canEdit(*it, &why)) {
        return;
    }
    const QString mountPoint = it->entry->mountPoint;
    const int driveLine = it->fstabLine;
    fstab::File file = fstab::File::parse(currentContent());

    // Places that show this drive (bind entries) go with it: left behind, they would show
    // the drive's empty folder instead (SPEC §8).
    QList<int> lines{driveLine};
    if (!binds::isBind(*it->entry)) {
        const auto shown = binds::findFor(file, mountPoint);
        if (!shown.isEmpty()) {
            QStringList places;
            for (const auto &[line, b] : shown) {
                places.append(b.subfolder.isEmpty() ? i18n("%1 (the whole drive)", b.target) : i18n("%1 (its folder %2)", b.target, b.subfolder));
                lines.append(line);
            }
            if (KMessageBox::warningContinueCancelList(this,
                                                       i18np("%2 is also shown at this place, which is removed from fstab too:",
                                                             "%2 is also shown at these places, which are removed from fstab too:",
                                                             int(shown.size()), mountPoint),
                                                       places, i18n("Remove Drive and Its Places?"),
                                                       KGuiItem(i18n("Remove All"), QStringLiteral("list-remove")))
                != KMessageBox::Continue) {
                return;
            }
        }
    }
    // Last line first, so the other line numbers stay valid.
    std::sort(lines.begin(), lines.end(), std::greater<int>());
    for (const int line : std::as_const(lines)) {
        if (!file.remove(line)) {
            return;
        }
    }
    m_selectedId.clear();
    setPending(file.serialize()); // invalidates `it`
    statusBar()->showMessage(i18n("Removed %1 from the pending changes. Click Apply to save, or Discard to undo.", mountPoint), 8000);
}

void MainWindow::discardChanges()
{
    m_pending.reset();
    m_pendingLogins.clear(); // forget typed passwords
    rebuild();
}

void MainWindow::previewChanges()
{
    if (!m_pending) {
        return;
    }
    auto *dlg = new PreviewDialog(m_baseline, *m_pending, false, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->open();
}

void MainWindow::setSystemEditing(bool on)
{
    if (on && !m_snapshot.me.fullAccess()) {
        on = false;
    }
    m_systemUnlocked = on;
    if (m_systemAction->isChecked() != on) {
        m_systemAction->setChecked(on);
    }
    updateBanners();
    updateSelection();
}

void MainWindow::applyChanges()
{
    if (!m_pending || m_applying) {
        return;
    }
    if (m_snapshot.fstab != m_baseline) {
        KMessageBox::error(this, i18n("/etc/fstab was changed by another program while you were editing. Discard your changes and make them again."));
        return;
    }
    const QByteArray proposed = *m_pending;

    bool confirm = false;
    policy::Plan plan;
    if (!checkWrite(proposed, &confirm, &plan)) {
        return;
    }

    // findmnt --verify, for the entries that change. Missing folders are fine (the
    // helper creates them), and so are missing devices for entries that may be missing.
    QStringList problems;
    QHash<QString, fstab::Entry> added;
    for (const auto &e : std::as_const(plan.added)) {
        added.insert(e.mountPoint, e);
    }
    for (const auto &f : m_source->verify(proposed)) {
        if (!f.error || !added.contains(f.target)) {
            continue;
        }
        const auto &e = added.value(f.target);
        const bool mayBeMissing = e.options.contains(QStringLiteral("nofail")) || e.options.contains(QStringLiteral("noauto"));
        if (f.message.contains(QLatin1String("target")) && plan.newMountPoints.contains(f.target)) {
            continue;
        }
        if (f.message.contains(QLatin1String("source")) && mayBeMissing) {
            continue;
        }
        problems.append(QStringLiteral("%1: %2").arg(f.target, f.message));
    }
    if (!problems.isEmpty()
        && KMessageBox::warningContinueCancelList(this, i18n("findmnt found problems that may stop the computer from starting:"), problems,
                                                  i18n("Check Before Saving"), KGuiItem(i18n("Save Anyway")))
            != KMessageBox::Continue) {
        return;
    }

    QStringList types;
    for (const auto &e : std::as_const(plan.added)) {
        types.append(e.type);
    }
    ensureClientTools(types, [this, proposed, confirm, plan] {
        writePendingLogins([this, proposed, confirm, plan](bool ok) {
            if (!ok) {
                return;
            }
    m_applying = true;
    updateActions();
    statusBar()->showMessage(i18n("Saving /etc/fstab…"));
    requests::WriteFstab request{service::fingerprint(m_baseline), proposed, confirm, true};
    const QList<fstab::Entry> changedEntries = plan.added;
    m_source->writeFstab(request, this, [this, proposed, changedEntries](const client::Reply &reply) {
        m_applying = false;
        switch (reply.status) {
        case client::Reply::Status::Ok: {
            if (m_pending == proposed) {
                m_pending.reset();
            }
            const QString backup = reply.data.value(QStringLiteral("backup")).toString();
            statusBar()->showMessage(backup.isEmpty() ? i18n("Saved /etc/fstab.") : i18n("Saved /etc/fstab. The previous version is in Backups (%1).", backup), 15000);
            reload();
            testMount(changedEntries, backup);
            break;
        }
        default:
            showHelperError(reply, i18n("/etc/fstab was not changed."));
            updateActions();
            break;
        }
        Q_EMIT applyFinished(reply.ok());
    });
        });
    });
}

bool MainWindow::checkWrite(const QByteArray &proposed, bool *confirm, policy::Plan *plan)
{
    // The helper checks everything again; checking here first gives clear messages and
    // asks for the typed confirmation before the password prompt.
    *confirm = false;
    *plan = policy::planFstabWrite(m_snapshot.fstab, proposed, m_snapshot.me, false);
    QStringList blocked;
    for (const QString &mp : std::as_const(plan->newMountPoints)) {
        if (policy::classifyMountPoint(mp).zone == policy::Zone::Blocked) {
            blocked.append(mp);
        }
    }
    if (!blocked.isEmpty() && m_snapshot.me.sudo) {
        ProtectedConfirmDialog dlg(blocked, this);
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        *confirm = true;
        *plan = policy::planFstabWrite(m_snapshot.fstab, proposed, m_snapshot.me, true);
    }
    if (!plan->ok()) {
        KMessageBox::detailedError(this, i18n("These changes can't be saved:"), plan->errors.join(QLatin1Char('\n')));
        return false;
    }
    return true;
}

void MainWindow::showHelperError(const client::Reply &reply, const QString &what)
{
    statusBar()->clearMessage();
    switch (reply.status) {
    case client::Reply::Status::Ok:
        break;
    case client::Reply::Status::Cancelled:
        statusBar()->showMessage(i18n("Nothing was changed: the password prompt was closed."), 8000);
        break;
    case client::Reply::Status::Denied:
        KMessageBox::error(this, i18n("%1 You are not allowed to do this, or the password was wrong.", what));
        break;
    case client::Reply::Status::Failed:
        KMessageBox::detailedError(this, what, reply.message);
        break;
    }
}

void MainWindow::restoreBackup(const QString &name)
{
    if (m_applying || name.isEmpty()) {
        return;
    }
    const QByteArray backup = m_source->readBackup(name);
    if (backup.isEmpty() && !m_snapshot.fstab.isEmpty()) {
        KMessageBox::error(this, i18n("The backup %1 could not be read.", name));
        return;
    }
    if (m_pending) {
        if (KMessageBox::warningContinueCancel(this, i18n("You have changes that are not saved yet. Restoring a backup discards them."),
                                               i18n("Discard Unsaved Changes?"), KGuiItem(i18n("Discard and Continue")))
            != KMessageBox::Continue) {
            return;
        }
        m_pending.reset();
        rebuild();
    }
    bool confirm = false;
    policy::Plan plan;
    if (!checkWrite(backup, &confirm, &plan)) {
        return;
    }
    const int n = linediff::changeCount(linediff::diff(m_snapshot.fstab, backup));
    if (KMessageBox::warningContinueCancel(this,
                                           i18np("Restore /etc/fstab to the version from %2? %1 line changes.<br><br>"
                                                 "The current file is backed up first, so this can be undone.",
                                                 "Restore /etc/fstab to the version from %2? %1 lines change.<br><br>"
                                                 "The current file is backed up first, so this can be undone.",
                                                 n, BackupsPage::displayName(name)),
                                           i18n("Restore Backup"), KGuiItem(i18n("Restore"), QStringLiteral("document-revert")))
        != KMessageBox::Continue) {
        return;
    }
    m_applying = true;
    updateActions();
    statusBar()->showMessage(i18n("Restoring /etc/fstab…"));
    m_source->restoreBackup({name, confirm}, this, [this, name](const client::Reply &reply) {
        m_applying = false;
        if (reply.ok()) {
            const QString saved = reply.data.value(QStringLiteral("backup")).toString();
            statusBar()->showMessage(i18n("Restored the version from %1. The file as it was before is in the list too (%2).",
                                          BackupsPage::displayName(name), BackupsPage::displayName(saved)),
                                     15000);
            reload();
        } else {
            showHelperError(reply, i18n("/etc/fstab was not restored."));
            updateActions();
        }
        Q_EMIT restoreFinished(reply.ok());
    });
}

void MainWindow::showSettings()
{
    auto *dlg = new SettingsDialog(m_snapshot.settings, m_snapshot.me.fullAccess(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        const settings::Settings wanted = dlg->settings();
        if (wanted == m_snapshot.settings) {
            return;
        }
        statusBar()->showMessage(i18n("Saving settings…"));
        m_source->writeSettings(wanted, this, [this](const client::Reply &reply) {
            if (reply.ok()) {
                statusBar()->showMessage(i18n("Settings saved."), 8000);
                reload();
            } else {
                showHelperError(reply, i18n("The settings were not saved."));
            }
            Q_EMIT settingsFinished(reply.ok());
        });
    });
    Q_EMIT settingsOpened(dlg);
    dlg->open();
}

MainWindow::MountChoices MainWindow::mountChoices(const inventory::Item &item) const
{
    MountChoices c;
    if (item.kind == inventory::Kind::Swap || !m_snapshot.fstabError.isEmpty()) {
        return c;
    }
    const bool full = m_snapshot.me.fullAccess();
    if (item.mounted) {
        if (item.entry && item.userMountable) {
            c.unmount = Route::AsUser;
        } else if (!item.entry || item.system) {
            c.unmount = full ? Route::Helper : Route::None;
        } else {
            c.unmount = Route::Helper;
        }
        return c;
    }
    if (!item.entry) {
        c.mountAt = full && (item.volume || item.kind == inventory::Kind::Network);
        return c;
    }
    if (item.orphanedBind) {
        c.hint = i18n("Its drive is no longer in /etc/fstab, so there is nothing to show here.");
        return c;
    }
    // Mounting reads /etc/fstab on disk, so the entry must be saved as shown.
    const fstab::File disk = fstab::File::parse(m_snapshot.fstab);
    const int line = disk.findMountPoint(item.entry->mountPoint);
    const auto &onDisk = line >= 0 ? disk.lines().at(static_cast<std::size_t>(line)).entry : std::nullopt;
    if (!onDisk || *onDisk != *item.entry) {
        c.hint = i18n("Apply your changes first to mount it.");
        return c;
    }
    if (item.userMountable) {
        c.mount = Route::AsUser;
    } else {
        c.mount = (item.system && !full) ? Route::None : Route::Helper;
    }
    return c;
}

QStringList MainWindow::bindsOf(const fstab::Entry &drive, bool mounted) const
{
    QStringList out;
    if (binds::isBind(drive)) {
        return out;
    }
    const fstab::File file = fstab::File::parse(m_snapshot.fstab); // what the helper reads
    for (const auto &[line, b] : binds::findFor(file, drive.mountPoint)) {
        if ((mounting::mountedAt(m_snapshot.mounts, b.target) != nullptr) == mounted && !out.contains(b.target)) {
            out.append(b.target);
        }
    }
    return out;
}

void MainWindow::mountSelected()
{
    const auto *it = selectedItem();
    if (!it || !it->entry || m_applying) {
        return;
    }
    const inventory::Item item = *it;
    const MountChoices mc = mountChoices(item);
    const QString mp = item.entry->mountPoint;
    const QString fsType = item.entry->type;
    // Its binds come along, after the drive (SPEC §8).
    const QStringList extra = bindsOf(*item.entry, false);
    const auto failed = [this, mp, fsType](const client::Reply &reply) {
        const QString which = reply.data.value(QStringLiteral("failed")).toString();
        const bool driveDone = reply.data.value(QStringLiteral("done")).toStringList().contains(mp);
        client::Reply explained = reply;
        const QString why = mounterrors::explain(which.isEmpty() || which == mp ? fsType : QStringLiteral("none"), reply.message);
        if (!why.isEmpty()) {
            explained.message += QStringLiteral("\n\n") + why;
        }
        showHelperError(explained, driveDone && !which.isEmpty() ? i18n("%1 was mounted, but %2 was not.", mp, which) : i18n("%1 was not mounted.", mp));
    };
    const auto mounted = [this, mp, extra] {
        statusBar()->showMessage(extra.isEmpty() ? i18n("Mounted %1.", mp)
                                                 : i18np("Mounted %2, and it is shown at %3 too.", "Mounted %2, and it is shown at %1 more places.",
                                                         int(extra.size()), mp, extra.value(0)),
                                 8000);
    };
    const auto finish = [this](bool ok) {
        reload();
        Q_EMIT mountFinished(ok);
    };
    const auto run = [this, mc, mp, extra, failed, mounted, finish] {
        statusBar()->showMessage(i18n("Mounting %1…", mp));
        if (mc.mount == Route::Helper) {
            m_source->mountEntry(QStringList{mp} + extra, this, [failed, mounted, finish](const client::Reply &reply) {
                if (reply.ok()) {
                    mounted();
                } else {
                    failed(reply);
                }
                finish(reply.ok());
            });
        } else if (mc.mount == Route::AsUser) {
            m_source->userMount(mp, true, [this, mp, extra, failed, mounted, finish](const client::Reply &reply) {
                if (!reply.ok()) {
                    failed(reply);
                    finish(false);
                    return;
                }
                if (extra.isEmpty()) {
                    mounted();
                    finish(true);
                    return;
                }
                // Binds aren't "any user" entries: they go through the helper.
                m_source->mountEntry(extra, this, [mp, failed, mounted, finish](const client::Reply &r) {
                    if (r.ok()) {
                        mounted();
                    } else {
                        client::Reply partial = r;
                        partial.data.insert(QStringLiteral("done"), QStringList{mp} + r.data.value(QStringLiteral("done")).toStringList());
                        if (partial.data.value(QStringLiteral("failed")).toString().isEmpty()) {
                            partial.data.insert(QStringLiteral("failed"), i18n("its other places"));
                        }
                        failed(partial);
                    }
                    finish(r.ok());
                });
            });
        }
    };
    ensureClientTools({item.entry->type}, run);
}

void MainWindow::unmountSelected()
{
    const auto *it = selectedItem();
    if (!it || !it->mounted || m_applying) {
        return;
    }
    const inventory::Item item = *it;
    const MountChoices mc = mountChoices(item);
    if (mc.unmount == Route::None) {
        return;
    }
    bool confirm = false;
    if (item.system) {
        ProtectedConfirmDialog dlg({item.mountedAt}, i18n("Unmount a System Entry?"),
                                   i18n("%1 is needed by the running system. Unmounting it can close programs, lose unsaved work or stop the "
                                        "computer from working until it restarts.",
                                        item.mountedAt),
                                   i18n("Unmount"), this);
        if (dlg.exec() != QDialog::Accepted) {
            return;
        }
        confirm = true;
    }
    // Its binds go first (SPEC §8): a bind left behind would keep showing the drive.
    UnmountJob job;
    job.confirmSystem = confirm;
    job.viaHelper = item.entry && item.mountedAt == item.entry->mountPoint ? bindsOf(*item.entry, true) : QStringList();
    if (mc.unmount == Route::AsUser) {
        job.asUser = item.mountedAt;
    } else {
        job.viaHelper.append(item.mountedAt);
    }
    runUnmount(job);
}

void MainWindow::runUnmount(const UnmountJob &job)
{
    const QString name = job.asUser.isEmpty() ? job.viaHelper.constLast() : job.asUser;
    const auto succeeded = [this, name] {
        statusBar()->showMessage(i18n("Unmounted %1.", name), 8000);
        reload();
        Q_EMIT mountFinished(true);
    };
    const auto asUser = [this, job, succeeded] {
        m_source->userMount(job.asUser, false, [this, job, succeeded](const client::Reply &reply) {
            if (reply.ok()) {
                succeeded();
            } else {
                unmountFailed(reply, job.asUser, UnmountJob{{}, job.asUser, job.confirmSystem});
            }
        });
    };
    statusBar()->showMessage(i18n("Unmounting %1…", name));
    if (job.viaHelper.isEmpty()) {
        asUser();
        return;
    }
    m_source->unmount({job.viaHelper, job.confirmSystem}, this, [this, job, succeeded, asUser](const client::Reply &reply) {
        if (reply.ok()) {
            if (job.asUser.isEmpty()) {
                succeeded();
            } else {
                asUser();
            }
            return;
        }
        // Try again with what is left: the ones before the failure are already unmounted.
        UnmountJob rest = job;
        for (const QString &done : reply.data.value(QStringLiteral("done")).toStringList()) {
            rest.viaHelper.removeAll(done);
        }
        const QString failed = reply.data.value(QStringLiteral("failed")).toString();
        unmountFailed(reply, failed.isEmpty() ? job.viaHelper.constFirst() : failed, rest);
    });
}

void MainWindow::unmountFailed(const client::Reply &reply, const QString &target, const UnmountJob &retry)
{
    if (!reply.data.value(QStringLiteral("done")).toStringList().isEmpty()) {
        reload(); // some places were unmounted
    }
    const QStringList programs = reply.data.value(QStringLiteral("processes")).toStringList();
    const bool busy = !programs.isEmpty() || reply.data.value(QStringLiteral("busy")).toBool() || reply.message.contains(QLatin1String("in use"));
    if (busy && reply.status == client::Reply::Status::Failed) {
        statusBar()->clearMessage();
        const QString text = programs.isEmpty()
            ? i18n("%1 is in use. Close the files and folders on it (and terminals open there), then try again.", target)
            : i18n("%1 is in use by: <b>%2</b>.<br><br>Close them, or leave the folder in them, then try again.", target,
                   programs.join(QStringLiteral(", ")).toHtmlEscaped());
        if (KMessageBox::warningTwoActions(this, text, i18n("Drive in Use"), KGuiItem(i18n("Try Again"), QStringLiteral("view-refresh")),
                                           KStandardGuiItem::cancel())
            == KMessageBox::PrimaryAction) {
            runUnmount(retry);
            return;
        }
    } else {
        showHelperError(reply, i18n("%1 was not unmounted.", target));
    }
    Q_EMIT mountFinished(false);
}

void MainWindow::mountAtSelected()
{
    const auto *it = selectedItem();
    if (!it || m_applying || !mountChoices(*it).mountAt) {
        return;
    }
    fstab::Entry e;
    if (it->volume) {
        e.spec = EntryEditor::specFor(*it->volume, EntryEditor::IdentifyBy::Uuid);
        e.type = it->volume->fsType == QLatin1String("ntfs") ? m_snapshot.settings.ntfsDriver : it->volume->fsType;
        if (e.type == QLatin1String("ntfs") && m_snapshot.settings.ntfsDriver == QLatin1String("ntfs3")) {
            e.type = QStringLiteral("ntfs3");
        }
        e.mountPoint = MountAtDialog::suggestedFolder(*it->volume);
    } else {
        e.spec = it->source;
        e.type = it->fsType;
        e.mountPoint = QStringLiteral("/mnt/") + it->title;
    }
    auto *dlg = new MountAtDialog(e, it->title, m_snapshot, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        const fstab::Entry wanted = dlg->entry();
        bool confirm = false;
        if (policy::classifyMountPoint(wanted.mountPoint).zone == policy::Zone::Blocked) {
            ProtectedConfirmDialog confirmDlg({wanted.mountPoint}, this);
            if (confirmDlg.exec() != QDialog::Accepted) {
                return;
            }
            confirm = true;
        }
        statusBar()->showMessage(i18n("Mounting at %1…", wanted.mountPoint));
        m_source->mountTemporary({wanted, confirm}, this, [this, wanted](const client::Reply &reply) {
            if (reply.ok()) {
                statusBar()->showMessage(i18n("Mounted at %1 until it is unmounted or the computer restarts.", wanted.mountPoint), 10000);
                reload();
            } else {
                showHelperError(reply, i18n("Nothing was mounted at %1.", wanted.mountPoint));
            }
            Q_EMIT mountFinished(reply.ok());
        });
    });
    Q_EMIT mountAtOpened(dlg);
    dlg->open();
}

void MainWindow::testMount(const QList<fstab::Entry> &entries, const QString &backup)
{
    // SPEC §9 / §6.1: mount each new or changed entry once to prove it works.
    QList<fstab::Entry> toTest;
    QStringList remount;
    for (const fstab::Entry &e : entries) {
        // Swap isn't mounted; a bind only shows a folder, which the helper won't do while its
        // drive is unmounted (and test-unmounting the drive first would make that likely).
        if (e.type == QLatin1String("swap") || binds::isBind(e)) {
            continue;
        }
        if (mounting::mountedAt(m_snapshot.mounts, e.mountPoint)) {
            remount.append(e.mountPoint);
            continue;
        }
        const bool network = options::Catalogue::builtIn().family(e.type) == options::Family::Network;
        const bool namesDevice = e.spec.startsWith(QLatin1String("UUID=")) || e.spec.startsWith(QLatin1String("LABEL="))
            || e.spec.startsWith(QLatin1String("PARTUUID=")) || e.spec.startsWith(QLatin1String("PARTLABEL=")) || e.spec.startsWith(QLatin1String("/dev/"));
        const bool present = std::any_of(m_snapshot.volumes.cbegin(), m_snapshot.volumes.cend(), [&e](const drives::Volume &v) {
            return inventory::specMatches(e.spec, v);
        });
        if (!network && namesDevice && !present) {
            continue; // not connected: nothing to test
        }
        toTest.append(e);
    }
    m_toRemount = remount;
    if (!remount.isEmpty()) {
        m_remountBar->setText(i18np("The new options for %2 take effect after it is remounted.", "The new options for %2 take effect after they are remounted.",
                                    int(remount.size()), remount.join(QStringLiteral(", "))));
        m_remountBar->show();
    }
    if (toTest.isEmpty()) {
        Q_EMIT testMountFinished(-1);
        return;
    }

    struct State {
        QList<fstab::Entry> left;
        QStringList failures;
    };
    auto state = std::make_shared<State>(State{toTest, {}});
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, state, step, backup] {
        if (state->left.isEmpty()) {
            reload();
            const int failures = int(state->failures.size());
            if (failures > 0) {
                const auto answer = KMessageBox::warningTwoActionsList(
                    this, i18n("The changes were saved, but the test mount failed. Undo restores /etc/fstab as it was before."), state->failures,
                    i18n("Test Mount Failed"), KGuiItem(i18n("Undo Changes"), QStringLiteral("edit-undo")), KGuiItem(i18n("Keep Changes")));
                if (answer == KMessageBox::PrimaryAction && !backup.isEmpty()) {
                    m_source->restoreBackup({backup, false}, this, [this](const client::Reply &reply) {
                        if (reply.ok()) {
                            statusBar()->showMessage(i18n("The previous /etc/fstab was restored."), 10000);
                            reload();
                        } else {
                            showHelperError(reply, i18n("/etc/fstab was not restored."));
                        }
                    });
                }
            } else {
                statusBar()->showMessage(i18n("Saved, and the test mount worked."), 10000);
            }
            Q_EMIT testMountFinished(failures);
            *step = nullptr; // break the cycle
            return;
        }
        const fstab::Entry e = state->left.takeFirst();
        statusBar()->showMessage(i18n("Test mounting %1…", e.mountPoint));
        m_source->mountEntry({e.mountPoint}, this, [this, state, step, e](const client::Reply &reply) {
            if (!reply.ok()) {
                const QString why = mounterrors::explain(e.type, reply.message);
                state->failures.append(QStringLiteral("%1: %2%3").arg(e.mountPoint, reply.message, why.isEmpty() ? QString() : QStringLiteral(" — ") + why));
                (*step)();
                return;
            }
            if (access::read(e.options, e.type).start == access::Start::AtBoot) {
                (*step)(); // it mounts at start-up anyway: leave it mounted
                return;
            }
            m_source->unmount({{e.mountPoint}, false}, this, [state, step, e](const client::Reply &r) {
                if (!r.ok()) {
                    state->failures.append(QStringLiteral("%1 (unmounting after the test): %2").arg(e.mountPoint, r.message));
                }
                (*step)();
            });
        });
    };
    (*step)();
}

void MainWindow::remountChanged()
{
    m_remountBar->hide();
    const QStringList drives = m_toRemount;
    m_toRemount.clear();
    if (drives.isEmpty()) {
        return;
    }
    // One request each way, so it asks for the password once (SPEC §8). Binds are taken
    // down first and brought back after their drive, so they show the remounted drive.
    const fstab::File file = fstab::File::parse(m_snapshot.fstab);
    QStringList shownToo;
    for (const QString &mp : drives) {
        for (const int i : file.entryLines()) {
            const auto &e = file.lines().at(static_cast<std::size_t>(i)).entry;
            if (e && e->mountPoint == mp) {
                for (const QString &b : bindsOf(*e, true)) {
                    if (!shownToo.contains(b) && !drives.contains(b)) {
                        shownToo.append(b);
                    }
                }
                break;
            }
        }
    }
    const QStringList down = shownToo + drives;
    const QStringList up = drives + shownToo;
    statusBar()->showMessage(i18n("Remounting %1…", drives.join(QStringLiteral(", "))));
    m_source->unmount({down, false}, this, [this, down, up](const client::Reply &reply) {
        if (!reply.ok()) {
            const QString failed = reply.data.value(QStringLiteral("failed")).toString();
            const QStringList done = reply.data.value(QStringLiteral("done")).toStringList();
            reload();
            showHelperError(reply, i18n("%1 was not remounted.", failed.isEmpty() ? down.join(QStringLiteral(", ")) : failed));
            if (!done.isEmpty()) {
                // Bring back what was already taken down.
                QStringList back;
                for (const QString &mp : up) {
                    if (done.contains(mp)) {
                        back.append(mp);
                    }
                }
                m_source->mountEntry(back, this, [this](const client::Reply &) {
                    reload();
                });
            }
            return;
        }
        m_source->mountEntry(up, this, [this, up](const client::Reply &r) {
            if (r.ok()) {
                statusBar()->showMessage(i18n("Remounted %1 with the new options.", up.join(QStringLiteral(", "))), 8000);
            } else {
                const QString failed = r.data.value(QStringLiteral("failed")).toString();
                showHelperError(r, i18n("%1 was unmounted but could not be mounted again.", failed.isEmpty() ? up.join(QStringLiteral(", ")) : failed));
            }
            reload();
        });
    });
}

void MainWindow::movePasswords()
{
    if (!m_snapshot.me.fullAccess() || m_applying) {
        return;
    }
    const fstab::File file = fstab::File::parse(currentContent());
    QList<secrets::Plaintext> todo;
    QStringList skipped;
    for (const auto &p : secrets::find(file)) {
        const auto &e = file.lines().at(static_cast<std::size_t>(p.line)).entry;
        if (!e) {
            continue;
        }
        if (p.server.isEmpty() || p.user.isEmpty()) {
            skipped.append(e->mountPoint); // a saved login needs a server and a user name
        } else {
            todo.append(p);
        }
    }
    if (todo.isEmpty()) {
        if (!skipped.isEmpty()) {
            KMessageBox::information(this, i18n("These entries have a password but no user name, so they can't be moved automatically: %1. Edit them "
                                                "and choose a login.",
                                                skipped.join(QStringLiteral(", "))));
        }
        return;
    }
    // Save the logins one after the other (one password prompt), then change the lines.
    struct State {
        QList<secrets::Plaintext> left;
        QHash<int, QString> paths; // line -> saved login
        QStringList errors;
    };
    auto state = std::make_shared<State>(State{todo, {}, {}});
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, state, step, skipped] {
        if (state->left.isEmpty()) {
            fstab::File f = fstab::File::parse(currentContent());
            for (auto it = state->paths.cbegin(); it != state->paths.cend(); ++it) {
                const auto &entry = f.lines().at(static_cast<std::size_t>(it.key())).entry;
                if (entry) {
                    f.replace(it.key(), secrets::withCredentials(*entry, it.value()));
                }
            }
            if (!state->paths.isEmpty()) {
                setPending(f.serialize());
            }
            QString text;
            if (!state->paths.isEmpty()) {
                text = i18np("The login of %1 entry is saved in a file only root can read. Click Apply to remove the password from /etc/fstab "
                             "and from its backups.<br><br>Until now every user on this computer could read it, so consider changing the "
                             "password on the server too.",
                             "The logins of %1 entries are saved in files only root can read. Click Apply to remove the passwords from "
                             "/etc/fstab and from its backups.<br><br>Until now every user on this computer could read them, so consider "
                             "changing those passwords on the server too.",
                             int(state->paths.size()));
            }
            if (!state->errors.isEmpty()) {
                text += QStringLiteral("<br><br>") + i18n("Not moved: %1", state->errors.join(QStringLiteral("; ")).toHtmlEscaped());
            }
            if (!skipped.isEmpty()) {
                text += QStringLiteral("<br><br>") + i18n("No user name, so not moved: %1", skipped.join(QStringLiteral(", ")));
            }
            KMessageBox::information(this, text, i18n("Passwords Moved"));
            Q_EMIT passwordsMoved(int(state->paths.size()));
            *step = nullptr;
            return;
        }
        const secrets::Plaintext p = state->left.takeFirst();
        m_source->writeCredentials({p.server, p.user, p.password, p.domain}, this, [state, step, p](const client::Reply &reply) {
            if (reply.ok()) {
                state->paths.insert(p.line, reply.data.value(QStringLiteral("path")).toString());
            } else if (reply.status == client::Reply::Status::Failed) {
                state->errors.append(reply.message);
            } else {
                state->left.clear(); // cancelled or denied: stop asking
            }
            (*step)();
        });
    };
    (*step)();
}

void MainWindow::ensureClientTools(const QStringList &types, const std::function<void()> &then)
{
    QList<clienttools::Protocol> missing;
    for (const QString &t : types) {
        clienttools::Protocol p;
        if (clienttools::protocolFor(t, &p) && !missing.contains(p) && !m_source->toolInstalled(p)) {
            missing.append(p);
        }
    }
    if (missing.isEmpty()) {
        then();
        return;
    }
    const clienttools::Protocol p = missing.first();
    const QString what = p == clienttools::Protocol::Smb ? i18n("SMB (Windows/Samba) shares") : i18n("NFS shares");
    const QString command = m_source->installCommand(p);
    const QString text = i18n("Mounting %1 needs a program that isn't installed on this computer.<br><br>To install it yourself:<br><tt>%2</tt>", what,
                              command.toHtmlEscaped());
    const auto rest = [this, types, then] {
        ensureClientTools(types, then); // the next missing one, if any
    };
    if (m_source->canInstall()) {
        const auto answer = KMessageBox::questionTwoActionsCancel(this, text, i18n("Program Missing"),
                                                                  KGuiItem(i18n("Install"), QStringLiteral("run-install")),
                                                                  KGuiItem(i18n("Copy Command"), QStringLiteral("edit-copy")));
        if (answer == KMessageBox::PrimaryAction) {
            statusBar()->showMessage(i18n("Installing…"));
            m_source->installTool(p, [this, rest](bool ok, bool cancelled, const QString &message) {
                statusBar()->clearMessage();
                if (ok) {
                    rest();
                } else if (!cancelled) {
                    KMessageBox::detailedError(this, i18n("The program could not be installed."), message);
                }
            });
        } else if (answer == KMessageBox::SecondaryAction) {
            QApplication::clipboard()->setText(command);
        }
        return;
    }
    if (KMessageBox::questionTwoActions(this, text, i18n("Program Missing"), KGuiItem(i18n("Copy Command"), QStringLiteral("edit-copy")),
                                        KStandardGuiItem::cancel())
        == KMessageBox::PrimaryAction) {
        QApplication::clipboard()->setText(command);
    }
}

void MainWindow::writePendingLogins(const std::function<void(bool ok)> &then)
{
    // Only logins whose entry is still in the file to be saved.
    QList<service::SavedLogin> todo;
    const fstab::File file = fstab::File::parse(currentContent());
    for (auto it = m_pendingLogins.cbegin(); it != m_pendingLogins.cend(); ++it) {
        if (file.findMountPoint(it.key()) >= 0) {
            todo.append(it.value());
        }
    }
    if (todo.isEmpty()) {
        then(true);
        return;
    }
    auto left = std::make_shared<QList<service::SavedLogin>>(todo);
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, left, step, then] {
        if (left->isEmpty()) {
            m_pendingLogins.clear(); // saved: don't keep the passwords in memory
            then(true);
            *step = nullptr;
            return;
        }
        const service::SavedLogin l = left->takeFirst();
        statusBar()->showMessage(i18n("Saving the login for %1…", l.server));
        m_source->writeCredentials({l.server, l.user, l.password, l.domain}, this, [this, step, then](const client::Reply &reply) {
            if (!reply.ok()) {
                showHelperError(reply, i18n("The login could not be saved, so /etc/fstab was not changed."));
                then(false);
                *step = nullptr;
                return;
            }
            (*step)();
        });
    };
    (*step)();
}

void MainWindow::connectToServer()
{
    auto *dlg = new ConnectDialog(m_source, m_snapshot.me.fullAccess(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->onAddToFstab = [this](const ConnectDialog::Choice &c) {
        openShareEditor(c.entry, c.login, c.guest, c.nfsVersions);
    };
    dlg->onMountAt = [this](const ConnectDialog::Choice &c) {
        mountShareAt(c.entry, c.login, c.guest);
    };
    Q_EMIT connectOpened(dlg);
    dlg->open();
}

void MainWindow::openShareEditor(const fstab::Entry &entry, const std::optional<shares::Login> &login, bool guest, const QList<int> &nfsVersions)
{
    EditorContext ctx = editorContext(-1);
    ctx.login = login;
    ctx.guest = guest;
    ctx.nfsVersions = nfsVersions;
    auto *editor = new EntryEditor(entry, true, ctx, nullptr, this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &QDialog::accepted, this, [this, editor] {
        fstab::File file = fstab::File::parse(currentContent());
        const int line = file.append(editor->entry());
        if (line < 0 || !binds::apply(file, QString(), editor->entry(), editor->binds())) {
            return;
        }
        if (const auto l = editor->pendingLogin()) {
            m_pendingLogins.insert(editor->entry().mountPoint, *l);
        }
        m_selectedId = QStringLiteral("fstab:%1").arg(line);
        setPending(file.serialize());
    });
    Q_EMIT editorOpened(editor);
    editor->open();
}

void MainWindow::mountShareAt(const fstab::Entry &share, const std::optional<shares::Login> &login, bool guest)
{
    // Like Mount At… for drives: a folder of your choice until restart; fstab unchanged.
    const auto withLogin = [this, share](const QStringList &loginOptions) {
        fstab::Entry e = share;
        e.options = loginOptions;
        auto *dlg = new MountAtDialog(e, share.spec, m_snapshot, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QDialog::accepted, this, [this, dlg] {
            const fstab::Entry wanted = dlg->entry();
            ensureClientTools({wanted.type}, [this, wanted] {
                m_source->mountTemporary({wanted, false}, this, [this, wanted](const client::Reply &reply) {
                    if (reply.ok()) {
                        statusBar()->showMessage(i18n("Mounted at %1 until it is unmounted or the computer restarts.", wanted.mountPoint), 10000);
                        reload();
                    } else {
                        client::Reply explained = reply;
                        const QString why = mounterrors::explain(wanted.type, reply.message);
                        if (!why.isEmpty()) {
                            explained.message += QStringLiteral("\n\n") + why;
                        }
                        showHelperError(explained, i18n("Nothing was mounted at %1.", wanted.mountPoint));
                    }
                    Q_EMIT mountFinished(reply.ok());
                });
            });
        });
        Q_EMIT mountAtOpened(dlg);
        dlg->open();
    };
    if (share.type != QLatin1String("cifs") && share.type != QLatin1String("smb3")) {
        withLogin({QStringLiteral("nofail")});
        return;
    }
    if (guest || !login) {
        withLogin({QStringLiteral("guest")});
        return;
    }
    // The password goes into a root-only saved login first, never onto a command line.
    const QString server = share.spec.mid(2).section(QLatin1Char('/'), 0, 0);
    m_source->writeCredentials({server, login->user, login->password, login->domain}, this, [this, withLogin](const client::Reply &reply) {
        if (!reply.ok()) {
            showHelperError(reply, i18n("The login could not be saved."));
            return;
        }
        withLogin({QStringLiteral("credentials=") + reply.data.value(QStringLiteral("path")).toString()});
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_pending && !m_applying) {
        const auto answer = KMessageBox::warningTwoActionsCancel(this, i18n("The changes to /etc/fstab are not saved yet."), i18n("Unsaved Changes"),
                                                                 KGuiItem(i18n("Discard Changes"), QStringLiteral("edit-delete-remove")),
                                                                 KGuiItem(i18n("Keep Editing"), QStringLiteral("document-edit")),
                                                                 KStandardGuiItem::cancel());
        if (answer != KMessageBox::PrimaryAction) {
            event->ignore();
            return;
        }
        m_pending.reset();
    }
    KXmlGuiWindow::closeEvent(event);
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
    updateActions();
    for (QAction *a : {m_mountAction, m_unmountAction, m_mountAtAction}) {
        if (a->isVisible()) {
            menu.addAction(a);
        }
    }
    menu.addSeparator();
    menu.addAction(m_editAction);
    menu.addAction(m_removeAction);
    menu.exec(m_tiles->viewport()->mapToGlobal(pos));
}
