// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "datasource.h"
#include "fstab.h"
#include "shares.h"

#include <QDialog>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// "Connect to Server…" (SPEC §11): type a server's address, see its SMB shares and NFS
// exports, then add one to fstab or mount it at a folder. Listing runs as the person using
// the app, off the GUI thread.
class ConnectDialog : public QDialog
{
    Q_OBJECT

public:
    struct Choice {
        anchorage::fstab::Entry entry; // spec, type, suggested mount point, default options
        std::optional<anchorage::shares::Login> login; // SMB login typed here (not guest)
        bool guest = false;
        QList<int> nfsVersions; // what the server offers, if known
    };

    ConnectDialog(DataSource *source, bool canMountAt, QWidget *parent = nullptr);

    // Called with the chosen share.
    std::function<void(const Choice &)> onAddToFstab;
    std::function<void(const Choice &)> onMountAt;

    void list(); // also the "List Shares" button

Q_SIGNALS:
    void listed(); // after a listing finished (tests)

private:
    std::optional<Choice> current() const;
    void updateButtons();

    DataSource *m_source;
    QLineEdit *m_address;
    QComboBox *m_protocol;
    QLineEdit *m_user;
    QLineEdit *m_password;
    QLineEdit *m_domain;
    QCheckBox *m_guest;
    QPushButton *m_listButton;
    QLabel *m_status;
    QTreeWidget *m_shares;
    QPushButton *m_add;
    QPushButton *m_mountAt;
    QString m_host;
    QList<int> m_nfsVersions;
    bool m_canMountAt;
};
