// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "datasource.h"
#include "inventory.h"
#include "mountpolicy.h"
#include "fstabservice.h"
#include "shares.h"

#include <KXmlGuiWindow>

#include <optional>

class BackupsPage;
class InfoPanel;
class InventoryFilter;
class InventoryModel;
class KCategorizedView;
class KMessageWidget;
class KToggleAction;
class PlacesPanel;
class QAction;
class QLabel;
class QLineEdit;
class QStackedWidget;
struct EditorContext;

// Main window, layout C (Dolphin-like): Places | tiles | info panel. See docs/SPEC.md §15.
//
// Editing works on a pending copy of /etc/fstab. Each change from the entry editor is
// applied to that copy; the Apply bar (Discard / Preview… / Apply) sends the whole file
// to the helper, which checks it again and writes it.
class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    // Takes ownership of `source`.
    explicit MainWindow(DataSource *source, QWidget *parent = nullptr);

    void reload();

    bool hasPendingChanges() const { return m_pending.has_value(); }
    QByteArray pendingContent() const { return m_pending.value_or(QByteArray()); }

    // Actions, also used by tests.
    void newEntry();
    void editSelected();
    void removeSelected();
    void applyChanges();
    void discardChanges();
    void previewChanges();
    void setSystemEditing(bool on);
    void restoreBackup(const QString &name);
    void showSettings();
    void mountSelected();
    void unmountSelected();
    void mountAtSelected();
    void remountChanged();
    void movePasswords(); // typed-in SMB passwords -> root-only saved logins (SPEC §11)
    void connectToServer();

Q_SIGNALS:
    // An editor dialog was created (tests use this to drive it).
    void editorOpened(QDialog *editor);
    void applyFinished(bool ok);
    void restoreFinished(bool ok);
    void settingsFinished(bool ok);
    void settingsOpened(QDialog *dialog);
    void mountAtOpened(QDialog *dialog);
    void mountFinished(bool ok);
    // After Apply: how many test mounts failed (-1: none were needed).
    void testMountFinished(int failures);
    void passwordsMoved(int count);
    void connectOpened(QDialog *dialog);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupActions();
    void rebuild();
    void updateSelection();
    void updateHeader();
    void updateActions();
    void updateBanners();
    void showContextMenu(const QPoint &pos);
    const anchorage::inventory::Item *selectedItem() const;
    QByteArray currentContent() const;
    void setPending(const QByteArray &content);
    EditorContext editorContext(int editedLine) const;
    void editItem(const anchorage::inventory::Item &item);
    bool canEdit(const anchorage::inventory::Item &item, QString *why = nullptr) const;
    // Shared by Apply and Restore: checks `proposed` against the caller's rights and asks
    // for the typed confirmation for protected locations. Returns false if not allowed or
    // cancelled; `confirm` says whether the confirmation was given.
    bool checkWrite(const QByteArray &proposed, bool *confirm, anchorage::policy::Plan *plan);
    void showHelperError(const anchorage::client::Reply &reply, const QString &what);

    enum class Route { None, AsUser, Helper };
    struct MountChoices {
        Route mount = Route::None;
        Route unmount = Route::None;
        bool mountAt = false;
        QString hint;
    };
    MountChoices mountChoices(const anchorage::inventory::Item &item) const;
    void runUnmount(const anchorage::inventory::Item &item, Route route, bool confirmSystem);
    void testMount(const QList<anchorage::fstab::Entry> &entries, const QString &backup);
    // Makes sure mount.cifs / mount.nfs exist for these file system types (offers to install
    // them), then runs `then`. Does nothing if the person cancels.
    void ensureClientTools(const QStringList &types, const std::function<void()> &then);
    void openShareEditor(const anchorage::fstab::Entry &entry, const std::optional<anchorage::shares::Login> &login, bool guest, const QList<int> &nfsVersions);
    void mountShareAt(const anchorage::fstab::Entry &entry, const std::optional<anchorage::shares::Login> &login, bool guest);
    void writePendingLogins(const std::function<void(bool ok)> &then);

    DataSource *m_source;
    Snapshot m_snapshot;
    std::optional<QByteArray> m_pending; // proposed /etc/fstab
    QByteArray m_baseline; // /etc/fstab when editing started
    bool m_systemUnlocked = false;
    bool m_applying = false;

    InventoryModel *m_model;
    InventoryFilter *m_filter;
    PlacesPanel *m_places;
    KCategorizedView *m_tiles;
    InfoPanel *m_info;
    QWidget *m_infoScroll;
    BackupsPage *m_backups;
    QStackedWidget *m_pages;
    KMessageWidget *m_banner;
    KMessageWidget *m_systemBanner;
    KMessageWidget *m_applyBar;
    KMessageWidget *m_remountBar;
    QStringList m_toRemount; // changed entries that were mounted while saving
    // SMB logins typed in the editor, saved (root-only) when applying: mount point -> login.
    QHash<QString, anchorage::service::SavedLogin> m_pendingLogins;
    QLabel *m_breadcrumb;
    QLineEdit *m_search;
    QLabel *m_statusLeft;
    QLabel *m_statusRight;
    QString m_selectedId;

    QAction *m_newAction = nullptr;
    QAction *m_editAction = nullptr;
    QAction *m_removeAction = nullptr;
    QAction *m_applyAction = nullptr;
    QAction *m_discardAction = nullptr;
    QAction *m_previewAction = nullptr;
    QAction *m_mountAction = nullptr;
    QAction *m_unmountAction = nullptr;
    QAction *m_mountAtAction = nullptr;
    QAction *m_removeOrphansAction = nullptr;
    QAction *m_movePasswordsAction = nullptr;
    QAction *m_connectAction = nullptr;
    KToggleAction *m_systemAction = nullptr;
};
