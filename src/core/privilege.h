// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace anchorage::privilege
{
// True when the process runs with root's effective user ID.
bool runningAsRoot();

// Explanation shown when someone starts the GUI as root (e.g. with sudo).
// The GUI always runs as the user; privileged work goes through the helper.
QString rootRefusalMessage();
}
