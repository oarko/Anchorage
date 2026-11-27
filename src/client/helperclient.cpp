// SPDX-License-Identifier: GPL-3.0-or-later
#include "helperclient.h"

#include "config-anchorage.h"
#include "logging.h"

#include <KAuth/Action>
#include <KAuth/ActionReply>
#include <KAuth/ExecuteJob>

#include <QPointer>

namespace anchorage::client
{

namespace
{
QString errorName(int code)
{
    using E = KAuth::ActionReply::Error;
    switch (code) {
    case E::NoError:
        return QStringLiteral("NoError");
    case E::NoResponderError:
        return QStringLiteral("NoResponder");
    case E::NoSuchActionError:
        return QStringLiteral("NoSuchAction");
    case E::InvalidActionError:
        return QStringLiteral("InvalidAction");
    case E::AuthorizationDeniedError:
        return QStringLiteral("AuthorizationDenied");
    case E::UserCancelledError:
        return QStringLiteral("UserCancelled");
    case E::HelperBusyError:
        return QStringLiteral("HelperBusy");
    case E::AlreadyStartedError:
        return QStringLiteral("AlreadyStarted");
    case E::DBusError:
        return QStringLiteral("DBusError");
    case E::BackendError:
        return QStringLiteral("BackendError");
    default:
        return QStringLiteral("HelperError(%1)").arg(code);
    }
}

QString statusName(Reply::Status status)
{
    switch (status) {
    case Reply::Status::Ok:
        return QStringLiteral("done");
    case Reply::Status::Cancelled:
        return QStringLiteral("cancelled at the password prompt");
    case Reply::Status::Denied:
        return QStringLiteral("not allowed");
    case Reply::Status::Failed:
        break;
    }
    return QStringLiteral("failed");
}

// The request's arguments are never logged: they can hold fstab content or a password.
void logReply(Action action, const Reply &r)
{
    if (r.status == Reply::Status::Ok) {
        qCInfo(logging::helper).noquote() << actionId(action) << statusName(r.status);
    } else {
        qCInfo(logging::helper).noquote() << actionId(action) << statusName(r.status) << QStringLiteral("(%1):").arg(r.codeName)
                                          << logging::scrub(r.message);
    }
}

KAuth::ExecuteJob *start(Action action, const QVariantMap &args, QWindow *parent)
{
    qCInfo(logging::helper).noquote() << "Asking the helper:" << actionId(action);
    KAuth::Action act(actionId(action));
    act.setHelperId(QStringLiteral(ANCHORAGE_APP_ID));
    act.setArguments(args);
    act.setTimeout(120000);
    if (parent) {
        act.setParentWindow(parent);
    }
    return act.execute();
}

Reply toReply(KAuth::ExecuteJob *job, bool ok)
{
    Reply r;
    r.code = job->error();
    r.codeName = errorName(r.code);
    r.data = job->data();
    if (ok) {
        r.status = Reply::Status::Ok;
        return r;
    }
    switch (r.code) {
    case KAuth::ActionReply::UserCancelledError:
        r.status = Reply::Status::Cancelled;
        break;
    case KAuth::ActionReply::AuthorizationDeniedError:
        r.status = Reply::Status::Denied;
        break;
    default:
        r.status = Reply::Status::Failed;
    }
    // The helper's own message (errorText) is more useful than KJob's generic one.
    r.message = job->errorText().isEmpty() ? job->errorString() : job->errorText();
    return r;
}
} // namespace

QString actionId(Action action)
{
    const QString prefix = QStringLiteral(ANCHORAGE_APP_ID ".");
    switch (action) {
    case Action::WriteFstab:
        return prefix + QStringLiteral("writefstab");
    case Action::RestoreBackup:
        return prefix + QStringLiteral("restorebackup");
    case Action::AddSharedMount:
        return prefix + QStringLiteral("addsharedmount");
    case Action::RemoveSharedMount:
        return prefix + QStringLiteral("removesharedmount");
    case Action::WriteSettings:
        return prefix + QStringLiteral("writesettings");
    case Action::MountEntry:
        return prefix + QStringLiteral("mountentry");
    case Action::MountTemporary:
        return prefix + QStringLiteral("mounttemporary");
    case Action::Unmount:
        return prefix + QStringLiteral("unmount");
    case Action::WriteCredentials:
        return prefix + QStringLiteral("writecredentials");
    }
    return {};
}

void run(Action action, const QVariantMap &args, QWindow *parent, QObject *context, std::function<void(const Reply &)> done)
{
    KAuth::ExecuteJob *job = start(action, args, parent);
    QPointer<QObject> guard(context);
    QObject::connect(job, &KJob::result, context, [action, job, guard, done = std::move(done)] {
        const Reply r = toReply(job, job->error() == 0);
        logReply(action, r);
        if (guard) {
            done(r);
        }
    });
    job->start();
}

Reply runBlocking(Action action, const QVariantMap &args)
{
    KAuth::ExecuteJob *job = start(action, args, nullptr);
    const bool ok = job->exec();
    const Reply r = toReply(job, ok);
    logReply(action, r);
    return r;
}

} // namespace anchorage::client
