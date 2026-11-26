// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "result.h"

#include <QByteArray>
#include <QList>
#include <QString>

// "Validate before writing" (SPEC §9): runs `findmnt --verify` on a proposed fstab, as
// the user, before it is sent to the helper. findmnt checks that devices exist, that
// the file system type matches, and that mount points are reachable.
namespace anchorage::verify
{

struct Finding {
    QString target; // mount point the message is about
    bool error = false; // [E] (otherwise [W])
    QString message;
};

// Parses `findmnt --verify --verbose` output; informational "[ ]" lines are dropped.
QList<Finding> parseFindmntOutput(const QByteArray &output);

// Writes `content` to a private temporary file and runs /usr/bin/findmnt on it.
// The exit code is ignored on purpose (findmnt 2.41 crashes after parse errors; our
// own parser has already rejected those).
Result<QList<Finding>> findmnt(const QByteArray &content);

} // namespace anchorage::verify
