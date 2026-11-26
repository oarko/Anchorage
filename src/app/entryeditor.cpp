// SPDX-License-Identifier: GPL-3.0-or-later
#include "entryeditor.h"

#include "inventory.h"
#include "locations.h"
#include "mountpolicy.h"

#include <KColorScheme>
#include <KLocalizedString>
#include <KMessageBox>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>


using namespace anchorage;
using access::Sharing;
using access::Start;
using access::Who;
using fstab::Issue;

namespace
{
constexpr int OptionRole = Qt::UserRole + 1;

const options::Option *optionOf(const QTreeWidgetItem *item)
{
    // The catalogue lives for the whole program; rows keep a pointer to their option.
    return item ? reinterpret_cast<const options::Option *>(item->data(0, OptionRole).value<quintptr>()) : nullptr; // NOLINT(performance-no-int-to-ptr)
}

QLabel *optionLabel(QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    l->setForegroundRole(QPalette::PlaceholderText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QString optionsText(const QStringList &opts)
{
    return opts.isEmpty() ? i18nc("no mount options are written", "(no option)") : opts.join(QLatin1Char(','));
}

QLabel *note(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

QString categoryName(options::Category c, const QString &type)
{
    switch (c) {
    case options::Category::General:
        return i18n("General");
    case options::Category::Boot:
        return i18n("Start-up (systemd)");
    case options::Category::Desktop:
        return i18n("Desktop");
    case options::Category::FileSystem:
        return i18n("%1 options", type);
    }
    return {};
}

// Value editor for the Advanced table: a drop-down for options with known values.
class ValueDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.column() != 1) {
            return nullptr;
        }
        const auto *opt = reinterpret_cast<const options::Option *>(index.siblingAtColumn(0).data(OptionRole).value<quintptr>()); // NOLINT(performance-no-int-to-ptr)
        if (!opt || !opt->takesValue()) {
            return nullptr;
        }
        if (!opt->choices.isEmpty()) {
            auto *combo = new QComboBox(parent);
            combo->setEditable(opt->valueType != options::ValueType::Choice);
            if (opt->kind == options::Kind::FlagOrValue) {
                combo->addItem(QString());
            }
            combo->addItems(opt->choices);
            return combo;
        }
        auto *edit = qobject_cast<QLineEdit *>(QStyledItemDelegate::createEditor(parent, option, index));
        if (edit) {
            edit->setPlaceholderText(opt->example);
        }
        return edit;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (auto *combo = qobject_cast<QComboBox *>(editor)) {
            combo->setCurrentText(index.data().toString());
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override
    {
        if (auto *combo = qobject_cast<QComboBox *>(editor)) {
            model->setData(index, combo->currentText());
            return;
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }
};

QString mountPointName(const drives::Volume &v)
{
    QString base = v.label.isEmpty() ? v.name : v.label;
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9._-]+"));
    base.replace(bad, QStringLiteral("-"));
    while (base.startsWith(QLatin1Char('.')) || base.startsWith(QLatin1Char('-'))) {
        base.remove(0, 1);
    }
    return base.isEmpty() ? QStringLiteral("drive") : base;
}

QString typeForVolume(const QString &fsType, const QString &ntfsDriver)
{
    // lsblk says "ntfs"; which driver to suggest is a setting (ntfs3 or ntfs-3g).
    if (fsType == QLatin1String("ntfs")) {
        return ntfsDriver == QLatin1String("ntfs") ? QStringLiteral("ntfs") : QStringLiteral("ntfs3");
    }
    return fsType;
}
} // namespace

QString EntryEditor::specFor(const drives::Volume &v, IdentifyBy by)
{
    switch (by) {
    case IdentifyBy::Uuid:
        if (!v.uuid.isEmpty()) {
            return QStringLiteral("UUID=") + v.uuid;
        }
        break;
    case IdentifyBy::Label:
        if (!v.label.isEmpty()) {
            return QStringLiteral("LABEL=") + v.label;
        }
        break;
    case IdentifyBy::PartUuid:
        if (!v.partUuid.isEmpty()) {
            return QStringLiteral("PARTUUID=") + v.partUuid;
        }
        break;
    case IdentifyBy::Path:
        break;
    }
    return v.path;
}

EntryEditor::EntryEditor(const fstab::Entry &initial, bool isNew, const EditorContext &context, const fstab::Line *layout, QWidget *parent)
    : QDialog(parent)
    , m_entry(initial)
    , m_initial(initial)
    , m_isNew(isNew)
    , m_ctx(context)
    , m_catalogue(options::Catalogue::builtIn())
{
    if (layout) {
        m_layout = *layout;
    }
    setObjectName(QStringLiteral("editor"));
    setWindowTitle(isNew ? i18n("New fstab Entry") : i18n("Edit fstab Entry"));

    auto *layoutV = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("editor.tabs"));
    m_tabs->addTab(buildBasicTab(), i18n("Basic"));
    m_tabs->addTab(buildAccessTab(), i18n("Access"));
    m_tabs->addTab(buildAdvancedTab(), i18n("Advanced"));
    layoutV->addWidget(m_tabs, 1);

    auto *previewTitle = new QLabel(i18n("Lines in /etc/fstab:"), this);
    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("editor.preview"));
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setFrameShape(QFrame::StyledPanel);
    m_preview->setMargin(6);
    m_preview->setWordWrap(true);
    m_preview->setTextFormat(Qt::PlainText);
    m_issues = new QLabel(this);
    m_issues->setObjectName(QStringLiteral("editor.issues"));
    m_issues->setWordWrap(true);
    m_issues->setTextFormat(Qt::RichText);
    layoutV->addWidget(previewTitle);
    layoutV->addWidget(m_preview);
    layoutV->addWidget(m_issues);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(isNew ? i18n("Add Entry") : i18n("Keep Changes"));
    m_buttons->button(QDialogButtonBox::Ok)->setToolTip(i18n("Nothing is saved yet: changes are collected until you click Apply."));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layoutV->addWidget(m_buttons);

    if (m_isNew && m_entry.type.isEmpty()) {
        m_entry.type = QStringLiteral("ext4");
        m_entry.options = access::apply({}, access::defaultsFor(m_entry.type), m_entry.type);
        m_entry.pass = 0;
    }
    m_binds = m_ctx.binds;
    rebuildAdvancedTable();
    refresh();
    resize(820, 760);
}

// ---- Basic --------------------------------------------------------------------------

