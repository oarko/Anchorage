// SPDX-License-Identifier: GPL-3.0-or-later
#include "connectdialog.h"

#include "access.h"
#include "locations.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace anchorage;

namespace
{
constexpr int ProtocolRole = Qt::UserRole + 1;

QString versionsText(const QList<int> &v)
{
    if (v.isEmpty()) {
        return i18n("NFS versions offered: unknown");
    }
    QStringList parts;
    for (int n : v) {
        parts.append(QString::number(n));
    }
    return i18n("Offers NFS %1", parts.join(i18nc("list separator for NFS versions", " and ")));
}
} // namespace

ConnectDialog::ConnectDialog(DataSource *source, bool canMountAt, QWidget *parent)
    : QDialog(parent)
    , m_source(source)
    , m_canMountAt(canMountAt)
{
    setObjectName(QStringLiteral("connect"));
    setWindowTitle(i18n("Connect to Server"));
    auto *v = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_address = new QLineEdit(this);
    m_address->setObjectName(QStringLiteral("connect.address"));
    m_address->setPlaceholderText(i18n("nas.local, 192.168.1.10, smb://server or nfs://server"));
    m_protocol = new QComboBox(this);
    m_protocol->setObjectName(QStringLiteral("connect.protocol"));
    m_protocol->addItem(i18n("Automatic (SMB and NFS)"), 0);
    m_protocol->addItem(i18n("SMB (Windows, Samba, most NAS)"), 1);
    m_protocol->addItem(i18n("NFS (Linux, NAS)"), 2);
    auto *row = new QHBoxLayout;
    row->addWidget(m_address, 1);
    row->addWidget(m_protocol);
    form->addRow(i18n("Server:"), row);
    m_user = new QLineEdit(this);
    m_user->setObjectName(QStringLiteral("connect.user"));
    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("connect.password"));
    m_password->setEchoMode(QLineEdit::Password);
    m_domain = new QLineEdit(this);
    m_domain->setObjectName(QStringLiteral("connect.domain"));
    m_domain->setPlaceholderText(i18n("usually empty"));
    m_guest = new QCheckBox(i18n("Connect as guest (no password)"), this);
    m_guest->setObjectName(QStringLiteral("connect.guest"));
    form->addRow(i18n("SMB user name:"), m_user);
    form->addRow(i18n("Password:"), m_password);
    form->addRow(i18n("Domain or workgroup:"), m_domain);
    form->addRow(QString(), m_guest);
    v->addLayout(form);
    connect(m_guest, &QCheckBox::toggled, this, [this](bool on) {
        m_user->setEnabled(!on);
        m_password->setEnabled(!on);
        m_domain->setEnabled(!on);
    });

    m_listButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("List Shares"), this);
    m_listButton->setObjectName(QStringLiteral("connect.list"));
    m_listButton->setDefault(true);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("connect.status"));
    m_status->setWordWrap(true);
    auto *listRow = new QHBoxLayout;
    listRow->addWidget(m_status, 1);
    listRow->addWidget(m_listButton);
    v->addLayout(listRow);
    connect(m_listButton, &QPushButton::clicked, this, &ConnectDialog::list);
    connect(m_address, &QLineEdit::returnPressed, this, &ConnectDialog::list);

    m_shares = new QTreeWidget(this);
    m_shares->setObjectName(QStringLiteral("connect.shares"));
    m_shares->setHeaderLabels({i18n("Share"), i18n("Description")});
    m_shares->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    v->addWidget(m_shares, 1);
    connect(m_shares, &QTreeWidget::currentItemChanged, this, &ConnectDialog::updateButtons);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_add = buttons->addButton(i18n("Add to fstab…"), QDialogButtonBox::ActionRole);
    m_add->setObjectName(QStringLiteral("connect.add"));
    m_add->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    m_mountAt = buttons->addButton(i18n("Mount At…"), QDialogButtonBox::ActionRole);
    m_mountAt->setObjectName(QStringLiteral("connect.mountAt"));
    m_mountAt->setIcon(QIcon::fromTheme(QStringLiteral("media-mount")));
    m_mountAt->setVisible(canMountAt);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (const auto c = current(); c && onAddToFstab) {
            onAddToFstab(*c);
            accept();
        }
    });
    connect(m_mountAt, &QPushButton::clicked, this, [this] {
        if (const auto c = current(); c && onMountAt) {
            onMountAt(*c);
            accept();
        }
    });
    v->addWidget(buttons);
    updateButtons();
    resize(620, 520);
}

