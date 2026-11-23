// SPDX-License-Identifier: GPL-3.0-or-later
#include "privilege.h"

#include "config-anchorage.h"

#include <unistd.h>

namespace anchorage::privilege
{
bool runningAsRoot()
{
    return geteuid() == 0;
}

QString rootRefusalMessage()
{
    // Plain text on stderr: no GUI is created when running as root.
    return QStringLiteral(
               "%1 must not be run as root (for example with sudo).\n"
               "Start it as your normal user. When a change needs administrator rights,\n"
               "%1 asks for a password through the standard KDE prompt.\n")
        .arg(QStringLiteral(ANCHORAGE_APP_NAME));
}
}