QWidget *EntryEditor::buildBasicTab()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    outer->addLayout(form);
    outer->addStretch(1);

    m_drive = new QComboBox(page);
    m_drive->setObjectName(QStringLiteral("editor.drive"));
    m_drive->addItem(i18n("Choose a detected drive…"), -1);
    for (int i = 0; i < m_ctx.volumes.size(); ++i) {
        const auto &v = m_ctx.volumes.at(i);
        if (!drives::isUserVisible(v)) {
            continue;
        }
        const QString name = v.label.isEmpty() ? v.partTypeName.isEmpty() ? v.name : v.partTypeName : v.label;
        m_drive->addItem(QIcon::fromTheme(v.removable ? QStringLiteral("drive-removable-media") : QStringLiteral("drive-harddisk")),
                         i18nc("drive name, device, size, file system", "%1  (%2, %3 %4)", name, v.path, inventory::formatSize(v.size), v.fsType),
                         i);
    }
    connect(m_drive, &QComboBox::activated, this, [this](int index) {
        const int i = m_drive->itemData(index).toInt();
        if (i >= 0 && i < m_ctx.volumes.size()) {
            useVolume(m_ctx.volumes.at(i));
        }
    });
    form->addRow(i18n("Drive:"), m_drive);

    m_spec = new QLineEdit(page);
    m_spec->setObjectName(QStringLiteral("editor.spec"));
    m_identify = new QComboBox(page);
    m_identify->setObjectName(QStringLiteral("editor.identify"));
    m_identify->addItem(i18n("by UUID (recommended)"), int(IdentifyBy::Uuid));
    m_identify->addItem(i18n("by label"), int(IdentifyBy::Label));
    m_identify->addItem(i18n("by partition UUID"), int(IdentifyBy::PartUuid));
    m_identify->addItem(i18n("by device path"), int(IdentifyBy::Path));
    m_identify->setToolTip(i18n("How fstab finds the drive. A UUID never changes, while device names like /dev/sdb1 can change "
                                "when drives are added or removed."));
    auto *specRow = new QHBoxLayout;
    specRow->addWidget(m_spec, 1);
    specRow->addWidget(m_identify);
    form->addRow(i18n("Device or share:"), specRow);
    m_specHint = note(QString(), page);
    form->addRow(QString(), m_specHint);
    connect(m_spec, &QLineEdit::textEdited, this, [this](const QString &t) {
        m_entry.spec = t;
        refresh();
    });
    connect(m_identify, &QComboBox::activated, this, [this] {
        if (const auto *v = matchingVolume()) {
            m_entry.spec = specFor(*v, IdentifyBy(m_identify->currentData().toInt()));
            refresh();
        }
    });

    m_mountPointBox = new QComboBox(page);
    m_mountPointBox->setObjectName(QStringLiteral("editor.mountPointBox"));
    m_mountPointBox->setEditable(true);
    m_mountPointBox->setInsertPolicy(QComboBox::NoInsert);
    m_mountPoint = m_mountPointBox->lineEdit();
    m_mountPoint->setObjectName(QStringLiteral("editor.mountPoint"));
    m_mountPoint->setPlaceholderText(QStringLiteral("/mnt/data"));
    auto *browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Browse…"), page);
    browse->setObjectName(QStringLiteral("editor.browse"));
    auto *mpRow = new QHBoxLayout;
    mpRow->addWidget(m_mountPointBox, 1);
    mpRow->addWidget(browse);
    form->addRow(i18n("Mount point:"), mpRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString start = QFileInfo::exists(m_entry.mountPoint) ? m_entry.mountPoint : QStringLiteral("/mnt");
        const QString dir = QFileDialog::getExistingDirectory(this, i18n("Choose the Mount Point"), start);
        if (!dir.isEmpty()) {
            m_entry.mountPoint = dir;
            refresh();
        }
    });
    connect(m_mountPointBox, &QComboBox::activated, this, [this](int index) {
        m_entry.mountPoint = m_mountPointBox->itemText(index);
        refresh();
    });
    m_zone = note(QString(), page);
    m_zone->setObjectName(QStringLiteral("editor.zone"));
    form->addRow(QString(), m_zone);
    connect(m_mountPoint, &QLineEdit::textEdited, this, [this](const QString &t) {
        m_entry.mountPoint = t;
        refresh();
    });

    m_type = new QComboBox(page);
    m_type->setObjectName(QStringLiteral("editor.type"));
    m_type->setEditable(true);
    for (const auto &fs : m_catalogue.fileSystems()) {
        m_type->addItem(fs.type);
        m_type->setItemData(m_type->count() - 1, fs.label, Qt::ToolTipRole);
    }
    form->addRow(i18n("File system:"), m_type);
    m_typeInfo = note(QString(), page);
    form->addRow(QString(), m_typeInfo);
    connect(m_type, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        if (!m_refreshing) {
            typeChanged(t.trimmed());
        }
    });

    // "Also show it at…": bind entries saved together with this one.
    auto *bindBox = new QGroupBox(i18n("Also show it at…"), page);
    m_bindBox = bindBox;
    auto *bv = new QVBoxLayout(bindBox);
    bv->addWidget(note(i18n("The same files at more places, e.g. where programs expect them. Each place is its own fstab line that waits "
                            "for this drive, so a missing drive never shows its empty folder."),
                       bindBox));
    m_bindList = new QTreeWidget(bindBox);
    m_bindList->setObjectName(QStringLiteral("editor.binds"));
    m_bindList->setHeaderLabels({i18n("Shown at"), i18n("Folder on the drive")});
    m_bindList->setRootIsDecorated(false);
    m_bindList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_bindList->setMaximumHeight(110);
    bv->addWidget(m_bindList);
    m_keepOld = new QCheckBox(bindBox);
    m_keepOld->setObjectName(QStringLiteral("editor.keepOld"));
    bv->addWidget(m_keepOld);
    auto *bindButtons = new QHBoxLayout;
    auto *addBindButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18n("Add Place…"), bindBox);
    addBindButton->setObjectName(QStringLiteral("editor.addBind"));
    auto *removeBindButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove"), bindBox);
    removeBindButton->setObjectName(QStringLiteral("editor.removeBind"));
    bindButtons->addWidget(addBindButton);
    bindButtons->addWidget(removeBindButton);
    bindButtons->addStretch(1);
    bv->addLayout(bindButtons);
    outer->insertWidget(1, bindBox);
    connect(m_keepOld, &QCheckBox::toggled, this, [this] {
        refresh();
    });
    connect(removeBindButton, &QPushButton::clicked, this, [this] {
        removeBind(m_bindList->currentIndex().row());
    });
    connect(addBindButton, &QPushButton::clicked, this, [this] {
        QDialog dlg(this);
        dlg.setObjectName(QStringLiteral("bindDialog"));
        dlg.setWindowTitle(i18n("Also Show It At…"));
        auto *f = new QFormLayout(&dlg);
        auto *target = new QLineEdit(&dlg);
        target->setObjectName(QStringLiteral("bind.target"));
        target->setPlaceholderText(QStringLiteral("/home/you/Games"));
        auto *pickTarget = new QPushButton(i18n("Browse…"), &dlg);
        auto *tRow = new QHBoxLayout;
        tRow->addWidget(target, 1);
        tRow->addWidget(pickTarget);
        auto *sub = new QLineEdit(&dlg);
        sub->setObjectName(QStringLiteral("bind.subfolder"));
        sub->setPlaceholderText(i18n("empty: the whole drive"));
        auto *pickSub = new QPushButton(i18n("Browse…"), &dlg);
        pickSub->setEnabled(!m_mountedAt.isEmpty());
        pickSub->setToolTip(i18n("Available while the drive is mounted."));
        auto *sRow = new QHBoxLayout;
        sRow->addWidget(sub, 1);
        sRow->addWidget(pickSub);
        f->addRow(i18n("Show it at:"), tRow);
        f->addRow(i18n("Folder on the drive:"), sRow);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        f->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        connect(pickTarget, &QPushButton::clicked, &dlg, [&dlg, target] {
            const QString d = QFileDialog::getExistingDirectory(&dlg, i18n("Choose a Folder"), QDir::homePath());
            if (!d.isEmpty()) {
                target->setText(d);
            }
        });
        connect(pickSub, &QPushButton::clicked, &dlg, [this, &dlg, sub] {
            const QString d = QFileDialog::getExistingDirectory(&dlg, i18n("Choose a Folder on the Drive"), m_mountedAt);
            if (d.startsWith(m_mountedAt + QLatin1Char('/'))) {
                sub->setText(d.mid(m_mountedAt.size() + 1));
            } else if (d == m_mountedAt) {
                sub->clear();
            }
        });
        if (dlg.exec() == QDialog::Accepted && !target->text().trimmed().isEmpty()) {
            addBind({sub->text().trimmed(), target->text().trimmed()});
        }
    });
    return page;
}

