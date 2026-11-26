// SPDX-License-Identifier: GPL-3.0-or-later
#include "helperclient.h"

#include "config-anchorage.h"

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

KAuth::ExecuteJob *start(Action action, const QVariantMap &args, QWindow *parent)
{
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
    }
    return {};
}

void run(Action action, const QVariantMap &args, QWindow *parent, QObject *context, std::function<void(const Reply &)> done)
{
    KAuth::ExecuteJob *job = start(action, args, parent);
    QPointer<QObject> guard(context);
    QObject::connect(job, &KJob::result, context, [job, guard, done = std::move(done)] {
        if (guard) {
            done(toReply(job, job->error() == 0));
        }
    });
    job->start();
}

Reply runBlocking(Action action, const QVariantMap &args)
{
    KAuth::ExecuteJob *job = start(action, args, nullptr);
    const bool ok = job->exec();
    return toReply(job, ok);
}

} // namespace anchorage::client
