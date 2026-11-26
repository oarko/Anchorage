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
#include <QTreeWidget>
#include <KMessageWidget>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <functional>

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
        s.backups = {QStringLiteral("fstab-2026-11-24T23-10-05Z"), QStringLiteral("fstab-2026-11-20T17-34-10Z")};
        s.me.name = QStringLiteral("oarko");
        s.me.sudo = m_admin;
        s.me.driveAdmin = m_admin;
        s.accounts = {{1000, 1000, QStringLiteral("oarko"), QStringLiteral("Oarko")}, {1001, 1001, QStringLiteral("anna"), QString()}};
        return s;
    }
    QByteArray readBackup(const QString &) override { return readData(QStringLiteral("kubuntu-default.fstab")); }

    // Behaves like the helper: refuses stale baselines, otherwise "writes" the file.
    void writeFstab(const requests::WriteFstab &request, QWidget *, Done done) override
    {
        writes.append(request);
        client::Reply r;
        r.status = nextStatus;
        if (r.ok() && request.baselineFingerprint != service::fingerprint(m_fstab)) {
            r.status = client::Reply::Status::Failed;
            r.message = QStringLiteral("changed meanwhile");
        }
        if (r.ok()) {
            m_fstab = request.content;
            r.data.insert(QStringLiteral("backup"), QStringLiteral("fstab-2026-11-25T10-00-00Z"));
        } else if (r.message.isEmpty()) {
            r.message = QStringLiteral("refused for the test");
        }
        done(r);
    }
    QList<verify::Finding> verify(const QByteArray &) override { return findings; }

    void changeOnDisk(const QByteArray &content)
    {
        m_fstab = content;
        Q_EMIT changed();
    }
    QByteArray onDisk() const { return m_fstab; }

    QList<requests::WriteFstab> writes;
    client::Reply::Status nextStatus = client::Reply::Status::Ok;
    QList<verify::Finding> findings;

private:
    bool m_admin;
    QByteArray m_fstab;
};

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
        QCOMPARE(w.findChild<QPushButton *>(QStringLiteral("info.edit"))->text(), QStringLiteral("Add to fstab…"));

        EntryEditor *e = openEditor(w, [&w] { w.editSelected(); });
        QVERIFY(e);
        QCOMPARE(e->entry().spec, QStringLiteral("UUID=3f2a7d10-88c4-4b6e-a0d9-5e7b1c2f9c1e"));
        QCOMPARE(e->entry().type, QStringLiteral("btrfs"));
        QCOMPARE(e->entry().mountPoint, QStringLiteral("/mnt/Lingames"));
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
        QCOMPARE(w.findChild<QPushButton *>(QStringLiteral("info.edit"))->text(), QStringLiteral("Edit…"));
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
};

QTEST_MAIN(GuiTest)
#include "guitest.moc"
