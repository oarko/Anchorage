// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the real main window from fixture data (tests/data) and checks what it shows.
// Set ANCHORAGE_SCREENSHOTS=<dir> to save screenshots of each view for review.
#include "datasource.h"
#include "inventorymodel.h"
#include "mainwindow.h"
#include "placespanel.h"

#include <KCategorizedView>
#include <KAboutData>
#include <KLocalizedString>

#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QTest>

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
    explicit FixtureSource(bool admin)
        : m_admin(admin)
    {
    }
    Snapshot load() override
    {
        Snapshot s;
        s.fstab = readData(QStringLiteral("kubuntu-default.fstab"));
        s.volumes = *drives::parseLsblk(readData(QStringLiteral("lsblk-desktop.json")));
        s.mounts = mounts::parseMountInfo(readData(QStringLiteral("mountinfo-desktop.txt")));
        s.backups = {QStringLiteral("fstab-2026-11-24T23-10-05Z"), QStringLiteral("fstab-2026-11-20T17-34-10Z")};
        s.me.name = QStringLiteral("oarko");
        s.me.sudo = m_admin;
        s.me.driveAdmin = m_admin;
        return s;
    }
    QByteArray readBackup(const QString &) override { return readData(QStringLiteral("kubuntu-default.fstab")); }

private:
    bool m_admin;
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

void screenshot(MainWindow &w, const QString &name)
{
    const QString dir = qEnvironmentVariable("ANCHORAGE_SCREENSHOTS");
    if (!dir.isEmpty()) {
        QCoreApplication::processEvents();
        w.grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
    }
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
};

QTEST_MAIN(GuiTest)
#include "guitest.moc"
