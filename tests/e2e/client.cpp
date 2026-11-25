// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end test client: sends real KAuth requests to the installed helper, exactly
// like the GUI will. Used by tests/vm/e2e.sh inside the test VM; not installed.
//
//   anchorage-e2e-client write <new-fstab-file> [--confirm] [--create]
//   anchorage-e2e-client add-shared <spec> <mount-point> <type> <opt1,opt2,...>
//   anchorage-e2e-client remove-shared <mount-point>
//   anchorage-e2e-client restore <backup-name> [--confirm]
//
// Prints one line "RESULT ok [key=value ...]" or "RESULT error <code> <message>".
//
// With ANCHORAGE_E2E_HANDSHAKE=<prefix> set, the client first writes its system-bus name to
// <prefix>.name and waits (up to 30 s) for <prefix>.go to exist. The test script uses that
// to register `pkttyagent --system-bus-name` for exactly this client before the request,
// because polkit asks the agent registered for the requesting D-Bus connection.
#include "config-anchorage.h"
#include "fstabservice.h"
#include "requests.h"

#include <KAuth/Action>
#include <KAuth/ActionReply>
#include <KAuth/ExecuteJob>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QFile>
#include <QTextStream>
#include <QThread>

using namespace anchorage;

namespace
{
QTextStream out(stdout);

int usage()
{
    out << "RESULT error usage see source for arguments\n";
    return 2;
}

QByteArray readFile(const QString &path, bool *ok)
{
    QFile f(path);
    *ok = f.open(QIODevice::ReadOnly);
    return *ok ? f.readAll() : QByteArray();
}

QString errorName(int code)
{
    using E = KAuth::ActionReply::Error;
    switch (code) {
    case E::NoError: return QStringLiteral("NoError");
    case E::NoResponderError: return QStringLiteral("NoResponder");
    case E::NoSuchActionError: return QStringLiteral("NoSuchAction");
    case E::InvalidActionError: return QStringLiteral("InvalidAction");
    case E::AuthorizationDeniedError: return QStringLiteral("AuthorizationDenied");
    case E::UserCancelledError: return QStringLiteral("UserCancelled");
    case E::HelperBusyError: return QStringLiteral("HelperBusy");
    case E::AlreadyStartedError: return QStringLiteral("AlreadyStarted");
    case E::DBusError: return QStringLiteral("DBusError");
    case E::BackendError: return QStringLiteral("BackendError");
    default: return QStringLiteral("HelperError(%1)").arg(code);
    }
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        return usage();
    }
    const QString cmd = args.at(1);
    const bool confirm = args.contains(QStringLiteral("--confirm"));
    const bool create = args.contains(QStringLiteral("--create"));

    QString action;
    QVariantMap request;
    if (cmd == QLatin1String("write")) {
        bool ok = false;
        const QByteArray current = readFile(QStringLiteral("/etc/fstab"), &ok);
        const QByteArray proposed = readFile(args.at(2), &ok);
        if (!ok) {
            out << "RESULT error usage cannot read " << args.at(2) << "\n";
            return 2;
        }
        action = QStringLiteral("writefstab");
        request = requests::encodeWriteFstab({service::fingerprint(current), proposed, confirm, create});
    } else if (cmd == QLatin1String("add-shared") && args.size() >= 6) {
        action = QStringLiteral("addsharedmount");
        request = requests::encodeEntry({args.at(2), args.at(3), args.at(4), args.at(5).split(QLatin1Char(','), Qt::SkipEmptyParts), 0, 0});
    } else if (cmd == QLatin1String("remove-shared")) {
        action = QStringLiteral("removesharedmount");
        request = requests::encodeMountPoint(args.at(2));
    } else if (cmd == QLatin1String("restore")) {
        action = QStringLiteral("restorebackup");
        request = requests::encodeRestore({args.at(2), confirm});
    } else {
        return usage();
    }

    const QString handshake = qEnvironmentVariable("ANCHORAGE_E2E_HANDSHAKE");
    if (!handshake.isEmpty()) {
        QFile name(handshake + QStringLiteral(".name"));
        if (!name.open(QIODevice::WriteOnly)) {
            out << "RESULT error usage cannot write " << name.fileName() << "\n";
            return 2;
        }
        name.write(QDBusConnection::systemBus().baseService().toUtf8());
        name.close();
        for (int i = 0; i < 300 && !QFile::exists(handshake + QStringLiteral(".go")); ++i) {
            QThread::msleep(100);
        }
    }

    KAuth::Action act(QStringLiteral(ANCHORAGE_APP_ID ".") + action);
    act.setHelperId(QStringLiteral(ANCHORAGE_APP_ID));
    act.setArguments(request);
    act.setTimeout(120000);
    KAuth::ExecuteJob *job = act.execute();
    const bool ok = job->exec();
    if (!ok) {
        QString message = job->errorText().isEmpty() ? job->errorString() : job->errorText();
        message.replace(QLatin1Char('\n'), QStringLiteral(" | "));
        out << "RESULT error " << errorName(job->error()) << " " << message << "\n";
        return 1;
    }
    out << "RESULT ok";
    const QVariantMap data = job->data();
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        const QVariant &v = it.value();
        out << " " << it.key() << "=" << (v.typeId() == QMetaType::QStringList ? v.toStringList().join(QLatin1Char(',')) : v.toString());
    }
    out << "\n";
    return 0;
}
