// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end test client: sends real KAuth requests to the installed helper, exactly
// like the GUI will. Used by tests/vm/e2e.sh inside the test VM; not installed.
//
//   anchorage-e2e-client write <new-fstab-file> [--confirm] [--create]
//   anchorage-e2e-client add-shared <spec> <mount-point> <type> <opt1,opt2,...>
//   anchorage-e2e-client remove-shared <mount-point>
//   anchorage-e2e-client restore <backup-name> [--confirm]
//   anchorage-e2e-client edit <spec> <mount-point> <type> <boot|request|opened> <admins|user|users>
//                             [nofail=0|1] [netdev=0|1] [uid=N] [gid=N] [sharing=owner|read|everyone]
//                             [opt=<option>]... [--confirm]
//   anchorage-e2e-client edit-remove <mount-point>
//   anchorage-e2e-client settings <keep-backups> <ntfs3|ntfs>
//
// "edit" builds the entry the way the entry editor does (access::defaultsFor, then
// access::apply for the Access tab choices, opt= for the Advanced tab), adds it or
// changes the entry already at that mount point with fstab::File, and sends the whole
// file through anchorage::client, the GUI's own KAuth code.
//
// Prints one line "RESULT ok [key=value ...]" or "RESULT error <code> <message>".
//
// With ANCHORAGE_E2E_HANDSHAKE=<prefix> set, the client first writes its system-bus name to
// <prefix>.name and waits (up to 30 s) for <prefix>.go to exist. The test script uses that
// to register `pkttyagent --system-bus-name` for exactly this client before the request,
// because polkit asks the agent registered for the requesting D-Bus connection.
#include "config-anchorage.h"
#include "fstabservice.h"
#include "access.h"
#include "helperclient.h"
#include "requests.h"

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

// Builds the new /etc/fstab for "edit" / "edit-remove". Empty result + message on error.
QByteArray editFstab(const QByteArray &current, const QStringList &args, QString *error)
{
    fstab::File file = fstab::File::parse(current);
    const bool remove = args.at(1) == QLatin1String("edit-remove");
    const QString mountPoint = args.at(remove ? 2 : 3);
    const int existing = file.findMountPoint(mountPoint);
    if (remove) {
        if (existing < 0 || !file.remove(existing)) {
            *error = QStringLiteral("no entry at ") + mountPoint;
            return {};
        }
        return file.serialize();
    }
    if (args.size() < 7) {
        *error = QStringLiteral("edit needs spec, mount point, type, start and who");
        return {};
    }
    fstab::Entry e;
    e.spec = args.at(2);
    e.mountPoint = mountPoint;
    e.type = args.at(4);
    QStringList base;
    if (existing >= 0) {
        base = file.lines().at(static_cast<std::size_t>(existing)).entry->options;
    }
    access::Settings s = existing >= 0 ? access::read(base, e.type) : access::defaultsFor(e.type);
    const QHash<QString, access::Start> starts = {{QStringLiteral("boot"), access::Start::AtBoot},
                                                  {QStringLiteral("request"), access::Start::OnRequest},
                                                  {QStringLiteral("opened"), access::Start::WhenOpened}};
    const QHash<QString, access::Who> whos = {{QStringLiteral("admins"), access::Who::Admins},
                                              {QStringLiteral("user"), access::Who::AnyUser},
                                              {QStringLiteral("users"), access::Who::AnyUserAnyoneUnmounts}};
    const QHash<QString, access::Sharing> sharings = {{QStringLiteral("owner"), access::Sharing::OwnerOnly},
                                                      {QStringLiteral("read"), access::Sharing::OthersRead},
                                                      {QStringLiteral("everyone"), access::Sharing::Everyone}};
    if (!starts.contains(args.at(5)) || !whos.contains(args.at(6))) {
        *error = QStringLiteral("bad start or who");
        return {};
    }
    s.start = starts.value(args.at(5));
    s.who = whos.value(args.at(6));
    QStringList extra;
    for (const QString &a : args.mid(7)) {
        const QString key = a.section(QLatin1Char('='), 0, 0);
        const QString value = a.section(QLatin1Char('='), 1);
        if (key == QLatin1String("nofail")) {
            s.nofail = value == QLatin1String("1");
        } else if (key == QLatin1String("netdev")) {
            s.netdev = value == QLatin1String("1");
        } else if (key == QLatin1String("uid")) {
            s.uid = value.toUInt();
        } else if (key == QLatin1String("gid")) {
            s.gid = value.toUInt();
        } else if (key == QLatin1String("sharing")) {
            s.sharing = sharings.value(value);
        } else if (key == QLatin1String("opt")) {
            extra.append(value);
        }
    }
    e.options = access::apply(base, s, e.type);
    for (const QString &o : extra) {
        if (!e.options.contains(o)) {
            e.options.append(o);
        }
    }
    QList<fstab::Issue> issues;
    const bool ok = existing >= 0 ? file.replace(existing, e, &issues) : file.append(e, &issues) >= 0;
    if (!ok) {
        *error = issues.isEmpty() ? QStringLiteral("refused") : issues.first().message;
        return {};
    }
    return file.serialize();
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
    } else if ((cmd == QLatin1String("edit") && args.size() >= 7) || cmd == QLatin1String("edit-remove")) {
        bool ok = false;
        const QByteArray current = readFile(QStringLiteral("/etc/fstab"), &ok);
        QString error;
        const QByteArray proposed = editFstab(current, args, &error);
        if (proposed.isEmpty()) {
            out << "RESULT error usage " << error << "\n";
            return 2;
        }
        action = QStringLiteral("writefstab");
        request = requests::encodeWriteFstab({service::fingerprint(current), proposed, confirm, true});
    } else if (cmd == QLatin1String("settings") && args.size() >= 4) {
        action = QStringLiteral("writesettings");
        settings::Settings st;
        st.keepBackups = args.at(2).toInt();
        st.ntfsDriver = args.at(3);
        request = requests::encodeSettings(st);
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

    const QHash<QString, client::Action> actions = {{QStringLiteral("writefstab"), client::Action::WriteFstab},
                                                    {QStringLiteral("addsharedmount"), client::Action::AddSharedMount},
                                                    {QStringLiteral("removesharedmount"), client::Action::RemoveSharedMount},
                                                    {QStringLiteral("restorebackup"), client::Action::RestoreBackup},
                                                    {QStringLiteral("writesettings"), client::Action::WriteSettings}};
    const client::Reply reply = client::runBlocking(actions.value(action), request);
    if (!reply.ok()) {
        QString message = reply.message;
        message.replace(QLatin1Char('\n'), QStringLiteral(" | "));
        out << "RESULT error " << reply.codeName << " " << message << "\n";
        return 1;
    }
    out << "RESULT ok";
    const QVariantMap data = reply.data;
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        const QVariant &v = it.value();
        out << " " << it.key() << "=" << (v.typeId() == QMetaType::QStringList ? v.toStringList().join(QLatin1Char(',')) : v.toString());
    }
    out << "\n";
    return 0;
}