void EntryEditor::useVolume(const drives::Volume &v)
{
    const auto by = IdentifyBy(m_identify->currentData().toInt());
    m_entry.spec = specFor(v, by);
    const QString type = typeForVolume(v.fsType, m_ctx.ntfsDriver);
    if (!type.isEmpty() && type != m_entry.type) {
        typeChanged(type);
    }
    if (m_entry.type == QLatin1String("swap")) {
        m_entry.mountPoint = QStringLiteral("none");
    } else if (m_isNew && (m_entry.mountPoint.isEmpty() || m_entry.mountPoint == m_initial.mountPoint)) {
        // Where it is mounted now, if anywhere usable; otherwise /mnt/<name> (SPEC §8).
        const QStringList s = locations::suggestions(v, m_ctx.me.name, m_ctx.mounts, [](const QString &p) {
            return QFileInfo::exists(p);
        });
        m_entry.mountPoint = s.isEmpty() ? QStringLiteral("/mnt/") + mountPointName(v) : s.first();
    }
    refresh();
}

void EntryEditor::updateSuggestions()
{
    QStringList items;
    m_mountedAt.clear();
    if (const auto *v = matchingVolume()) {
        items = locations::suggestions(*v, m_ctx.me.name, m_ctx.mounts, [](const QString &p) {
            return QFileInfo::exists(p);
        });
        for (const QString &mp : v->mountPoints) {
            if (!mp.isEmpty() && items.contains(mp)) {
                m_mountedAt = mp; // first usable place it is mounted at now
                break;
            }
        }
    }
    if (!m_initial.mountPoint.isEmpty() && !items.contains(m_initial.mountPoint) && m_initial.mountPoint != QLatin1String("none")) {
        items.prepend(m_initial.mountPoint);
    }
    QStringList now;
    for (int i = 0; i < m_mountPointBox->count(); ++i) {
        now.append(m_mountPointBox->itemText(i));
    }
    if (now != items) {
        const QSignalBlocker block(m_mountPointBox);
        m_mountPointBox->clear();
        m_mountPointBox->addItems(items);
        m_mountPoint->setText(m_entry.mountPoint);
    }
}

QList<binds::Bind> EntryEditor::binds() const
{
    QList<binds::Bind> out = m_binds;
    // isHidden, not isVisible: this is also read after the dialog has closed.
    if (!m_keepOld->isHidden() && m_keepOld->isChecked() && !m_mountedAt.isEmpty()) {
        const binds::Bind keep{QString(), m_mountedAt};
        if (!out.contains(keep)) {
            out.append(keep);
        }
    }
    return out;
}

void EntryEditor::addBind(const binds::Bind &bind)
{
    m_binds.append(bind);
    refresh();
}

void EntryEditor::removeBind(int index)
{
    if (index >= 0 && index < m_binds.size()) {
        m_binds.removeAt(index);
        refresh();
    }
}

void EntryEditor::refreshBinds()
{
    const bool can = m_entry.type != QLatin1String("swap") && m_entry.type != QLatin1String("none") && !binds::isBind(m_entry);
    m_bindBox->setVisible(can);
    {
        const QSignalBlocker block(m_bindList);
        m_bindList->clear();
        for (const auto &b : std::as_const(m_binds)) {
            new QTreeWidgetItem(m_bindList, {b.target, b.subfolder.isEmpty() ? i18n("(the whole drive)") : b.subfolder});
        }
    }
    // Moving a mounted drive: offer to keep it reachable where it is now.
    const bool moving = !m_mountedAt.isEmpty() && m_entry.mountPoint != m_mountedAt && !m_binds.contains(binds::Bind{QString(), m_mountedAt});
    m_keepOld->setHidden(!(can && moving));
    m_keepOld->setText(i18n("Keep it reachable at %1 too", m_mountedAt));
}

const drives::Volume *EntryEditor::matchingVolume() const
{
    for (const auto &v : m_ctx.volumes) {
        if (inventory::specMatches(m_entry.spec, v)) {
            return &v;
        }
    }
    return nullptr;
}

