// SPDX-License-Identifier: GPL-3.0-or-later
#include "betawarning.h"
#include "config-anchorage.h"
#include "datasource.h"
#include "mainwindow.h"
#include "logging.h"
#include "privilege.h"

#include <KAboutData>
#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>

#include <cstdio>

int main(int argc, char *argv[])
{
    // Refuse before any GUI object exists (CLAUDE.md: the GUI never runs as root).
    if (anchorage::privilege::runningAsRoot()) {
        // Nothing useful to do if stderr is unavailable; we exit either way.
        (void)std::fputs(anchorage::privilege::rootRefusalMessage().toLocal8Bit().constData(), stderr);
        return 1;
    }

    anchorage::logging::setMessagePattern();
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(anchorage); // the UI description lives in the static anchorage_ui library
    KLocalizedString::setApplicationDomain(ANCHORAGE_APP_BINARY);

    KAboutData about(QStringLiteral(ANCHORAGE_APP_BINARY),
                     i18n("Anchorage"),
                     QStringLiteral(ANCHORAGE_VERSION),
                     i18n("Manage drives, network shares and /etc/fstab"),
                     KAboutLicense::GPL_V3,
                     i18n("© 2026 The Anchorage authors"));
    about.setDesktopFileName(QStringLiteral(ANCHORAGE_APP_ID));
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(ANCHORAGE_APP_ID)));

    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    parser.process(app);
    about.processCommandLine(&parser);

    if (anchorage::logging::betaBuild()) {
        qCInfo(anchorage::logging::helper).noquote() << "Starting Anchorage" << ANCHORAGE_VERSION << "(beta build)";
        if (!anchorage::beta::confirmLaunch(nullptr, QStringLiteral(ANCHORAGE_VERSION))) {
            return 0;
        }
    }

    auto *window = new MainWindow(new SystemDataSource);
    window->show();
    return app.exec();
}
