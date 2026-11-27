// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

class QWidget;

// The warning a beta build shows at every launch (SPEC §12a).
namespace anchorage::beta
{
// The "Don't show again" key: one per version, so the next beta warns again.
QString dontShowAgainKey(const QString &version);

// Shows the warning unless it was turned off for this version. False: the person chose Quit.
bool confirmLaunch(QWidget *parent, const QString &version);
} // namespace anchorage::beta