void EntryEditor::typeChanged(const QString &type)
{
    if (type == m_entry.type) {
        return;
    }
    const QString old = m_entry.type;
    if (m_isNew) {
        // Untouched suggestions follow the new type.
        if (m_entry.options == access::apply({}, access::defaultsFor(old), old)) {
            m_entry.options = access::apply({}, access::defaultsFor(type), type);
        }
        const auto *oldFs = m_catalogue.fileSystem(old);
        const auto *newFs = m_catalogue.fileSystem(type);
        if (newFs && (!oldFs || m_entry.pass == oldFs->pass)) {
            m_entry.pass = newFs->pass;
        }
    }
    if (type == QLatin1String("swap")) {
        m_entry.mountPoint = QStringLiteral("none");
    } else if (old == QLatin1String("swap") && (m_entry.mountPoint == QLatin1String("none") || m_entry.mountPoint == QLatin1String("swap"))) {
        m_entry.mountPoint.clear();
    }
    m_entry.type = type;
    rebuildAdvancedTable();
    refresh();
}

// ---- Access -------------------------------------------------------------------------

QWidget *EntryEditor::buildAccessTab()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(scroll);
    auto *v = new QVBoxLayout(page);

    const auto radioGroup = [&](const QString &title, const QString &name, QButtonGroup *&group,
                                const QList<std::tuple<int, QString, QStringList, QString>> &choices) {
        auto *box = new QGroupBox(title, page);
        auto *grid = new QGridLayout(box);
        group = new QButtonGroup(box);
        int row = 0;
        for (const auto &[id, text, opts, tip] : choices) {
            auto *r = new QRadioButton(text, box);
            r->setObjectName(QStringLiteral("access.%1.%2").arg(name).arg(id));
            r->setToolTip(tip);
            group->addButton(r, id);
            auto *ol = optionLabel(box);
            ol->setText(optionsText(opts));
            grid->addWidget(r, row, 0);
            grid->addWidget(ol, row, 1, Qt::AlignRight);
            ++row;
        }
        grid->setColumnStretch(0, 1);
        v->addWidget(box);
        return box;
    };

    radioGroup(i18n("When it mounts"), QStringLiteral("start"), m_start,
               {{int(Start::AtBoot), i18n("While the computer starts"), access::startOptions(Start::AtBoot),
                 i18n("Mounted every time the computer starts.")},
                {int(Start::OnRequest), i18n("Only when asked (clicking it in Dolphin, or the mount command)"), access::startOptions(Start::OnRequest),
                 i18n("Not mounted at start-up.")},
                {int(Start::WhenOpened), i18n("The first time someone opens the folder"), access::startOptions(Start::WhenOpened),
                 i18n("systemd watches the folder and mounts the drive or share the moment it is opened. Start-up doesn't wait for it.")}});
    connect(m_start, &QButtonGroup::idClicked, this, [this](int id) {
        changeAccess([id](access::Settings &s) {
            s.start = Start(id);
        });
    });

    auto *whoBox = radioGroup(i18n("Who can mount it"), QStringLiteral("who"), m_who,
                              {{int(Who::Admins), i18n("Administrators only"), access::whoOptions(Who::Admins), QString()},
                               {int(Who::AnyUser), i18n("Any user; only they can unmount it"), access::whoOptions(Who::AnyUser), QString()},
                               {int(Who::AnyUserAnyoneUnmounts), i18n("Any user; anyone can unmount it"), access::whoOptions(Who::AnyUserAnyoneUnmounts),
                                QString()}});
    whoBox->layout()->addWidget(note(i18n("'user' and 'users' also switch on noexec, nosuid and nodev: programs on the drive can't be run "
                                          "unless 'exec' is added after them on the Advanced tab."),
                                     whoBox));
    connect(m_who, &QButtonGroup::idClicked, this, [this](int id) {
        changeAccess([id](access::Settings &s) {
            s.who = Who(id);
        });
    });

    m_ownerBox = new QGroupBox(i18n("Who owns the files"), page);
    auto *ownerForm = new QFormLayout(m_ownerBox);
    m_owner = new QComboBox(m_ownerBox);
    m_owner->setObjectName(QStringLiteral("access.owner"));
    m_owner->addItem(i18n("Not set: whoever mounts it"), qlonglong(-1));
    for (const auto &u : m_ctx.users) {
        m_owner->addItem(u.fullName.isEmpty() ? u.name : QStringLiteral("%1 (%2)").arg(u.fullName, u.name), qlonglong(u.uid));
        m_owner->setItemData(m_owner->count() - 1, qlonglong(u.gid), Qt::UserRole + 1);
    }
    m_sharing = new QComboBox(m_ownerBox);
    m_sharing->setObjectName(QStringLiteral("access.sharing"));
    m_sharing->addItem(i18n("Driver default (others can read)"), int(Sharing::Unset));
    m_sharing->addItem(i18n("Only the owner can open the files"), int(Sharing::OwnerOnly));
    m_sharing->addItem(i18n("The owner can change, others can read"), int(Sharing::OthersRead));
    m_sharing->addItem(i18n("Everyone can change the files"), int(Sharing::Everyone));
    m_sharing->addItem(i18n("Custom (set on the Advanced tab)"), int(Sharing::Custom));
    m_sharingOptions = optionLabel(m_ownerBox);
    m_ownerNote = note(QString(), m_ownerBox);
    ownerForm->addRow(i18n("Owner:"), m_owner);
    ownerForm->addRow(i18n("Permissions:"), m_sharing);
    ownerForm->addRow(QString(), m_sharingOptions);
    ownerForm->addRow(m_ownerNote);
    v->addWidget(m_ownerBox);
    connect(m_owner, &QComboBox::activated, this, [this](int index) {
        const qlonglong uid = m_owner->itemData(index).toLongLong();
        const QVariant gid = m_owner->itemData(index, Qt::UserRole + 1);
        changeAccess([uid, gid](access::Settings &s) {
            if (uid < 0) {
                s.uid.reset();
                s.gid.reset();
            } else {
                s.uid = quint32(uid);
                if (gid.isValid()) {
                    s.gid = quint32(gid.toLongLong());
                }
            }
        });
    });
    connect(m_sharing, &QComboBox::activated, this, [this](int index) {
        const auto sharing = Sharing(m_sharing->itemData(index).toInt());
        changeAccess([sharing](access::Settings &s) {
            s.sharing = sharing;
        });
    });

    auto *safety = new QGroupBox(i18n("Boot safety"), page);
    auto *sv = new QGridLayout(safety);
    m_nofail = new QCheckBox(i18n("Start the computer even if this is missing or fails"), safety);
    m_nofail->setObjectName(QStringLiteral("access.nofail"));
    m_nofail->setToolTip(m_catalogue.find(QStringLiteral("ext4"), QStringLiteral("nofail"))->description);
    m_netdev = new QCheckBox(i18n("Needs the network (wait for it while starting)"), safety);
    m_netdev->setObjectName(QStringLiteral("access.netdev"));
    m_netdev->setToolTip(m_catalogue.find(QStringLiteral("ext4"), QStringLiteral("_netdev"))->description);
    auto *nofailOpt = optionLabel(safety);
    nofailOpt->setText(QStringLiteral("nofail"));
    auto *netdevOpt = optionLabel(safety);
    netdevOpt->setText(QStringLiteral("_netdev"));
    sv->addWidget(m_nofail, 0, 0);
    sv->addWidget(nofailOpt, 0, 1, Qt::AlignRight);
    sv->addWidget(m_netdev, 1, 0);
    sv->addWidget(netdevOpt, 1, 1, Qt::AlignRight);
    sv->setColumnStretch(0, 1);
    v->addWidget(safety);
    connect(m_nofail, &QCheckBox::clicked, this, [this](bool on) {
        changeAccess([on](access::Settings &s) {
            s.nofail = on;
        });
    });
    connect(m_netdev, &QCheckBox::clicked, this, [this](bool on) {
        changeAccess([on](access::Settings &s) {
            s.netdev = on;
        });
    });

    m_accessSummary = note(QString(), page);
    v->addWidget(m_accessSummary);
    v->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

