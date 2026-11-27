// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

#include <functional>

// Installing a missing client package (cifs-utils, nfs-common, ...) through PackageKit's
// D-Bus API, the way Discover does (SPEC §11). PackageKit asks for the password through its
// own polkit rules; Anchorage's helper is not involved.
namespace anchorage::packagekit
{

// The PackageKit service is installed (and can be started on demand).
bool available();

struct Result {
    bool ok = false;
    bool cancelled = false; // the password prompt was closed
    QString message;
};

// Resolves `packageName` to an installable package and installs it. `done` runs once, in
// `context`'s thread, unless `context` is destroyed first.
void install(const QString &packageName, QObject *context, std::function<void(const Result &)> done);

} // namespace anchorage::packagekit
