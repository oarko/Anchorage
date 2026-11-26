// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVariantMap>

#include <functional>

class QObject;
class QWindow;

// Talks to the root helper through KAuth (D-Bus + polkit). Used by the GUI and by the
// end-to-end test client, so the VM tests exercise exactly the GUI's code path.
namespace anchorage::client
{

enum class Action { WriteFstab, RestoreBackup, AddSharedMount, RemoveSharedMount };
QString actionId(Action action); // io.github.oarko.anchorage.writefstab, ...

struct Reply {
    enum class Status {
        Ok,
        Cancelled, // the person closed the password prompt
        Denied, // polkit said no (wrong password, remote session, ...)
        Failed, // the helper refused the request or something broke
    };
    Status status = Status::Failed;
    int code = 0; // KAuth::ActionReply::Error
    QString codeName; // "AuthorizationDenied", ... (for logs and tests)
    QString message; // what to show
    QVariantMap data; // helper's reply data (backup, createdDirs, errors)
    bool ok() const { return status == Status::Ok; }
};

// Runs `action` asynchronously; `done` is called exactly once, in `context`'s thread,
// unless `context` is destroyed first. `parent` is the window the password prompt
// belongs to (may be null).
void run(Action action, const QVariantMap &args, QWindow *parent, QObject *context, std::function<void(const Reply &)> done);

// Blocking variant for command-line tools.
Reply runBlocking(Action action, const QVariantMap &args);

} // namespace anchorage::client