void EntryEditor::changeAccess(const std::function<void(access::Settings &)> &change)
{
    access::Settings s = access::read(m_entry.options, m_entry.type, m_catalogue);
    change(s);
    setOptions(access::apply(m_entry.options, s, m_entry.type, m_catalogue));
}

void EntryEditor::setOptions(const QStringList &opts)
{
    m_entry.options = opts;
    if (m_entry.options == QStringList{QStringLiteral("defaults")}) {
        m_entry.options.clear(); // written as "defaults" anyway
    }
    refresh();
}

// ---- Advanced -----------------------------------------------------------------------

QWidget *EntryEditor::buildAdvancedTab()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    m_search = new QLineEdit(page);
    m_search->setObjectName(QStringLiteral("advanced.search"));
    m_search->setPlaceholderText(i18n("Search options…"));
    m_search->setClearButtonEnabled(true);
    v->addWidget(m_search);

    auto *split = new QSplitter(Qt::Vertical, page);
    m_table = new QTreeWidget(split);
    m_table->setObjectName(QStringLiteral("advanced.table"));
    m_table->setHeaderLabels({i18n("Option"), i18n("Value"), i18n("Default"), i18n("What it does")});
    m_table->setItemDelegate(new ValueDelegate(m_table));
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_details = new QLabel(split);
    m_details->setObjectName(QStringLiteral("advanced.details"));
    m_details->setWordWrap(true);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_details->setMargin(6);
    m_details->setTextFormat(Qt::RichText);
    split->addWidget(m_table);
    split->addWidget(m_details);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    v->addWidget(split, 1);

    auto *form = new QFormLayout;
    m_other = new QLineEdit(page);
    m_other->setObjectName(QStringLiteral("advanced.other"));
    m_other->setPlaceholderText(i18n("option1,option2=value"));
    m_other->setToolTip(i18n("Options that aren't in the list above, separated by commas."));
    form->addRow(i18n("Other options:"), m_other);
    auto *numbers = new QHBoxLayout;
    m_dump = new QSpinBox(page);
    m_dump->setObjectName(QStringLiteral("advanced.dump"));
    m_dump->setRange(0, 9);
    m_dump->setToolTip(i18n("Used only by the old 'dump' backup program. Leave at 0."));
    m_pass = new QSpinBox(page);
    m_pass->setObjectName(QStringLiteral("advanced.pass"));
    m_pass->setRange(0, 9);
    m_pass->setToolTip(i18n("Order in which file systems are checked for errors while starting: 1 for the root file system, 2 for other "
                            "Linux file systems, 0 to never check (FAT, NTFS, Btrfs, XFS, network shares)."));
    numbers->addWidget(new QLabel(i18n("Backup (dump):"), page));
    numbers->addWidget(m_dump);
    numbers->addSpacing(20);
    numbers->addWidget(new QLabel(i18n("Check at start-up (pass):"), page));
    numbers->addWidget(m_pass);
    numbers->addStretch(1);
    form->addRow(numbers);
    v->addLayout(form);

    connect(m_table, &QTreeWidget::itemChanged, this, &EntryEditor::advancedItemChanged);
    connect(m_table, &QTreeWidget::currentItemChanged, this, &EntryEditor::showOptionDetails);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
            QTreeWidgetItem *cat = m_table->topLevelItem(i);
            int shown = 0;
            for (int j = 0; j < cat->childCount(); ++j) {
                QTreeWidgetItem *it = cat->child(j);
                const auto *opt = optionOf(it);
                const bool match = text.isEmpty() || it->text(0).contains(text, Qt::CaseInsensitive)
                    || (opt && opt->description.contains(text, Qt::CaseInsensitive));
                it->setHidden(!match);
                shown += match ? 1 : 0;
            }
            cat->setHidden(shown == 0);
        }
    });
    connect(m_other, &QLineEdit::editingFinished, this, [this] {
        // Replace the options the catalogue doesn't know with what was typed.
        QStringList opts;
        for (const QString &o : std::as_const(m_entry.options)) {
            if (m_catalogue.find(m_entry.type, o)) {
                opts.append(o);
            }
        }
        for (const QString &o : m_other->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            opts.append(o.trimmed());
        }
        setOptions(opts);
    });
    connect(m_dump, &QSpinBox::valueChanged, this, [this](int value) {
        if (!m_refreshing) {
            m_entry.dump = value;
            refresh();
        }
    });
    connect(m_pass, &QSpinBox::valueChanged, this, [this](int value) {
        if (!m_refreshing) {
            m_entry.pass = value;
            refresh();
        }
    });
    return page;
}

