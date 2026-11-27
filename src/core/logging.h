// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLoggingCategory>
#include <QString>
#include <QStringList>

// Logging categories (SPEC §12a). A beta build shows their INFO lines by default; a release
// shows only warnings unless switched on for a bug report, for example
//   QT_LOGGING_RULES="anchorage.*.info=true" anchorage
// Never log a password, a saved-login file's contents, request maps or fstab content:
// anything that might hold a secret goes through scrub() first.
namespace anchorage::logging
{
Q_DECLARE_LOGGING_CATEGORY(network) // "anchorage.network": listing servers and shares
Q_DECLARE_LOGGING_CATEGORY(mount) // "anchorage.mount": mounting and unmounting
Q_DECLARE_LOGGING_CATEGORY(fstab) // "anchorage.fstab": writing fstab, backups, settings
Q_DECLARE_LOGGING_CATEGORY(helper) // "anchorage.helper": requests the root helper received

// True in a beta build (ANCHORAGE_BETA).
bool betaBuild();

// "INFO: anchorage.network: Found share at //nas/media" (unless QT_MESSAGE_PATTERN is set).
void setMessagePattern();

// Replaces the value of every password=/pass= option and every one of `secrets` with "…".
QString scrub(const QString &text, const QStringList &secrets = {});
} // namespace anchorage::logging
