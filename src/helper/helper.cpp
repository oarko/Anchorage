// SPDX-License-Identifier: GPL-3.0-or-later
//
// Anchorage's privileged helper. Runs as root, started by KAuth/polkit for one action.
// It is deliberately thin: decode the request, find out who the caller really is, and
// hand over to anchorage::service with the fixed system paths. All rules live in
// src/core, where they are unit-tested and fuzzed.
#include "caller.h"
#include "config-anchorage.h"
#include "fstabservice.h"
#include "requests.h"

#include <KAuth/ActionReply>
#include <KAuth/HelperSupport>

#include <QObject>
#include <QProcess>

using namespace anchorage;
using KAuth::ActionReply;

class AnchorageHelper : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    ActionReply writefstab(const QVariantMap &args)
    {
        const auto request = requests::decodeWriteFstab(args);
        if (!request) {
            return error(request.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        return reply(service::writeFstab(service::systemPaths(), *caller, request->baselineFingerprint, request->content,
                                         request->confirmProtected, request->createMountPoints));
    }

    ActionReply addsharedmount(const QVariantMap &args)
    {
        const auto entry = requests::decodeEntry(args);
        if (!entry) {
            return error(entry.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        return reply(service::addSharedMount(service::systemPaths(), *caller, *entry));
    }

    ActionReply removesharedmount(const QVariantMap &args)
    {
        const auto mountPoint = requests::decodeMountPoint(args);
        if (!mountPoint) {
            return error(mountPoint.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        return reply(service::removeSharedMount(service::systemPaths(), *caller, *mountPoint));
    }

    ActionReply restorebackup(const QVariantMap &args)
    {
        const auto request = requests::decodeRestore(args);
        if (!request) {
            return error(request.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        return reply(service::restoreBackup(service::systemPaths(), *caller, request->backupName, request->confirmProtected));
    }

    ActionReply writesettings(const QVariantMap &args)
    {
        const auto request = requests::decodeSettings(args);
        if (!request) {
            return error(request.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        const service::Outcome outcome = service::writeSettings(service::systemPaths(), *caller, *request);
        if (!outcome.ok()) {
            ActionReply r = error(outcome.errors.join(QLatin1Char('\n')));
            r.addData(QStringLiteral("errors"), outcome.errors);
            return r;
        }
        return ActionReply::SuccessReply();
    }

    ActionReply mountentry(const QVariantMap &args)
    {
        const auto mountPoint = requests::decodeMountPoint(args);
        if (!mountPoint) {
            return error(mountPoint.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        return mountReply(service::mountEntry(service::systemPaths(), *caller, *mountPoint, service::systemRunner()));
    }

    ActionReply mounttemporary(const QVariantMap &args)
    {
        const auto request = requests::decodeTemporaryMount(args);
        if (!request) {
            return error(request.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        return mountReply(service::mountTemporary(service::systemPaths(), *caller, request->entry, request->confirmProtected, service::systemRunner()));
    }

    ActionReply unmount(const QVariantMap &args)
    {
        const auto request = requests::decodeUnmount(args);
        if (!request) {
            return error(request.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        return mountReply(service::unmount(service::systemPaths(), *caller, request->mountPoint, request->confirmSystem, service::systemRunner()));
    }

    ActionReply writecredentials(const QVariantMap &args)
    {
        const auto request = requests::decodeCredentials(args);
        if (!request) {
            return error(request.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        const service::CredentialsOutcome out =
            service::writeCredentials(service::systemPaths(), *caller, {request->server, request->user, request->password, request->domain});
        if (!out.ok()) {
            return error(out.errors.join(QLatin1Char('\n')));
        }
        ActionReply r = ActionReply::SuccessReply();
        r.addData(QStringLiteral("path"), out.path);
        return r;
    }

private:
    static ActionReply mountReply(const service::MountOutcome &outcome)
    {
        ActionReply r = outcome.ok() ? ActionReply::SuccessReply() : error(outcome.errors.join(QLatin1Char('\n')));
        r.addData(QStringLiteral("errors"), outcome.errors);
        r.addData(QStringLiteral("createdDirs"), outcome.createdDirs);
        r.addData(QStringLiteral("processes"), outcome.processes);
        return r;
    }

    static Result<Caller> who()
    {
        const int uid = KAuth::HelperSupport::callerUid();
        if (uid < 0) {
            return Result<Caller>::failure(QStringLiteral("Could not identify the caller."));
        }
        return resolveCaller(static_cast<uid_t>(uid), defaultGroupNames());
    }

    static ActionReply error(const QString &message)
    {
        ActionReply r = ActionReply::HelperErrorReply();
        r.setErrorDescription(message);
        return r;
    }

    static ActionReply reply(const service::Outcome &outcome)
    {
        if (!outcome.ok()) {
            ActionReply r = error(outcome.errors.join(QLatin1Char('\n')));
            r.addData(QStringLiteral("errors"), outcome.errors);
            return r;
        }
        // Let systemd regenerate its mount units from the new file.
        QProcess::execute(QStringLiteral("/usr/bin/systemctl"), {QStringLiteral("daemon-reload")});
        ActionReply r = ActionReply::SuccessReply();
        r.addData(QStringLiteral("backup"), outcome.backupName);
        r.addData(QStringLiteral("createdDirs"), outcome.createdDirs);
        return r;
    }
};

KAUTH_HELPER_MAIN(ANCHORAGE_APP_ID, AnchorageHelper)

#include "helper.moc"