void EntryEditor::rebuildAdvancedTable()
{
    if (m_tableType == m_entry.type && m_table->topLevelItemCount() > 0) {
        return;
    }
    m_tableType = m_entry.type;
    const QSignalBlocker block(m_table);
    m_table->clear();
    QHash<int, QTreeWidgetItem *> cats;
    for (const auto *opt : m_catalogue.optionsFor(m_entry.type)) {
        const int c = int(opt->category);
        if (!cats.contains(c)) {
            auto *cat = new QTreeWidgetItem(m_table, {categoryName(opt->category, m_entry.type)});
            cat->setFlags(Qt::ItemIsEnabled);
            QFont f = cat->font(0);
            f.setBold(true);
            cat->setFont(0, f);
            cat->setFirstColumnSpanned(true);
            cats.insert(c, cat);
        }
        auto *it = new QTreeWidgetItem(cats.value(c));
        it->setText(0, opt->name);
        it->setData(0, OptionRole, QVariant::fromValue(reinterpret_cast<quintptr>(opt)));
        it->setText(2, opt->defaultText);
        it->setText(3, opt->description);
        QString tip = opt->description;
        if (!opt->warning.isEmpty()) {
            tip += QStringLiteral("\n\n⚠ ") + opt->warning;
            it->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-warning")));
        }
        if (opt->access) {
            tip += QStringLiteral("\n\n") + i18n("This option is also set on the Access tab.");
        }
        it->setToolTip(0, tip);
        it->setToolTip(3, tip);
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
        if (opt->takesValue()) {
            flags |= Qt::ItemIsEditable;
        }
        it->setFlags(flags);
        it->setCheckState(0, Qt::Unchecked);
    }
    // File-system options first: that's what people come here for.
    if (auto *fsCat = cats.value(int(options::Category::FileSystem))) {
        m_table->insertTopLevelItem(0, m_table->takeTopLevelItem(m_table->indexOfTopLevelItem(fsCat)));
    }
    m_table->expandAll();
    m_table->resizeColumnToContents(0);
    m_table->setColumnWidth(1, 120);
    m_table->setColumnWidth(2, 120);
}

void EntryEditor::advancedItemChanged(QTreeWidgetItem *item, int column)
{
    const auto *opt = optionOf(item);
    if (m_refreshing || !opt || (column != 0 && column != 1)) {
        return;
    }
    const auto mapsTo = [this](const QString &o, const options::Option *target) {
        return m_catalogue.find(m_entry.type, o) == target;
    };
    QStringList opts = m_entry.options;
    const QString text = item->text(1).trimmed();
    const std::optional<QString> value = !text.isEmpty() ? std::optional<QString>(text)
        : opt->needsValue()                              ? std::optional<QString>(opt->example.isEmpty() && !opt->choices.isEmpty() ? opt->choices.first() : opt->example)
                                                         : std::nullopt;
    const QString rendered = options::join(opt->name, value);

    if (column == 0) {
        if (item->checkState(0) == Qt::Checked) {
            opts.erase(std::remove_if(opts.begin(), opts.end(),
                                      [&](const QString &o) {
                                          const auto *other = m_catalogue.find(m_entry.type, o);
                                          return other == opt || (other && !opt->exclusive.isEmpty() && other->exclusive == opt->exclusive);
                                      }),
                       opts.end());
            opts.append(rendered);
        } else {
            opts.erase(std::remove_if(opts.begin(), opts.end(),
                                      [&](const QString &o) {
                                          return mapsTo(o, opt);
                                      }),
                       opts.end());
        }
    } else {
        bool replaced = false;
        for (QString &o : opts) {
            if (mapsTo(o, opt)) {
                o = rendered;
                replaced = true;
                break;
            }
        }
        if (!replaced && !text.isEmpty()) {
            opts.append(rendered);
        }
    }
    setOptions(opts);
}

void EntryEditor::showOptionDetails()
{
    const auto *opt = optionOf(m_table->currentItem());
    if (!opt) {
        m_details->clear();
        return;
    }
    QString html = QStringLiteral("<b><tt>%1</tt></b>").arg(opt->name.toHtmlEscaped());
    if (opt->takesValue()) {
        html += QStringLiteral("<tt>%1=%2</tt>").arg(opt->kind == options::Kind::FlagOrValue ? QStringLiteral("[") : QString(),
                                                    (opt->choices.isEmpty() ? opt->example : opt->choices.join(QLatin1Char('|'))).toHtmlEscaped())
            + (opt->kind == options::Kind::FlagOrValue ? QStringLiteral("<tt>]</tt>") : QString());
    }
    html += QStringLiteral("<br>") + opt->description.toHtmlEscaped();
    if (!opt->alias.isEmpty()) {
        html += QStringLiteral("<br>") + i18n("Same as <tt>%1</tt>.", opt->alias.toHtmlEscaped());
    }
    if (!opt->defaultText.isEmpty()) {
        html += QStringLiteral("<br><i>%1</i> %2").arg(i18n("Default:"), opt->defaultText.toHtmlEscaped());
    }
    if (!opt->warning.isEmpty()) {
        const QColor c = KColorScheme(QPalette::Active, KColorScheme::View).foreground(KColorScheme::NeutralText).color();
        html += QStringLiteral("<br><span style=\"color:%1\">⚠ %2</span>").arg(c.name(), opt->warning.toHtmlEscaped());
    }
    m_details->setText(html);
}

// ---- Refresh and checks ---------------------------------------------------------------

