// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KStandardAction>

#include <QApplication>
#include <QLabel>

MainWindow::MainWindow(QWidget *parent)
    : KXmlGuiWindow(parent)
{
    auto *placeholder = new QLabel(i18n("Anchorage is under construction."), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);

    KStandardAction::quit(qApp, &QApplication::closeAllWindows, actionCollection());

    // The UI description is embedded as a Qt resource (:/kxmlgui5/anchorage/anchorageui.rc).
    setupGUI(QSize(1100, 700), Default, QStringLiteral("anchorageui.rc"));
}
