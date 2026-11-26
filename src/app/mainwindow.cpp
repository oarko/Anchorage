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
    m_banner->setMessageType(KMessageWidget::Warning);
    m_banner->setText(problems.join(QLatin1Char('\n')));
    m_banner->setVisible(!problems.isEmpty());

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
    const QString mountPoint = it->mountPoint;
    fstab::File file = fstab::File::parse(currentContent());
    if (file.remove(it->fstabLine)) {
        m_selectedId.clear();
        setPending(file.serialize()); // invalidates `it`
        statusBar()->showMessage(i18n("Removed %1 from the pending changes. Click Apply to save, or Discard to undo.", mountPoint), 8000);
    }
}

void MainWindow::discardChanges()
{
    m_pending.reset();
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

void MainWindow::mountSelected()
{
    const auto *it = selectedItem();
    if (!it || !it->entry || m_applying) {
        return;
    }
    const inventory::Item item = *it;
    const MountChoices mc = mountChoices(item);
    const QString mp = item.entry->mountPoint;
    const auto done = [this, mp](const client::Reply &reply) {
        if (reply.ok()) {
            statusBar()->showMessage(i18n("Mounted %1.", mp), 8000);
            reload();
        } else {
            showHelperError(reply, i18n("%1 was not mounted.", mp));
        }
        Q_EMIT mountFinished(reply.ok());
    };
    statusBar()->showMessage(i18n("Mounting %1…", mp));
    if (mc.mount == Route::AsUser) {
        m_source->userMount(mp, true, done);
    } else if (mc.mount == Route::Helper) {
        m_source->mountEntry(mp, this, done);
    }
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
    runUnmount(item, mc.unmount, confirm);
}

void MainWindow::runUnmount(const inventory::Item &item, Route route, bool confirmSystem)
{
    const QString target = item.mountedAt;
    const auto done = [this, item, route, confirmSystem, target](const client::Reply &reply) {
        if (reply.ok()) {
            statusBar()->showMessage(i18n("Unmounted %1.", target), 8000);
            reload();
            Q_EMIT mountFinished(true);
            return;
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
                runUnmount(item, route, confirmSystem);
                return;
            }
        } else {
            showHelperError(reply, i18n("%1 was not unmounted.", target));
        }
        Q_EMIT mountFinished(false);
    };
    statusBar()->showMessage(i18n("Unmounting %1…", target));
    if (route == Route::AsUser) {
        m_source->userMount(target, false, done);
    } else {
        m_source->unmount({target, confirmSystem}, this, done);
    }
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
        m_source->mountEntry(e.mountPoint, this, [this, state, step, e](const client::Reply &reply) {
            if (!reply.ok()) {
                state->failures.append(QStringLiteral("%1: %2").arg(e.mountPoint, reply.message));
                (*step)();
                return;
            }
            if (access::read(e.options, e.type).start == access::Start::AtBoot) {
                (*step)(); // it mounts at start-up anyway: leave it mounted
                return;
            }
            m_source->unmount({e.mountPoint, false}, this, [state, step, e](const client::Reply &r) {
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
    const QStringList targets = m_toRemount;
    m_toRemount.clear();
    for (const QString &mp : targets) {
        m_source->unmount({mp, false}, this, [this, mp](const client::Reply &reply) {
            if (!reply.ok()) {
                showHelperError(reply, i18n("%1 was not remounted.", mp));
                return;
            }
            m_source->mountEntry(mp, this, [this, mp](const client::Reply &r) {
                if (r.ok()) {
                    statusBar()->showMessage(i18n("Remounted %1 with the new options.", mp), 8000);
                    reload();
                } else {
                    showHelperError(r, i18n("%1 was unmounted but could not be mounted again.", mp));
                }
            });
        });
    }
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
