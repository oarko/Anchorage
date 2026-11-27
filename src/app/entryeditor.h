// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "access.h"
#include "binds.h"
#include "fstabservice.h"
#include "shares.h"
#include "caller.h"
#include "datasource.h"
#include "drives.h"
#include "fstab.h"
#include "options.h"

#include <QDialog>

#include <optional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

// What the editor needs to know about the rest of the system.
struct EditorContext {
    QList<anchorage::drives::Volume> volumes;
    anchorage::Caller me;
    QStringList otherMountPoints; // mount points of the other entries in the file
    QStringList rootSpecs; // device fields of the "/" entries (for system-entry detection)
    bool systemUnlocked = false; // "Allow editing system entries" is on
    QList<Account> users; // people who can own files (Access tab)
    QString ntfsDriver = QStringLiteral("ntfs3"); // suggested for NTFS drives (settings)
    QList<anchorage::mounts::Mount> mounts; // what is mounted now
    QList<anchorage::binds::Bind> binds; // existing "also show it at" entries of this drive
    // Network shares (SPEC §11)
    QString credentialsDir = QStringLiteral("/etc/anchorage/credentials");
    std::optional<anchorage::shares::Login> login; // typed in Connect to Server
    bool guest = false;
    QList<int> nfsVersions; // what the server offers, if known
};

// The entry editor dialog (SPEC §8, §15): Basic / Access / Advanced tabs and a live
// preview of the fstab line. Editing only changes the dialog's copy; the main window
// collects the result as a pending change.
class EntryEditor : public QDialog
{
    Q_OBJECT

public:
    // `layout` is the existing line (keeps its column alignment in the preview).
    EntryEditor(const anchorage::fstab::Entry &initial,
                bool isNew,
                const EditorContext &context,
                const anchorage::fstab::Line *layout = nullptr,
                QWidget *parent = nullptr);

    anchorage::fstab::Entry entry() const { return m_entry; }
    // Everything that is wrong (errors block OK) or worth knowing.
    QList<anchorage::fstab::Issue> issues() const;

    // Fills in device, type and a suggested mount point from a detected drive.
    void useVolume(const anchorage::drives::Volume &volume);

    enum class IdentifyBy { Uuid, Label, PartUuid, Path };
    static QString specFor(const anchorage::drives::Volume &volume, IdentifyBy by);

    // "Also show it at…" (bind entries saved with this one), including "keep it at the
    // old place too" when that is ticked.
    QList<anchorage::binds::Bind> binds() const;
    void addBind(const anchorage::binds::Bind &bind);
    void removeBind(int index);

    // SMB login typed here, to save as a root-only file when applying (SPEC §11).
    std::optional<anchorage::service::SavedLogin> pendingLogin() const;

    // Asks for confirmation first when ntfs3 is combined with restrictive permissions.
    // (QDialog::accept is already a virtual slot.)
    void accept() override;

private:
    QWidget *buildBasicTab();
    QWidget *buildAccessTab();
    QWidget *buildAdvancedTab();
    void rebuildAdvancedTable();
    void refresh(); // pushes m_entry into every widget
    void setOptions(const QStringList &options);
    void changeAccess(const std::function<void(anchorage::access::Settings &)> &change);
    void typeChanged(const QString &type);
    void advancedItemChanged(QTreeWidgetItem *item, int column);
    void showOptionDetails();
    const anchorage::drives::Volume *matchingVolume() const;
    void updateSuggestions();
    void refreshBinds();
    QWidget *buildShareSection(QWidget *page);
    void applyLogin(); // the Login section -> options
    QString shareServer() const;
    QString credentialsPathFor(const QString &user) const;

    anchorage::fstab::Entry m_entry;
    anchorage::fstab::Entry m_initial;
    bool m_isNew;
    EditorContext m_ctx;
    std::optional<anchorage::fstab::Line> m_layout;
    const anchorage::options::Catalogue &m_catalogue;
    bool m_refreshing = false;
    QString m_tableType; // type the Advanced table was built for

    QTabWidget *m_tabs;
    // Basic
    QComboBox *m_drive;
    class QFormLayout *m_basicForm = nullptr;
    QLineEdit *m_spec;
    QComboBox *m_identify;
    QLabel *m_specHint;
    QComboBox *m_mountPointBox;
    QLineEdit *m_mountPoint; // the box's text field
    QWidget *m_bindBox;
    QTreeWidget *m_bindList;
    QCheckBox *m_keepOld;
    QList<anchorage::binds::Bind> m_binds;
    QString m_mountedAt; // where the drive is mounted now (outside the system folders)
    QWidget *m_loginBox;
    QComboBox *m_loginMode;
    QLineEdit *m_loginUser;
    QLineEdit *m_loginPassword;
    QLineEdit *m_loginDomain;
    QWidget *m_nfsBox;
    QComboBox *m_nfsVersion;
    QLabel *m_nfsOffered;
    QLabel *m_zone;
    QComboBox *m_type;
    QLabel *m_typeInfo;
    // Access
    QButtonGroup *m_start;
    QButtonGroup *m_who;
    QWidget *m_ownerBox;
    QComboBox *m_owner;
    QComboBox *m_sharing;
    QLabel *m_ownerNote;
    QLabel *m_sharingOptions;
    QCheckBox *m_nofail;
    QCheckBox *m_netdev;
    QLabel *m_accessSummary;
    // Advanced
    QLineEdit *m_search;
    QTreeWidget *m_table;
    QLabel *m_details;
    QLineEdit *m_other;
    QSpinBox *m_dump;
    QSpinBox *m_pass;
    // Bottom
    QLabel *m_preview;
    QLabel *m_issues;
    QDialogButtonBox *m_buttons;
};
