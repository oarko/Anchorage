// SPDX-License-Identifier: GPL-3.0-or-later
//
// Anchorage's privileged helper. Runs as root, started by KAuth/polkit for one action.
// It is deliberately thin: decode the request, find out who the caller really is, and
// hand over to anchorage::service with the fixed system paths. All rules live in
// src/core, where they are unit-tested and fuzzed.
#include "caller.h"
#include "config-anchorage.h"
#include "fstabservice.h"
#include "logging.h"
#include "requests.h"

#include <KAuth/ActionReply>
#include <KAuth/HelperSupport>

#include <QObject>
#include <QProcess>

#include <cstdio>

using namespace anchorage;
using KAuth::ActionReply;

class AnchorageHelper : public QObject
{
    Q_OBJECT

public:
    AnchorageHelper()
    {
        logging::setMessagePattern();
    }

public Q_SLOTS: // NOLINT(readability-redundant-access-specifiers): moc needs its own slots section
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
        log("writefstab", *caller, QStringLiteral("%1 bytes").arg(request->content.size()));
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
        log("addsharedmount", *caller, entry->spec + QStringLiteral(" at ") + entry->mountPoint);
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
        log("removesharedmount", *caller, *mountPoint);
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
        log("restorebackup", *caller, request->backupName);
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
        log("writesettings", *caller, {});
        const service::Outcome outcome = service::writeSettings(service::systemPaths(), *caller, *request);
        if (!outcome.ok()) {
            ActionReply r = error(outcome.errors.join(QLatin1Char('\n')));
            r.addData(QStringLiteral("errors"), outcome.errors);
            return r;
        }
        qCInfo(logging::fstab) << "Wrote the settings.";
        return ActionReply::SuccessReply();
    }

    ActionReply mountentry(const QVariantMap &args)
    {
        const auto mountPoints = requests::decodeMountPoints(args);
        if (!mountPoints) {
            return error(mountPoints.error());
        }
        const auto caller = who();
        if (!caller) {
            return error(caller.error());
        }
        log("mountentry", *caller, mountPoints->join(QStringLiteral(", ")));
        return mountReply(service::mountEntries(service::systemPaths(), *caller, *mountPoints, service::systemRunner()));
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
        log("mounttemporary", *caller, request->entry.spec + QStringLiteral(" at ") + request->entry.mountPoint);
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
        log("unmount", *caller, request->mountPoints.join(QStringLiteral(", ")));
        return mountReply(service::unmountAll(service::systemPaths(), *caller, request->mountPoints, request->confirmSystem, service::systemRunner()));
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
        log("writecredentials", *caller, QStringLiteral("for %1 on %2").arg(request->user, request->server)); // never the password
        const service::CredentialsOutcome out =
            service::writeCredentials(service::systemPaths(), *caller, {request->server, request->user, request->password, request->domain});
        if (!out.ok()) {
            return error(out.errors.join(QLatin1Char('\n')), {request->password});
        }
        qCInfo(logging::helper).noquote() << "Saved the login in" << out.path;
        ActionReply r = ActionReply::SuccessReply();
        r.addData(QStringLiteral("path"), out.path);
        return r;
    }

private:
    static inline QtMessageHandler previousHandler = nullptr;
    static inline bool handlerInstalled = false;

    // KAuth forwards log lines to the calling app while an action runs ("Info message from
    // helper: ..."). Also write them to stderr, which D-Bus activation sends to the journal,
    // so `journalctl -b | grep anchorage` shows them. Installed at the first request: KAuth
    // installs its own handler after constructing this object, which would replace ours.
    static void chainLogHandler()
    {
        if (!handlerInstalled) {
            handlerInstalled = true;
            previousHandler = qInstallMessageHandler(&toStderrAndKAuth);
        }
    }

    static void toStderrAndKAuth(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        const QByteArray line = qFormatLogMessage(type, context, message).toUtf8() + '\n';
        (void)std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
        (void)std::fflush(stderr);
        if (previousHandler) {
            previousHandler(type, context, message);
        }
    }

    static void log(const char *action, const Caller &caller, const QString &detail)
    {
        chainLogHandler();
        const QString who = QStringLiteral("%1 from %2 (uid %3)").arg(QLatin1String(action), caller.name).arg(caller.uid);
        qCInfo(logging::helper).noquote() << (detail.isEmpty() ? who : who + QStringLiteral(": ") + logging::scrub(detail));
    }

    static ActionReply mountReply(const service::MountOutcome &outcome)
    {
        for (const QString &mp : outcome.done) {
            qCInfo(logging::mount).noquote() << "Done:" << mp;
        }
        if (outcome.ok() && outcome.done.isEmpty()) {
            qCInfo(logging::mount) << "Done.";
        }
        ActionReply r = outcome.ok() ? ActionReply::SuccessReply() : error(outcome.errors.join(QLatin1Char('\n')));
        r.addData(QStringLiteral("errors"), outcome.errors);
        r.addData(QStringLiteral("createdDirs"), outcome.createdDirs);
        r.addData(QStringLiteral("processes"), outcome.processes);
        r.addData(QStringLiteral("done"), outcome.done);
        r.addData(QStringLiteral("failed"), outcome.failed);
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

    static ActionReply error(const QString &message, const QStringList &secrets = {})
    {
        chainLogHandler();
        qCWarning(logging::helper).noquote() << "Failed:" << logging::scrub(message, secrets);
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
        qCInfo(logging::fstab).noquote() << "Wrote /etc/fstab; backup" << (outcome.backupName.isEmpty() ? QStringLiteral("not needed") : outcome.backupName)
                                         << "; created folders:" << outcome.createdDirs;
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