void EntryEditor::refresh()
{
    if (m_refreshing) {
        return;
    }
    m_refreshing = true;
    const auto fam = m_catalogue.family(m_entry.type);
    const bool swap = fam == options::Family::Swap;
    const bool network = fam == options::Family::Network;

    // Basic
    if (m_spec->text() != m_entry.spec) {
        m_spec->setText(m_entry.spec);
    }
    if (m_mountPoint->text() != m_entry.mountPoint) {
        m_mountPoint->setText(m_entry.mountPoint);
    }
    m_mountPointBox->setEnabled(!swap);
    updateSuggestions();
    refreshBinds();
    if (m_type->currentText() != m_entry.type) {
        m_type->setCurrentText(m_entry.type);
    }
    const auto *vol = matchingVolume();
    m_identify->setEnabled(vol != nullptr && !network);
    if (!vol) {
        m_drive->setCurrentIndex(0);
    }
    if (vol) {
        IdentifyBy by = IdentifyBy::Path;
        if (m_entry.spec.startsWith(QLatin1String("UUID="))) {
            by = IdentifyBy::Uuid;
        } else if (m_entry.spec.startsWith(QLatin1String("LABEL="))) {
            by = IdentifyBy::Label;
        } else if (m_entry.spec.startsWith(QLatin1String("PARTUUID="))) {
            by = IdentifyBy::PartUuid;
        }
        m_identify->setCurrentIndex(m_identify->findData(int(by)));
        const int volIndex = static_cast<int>(vol - m_ctx.volumes.constData());
        const int comboIndex = m_drive->findData(volIndex);
        m_drive->setCurrentIndex(comboIndex < 0 ? 0 : comboIndex);
        const QString name = vol->label.isEmpty() ? vol->path : vol->label;
        m_specHint->setText(i18n("This is %1 (%2, %3).", name, vol->path, inventory::formatSize(vol->size)));
    } else if (network) {
        m_specHint->setText(m_entry.type.startsWith(QLatin1String("nfs")) ? i18n("Write an NFS share as server:/path, e.g. nas.local:/srv/media")
                                                                           : i18n("Write an SMB share as //server/share, e.g. //nas.local/Photos"));
    } else if (m_entry.type == QLatin1String("tmpfs")) {
        m_specHint->setText(i18n("Usually just 'tmpfs'."));
    } else if (m_entry.type == QLatin1String("none")) {
        m_specHint->setText(i18n("The existing folder to show at the mount point, e.g. /srv/data/photos"));
    } else {
        m_specHint->setText(m_entry.spec.isEmpty() ? i18n("Choose a detected drive above, or type UUID=…, LABEL=… or /dev/…")
                                                   : i18n("No connected drive matches this. That's fine for a drive that is plugged in later."));
    }
    const auto *fs = m_catalogue.fileSystem(m_entry.type);
    m_typeInfo->setText(fs ? QStringLiteral("<b>%1</b>: %2").arg(fs->label.toHtmlEscaped(), fs->description.toHtmlEscaped())
                           : i18n("Not a file system Anchorage knows; only general options are listed."));
    if (swap) {
        m_zone->setText(i18n("Swap has no mount point."));
    } else {
        const auto zone = policy::classifyMountPoint(m_entry.mountPoint);
        switch (zone.zone) {
        case policy::Zone::Safe:
            m_zone->setText(i18n("✔ A usual place for mounts."));
            break;
        case policy::Zone::Override:
            m_zone->setText(i18n("Outside the usual places (/mnt, /media, /srv, home folders): only driveadmins and administrators, and the "
                                 "folder must be empty or new."));
            break;
        case policy::Zone::Blocked:
            m_zone->setText(i18n("Protected system location: administrators only, after typing the path to confirm."));
            break;
        case policy::Zone::Invalid:
            m_zone->setText(m_entry.mountPoint.isEmpty() ? i18n("Where the drive's files will appear, e.g. /mnt/data.") : zone.reason);
            break;
        }
    }

    // Access
    const access::Settings s = access::read(m_entry.options, m_entry.type, m_catalogue);
    const access::Applicable can = access::applicable(m_entry.type, m_catalogue);
    m_start->button(int(s.start))->setChecked(true);
    m_start->button(int(Start::WhenOpened))->setEnabled(can.automount);
    m_who->button(int(s.who))->setChecked(true);
    for (auto *b : m_who->buttons()) {
        b->setEnabled(can.who);
    }
    const auto model = m_catalogue.ownerModel(m_entry.type);
    m_ownerBox->setVisible(model != options::OwnerModel::None);
    m_owner->setEnabled(can.owner);
    m_sharing->setEnabled(can.owner);
    if (can.owner) {
        int idx = 0;
        if (s.uid) {
            idx = m_owner->findData(qlonglong(*s.uid));
            if (idx < 0) {
                m_owner->addItem(i18n("User number %1", *s.uid), qlonglong(*s.uid));
                idx = m_owner->count() - 1;
            }
        }
        m_owner->setCurrentIndex(idx);
        m_sharing->setCurrentIndex(m_sharing->findData(int(s.sharing)));
        m_sharingOptions->setText(s.sharing == Sharing::Custom ? i18n("(see the Advanced tab)") : optionsText(sharingOptions(s.sharing, model)));
        m_ownerNote->setText(model == options::OwnerModel::Cifs
                                 ? i18n("Used when the server doesn't send Linux owners and permissions (most Windows servers and NAS devices).")
                                 : i18n("%1 has no Linux owners: everything on it gets the owner and permissions chosen here.", m_entry.type));
    } else {
        m_owner->setCurrentIndex(0);
        m_sharingOptions->clear();
        m_ownerNote->setText(i18n("%1 keeps its own owners and permissions for every file. Change them after mounting, e.g. in Dolphin's "
                                  "Properties dialog or with chown.",
                                  m_entry.type));
    }
    m_nofail->setChecked(s.nofail);
    m_netdev->setChecked(s.netdev);
    m_netdev->setEnabled(can.netdev);
    m_accessSummary->setText(i18n("All options: %1", m_entry.options.isEmpty() ? QStringLiteral("defaults") : m_entry.options.join(QLatin1Char(','))));

    // Advanced
    {
        const QSignalBlocker block(m_table);
        QStringList unknown;
        QHash<const options::Option *, std::optional<QString>> present;
        for (const QString &o : std::as_const(m_entry.options)) {
            const auto item = options::split(o);
            if (const auto *opt = m_catalogue.find(m_entry.type, item)) {
                if (!present.contains(opt)) {
                    present.insert(opt, item.value);
                }
            } else {
                unknown.append(o);
            }
        }
        for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
            QTreeWidgetItem *cat = m_table->topLevelItem(i);
            for (int j = 0; j < cat->childCount(); ++j) {
                QTreeWidgetItem *it = cat->child(j);
                const auto *opt = optionOf(it);
                const bool on = present.contains(opt);
                it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
                it->setText(1, on ? present.value(opt).value_or(QString()) : QString());
                QFont f = it->font(0);
                f.setBold(on);
                it->setFont(0, f);
            }
        }
        if (!m_other->hasFocus()) {
            m_other->setText(unknown.join(QLatin1Char(',')));
        }
    }
    m_dump->setValue(m_entry.dump);
    m_pass->setValue(m_entry.pass);

    // Preview and checks
    QString preview = QString::fromUtf8(fstab::formatEntry(m_entry, m_layout ? &*m_layout : nullptr));
    for (const auto &b : binds()) {
        preview += QLatin1Char('\n') + QString::fromUtf8(fstab::formatEntry(binds::makeEntry(m_entry, b)));
    }
    m_preview->setText(preview);
    const QList<Issue> list = issues();
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    const QString red = scheme.foreground(KColorScheme::NegativeText).color().name();
    const QString yellow = scheme.foreground(KColorScheme::NeutralText).color().name();
    QStringList lines;
    for (const Issue &i : list) {
        const bool err = i.severity == Issue::Severity::Error;
        lines.append(QStringLiteral("<span style=\"color:%1\">%2 %3</span>").arg(err ? red : yellow, err ? QStringLiteral("✖") : QStringLiteral("⚠"), i.message.toHtmlEscaped()));
    }
    m_issues->setText(lines.join(QStringLiteral("<br>")));
    m_issues->setVisible(!lines.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!fstab::hasErrors(list));
    m_refreshing = false;
}

