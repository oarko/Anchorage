// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the real main window from fixture data (tests/data) and checks what it shows.
// Set ANCHORAGE_SCREENSHOTS=<dir> to save screenshots of each view for review.
#include "datasource.h"
#include "entryeditor.h"
#include "fstabservice.h"
#include "inventorymodel.h"
#include "mainwindow.h"
#include "placespanel.h"
#include "applydialogs.h"
#include "connectdialog.h"
#include "mountatdialog.h"
#include <QClipboard>
#include "backupspage.h"
#include <KActionCollection>

#include <KCategorizedView>
#include <KAboutData>
#include <KLocalizedString>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QPushButton>
#include <QRadioButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextEdit>
#include <QSpinBox>
#include <QScrollBar>
#include <QApplication>
#include <QTreeWidget>
#include <KMessageWidget>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>

using namespace anchorage;

namespace
{
QByteArray readData(const QString &name)
{
    QFile f(QStringLiteral(TEST_DATA_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("cannot open test data %s", qPrintable(name));
    }
    return f.readAll();
}

class FixtureSource : public DataSource
{
public:
    explicit FixtureSource(bool admin, const QByteArray &extraLines = {})
        : m_admin(admin)
        , m_fstab(readData(QStringLiteral("kubuntu-default.fstab")) + extraLines)
    {
    }
    Snapshot load() override
    {
        Snapshot s;
        s.fstab = m_fstab;
        s.volumes = *drives::parseLsblk(readData(QStringLiteral("lsblk-desktop.json")));
        s.mounts = mounts::parseMountInfo(readData(QStringLiteral("mountinfo-desktop.txt")));
        s.mounts.erase(std::remove_if(s.mounts.begin(), s.mounts.end(),
                                      [this](const mounts::Mount &m) {
                                          return unmounted.contains(m.target);
                                      }),
                       s.mounts.end());
        s.mounts += mountedNow;
        s.backups = m_backupNames;
        s.me.name = QStringLiteral("oarko");
        s.me.sudo = m_admin;
        s.me.driveAdmin = m_admin;
        s.me.uid = 1000;
        s.me.gid = 1000;
        s.accounts = {{1000, 1000, QStringLiteral("oarko"), QStringLiteral("Oarko")}, {1001, 1001, QStringLiteral("anna"), QString()}};
        s.settings = settings;
        return s;
    }
    QByteArray readBackup(const QString &name) override { return m_backups.value(name); }

    void restoreBackup(const requests::Restore &request, QWidget *, Done done) override
    {
        restores.append(request);
        client::Reply r;
        r.status = nextStatus;
        if (r.ok()) {
            const QString saved = QStringLiteral("fstab-2026-11-26T09-00-00Z");
            m_backups.insert(saved, m_fstab);
            m_backupNames.prepend(saved);
            m_fstab = m_backups.value(request.backupName);
            r.data.insert(QStringLiteral("backup"), saved);
        } else {
            r.message = QStringLiteral("refused for the test");
        }
        done(r);
    }
    void writeSettings(const settings::Settings &s, QWidget *, Done done) override
    {
        client::Reply r;
        r.status = nextStatus;
        if (r.ok()) {
            settings = s;
        }
        settingsWrites.append(s);
        done(r);
    }

    // Behaves like the helper: refuses stale baselines, otherwise "writes" the file.
    void writeFstab(const requests::WriteFstab &request, QWidget *, Done done) override
    {
        writes.append(request);
        calls.append(QStringLiteral("writefstab"));
        client::Reply r;
        r.status = nextStatus;
        if (r.ok() && request.baselineFingerprint != service::fingerprint(m_fstab)) {
            r.status = client::Reply::Status::Failed;
            r.message = QStringLiteral("changed meanwhile");
        }
        if (r.ok()) {
            const QString saved = QStringLiteral("fstab-2026-11-25T10-00-0%1Z").arg(writes.size() % 10);
            m_backups.insert(saved, m_fstab);
            m_backupNames.prepend(saved);
            m_fstab = request.content;
            r.data.insert(QStringLiteral("backup"), saved);
        } else if (r.message.isEmpty()) {
            r.message = QStringLiteral("refused for the test");
        }
        done(r);
    }
    QList<verify::Finding> verify(const QByteArray &) override { return findings; }

    // Mounting: recorded, and reflected in what load() reports.
    void mountEntry(const QString &mountPoint, QWidget *, Done done) override
    {
        calls.append(QStringLiteral("mount:") + mountPoint);
        client::Reply r;
        if (failMount.contains(mountPoint)) {
            r.message = QStringLiteral("mount: %1: can't find the device.").arg(mountPoint);
        } else {
            r.status = client::Reply::Status::Ok;
            setMounted(mountPoint, true);
        }
        done(r);
    }
    void mountTemporary(const requests::TemporaryMount &request, QWidget *, Done done) override
    {
        calls.append(QStringLiteral("mount-at:") + request.entry.mountPoint);
        temporary.append(request);
        client::Reply r;
        r.status = client::Reply::Status::Ok;
        setMounted(request.entry.mountPoint, true);
        done(r);
    }
    void unmount(const requests::Unmount &request, QWidget *, Done done) override
    {
        calls.append(QStringLiteral("unmount:%1%2").arg(request.mountPoint, request.confirmSystem ? QStringLiteral(" (confirmed)") : QString()));
        client::Reply r;
        if (busyTimes > 0) {
            --busyTimes;
            r.message = QStringLiteral("%1 is in use by: dolphin, bash.").arg(request.mountPoint);
            r.data.insert(QStringLiteral("processes"), QStringList{QStringLiteral("bash"), QStringLiteral("dolphin")});
        } else {
            r.status = client::Reply::Status::Ok;
            setMounted(request.mountPoint, false);
        }
        done(r);
    }
    void writeCredentials(const requests::Credentials &request, QWidget *, Done done) override
    {
        savedLogins.append(request);
        calls.append(QStringLiteral("savelogin:") + request.server);
        client::Reply r;
        r.status = client::Reply::Status::Ok;
        r.data.insert(QStringLiteral("path"), QStringLiteral("/etc/anchorage/credentials/oarko-%1-%2").arg(request.server, request.user));
        done(r);
    }
    QList<requests::Credentials> savedLogins;

    // Network shares: a pretend NAS.
    void browse(const QString &host, bool smb, bool nfs, const shares::Login &login, BrowseDone done) override
    {
        calls.append(QStringLiteral("browse:%1 smb=%2 nfs=%3 user=%4").arg(host).arg(smb).arg(nfs).arg(login.user));
        Browse b;
        if (smb) {
            b.smbTried = true;
            if (login.user.isEmpty() && !login.guest) {
                b.smb.error = shares::SmbError::NeedsLogin;
                b.smb.message = QStringLiteral("%1 needs a user name and password.").arg(host);
            } else {
                b.smb.shares = {{QStringLiteral("test_share"), QString()}, {QStringLiteral("Media"), QStringLiteral("films")}};
            }
        }
        if (nfs) {
            b.nfsTried = true;
            b.nfs = {{QStringLiteral("/mnt/NAS_Pool/TVshows"), QStringLiteral("*")}};
            b.nfsVersions = {4, 3};
        }
        done(b);
    }
    bool toolInstalled(clienttools::Protocol p) override { return !missingTools.contains(int(p)); }
    QString installCommand(clienttools::Protocol p) override { return p == clienttools::Protocol::Smb ? QStringLiteral("sudo apt install cifs-utils") : QStringLiteral("sudo apt install nfs-common"); }
    bool canInstall() override { return packageKit; }
    void installTool(clienttools::Protocol p, InstallDone done) override
    {
        calls.append(QStringLiteral("install:%1").arg(int(p)));
        missingTools.removeAll(int(p));
        done(true, false, QString());
    }
    QList<int> missingTools;
    bool packageKit = true;
    void userMount(const QString &mountPoint, bool mount, Done done) override
    {
        calls.append((mount ? QStringLiteral("user-mount:") : QStringLiteral("user-umount:")) + mountPoint);
        client::Reply r;
        r.status = client::Reply::Status::Ok;
        setMounted(mountPoint, mount);
        done(r);
    }
    void setMounted(const QString &target, bool on)
    {
        if (on) {
            unmounted.removeAll(target);
            mountedNow.append({QStringLiteral("/dev/test"), target, QStringLiteral("ext4"), QStringLiteral("/"), QStringLiteral("rw")});
        } else {
            unmounted.append(target);
            mountedNow.erase(std::remove_if(mountedNow.begin(), mountedNow.end(),
                                            [&target](const mounts::Mount &m) {
                                                return m.target == target;
                                            }),
                             mountedNow.end());
        }
    }
    QStringList calls;
    QStringList failMount;
    int busyTimes = 0;
    QList<requests::TemporaryMount> temporary;
    QList<mounts::Mount> mountedNow;
    QStringList unmounted;

    void changeOnDisk(const QByteArray &content)
    {
        m_fstab = content;
        Q_EMIT changed();
    }
    QByteArray onDisk() const { return m_fstab; }

    QList<requests::WriteFstab> writes;
    QList<requests::Restore> restores;
    QList<settings::Settings> settingsWrites;
    settings::Settings settings;
    client::Reply::Status nextStatus = client::Reply::Status::Ok;
    QList<verify::Finding> findings;

private:
    bool m_admin;
    QByteArray m_fstab;
    // Newest first. The newest differs from the current file in every way the view shows:
    // a changed line, a line only in the file now, a line only in the backup.
    QStringList m_backupNames = {QStringLiteral("fstab-2026-11-24T23-10-05Z"), QStringLiteral("fstab-2026-11-20T17-34-10Z")};
    QHash<QString, QByteArray> m_backups = {
        {QStringLiteral("fstab-2026-11-24T23-10-05Z"),
         readData(QStringLiteral("kubuntu-default.fstab"))
             .replace("subvol=/@,defaults,noatime,autodefrag", "subvol=/@,defaults,noatime")
             .replace("tmpfs                                     /tmp           tmpfs   defaults,noatime,mode=1777 0 0\n", "")
             + "LABEL=Old  /mnt/old  ext4  nofail  0 0\n"},
        {QStringLiteral("fstab-2026-11-20T17-34-10Z"), readData(QStringLiteral("kubuntu-default.fstab"))},
    };
};

// Clicks the button labelled `text` in the next message box that appears.
void clickInNextDialog(const QString &text, bool *clicked)
{
    *clicked = false;
    auto *timer = new QTimer;
    timer->setInterval(50);
    auto tries = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, [timer, text, clicked, tries] {
        // Only the dialog on top: the editor behind a message box has buttons too.
        if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()); dlg && dlg->isVisible()) {
            for (auto *b : dlg->findChildren<QPushButton *>()) {
                if (b->text().remove(QLatin1Char('&')) == text) {
                    *clicked = true;
                    timer->stop();
                    timer->deleteLater();
                    b->click();
                    return;
                }
            }
        }
        if (++*tries > 100) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

int visibleTiles(MainWindow &w)
{
    return w.findChild<KCategorizedView *>(QStringLiteral("tiles"))->model()->rowCount();
}

void selectPlace(MainWindow &w, PlacesPanel::Place p)
{
    w.findChild<PlacesPanel *>(QStringLiteral("places"))->select(p);
    QCoreApplication::processEvents();
}

void screenshot(QWidget &w, const QString &name)
{
    const QString dir = qEnvironmentVariable("ANCHORAGE_SCREENSHOTS");
    if (!dir.isEmpty()) {
        QCoreApplication::processEvents();
        w.grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
    }
}

const QByteArray dataLine = "UUID=3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e /mnt/games btrfs noatime,nofail 0 0\n";

// Opens the editor through `open` and returns it (the window emits editorOpened).
EntryEditor *openEditor(MainWindow &w, const std::function<void()> &open)
{
    EntryEditor *editor = nullptr;
    auto c = QObject::connect(&w, &MainWindow::editorOpened, [&editor](QDialog *d) {
        editor = qobject_cast<EntryEditor *>(d);
    });
    open();
    QObject::disconnect(c);
    if (editor) {
        (void)QTest::qWaitForWindowExposed(editor);
    }
    return editor;
}

void selectTile(MainWindow &w, const QString &title)
{
    auto *tiles = w.findChild<KCategorizedView *>(QStringLiteral("tiles"));
    for (int i = 0; i < tiles->model()->rowCount(); ++i) {
        const QModelIndex idx = tiles->model()->index(i, 0);
        if (idx.data(Qt::DisplayRole).toString() == title) {
            tiles->setCurrentIndex(idx);
            QCoreApplication::processEvents();
            return;
        }
    }
    QFAIL(qPrintable(QStringLiteral("no tile ") + title));
}

QTreeWidgetItem *advancedRow(EntryEditor *e, const QString &name)
{
    auto *table = e->findChild<QTreeWidget *>(QStringLiteral("advanced.table"));
    for (int i = 0; i < table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *cat = table->topLevelItem(i);
        for (int j = 0; cat && j < cat->childCount(); ++j) {
            QTreeWidgetItem *row = cat->child(j);
            if (row && row->text(0) == name) {
                return row;
            }
        }
    }
    return nullptr;
}

QPushButton *okButton(EntryEditor *e)
{
    return e->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
}

void typeInto(QLineEdit *edit, const QString &text)
{
    edit->selectAll();
    QTest::keyClicks(edit, text);
    QCoreApplication::processEvents();
}
} // namespace

class GuiTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(anchorage);
        KLocalizedString::setApplicationDomain("anchorage");
        // Same component name as the real app, so the window finds anchorageui.rc.
        KAboutData::setApplicationData(KAboutData(QStringLiteral("anchorage"), QStringLiteral("Anchorage"), QStringLiteral("test")));
    }

    // Message boxes closed during a test are deleted later; do it before the next test.
    void cleanup()
    {
        for (int i = 0; i < 3; ++i) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }
    }

    void adminSeesEverything()
    {
        MainWindow w(new FixtureSource(true));
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        // fstab: 6 entries; drives not in fstab: Lingames, Windows EFI, 2 NTFS; mounted shares: 2
        QCOMPARE(visibleTiles(w), 12);
        screenshot(w, QStringLiteral("all-admin"));

        selectPlace(w, PlacesPanel::Place::System);
        QCOMPARE(visibleTiles(w), 6);
        selectPlace(w, PlacesPanel::Place::Network);
        QCOMPARE(visibleTiles(w), 2);
        selectPlace(w, PlacesPanel::Place::Drives);
        QCOMPARE(visibleTiles(w), 10);
        selectPlace(w, PlacesPanel::Place::Backups);
        screenshot(w, QStringLiteral("backups"));
    }

    void standardUserDoesNotSeeSystemEntries()
    {
        MainWindow w(new FixtureSource(false));
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(visibleTiles(w), 6);
        auto *places = w.findChild<PlacesPanel *>(QStringLiteral("places"));
        for (int i = 0; i < places->count(); ++i) {
            if (places->item(i)->text().startsWith(PlacesPanel::name(PlacesPanel::Place::System))) {
                QVERIFY(places->item(i)->isHidden());
            }
        }
        screenshot(w, QStringLiteral("all-standard"));
    }

    void searchAndSelection()
    {
        MainWindow w(new FixtureSource(true));
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *search = w.findChild<QLineEdit *>(QStringLiteral("search"));
        search->setText(QStringLiteral("linga"));
        QCoreApplication::processEvents();
        QCOMPARE(visibleTiles(w), 1);
        auto *tiles = w.findChild<KCategorizedView *>(QStringLiteral("tiles"));
        tiles->setCurrentIndex(tiles->model()->index(0, 0));
        QCoreApplication::processEvents();
        QCOMPARE(w.findChild<QLabel *>(QStringLiteral("info.title"))->text(), QStringLiteral("Lingames"));
        screenshot(w, QStringLiteral("selected-lingames"));

        search->clear();
        QCoreApplication::processEvents();
        QCOMPARE(visibleTiles(w), 12);
        QCOMPARE(w.findChild<QLabel *>(QStringLiteral("info.title"))->text(), QStringLiteral("Lingames")); // selection kept
    }

    void addDriveToFstab()
    {
        MainWindow w(new FixtureSource(true));
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("Lingames"));
        QCOMPARE(w.findChild<QPushButton *>(QStringLiteral("info.entry_edit"))->text(), QStringLiteral("Add to fstab…"));

        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        QVERIFY(e);
        QCOMPARE(e->entry().spec, QStringLiteral("UUID=3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e"));
        QCOMPARE(e->entry().type, QStringLiteral("btrfs"));
        // Mounted by Dolphin right now: the default is where it is (SPEC §8), with others offered.
        QCOMPARE(e->entry().mountPoint, QStringLiteral("/run/media/oarko/Lingames"));
        auto *box = e->findChild<QComboBox *>(QStringLiteral("editor.mountPointBox"));
        QStringList offered;
        for (int i = 0; i < box->count(); ++i) {
            offered.append(box->itemText(i));
        }
        QCOMPARE(offered.mid(0, 3), QStringList({QStringLiteral("/run/media/oarko/Lingames"), QStringLiteral("/mnt/Lingames"), QStringLiteral("/media/Lingames")}));
        QVERIFY(e->findChild<QPushButton *>(QStringLiteral("editor.browse")));
        QVERIFY(!e->findChild<QCheckBox *>(QStringLiteral("editor.keepOld"))->isVisible()); // not moving
        // Choose /mnt/Lingames instead: offered to keep the old place reachable too.
        box->setCurrentIndex(1);
        Q_EMIT box->activated(1);
        QCOMPARE(e->entry().mountPoint, QStringLiteral("/mnt/Lingames"));
        auto *keepOld = e->findChild<QCheckBox *>(QStringLiteral("editor.keepOld"));
        QVERIFY(keepOld->isVisible());
        QVERIFY(keepOld->text().contains(QLatin1String("/run/media/oarko/Lingames")));
        QCOMPARE(e->entry().options, QStringList{QStringLiteral("nofail")}); // safe default for new drives
        QVERIFY(e->findChild<QCheckBox *>(QStringLiteral("access.nofail"))->isChecked());
        screenshot(*e, QStringLiteral("editor-basic"));

        // Identify by label instead of UUID.
        auto *identify = e->findChild<QComboBox *>(QStringLiteral("editor.identify"));
        identify->setCurrentIndex(1);
        Q_EMIT identify->activated(1);
        QCOMPARE(e->entry().spec, QStringLiteral("LABEL=Lingames"));

        // Access tab: mount when opened, any user.
        e->findChild<QTabWidget *>(QStringLiteral("editor.tabs"))->setCurrentIndex(1);
        e->findChild<QRadioButton *>(QStringLiteral("access.start.2"))->click();
        e->findChild<QRadioButton *>(QStringLiteral("access.who.1"))->click();
        QCOMPARE(e->entry().options, QStringList({QStringLiteral("nofail"), QStringLiteral("noauto"), QStringLiteral("x-systemd.automount"), QStringLiteral("user")}));
        QVERIFY(!e->findChild<QComboBox *>(QStringLiteral("access.owner"))->isEnabled()); // btrfs keeps its own owners
        screenshot(*e, QStringLiteral("editor-access"));

        // Advanced tab: compression from the btrfs options.
        e->findChild<QTabWidget *>(QStringLiteral("editor.tabs"))->setCurrentIndex(2);
        QTreeWidgetItem *compress = advancedRow(e, QStringLiteral("compress"));
        QVERIFY(compress);
        compress->setText(1, QStringLiteral("zstd:3"));
        QVERIFY(e->entry().options.contains(QStringLiteral("compress=zstd:3")));
        QCOMPARE(compress->checkState(0), Qt::Checked);
        e->findChild<QTreeWidget *>(QStringLiteral("advanced.table"))->setCurrentItem(compress);
        screenshot(*e, QStringLiteral("editor-advanced"));
        QVERIFY(e->findChild<QLabel *>(QStringLiteral("editor.preview"))->text().contains(QLatin1String("compress=zstd:3")));
        QVERIFY(okButton(e)->isEnabled());
        e->accept();
        QCoreApplication::processEvents();

        QVERIFY(w.hasPendingChanges());
        QVERIFY(w.findChild<KMessageWidget *>(QStringLiteral("applyBar"))->isVisibleTo(&w));
        // New lines line up with the columns of the line above.
        const QByteArray expected = "LABEL=Lingames                            /mnt/Lingames  btrfs   nofail,noauto,x-systemd.automount,user,compress=zstd:3 0 0\n";
        // (keepOld wasn't ticked: no bind line)
        QVERIFY2(w.pendingContent().endsWith(expected), w.pendingContent().constData());
        screenshot(w, QStringLiteral("pending"));

        QSignalSpy finished(&w, &MainWindow::applyFinished);
        w.applyChanges();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QVERIFY(!w.hasPendingChanges());
        // After saving, the drive is an fstab entry.
        QCOMPARE(visibleTiles(w), 12);
        selectTile(w, QStringLiteral("Lingames"));
        QCOMPARE(w.findChild<QPushButton *>(QStringLiteral("info.entry_edit"))->text(), QStringLiteral("Edit…"));
    }

    void applySendsBaselineFingerprintAndKeepsChangesOnFailure()
    {
        auto *src = new FixtureSource(true, dataLine);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        const QByteArray original = src->onDisk();
        selectTile(w, QStringLiteral("Lingames"));
        w.removeSelected();
        QVERIFY(w.hasPendingChanges());
        QVERIFY(!w.pendingContent().contains("/mnt/games"));

        src->nextStatus = client::Reply::Status::Cancelled;
        w.applyChanges();
        QCOMPARE(src->writes.size(), 1);
        QCOMPARE(src->writes[0].baselineFingerprint, service::fingerprint(original));
        QVERIFY(src->writes[0].createMountPoints);
        QVERIFY(!src->writes[0].confirmProtected);
        QVERIFY(w.hasPendingChanges()); // cancelled: nothing lost
        QCOMPARE(src->onDisk(), original);

        // Someone else changes the file: Apply is refused until the changes are discarded.
        src->changeOnDisk(original + "tmpfs /mnt/other tmpfs nofail 0 0\n");
        QCoreApplication::processEvents();
        QVERIFY(w.hasPendingChanges());
        QVERIFY(!w.actionCollection()->action(QStringLiteral("changes_apply"))->isEnabled());
        w.discardChanges();
        QVERIFY(!w.hasPendingChanges());
        QVERIFY(w.actionCollection()->action(QStringLiteral("entry_new"))->isEnabled());
    }

    void editKeepsColumnsAndOtherOptions()
    {
        auto *src = new FixtureSource(true, dataLine);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("Lingames"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        QVERIFY(e);
        QCOMPARE(e->entry().mountPoint, QStringLiteral("/mnt/games"));
        e->findChild<QRadioButton *>(QStringLiteral("access.start.1"))->click();
        QCOMPARE(e->entry().options, QStringList({QStringLiteral("noatime"), QStringLiteral("nofail"), QStringLiteral("noauto")}));
        // Unchecking noatime on the Advanced tab.
        advancedRow(e, QStringLiteral("noatime"))->setCheckState(0, Qt::Unchecked);
        QCOMPARE(e->entry().options, QStringList({QStringLiteral("nofail"), QStringLiteral("noauto")}));
        // Choosing an exclusive sibling replaces the other (relatime vs. strictatime).
        advancedRow(e, QStringLiteral("strictatime"))->setCheckState(0, Qt::Checked);
        advancedRow(e, QStringLiteral("relatime"))->setCheckState(0, Qt::Checked);
        QCOMPARE(e->entry().options, QStringList({QStringLiteral("nofail"), QStringLiteral("noauto"), QStringLiteral("relatime")}));
        // An option Anchorage doesn't know goes to "Other options" and is kept.
        auto *other = e->findChild<QLineEdit *>(QStringLiteral("advanced.other"));
        other->setText(QStringLiteral("x-custom=1"));
        Q_EMIT other->editingFinished();
        QVERIFY(e->entry().options.contains(QStringLiteral("x-custom=1")));
        e->accept();
        QCoreApplication::processEvents();
        const QByteArray line = w.pendingContent().split('\n').at(w.pendingContent().split('\n').size() - 2);
        // Same columns as the original line (space-separated, original positions).
        QCOMPARE(line, QByteArray("UUID=3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e /mnt/games btrfs nofail,noauto,relatime,x-custom=1 0 0"));

        // Preview shows exactly one removed and one added line.
        w.previewChanges();
        auto *preview = w.findChild<PreviewDialog *>();
        QVERIFY(preview);
        const QString diff = preview->findChild<QTextBrowser *>(QStringLiteral("preview.diff"))->toPlainText();
        QCOMPARE(diff.count(QLatin1String("\n- ")) + (diff.startsWith(QLatin1String("- ")) ? 1 : 0), 1);
        QCOMPARE(diff.count(QLatin1String("\n+ ")), 1);
        screenshot(*preview, QStringLiteral("preview"));
        preview->close();
    }

    void invalidInputBlocksOk()
    {
        MainWindow w(new FixtureSource(true));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        EntryEditor *e = openEditor(w, [&w] { w.newEntry(); });
        QVERIFY(e);
        QVERIFY(!okButton(e)->isEnabled()); // nothing filled in yet
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.spec")), QStringLiteral("tmpfs"));
        auto *type = e->findChild<QComboBox *>(QStringLiteral("editor.type"));
        type->setCurrentText(QStringLiteral("tmpfs"));
        auto *mp = e->findChild<QLineEdit *>(QStringLiteral("editor.mountPoint"));
        typeInto(mp, QStringLiteral("relative/path"));
        QVERIFY(!okButton(e)->isEnabled());
        typeInto(mp, QStringLiteral("/tmp")); // already used by another entry
        QVERIFY(!okButton(e)->isEnabled());
        QVERIFY(e->findChild<QLabel *>(QStringLiteral("editor.issues"))->text().contains(QLatin1String("Another entry")));
        typeInto(mp, QStringLiteral("/mnt/a#b"));
        QVERIFY(!okButton(e)->isEnabled());
        typeInto(mp, QStringLiteral("/mnt/ram disk"));
        QVERIFY(okButton(e)->isEnabled());
        QVERIFY(e->findChild<QLabel *>(QStringLiteral("editor.preview"))->text().contains(QLatin1String("/mnt/ram\\040disk")));
        // A bad value on the Advanced tab blocks too.
        QTreeWidgetItem *size = advancedRow(e, QStringLiteral("size"));
        size->setText(1, QStringLiteral("lots"));
        QVERIFY(!okButton(e)->isEnabled());
        size->setText(1, QStringLiteral("2G"));
        QVERIFY(okButton(e)->isEnabled());
        // Pasted line breaks can't get into the file, even if the dialog is accepted anyway.
        auto *spec = e->findChild<QLineEdit *>(QStringLiteral("editor.spec"));
        spec->setText(QStringLiteral("tmpfs\nUUID=x / ext4"));
        Q_EMIT spec->textEdited(spec->text());
        QVERIFY(!okButton(e)->isEnabled());
        e->accept();
        QCoreApplication::processEvents();
        QVERIFY(!w.pendingContent().contains("UUID=x"));
    }

    void vfatOwnerAndPermissions()
    {
        MainWindow w(new FixtureSource(true));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        EntryEditor *e = openEditor(w, [&w] { w.newEntry(); });
        QVERIFY(e);
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.spec")), QStringLiteral("UUID=0002-0002"));
        e->findChild<QComboBox *>(QStringLiteral("editor.type"))->setCurrentText(QStringLiteral("vfat"));
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.mountPoint")), QStringLiteral("/mnt/stick"));
        auto *owner = e->findChild<QComboBox *>(QStringLiteral("access.owner"));
        QVERIFY(owner->isEnabled());
        owner->setCurrentIndex(2); // anna
        Q_EMIT owner->activated(2);
        auto *sharing = e->findChild<QComboBox *>(QStringLiteral("access.sharing"));
        sharing->setCurrentIndex(1); // owner only
        Q_EMIT sharing->activated(1);
        QCOMPARE(e->entry().options, QStringList({QStringLiteral("nofail"), QStringLiteral("uid=1001"), QStringLiteral("gid=1001"), QStringLiteral("umask=077")}));
        QCOMPARE(e->entry().pass, 0);
        e->findChild<QTabWidget *>(QStringLiteral("editor.tabs"))->setCurrentIndex(1);
        screenshot(*e, QStringLiteral("editor-access-vfat"));
    }

    void systemEntriesAreLocked()
    {
        MainWindow w(new FixtureSource(true));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectPlace(w, PlacesPanel::Place::System);
        auto *tiles = w.findChild<KCategorizedView *>(QStringLiteral("tiles"));
        tiles->setCurrentIndex(tiles->model()->index(0, 0));
        QCoreApplication::processEvents();
        auto *edit = w.actionCollection()->action(QStringLiteral("entry_edit"));
        QVERIFY(!edit->isEnabled());
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("info.note"))->text().contains(QLatin1String("Allow Editing System Entries")));
        w.setSystemEditing(true);
        QVERIFY(edit->isEnabled());
        QVERIFY(w.findChild<KMessageWidget *>(QStringLiteral("systemBanner"))->isVisibleTo(&w));
        screenshot(w, QStringLiteral("system-unlocked"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        QVERIFY(e);
        QVERIFY(okButton(e)->isEnabled());
        e->reject();
        w.setSystemEditing(false);
        QVERIFY(!edit->isEnabled());
    }

    void standardUserCannotUseOverrideZone()
    {
        MainWindow w(new FixtureSource(false));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(!w.actionCollection()->action(QStringLiteral("allow_system_editing"))->isVisible());
        EntryEditor *e = openEditor(w, [&w] { w.newEntry(); });
        QVERIFY(e);
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.spec")), QStringLiteral("LABEL=data"));
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.mountPoint")), QStringLiteral("/opt/data"));
        QVERIFY(!okButton(e)->isEnabled());
        QVERIFY(e->findChild<QLabel *>(QStringLiteral("editor.issues"))->text().contains(QLatin1String("Only driveadmins")));
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.mountPoint")), QStringLiteral("/mnt/data"));
        QVERIFY(okButton(e)->isEnabled());
    }

    void findmntProblemsAreShownBeforeSaving()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        EntryEditor *e = openEditor(w, [&w] { w.newEntry(); });
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.spec")), QStringLiteral("UUID=feed-beef"));
        e->findChild<QComboBox *>(QStringLiteral("editor.type"))->setCurrentText(QStringLiteral("vfat"));
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.mountPoint")), QStringLiteral("/mnt/x"));
        e->findChild<QCheckBox *>(QStringLiteral("access.nofail"))->click(); // required at boot, no nofail
        e->accept();
        QCoreApplication::processEvents();
        src->findings = {{QStringLiteral("/mnt/x"), true, QStringLiteral("unreachable on boot required source: UUID=feed-beef")},
                         {QStringLiteral("/mnt/x"), true, QStringLiteral("unreachable on boot required target: No such file or directory")}};
        bool asked = false;
        QTimer::singleShot(200, &w, [&asked] {
            for (QWidget *top : QApplication::topLevelWidgets()) {
                if (auto *box = qobject_cast<QDialog *>(top); box && box->isVisible() && box->objectName() != QLatin1String("editor")) {
                    asked = true;
                    box->reject();
                }
            }
        });
        w.applyChanges();
        QVERIFY(asked);
        QVERIFY(src->writes.isEmpty()); // cancelled before the helper was asked
        QVERIFY(w.hasPendingChanges());
    }

    void backupsPageComparesSideBySide()
    {
        MainWindow w(new FixtureSource(true));
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectPlace(w, PlacesPanel::Place::Backups);
        auto *left = w.findChild<QTextEdit *>(QStringLiteral("diff.left"));
        auto *right = w.findChild<QTextEdit *>(QStringLiteral("diff.right"));
        QVERIFY(left && right);
        // Both sides have the same number of lines, so they line up.
        QCOMPARE(left->document()->blockCount(), right->document()->blockCount());
        const QStringList l = left->toPlainText().split(QChar::ParagraphSeparator).join(QLatin1Char('\n')).split(QLatin1Char('\n'));
        const QStringList r = right->toPlainText().split(QChar::ParagraphSeparator).join(QLatin1Char('\n')).split(QLatin1Char('\n'));
        QCOMPARE(l.size(), r.size());
        // Every real line ends with ↵; fillers are blank (no ↵); a real blank line is just its number and ↵.
        int leftFillers = 0;
        int rightFillers = 0;
        for (int i = 0; i < l.size(); ++i) {
            leftFillers += l[i].trimmed().isEmpty() ? 1 : 0;
            rightFillers += r[i].trimmed().isEmpty() ? 1 : 0;
            QVERIFY2(l[i].trimmed().isEmpty() || l[i].endsWith(QStringLiteral("↵")), qPrintable(l[i]));
        }
        QCOMPARE(leftFillers, 1); // opposite LABEL=Old (only in the backup)
        QCOMPARE(rightFillers, 1); // opposite the tmpfs line (only in the file now)
        QVERIFY(r.filter(QStringLiteral("LABEL=Old")).size() == 1);
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("backups.summary"))->text().startsWith(QLatin1String("3 lines differ")));
        // The changed line: the removed characters are underlined on the left.
        bool underlined = false;
        for (QTextBlock b = left->document()->begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                if (it.fragment().charFormat().fontUnderline() && it.fragment().text().contains(QLatin1String("autodefrag"))) {
                    underlined = true;
                }
            }
        }
        QVERIFY(underlined);
        screenshot(w, QStringLiteral("backups"));
        left->horizontalScrollBar()->setValue(left->horizontalScrollBar()->maximum());
        screenshot(w, QStringLiteral("backups-scrolled")); // the right side follows
    }

    void restoreABackup()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        // An unsaved change is discarded first (after asking).
        selectTile(w, QStringLiteral("Lingames"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        e->accept();
        QCoreApplication::processEvents();
        QVERIFY(w.hasPendingChanges());

        selectPlace(w, PlacesPanel::Place::Backups);
        auto *page = w.findChild<BackupsPage *>();
        QCOMPARE(page->selected(), QStringLiteral("fstab-2026-11-24T23-10-05Z"));
        const QByteArray wanted = src->readBackup(page->selected());
        bool discard = false;
        bool restore = false;
        clickInNextDialog(QStringLiteral("Discard and Continue"), &discard);
        QTimer::singleShot(0, [&restore] { clickInNextDialog(QStringLiteral("Restore"), &restore); });
        QSignalSpy finished(&w, &MainWindow::restoreFinished);
        w.findChild<QPushButton *>(QStringLiteral("backups.restore"))->click();
        QVERIFY(discard);
        QVERIFY(restore);
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.at(0).at(0).toBool());
        QVERIFY(!w.hasPendingChanges());
        QCOMPARE(src->restores.size(), 1);
        QCOMPARE(src->restores[0].backupName, QStringLiteral("fstab-2026-11-24T23-10-05Z"));
        QCOMPARE(src->onDisk(), wanted);
        // The file as it was is now the newest backup, and can be restored in turn.
        QCOMPARE(page->selected(), QStringLiteral("fstab-2026-11-24T23-10-05Z"));
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("backups.summary"))->text().contains(QLatin1String("same as the current file")));
        QVERIFY(!w.findChild<QPushButton *>(QStringLiteral("backups.restore"))->isEnabled());
        page->select(QStringLiteral("fstab-2026-11-26T09-00-00Z"));
        QVERIFY(w.findChild<QPushButton *>(QStringLiteral("backups.restore"))->isEnabled());
    }

    void standardUserCannotRestoreSystemChanges()
    {
        // The newest backup changes the "/" entry: a standard user is told before any prompt.
        auto *src = new FixtureSource(false);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectPlace(w, PlacesPanel::Place::Backups);
        bool closed = false;
        clickInNextDialog(QStringLiteral("OK"), &closed);
        w.restoreBackup(QStringLiteral("fstab-2026-11-24T23-10-05Z"));
        QVERIFY(closed);
        QVERIFY(src->restores.isEmpty());
    }

    void settingsAreSavedThroughTheHelper()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QDialog *dlg = nullptr;
        connect(&w, &MainWindow::settingsOpened, this, [&dlg](QDialog *d) { dlg = d; });
        w.showSettings();
        QVERIFY(dlg);
        auto *keep = dlg->findChild<QSpinBox *>(QStringLiteral("settings.keepBackups"));
        QCOMPARE(keep->value(), 10);
        QCOMPARE(keep->minimum(), 1);
        QCOMPARE(keep->maximum(), 100);
        keep->setValue(25);
        auto *ntfs = dlg->findChild<QComboBox *>(QStringLiteral("settings.ntfsDriver"));
        ntfs->setCurrentIndex(ntfs->findData(QStringLiteral("ntfs")));
        screenshot(*dlg, QStringLiteral("settings"));
        QSignalSpy finished(&w, &MainWindow::settingsFinished);
        dlg->accept();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(src->settingsWrites.size(), 1);
        QCOMPARE(src->settings.keepBackups, 25);
        QCOMPARE(src->settings.ntfsDriver, QStringLiteral("ntfs"));

        // The new default driver is used for NTFS drives.
        selectTile(w, QStringLiteral("2.0 TB NTFS partition"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        QCOMPARE(e->entry().type, QStringLiteral("ntfs"));
        e->reject();
    }

    void standardUserSeesSettingsReadOnly()
    {
        MainWindow w(new FixtureSource(false));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QDialog *dlg = nullptr;
        connect(&w, &MainWindow::settingsOpened, this, [&dlg](QDialog *d) { dlg = d; });
        w.showSettings();
        QVERIFY(dlg);
        QVERIFY(!dlg->findChild<QSpinBox *>(QStringLiteral("settings.keepBackups"))->isEnabled());
        QVERIFY(!dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save));
        dlg->reject();
    }

    void ntfs3WithRestrictivePermissionsAsks()
    {
        MainWindow w(new FixtureSource(true));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("2.0 TB NTFS partition"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        QCOMPARE(e->entry().type, QStringLiteral("ntfs3")); // the default setting
        auto *sharing = e->findChild<QComboBox *>(QStringLiteral("access.sharing"));
        sharing->setCurrentIndex(1); // only the owner
        Q_EMIT sharing->activated(1);
        QVERIFY(e->findChild<QLabel *>(QStringLiteral("editor.issues"))->text().contains(QLatin1String("top folder"))); // soft notice

        // Cancel keeps the dialog open and unchanged.
        bool clicked = false;
        clickInNextDialog(QStringLiteral("Cancel"), &clicked);
        e->accept();
        QVERIFY(clicked);
        QVERIFY(e->isVisible());
        QCOMPARE(e->entry().type, QStringLiteral("ntfs3"));

        // "Use ntfs-3g Instead" switches the driver and adds the entry.
        clickInNextDialog(QStringLiteral("Use ntfs-3g Instead"), &clicked);
        e->accept();
        QVERIFY(clicked);
        QCoreApplication::processEvents();
        QVERIFY(w.hasPendingChanges());
        QVERIFY(w.pendingContent().contains(" ntfs "));
        QVERIFY(w.pendingContent().contains("umask=077"));
    }

    void ntfs3WithOpenPermissionsDoesNotAsk()
    {
        MainWindow w(new FixtureSource(true));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("2.0 TB NTFS partition"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        auto *sharing = e->findChild<QComboBox *>(QStringLiteral("access.sharing"));
        sharing->setCurrentIndex(3); // everyone
        Q_EMIT sharing->activated(3);
        e->accept();
        QCoreApplication::processEvents();
        QVERIFY(w.pendingContent().contains(" ntfs3 "));
    }

    void mountAndUnmountAnEntry()
    {
        auto *src = new FixtureSource(true, "UUID=0000000000000003 /mnt/win ntfs3 nofail 0 0\n");
        MainWindow w(src);
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("win"));
        auto *mount = w.findChild<QPushButton *>(QStringLiteral("info.mount"));
        QVERIFY(mount && mount->isVisible());
        QVERIFY(!w.findChild<QPushButton *>(QStringLiteral("info.unmount"))); // not mounted
        screenshot(w, QStringLiteral("mount-button"));
        mount->click();
        QCoreApplication::processEvents();
        QCOMPARE(src->calls, QStringList{QStringLiteral("mount:/mnt/win")});

        // In use the first time: the programs are named, Try Again unmounts.
        selectTile(w, QStringLiteral("win"));
        QVERIFY(w.actionCollection()->action(QStringLiteral("unmount"))->isEnabled());
        src->busyTimes = 1;
        bool retried = false;
        clickInNextDialog(QStringLiteral("Try Again"), &retried);
        QSignalSpy finished(&w, &MainWindow::mountFinished);
        w.unmountSelected();
        QVERIFY(retried);
        QCOMPARE(src->calls.mid(1), QStringList({QStringLiteral("unmount:/mnt/win"), QStringLiteral("unmount:/mnt/win")}));
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.at(0).at(0).toBool());
    }

    void mountWaitsForApply()
    {
        auto *src = new FixtureSource(true, "UUID=0000000000000003 /mnt/win ntfs3 nofail 0 0\n");
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("win"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        e->findChild<QRadioButton *>(QStringLiteral("access.start.1"))->click();
        e->accept();
        QCoreApplication::processEvents();
        selectTile(w, QStringLiteral("win"));
        QVERIFY(!w.actionCollection()->action(QStringLiteral("mount"))->isEnabled());
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("info.note"))->text().contains(QLatin1String("Apply")));
    }

    void anyUserEntriesMountAsThePerson()
    {
        auto *src = new FixtureSource(false, "UUID=0000000000000003 /mnt/win ntfs3 nofail,noauto,users 0 0\n");
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("win"));
        w.mountSelected();
        selectTile(w, QStringLiteral("win"));
        w.unmountSelected();
        QCOMPARE(src->calls, QStringList({QStringLiteral("user-mount:/mnt/win"), QStringLiteral("user-umount:/mnt/win")}));
    }

    void systemUnmountNeedsTypedConfirmation()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectPlace(w, PlacesPanel::Place::System);
        selectTile(w, QStringLiteral("Home"));
        QVERIFY(w.actionCollection()->action(QStringLiteral("unmount"))->isEnabled());
        bool typed = false;
        QTimer::singleShot(100, [&typed] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dlg || dlg->objectName() != QLatin1String("confirmProtected")) {
                return;
            }
            auto *ok = dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            auto *edit = dlg->findChild<QLineEdit *>(QStringLiteral("confirm.path"));
            edit->setText(QStringLiteral("/hom")); // not yet
            typed = !ok->isEnabled();
            edit->setText(QStringLiteral("/home"));
            typed = typed && ok->isEnabled();
            ok->click();
        });
        w.unmountSelected();
        QVERIFY(typed);
        QCOMPARE(src->calls, QStringList{QStringLiteral("unmount:/home (confirmed)")});

        // A standard user can't unmount system entries at all.
        MainWindow w2(new FixtureSource(false));
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        selectTile(w2, QStringLiteral("Lingames")); // mounted by Dolphin, not in fstab
        QVERIFY(!w2.actionCollection()->action(QStringLiteral("unmount"))->isEnabled());
    }

    void mountAtAFolder()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("2.0 TB NTFS partition"));
        QDialog *dlg = nullptr;
        connect(&w, &MainWindow::mountAtOpened, this, [&dlg](QDialog *d) { dlg = d; });
        w.findChild<QPushButton *>(QStringLiteral("info.mount_at"))->click();
        QVERIFY(dlg);
        auto *mp = dlg->findChild<QLineEdit *>(QStringLiteral("mountAt.mountPoint"));
        QCOMPARE(mp->text(), QStringLiteral("/mnt/nvme0n1p3")); // no label: the device name
        QCOMPARE(dlg->findChild<QComboBox *>(QStringLiteral("mountAt.type"))->currentText(), QStringLiteral("ntfs3"));
        QVERIFY(dlg->findChild<QPushButton *>(QStringLiteral("mountAt.browse")));
        screenshot(*dlg, QStringLiteral("mount-at"));
        auto *ok = dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        mp->setText(QStringLiteral("/tmp")); // protected and not allowed
        QVERIFY(!ok->isEnabled());
        mp->setText(QStringLiteral("/mnt/windows"));
        QVERIFY(ok->isEnabled());
        dlg->accept();
        QCoreApplication::processEvents();
        QCOMPARE(src->calls, QStringList{QStringLiteral("mount-at:/mnt/windows")});
        QCOMPARE(src->temporary[0].entry.spec, QStringLiteral("UUID=0000000000000003"));
        QCOMPARE(src->temporary[0].entry.options, QStringList({QStringLiteral("uid=1000"), QStringLiteral("gid=1000")}));

        MainWindow w2(new FixtureSource(false));
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        selectTile(w2, QStringLiteral("2.0 TB NTFS partition"));
        QVERIFY(!w2.actionCollection()->action(QStringLiteral("mount_at"))->isVisible());
    }

    void testMountAfterApply()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        // A new entry mounted when opened: test-mounted, then unmounted again.
        selectTile(w, QStringLiteral("2.0 TB NTFS partition"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        e->findChild<QRadioButton *>(QStringLiteral("access.start.2"))->click();
        e->accept();
        QCoreApplication::processEvents();
        // A drive that isn't connected: not tested.
        e = openEditor(w, [&w] { w.newEntry(); });
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.spec")), QStringLiteral("UUID=feed-beef"));
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.mountPoint")), QStringLiteral("/mnt/away"));
        e->accept();
        QCoreApplication::processEvents();
        QSignalSpy tested(&w, &MainWindow::testMountFinished);
        w.applyChanges();
        QCOMPARE(tested.count(), 1);
        QCOMPARE(tested.at(0).at(0).toInt(), 0);
        QCOMPARE(src->calls, QStringList({QStringLiteral("writefstab"), QStringLiteral("mount:/mnt/nvme0n1p3"), QStringLiteral("unmount:/mnt/nvme0n1p3")}));

        // A failing test mount offers Undo, which restores the backup just made.
        src->calls.clear();
        const QByteArray before = src->onDisk();
        selectTile(w, QStringLiteral("Windows recovery partition"));
        e = openEditor(w, [&w] { w.editSelected(); });
        src->failMount.append(e->entry().mountPoint);
        e->accept();
        QCoreApplication::processEvents();
        bool undone = false;
        clickInNextDialog(QStringLiteral("Undo Changes"), &undone);
        w.applyChanges();
        QVERIFY(undone);
        QCOMPARE(tested.count(), 2);
        QCOMPARE(tested.at(1).at(0).toInt(), 1);
        QCOMPARE(src->restores.size(), 1);
        QCOMPARE(src->onDisk(), before);
    }

    void changedMountedEntryOffersRemount()
    {
        auto *src = new FixtureSource(true, "UUID=0000000000000003 /mnt/win ntfs3 nofail 0 0\n");
        src->setMounted(QStringLiteral("/mnt/win"), true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("win"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        advancedRow(e, QStringLiteral("noatime"))->setCheckState(0, Qt::Checked);
        e->accept();
        QCoreApplication::processEvents();
        w.applyChanges();
        auto *bar = w.findChild<KMessageWidget *>(QStringLiteral("remountBar"));
        QVERIFY(bar->isVisibleTo(&w));
        QCOMPARE(src->calls, QStringList{QStringLiteral("writefstab")}); // mounted entries aren't touched
        src->calls.clear();
        screenshot(w, QStringLiteral("remount-bar"));
        bar->actions().first()->trigger();
        QCOMPARE(src->calls, QStringList({QStringLiteral("unmount:/mnt/win"), QStringLiteral("mount:/mnt/win")}));
        QVERIFY(!bar->isVisibleTo(&w));
    }

    void bindsAreSavedWithTheDrive()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("Lingames"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        auto *box = e->findChild<QComboBox *>(QStringLiteral("editor.mountPointBox"));
        box->setCurrentIndex(1); // /mnt/Lingames
        Q_EMIT box->activated(1);
        e->findChild<QCheckBox *>(QStringLiteral("editor.keepOld"))->setChecked(true);
        e->addBind({QStringLiteral("SteamLibrary"), QStringLiteral("/home/oarko/Games")});
        QCOMPARE(e->findChild<QTreeWidget *>(QStringLiteral("editor.binds"))->topLevelItemCount(), 1);
        const QString preview = e->findChild<QLabel *>(QStringLiteral("editor.preview"))->text();
        QVERIFY2(preview.contains(QLatin1String("/mnt/Lingames  /run/media/oarko/Lingames  none  bind,nofail,x-systemd.requires=/mnt/Lingames,x-systemd.requires-mounts-for=/mnt/Lingames")),
                 qPrintable(preview));
        screenshot(*e, QStringLiteral("editor-binds"));
        // A bind place that isn't a usable path is refused right away (protected locations
        // are allowed for administrators, who confirm them when applying).
        e->addBind({QString(), QStringLiteral("games")});
        QVERIFY(!okButton(e)->isEnabled());
        e->removeBind(1);
        QVERIFY(okButton(e)->isEnabled());
        e->accept();
        QCoreApplication::processEvents();
        const QByteArray out = w.pendingContent();
        // New lines line up with the columns above (spacing aside, these are the lines).
        const auto has = [](const QByteArray &file, const QByteArray &line) {
            for (const QByteArray &l : file.split('\n')) {
                if (l.simplified() == line) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(has(out, "/mnt/Lingames/SteamLibrary /home/oarko/Games none bind,nofail,x-systemd.requires=/mnt/Lingames,x-systemd.requires-mounts-for=/mnt/Lingames 0 0"));
        QVERIFY(has(out, "/mnt/Lingames /run/media/oarko/Lingames none bind,nofail,x-systemd.requires=/mnt/Lingames,x-systemd.requires-mounts-for=/mnt/Lingames 0 0"));

        // The binds show up as their own tiles.
        selectTile(w, QStringLiteral("Games"));
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("info.title"))->text() == QStringLiteral("Games"));

        // Editing the drive again: its binds are listed, and they move with the drive.
        selectTile(w, QStringLiteral("Lingames"));
        e = openEditor(w, [&w] { w.editSelected(); });
        QCOMPARE(e->binds().size(), 2);
        typeInto(e->findChild<QLineEdit *>(QStringLiteral("editor.mountPoint")), QStringLiteral("/srv/Lingames"));
        e->accept();
        QCoreApplication::processEvents();
        const QByteArray moved = w.pendingContent();
        QVERIFY(has(moved, "/srv/Lingames/SteamLibrary /home/oarko/Games none bind,nofail,x-systemd.requires=/srv/Lingames,x-systemd.requires-mounts-for=/srv/Lingames 0 0"));
        QVERIFY(!moved.contains("=/mnt/Lingames"));
        QVERIFY(has(moved, "/srv/Lingames /run/media/oarko/Lingames none bind,nofail,x-systemd.requires=/srv/Lingames,x-systemd.requires-mounts-for=/srv/Lingames 0 0"));

        // Removing a bind in the editor removes only its line.
        e = openEditor(w, [&w] { w.editSelected(); });
        e->removeBind(0);
        e->accept();
        QCoreApplication::processEvents();
        QVERIFY(!w.pendingContent().contains("/home/oarko/Games"));
        QVERIFY(w.pendingContent().contains("/run/media/oarko/Lingames"));

        // Saved, the drive is test-mounted but its binds are not.
        selectTile(w, QStringLiteral("Lingames"));
        w.applyChanges();
        QVERIFY(!src->calls.join(QLatin1Char(' ')).contains(QLatin1String("/run/media/oarko/Lingames")));
    }

    void removingADriveRemovesItsBinds()
    {
        auto *src = new FixtureSource(true,
                                      "UUID=3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e /mnt/games btrfs nofail 0 0\n"
                                      "/mnt/games /run/media/oarko/Lingames none bind,nofail,x-systemd.requires-mounts-for=/mnt/games 0 0\n"
                                      "/mnt/games/Steam /home/oarko/Steam none bind 0 0\n"
                                      "/srv/other /srv/elsewhere none bind 0 0\n");
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("Lingames"));
        QCOMPARE(w.findChild<QLabel *>(QStringLiteral("info.title"))->text(), QStringLiteral("Lingames"));

        // Cancel: nothing changes.
        bool clicked = false;
        clickInNextDialog(QStringLiteral("Cancel"), &clicked);
        w.removeSelected();
        QVERIFY(clicked);
        QVERIFY(!w.hasPendingChanges());

        // Remove All: the drive and both places that show it; the unrelated bind stays.
        clickInNextDialog(QStringLiteral("Remove All"), &clicked);
        w.removeSelected();
        QVERIFY(clicked);
        const QByteArray out = w.pendingContent();
        QVERIFY(!out.contains("/mnt/games"));
        QVERIFY(!out.contains("/run/media/oarko/Lingames"));
        QVERIFY(out.contains("/srv/other /srv/elsewhere none bind 0 0\n"));
        QVERIFY(out.contains("tmpfs                                     /tmp")); // the rest is untouched
    }

    void orphanedBindsAreFlagged()
    {
        auto *src = new FixtureSource(true,
                                      "/mnt/Lingames /home/oarko/games none bind,nofail,x-systemd.requires=/mnt/Lingames,x-systemd.requires-mounts-for=/mnt/Lingames 0 0\n"
                                      "/mnt/Lingames /mnt/games none bind,nofail,x-systemd.requires-mounts-for=/mnt/Lingames 0 0\n");
        MainWindow w(src);
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *banner = w.findChild<KMessageWidget *>(QStringLiteral("banner"));
        QVERIFY(banner->isVisibleTo(&w));
        QVERIFY(banner->text().contains(QLatin1String("2 places show a drive that is no longer in /etc/fstab")));
        selectTile(w, QStringLiteral("games"));
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("info.title"))->text() == QStringLiteral("games"));
        screenshot(w, QStringLiteral("orphaned-binds"));
        QVERIFY(!w.actionCollection()->action(QStringLiteral("mount"))->isEnabled());
        // The editor says why it won't be mounted.
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        QVERIFY(e->findChild<QLabel *>(QStringLiteral("editor.issues"))->text().contains(QLatin1String("no entry in /etc/fstab mounts")));
        e->reject();
        // Remove Them: both lines go, nothing else.
        w.findChild<QAction *>(QStringLiteral("removeOrphans"))->trigger();
        QVERIFY(w.hasPendingChanges());
        QVERIFY(!w.pendingContent().contains("/mnt/Lingames"));
        QVERIFY(!banner->isVisibleTo(&w));
    }

    void typedPasswordsAreMovedForAdminsOnly()
    {
        const QByteArray lines = "//nas.local/Photos /mnt/photos cifs username=anna,password=s3cret,nofail 0 0\n"
                                 "//nas.local/Music /mnt/music cifs password=nouser,nofail 0 0\n";
        // A standard user isn't told.
        {
            MainWindow w(new FixtureSource(false, lines));
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            QVERIFY(!w.findChild<KMessageWidget *>(QStringLiteral("banner"))->text().contains(QLatin1String("password")));
            QVERIFY(!w.findChild<QAction *>(QStringLiteral("movePasswords"))->isVisible());
        }
        auto *src = new FixtureSource(true, lines);
        MainWindow w(src);
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *banner = w.findChild<KMessageWidget *>(QStringLiteral("banner"));
        QVERIFY(banner->text().contains(QLatin1String("2 entries store a password")));
        screenshot(w, QStringLiteral("typed-passwords"));
        bool ok = false;
        clickInNextDialog(QStringLiteral("OK"), &ok);
        QSignalSpy moved(&w, &MainWindow::passwordsMoved);
        w.findChild<QAction *>(QStringLiteral("movePasswords"))->trigger();
        QVERIFY(ok);
        QCOMPARE(moved.count(), 1);
        QCOMPARE(moved.at(0).at(0).toInt(), 1); // the one without a user name stays
        QCOMPARE(src->savedLogins.size(), 1);
        QCOMPARE(src->savedLogins[0].password, QStringLiteral("s3cret"));
        const QByteArray out = w.pendingContent();
        QVERIFY(!out.contains("s3cret"));
        QVERIFY(out.contains("//nas.local/Photos /mnt/photos cifs credentials=/etc/anchorage/credentials/oarko-nas.local-anna,nofail 0 0"));
        QVERIFY(out.contains("password=nouser"));
    }

    void connectToServerAndAddAnSmbShare()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QDialog *dlg = nullptr;
        connect(&w, &MainWindow::connectOpened, this, [&dlg](QDialog *d) { dlg = d; });
        w.actionCollection()->action(QStringLiteral("connect_server"))->trigger();
        QVERIFY(dlg);
        auto *connectDlg = qobject_cast<ConnectDialog *>(dlg);
        dlg->findChild<QLineEdit *>(QStringLiteral("connect.address"))->setText(QStringLiteral("smb://nas.example"));
        // Without a login the NAS says so.
        connectDlg->list();
        QVERIFY(dlg->findChild<QLabel *>(QStringLiteral("connect.status"))->text().contains(QLatin1String("needs a user name")));
        dlg->findChild<QLineEdit *>(QStringLiteral("connect.user"))->setText(QStringLiteral("test"));
        dlg->findChild<QLineEdit *>(QStringLiteral("connect.password"))->setText(QStringLiteral("pa ss"));
        connectDlg->list();
        auto *tree = dlg->findChild<QTreeWidget *>(QStringLiteral("connect.shares"));
        QCOMPARE(tree->topLevelItemCount(), 1); // smb:// typed: SMB only
        QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        screenshot(*dlg, QStringLiteral("connect"));
        EntryEditor *e = openEditor(w, [dlg] { dlg->findChild<QPushButton *>(QStringLiteral("connect.add"))->click(); });
        QVERIFY(e);
        QCOMPARE(e->entry().spec, QStringLiteral("//nas.example/test_share"));
        QCOMPARE(e->entry().type, QStringLiteral("cifs"));
        QCOMPARE(e->entry().mountPoint, QStringLiteral("/mnt/test_share"));
        QVERIFY(e->entry().options.contains(QStringLiteral("credentials=/etc/anchorage/credentials/oarko-nas.example-test")));
        QVERIFY(!e->entry().options.join(QLatin1Char(',')).contains(QLatin1String("pa ss"))); // never in fstab
        QCOMPARE(e->findChild<QLineEdit *>(QStringLiteral("editor.loginUser"))->text(), QStringLiteral("test"));
        QVERIFY(e->pendingLogin().has_value());
        screenshot(*e, QStringLiteral("editor-smb"));
        e->accept();
        QCoreApplication::processEvents();
        QVERIFY(w.pendingContent().contains("//nas.example/test_share"));
        QVERIFY(!w.pendingContent().contains("pa ss"));

        // Apply: the login is saved first, then fstab.
        src->calls.clear();
        w.applyChanges();
        QCOMPARE(src->calls.mid(0, 2), QStringList({QStringLiteral("savelogin:nas.example"), QStringLiteral("writefstab")}));
        QCOMPARE(src->savedLogins.last().password, QStringLiteral("pa ss"));
        QVERIFY(!src->onDisk().contains("pa ss"));
    }

    void nfsExportWithVersionChoice()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QDialog *dlg = nullptr;
        connect(&w, &MainWindow::connectOpened, this, [&dlg](QDialog *d) { dlg = d; });
        w.connectToServer();
        dlg->findChild<QLineEdit *>(QStringLiteral("connect.address"))->setText(QStringLiteral("nas.example"));
        auto *protocol = dlg->findChild<QComboBox *>(QStringLiteral("connect.protocol"));
        protocol->setCurrentIndex(2); // NFS
        qobject_cast<ConnectDialog *>(dlg)->list();
        auto *tree = dlg->findChild<QTreeWidget *>(QStringLiteral("connect.shares"));
        QCOMPARE(tree->topLevelItemCount(), 1);
        QVERIFY(tree->topLevelItem(0)->text(1).contains(QLatin1String("Offers NFS 4 and 3")));
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        EntryEditor *e = openEditor(w, [dlg] { dlg->findChild<QPushButton *>(QStringLiteral("connect.add"))->click(); });
        QCOMPARE(e->entry().spec, QStringLiteral("nas.example:/mnt/NAS_Pool/TVshows"));
        QCOMPARE(e->entry().type, QStringLiteral("nfs")); // Automatic, not nfs4
        QCOMPARE(e->entry().mountPoint, QStringLiteral("/mnt/TVshows"));
        QVERIFY(e->findChild<QLabel *>(QStringLiteral("editor.nfsOffered"))->text().contains(QLatin1String("4 and 3")));
        auto *ver = e->findChild<QComboBox *>(QStringLiteral("editor.nfsVersion"));
        ver->setCurrentIndex(ver->findData(QStringLiteral("4.2")));
        Q_EMIT ver->activated(ver->currentIndex());
        QVERIFY(e->entry().options.contains(QStringLiteral("vers=4.2")));
        QVERIFY(!e->findChild<QLabel *>(QStringLiteral("editor.issues"))->text().contains(QLatin1String("doesn't offer")));
        screenshot(*e, QStringLiteral("editor-nfs"));
        ver->setCurrentIndex(0);
        Q_EMIT ver->activated(0);
        QVERIFY(!e->entry().options.join(QLatin1Char(',')).contains(QLatin1String("vers=")));
        e->reject();
    }

    void missingClientToolsAreOffered()
    {
        auto *src = new FixtureSource(true, "//nas/x /mnt/x cifs guest,nofail 0 0\n");
        src->missingTools = {int(clienttools::Protocol::Smb)};
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        selectTile(w, QStringLiteral("x"));
        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        advancedRow(e, QStringLiteral("noatime"))->setCheckState(0, Qt::Checked);
        e->accept();
        QCoreApplication::processEvents();
        // Copy Command: nothing is saved, the command is on the clipboard.
        bool clicked = false;
        clickInNextDialog(QStringLiteral("Copy Command"), &clicked);
        w.applyChanges();
        QVERIFY(clicked);
        QVERIFY(src->writes.isEmpty());
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("sudo apt install cifs-utils"));
        // Install: PackageKit installs it, then the save goes ahead.
        clickInNextDialog(QStringLiteral("Install"), &clicked);
        w.applyChanges();
        QVERIFY(clicked);
        QVERIFY(src->calls.contains(QStringLiteral("install:%1").arg(int(clienttools::Protocol::Smb))));
        QCOMPARE(src->writes.size(), 1);
    }

    void mountAShareAtAFolder()
    {
        auto *src = new FixtureSource(true);
        MainWindow w(src);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QDialog *dlg = nullptr;
        connect(&w, &MainWindow::connectOpened, this, [&dlg](QDialog *d) { dlg = d; });
        QDialog *mountAt = nullptr;
        connect(&w, &MainWindow::mountAtOpened, this, [&mountAt](QDialog *d) { mountAt = d; });
        w.connectToServer();
        dlg->findChild<QLineEdit *>(QStringLiteral("connect.address"))->setText(QStringLiteral("//nas.example"));
        dlg->findChild<QLineEdit *>(QStringLiteral("connect.user"))->setText(QStringLiteral("test"));
        dlg->findChild<QLineEdit *>(QStringLiteral("connect.password"))->setText(QStringLiteral("secret"));
        qobject_cast<ConnectDialog *>(dlg)->list();
        auto *tree = dlg->findChild<QTreeWidget *>(QStringLiteral("connect.shares"));
        tree->setCurrentItem(tree->topLevelItem(0)->child(1)); // Media
        dlg->findChild<QPushButton *>(QStringLiteral("connect.mountAt"))->click();
        QVERIFY(mountAt);
        QCOMPARE(src->savedLogins.size(), 1); // saved first, never on a command line
        mountAt->accept();
        QCoreApplication::processEvents();
        QCOMPARE(src->temporary.size(), 1);
        const auto &t = src->temporary[0].entry;
        QCOMPARE(t.spec, QStringLiteral("//nas.example/Media"));
        QVERIFY(t.options.contains(QStringLiteral("credentials=/etc/anchorage/credentials/oarko-nas.example-test")));
        QVERIFY(t.options.contains(QStringLiteral("uid=1000")));
        QVERIFY(!t.options.join(QLatin1Char(',')).contains(QLatin1String("secret")));
    }
};

QTEST_MAIN(GuiTest)
#include "guitest.moc"