void ConnectDialog::list()
{
    const auto address = shares::parseAddress(m_address->text());
    if (!address) {
        m_status->setText(address.error());
        return;
    }
    int mode = m_protocol->currentData().toInt();
    if (mode == 0 && address->protocolGiven) {
        mode = address->protocol == shares::Protocol::Smb ? 1 : 2;
    }
    m_host = address->host;
    m_shares->clear();
    m_listButton->setEnabled(false);
    m_status->setText(i18n("Asking %1…", m_host));
    shares::Login login{m_user->text().trimmed(), m_password->text(), m_domain->text().trimmed(), m_guest->isChecked()};
    m_source->browse(m_host, mode != 2, mode != 1, login, [this](const Browse &b) {
        m_listButton->setEnabled(true);
        m_nfsVersions = b.nfsVersions;
        QStringList notes;
        if (b.smbTried) {
            if (b.smb.ok()) {
                auto *cat = new QTreeWidgetItem(m_shares, {i18n("SMB shares")});
                cat->setFlags(Qt::ItemIsEnabled);
                for (const auto &s : b.smb.shares) {
                    auto *it = new QTreeWidgetItem(cat, {s.name, s.comment});
                    it->setIcon(0, QIcon::fromTheme(QStringLiteral("folder-remote")));
                    it->setData(0, ProtocolRole, int(shares::Protocol::Smb));
                }
                if (b.smb.shares.isEmpty()) {
                    notes.append(i18n("SMB: no shares visible to this user."));
                }
            } else {
                notes.append(QStringLiteral("SMB: ") + b.smb.message);
                if (b.smb.error == shares::SmbError::NeedsLogin) {
                    m_user->setFocus();
                }
            }
        }
        if (b.nfsTried) {
            if (!b.nfs.isEmpty()) {
                auto *cat = new QTreeWidgetItem(m_shares, {i18n("NFS exports"), versionsText(b.nfsVersions)});
                cat->setFlags(Qt::ItemIsEnabled);
                for (const auto &s : b.nfs) {
                    auto *it = new QTreeWidgetItem(cat, {s.name, i18n("for: %1", s.comment)});
                    it->setIcon(0, QIcon::fromTheme(QStringLiteral("folder-remote")));
                    it->setData(0, ProtocolRole, int(shares::Protocol::Nfs));
                }
            } else {
                notes.append(QStringLiteral("NFS: ") + (b.nfsError.isEmpty() ? i18n("no exports listed.") : b.nfsError));
            }
        }
        m_shares->expandAll();
        m_status->setText(notes.isEmpty() ? i18n("Choose a share.") : notes.join(QLatin1Char('\n')));
        updateButtons();
        Q_EMIT listed();
    });
}

std::optional<ConnectDialog::Choice> ConnectDialog::current() const
{
    const QTreeWidgetItem *it = m_shares->currentItem();
    if (!it || !it->data(0, ProtocolRole).isValid()) {
        return std::nullopt;
    }
    const auto protocol = shares::Protocol(it->data(0, ProtocolRole).toInt());
    const shares::Share share{it->text(0), QString()};
    Choice c;
    c.entry.spec = shares::deviceFor(m_host, protocol, share);
    // NFS: "nfs" lets client and server agree on the newest version (SPEC §11).
    c.entry.type = protocol == shares::Protocol::Smb ? QStringLiteral("cifs") : QStringLiteral("nfs");
    c.entry.mountPoint = QStringLiteral("/mnt/") + locations::folderName(share.name.section(QLatin1Char('/'), -1));
    c.entry.options = access::apply({}, access::defaultsFor(c.entry.type), c.entry.type);
    if (protocol == shares::Protocol::Smb) {
        c.guest = m_guest->isChecked();
        if (!c.guest && !m_user->text().trimmed().isEmpty()) {
            c.login = shares::Login{m_user->text().trimmed(), m_password->text(), m_domain->text().trimmed(), false};
        }
    } else {
        c.nfsVersions = m_nfsVersions;
    }
    return c;
}

void ConnectDialog::updateButtons()
{
    const bool chosen = current().has_value();
    m_add->setEnabled(chosen);
    m_mountAt->setEnabled(chosen && m_canMountAt);
}