QList<Issue> EntryEditor::issues() const
{
    QList<Issue> out = fstab::validate(m_entry);
    out.append(options::check(m_entry.options, m_entry.type, m_catalogue));
    const auto error = [&out](const QString &field, const QString &msg) {
        out.append({Issue::Severity::Error, field, msg});
    };
    const auto warn = [&out](const QString &field, const QString &msg) {
        out.append({Issue::Severity::Warning, field, msg});
    };
    const auto fam = m_catalogue.family(m_entry.type);
    const bool swap = fam == options::Family::Swap;
    const access::Settings s = access::read(m_entry.options, m_entry.type, m_catalogue);
    const bool system = policy::isSystemEntry(m_entry, m_ctx.rootSpecs);

    if (system && !(m_ctx.me.fullAccess() && m_ctx.systemUnlocked)) {
        error(QStringLiteral("mountPoint"), i18n("This is a system entry (needed to start the computer). Only driveadmins and administrators can change "
                                                 "it, after switching on \"Allow editing system entries\"."));
    }

    const QString &mp = m_entry.mountPoint;
    const bool mpChanged = m_isNew || mp != m_initial.mountPoint;
    if (!swap && mp.startsWith(QLatin1Char('/'))) {
        if (m_ctx.otherMountPoints.contains(mp)) {
            error(QStringLiteral("mountPoint"), i18n("Another entry already mounts something at %1.", mp));
        }
        if (mpChanged) {
            const auto zone = policy::classifyMountPoint(mp);
            switch (zone.zone) {
            case policy::Zone::Safe:
                break;
            case policy::Zone::Override:
                if (!m_ctx.me.fullAccess()) {
                    error(QStringLiteral("mountPoint"),
                          i18n("Only driveadmins and administrators may mount outside /mnt, /media, /srv and home folders. Choose a folder there."));
                } else {
                    warn(QStringLiteral("mountPoint"), i18n("%1 is outside the usual places. Saving only works if the folder is empty or doesn't exist yet.", mp));
                    if (fam == options::Family::Network && !(s.nofail && s.start == Start::WhenOpened)) {
                        warn(QStringLiteral("options"), i18n("For a network share here, \"start even if missing\" and \"mount when opened\" are recommended."));
                    }
                }
                break;
            case policy::Zone::Blocked:
                if (!m_ctx.me.sudo) {
                    error(QStringLiteral("mountPoint"), i18n("%1 is a protected system location. Only administrators can mount there.", mp));
                } else {
                    warn(QStringLiteral("mountPoint"), i18n("%1 is a protected system location. You'll be asked to type the path to confirm.", mp));
                }
                break;
            case policy::Zone::Invalid:
                error(QStringLiteral("mountPoint"), zone.reason);
                break;
            }
            const QDir dir(mp);
            if (dir.exists() && !dir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
                warn(QStringLiteral("mountPoint"), i18n("The folder %1 isn't empty. Its files will be hidden while something is mounted there.", mp));
            }
        }
    }

    if (!system && !s.nofail && s.start == Start::AtBoot) {
        warn(QStringLiteral("options"), fam == options::Family::Network
                 ? i18n("Without \"start even if missing\" (nofail), a server that is down stops the computer at an emergency prompt while starting.")
                 : i18n("Without \"start even if missing\" (nofail), the computer stops at an emergency prompt if this drive is missing while starting."));
    }
    if (fam == options::Family::Network && !s.netdev && !policy::isNetworkType(m_entry.type)) {
        warn(QStringLiteral("options"), i18n("%1 needs the network; \"needs the network\" (_netdev) is recommended.", m_entry.type));
    }
    if (m_entry.type == QLatin1String("ntfs3") && (s.sharing == Sharing::OwnerOnly || s.sharing == Sharing::OthersRead)) {
        // Found by tests/mounttest.cpp on kernel 7.0.
        warn(QStringLiteral("options"), i18n("The ntfs3 driver ignores these permissions for the drive's top folder: every user can create files there. "
                                             "Choose \"ntfs\" (ntfs-3g) as the file system if that matters."));
    }
    QStringList targets;
    for (const auto &b : binds()) {
        if (const QString e = binds::checkSubfolder(b.subfolder); !e.isEmpty()) {
            error(QStringLiteral("binds"), e);
        }
        const auto zone = policy::classifyMountPoint(b.target);
        if (zone.zone == policy::Zone::Invalid) {
            error(QStringLiteral("binds"), i18n("Also show it at %1: %2", b.target, zone.reason));
        } else if (zone.zone == policy::Zone::Blocked && !m_ctx.me.sudo) {
            error(QStringLiteral("binds"), i18n("%1 is a protected system location.", b.target));
        } else if (zone.zone == policy::Zone::Override && !m_ctx.me.fullAccess()) {
            error(QStringLiteral("binds"), i18n("Only driveadmins and administrators may use %1.", b.target));
        }
        if (b.target == m_entry.mountPoint || targets.contains(b.target) || m_ctx.otherMountPoints.contains(b.target)) {
            if (!m_ctx.binds.contains(b) || b.target == m_entry.mountPoint || targets.contains(b.target)) {
                error(QStringLiteral("binds"), i18n("%1 is already used.", b.target));
            }
        }
        targets.append(b.target);
    }
    if (m_entry.type == QLatin1String("none") && !m_entry.options.contains(QStringLiteral("bind")) && !m_entry.options.contains(QStringLiteral("rbind"))) {
        error(QStringLiteral("options"), i18n("A bind mount needs the 'bind' option (Advanced tab)."));
    }
    return out;
}

void EntryEditor::accept()
{
    const access::Settings s = access::read(m_entry.options, m_entry.type, m_catalogue);
    if (m_entry.type == QLatin1String("ntfs3") && (s.sharing == Sharing::OwnerOnly || s.sharing == Sharing::OthersRead)) {
        // Found by tests/mounttest.cpp on kernel 7.0: ntfs3 shows the top folder as 0777.
        const auto answer = KMessageBox::questionTwoActionsCancel(
            this,
            i18n("With the ntfs3 driver, every user can open the drive's top folder and create files in it, whatever permissions are "
                 "chosen here. Folders inside it do follow the permissions.<br><br>"
                 "The ntfs-3g driver applies the permissions to the top folder too, but is slower."),
            i18n("Top Folder Is Open to Everyone"),
            KGuiItem(i18n("Use ntfs-3g Instead"), QStringLiteral("security-high")),
            KGuiItem(i18n("Keep ntfs3"), QStringLiteral("dialog-ok")),
            KStandardGuiItem::cancel(),
            QString(),
            KMessageBox::Dangerous);
        if (answer == KMessageBox::Cancel) {
            return;
        }
        if (answer == KMessageBox::PrimaryAction) {
            typeChanged(QStringLiteral("ntfs"));
            if (fstab::hasErrors(issues())) {
                return; // show what needs fixing instead of closing
            }
        }
    }
    QDialog::accept();
}
