// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountatdialog.h"

#include "mountplan.h"
#include "mountpolicy.h"
#include "options.h"

#include <KColorScheme>
#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

using namespace anchorage;

QString MountAtDialog::suggestedFolder(const drives::Volume &v)
{
    QString base = v.label.isEmpty() ? v.name : v.label;
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9._-]+"));
    base.replace(bad, QStringLiteral("-"));
    while (base.startsWith(QLatin1Char('.')) || base.startsWith(QLatin1Char('-'))) {
        base.remove(0, 1);
    }
    return QStringLiteral("/mnt/") + (base.isEmpty() ? QStringLiteral("drive") : base);
}

MountAtDialog::MountAtDialog(const fstab::Entry &initial, const QString &title, const Snapshot &snapshot, QWidget *parent)
    : QDialog(parent)
    , m_initial(initial)
    , m_snapshot(snapshot)
{
    setObjectName(QStringLiteral("mountAt"));
    setWindowTitle(i18n("Mount %1", title));
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(i18n("Mounts the drive at a folder of your choice until it is unmounted or the computer restarts. "
                                  "/etc/fstab is not changed; use \"Add to fstab…\" to mount it every time."),
                             this);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *form = new QFormLayout;
    auto *device = new QLabel(initial.spec, this);
    device->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    device->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(i18n("Device or share:"), device);

    m_mountPoint = new QLineEdit(initial.mountPoint, this);
    m_mountPoint->setObjectName(QStringLiteral("mountAt.mountPoint"));
    auto *browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Browse…"), this);
    browse->setObjectName(QStringLiteral("mountAt.browse"));
    auto *row = new QHBoxLayout;
    row->addWidget(m_mountPoint, 1);
    row->addWidget(browse);
    form->addRow(i18n("Mount at:"), row);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString start = m_mountPoint->text().isEmpty() ? QStringLiteral("/mnt") : m_mountPoint->text();
        const QString dir = QFileDialog::getExistingDirectory(this, i18n("Choose a Folder"), start);
        if (!dir.isEmpty()) {
            m_mountPoint->setText(dir);
        }
    });

    m_type = new QComboBox(this);
    m_type->setObjectName(QStringLiteral("mountAt.type"));
    m_type->setEditable(true);
    for (const auto &fs : options::Catalogue::builtIn().fileSystems()) {
        if (fs.family != options::Family::Swap && fs.type != QLatin1String("none") && fs.type != QLatin1String("auto")) {
            m_type->addItem(fs.type);
        }
    }
    m_type->setCurrentText(initial.type);
    form->addRow(i18n("File system:"), m_type);

    m_readOnly = new QCheckBox(i18n("Read-only"), this);
    m_readOnly->setObjectName(QStringLiteral("mountAt.readOnly"));
    m_ownFiles = new QCheckBox(i18n("I own the files (FAT, exFAT, NTFS and SMB have no Linux owners)"), this);
    m_ownFiles->setObjectName(QStringLiteral("mountAt.ownFiles"));
    m_ownFiles->setChecked(true);
    form->addRow(QString(), m_readOnly);
    form->addRow(QString(), m_ownFiles);
    m_options = new QLineEdit(initial.options.join(QLatin1Char(',')), this);
    m_options->setObjectName(QStringLiteral("mountAt.options"));
    m_options->setPlaceholderText(i18n("more options, e.g. noatime,nosuid"));
    form->addRow(i18n("Other options:"), m_options);
    v->addLayout(form);

    m_issues = new QLabel(this);
    m_issues->setObjectName(QStringLiteral("mountAt.issues"));
    m_issues->setWordWrap(true);
    m_issues->setTextFormat(Qt::RichText);
    v->addWidget(m_issues);
    v->addStretch(1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(i18n("Mount"));
    m_buttons->button(QDialogButtonBox::Ok)->setIcon(QIcon::fromTheme(QStringLiteral("media-mount")));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(m_buttons);

    for (auto *e : {m_mountPoint, m_options}) {
        connect(e, &QLineEdit::textChanged, this, &MountAtDialog::refresh);
    }
    connect(m_type, &QComboBox::currentTextChanged, this, &MountAtDialog::refresh);
    connect(m_readOnly, &QCheckBox::toggled, this, &MountAtDialog::refresh);
    connect(m_ownFiles, &QCheckBox::toggled, this, &MountAtDialog::refresh);
    refresh();
    resize(560, 360);
}

fstab::Entry MountAtDialog::entry() const
{
    fstab::Entry e = m_initial;
    e.mountPoint = m_mountPoint->text().trimmed();
    e.type = m_type->currentText().trimmed();
    e.options.clear();
    if (m_readOnly->isChecked()) {
        e.options.append(QStringLiteral("ro"));
    }
    const auto model = options::Catalogue::builtIn().ownerModel(e.type);
    if (m_ownFiles->isChecked() && (model == options::OwnerModel::Mask || model == options::OwnerModel::Cifs)) {
        e.options.append(QStringLiteral("uid=%1").arg(m_snapshot.me.uid));
        e.options.append(QStringLiteral("gid=%1").arg(m_snapshot.me.gid));
    }
    for (const QString &o : m_options->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        e.options.append(o.trimmed());
    }
    return e;
}

QStringList MountAtDialog::errors() const
{
    const fstab::Entry e = entry();
    // The helper's own rules, so problems show up before the password prompt. A
    // protected location is fine for administrators: they confirm it by typing the path.
    const auto plan = mounting::planTemporaryMount(e, m_snapshot.me, true, m_snapshot.fstab, m_snapshot.mounts);
    QStringList out = plan.errors;
    for (const auto &i : options::check(e.options, e.type)) {
        if (i.severity == fstab::Issue::Severity::Error) {
            out.append(i.message);
        }
    }
    return out;
}

void MountAtDialog::refresh()
{
    const auto model = options::Catalogue::builtIn().ownerModel(m_type->currentText().trimmed());
    m_ownFiles->setEnabled(model == options::OwnerModel::Mask || model == options::OwnerModel::Cifs);
    const QStringList errs = errors();
    QStringList lines;
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    for (const QString &e : errs) {
        lines.append(QStringLiteral("<span style=\"color:%1\">✖ %2</span>").arg(scheme.foreground(KColorScheme::NegativeText).color().name(), e.toHtmlEscaped()));
    }
    const fstab::Entry e = entry();
    for (const auto &i : options::check(e.options, e.type)) {
        if (i.severity == fstab::Issue::Severity::Warning) {
            lines.append(QStringLiteral("<span style=\"color:%1\">⚠ %2</span>").arg(scheme.foreground(KColorScheme::NeutralText).color().name(), i.message.toHtmlEscaped()));
        }
    }
    if (policy::classifyMountPoint(e.mountPoint).zone == policy::Zone::Blocked && errs.isEmpty()) {
        lines.append(i18n("This is a protected location: you'll be asked to type the path to confirm."));
    }
    m_issues->setText(lines.join(QStringLiteral("<br>")));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(errs.isEmpty());
}
