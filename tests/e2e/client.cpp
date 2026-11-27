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
//                             [opt=<option>]... [bind=<target>[:<folder on the drive>]]... [--confirm]
//   anchorage-e2e-client edit-remove <mount-point>
//   anchorage-e2e-client settings <keep-backups> <ntfs3|ntfs>
//   anchorage-e2e-client mount-entry <mount-point>...   (one request, in order)
//   anchorage-e2e-client mount-temp <spec> <mount-point> <type> [opt1,opt2,...] [--confirm]
//   anchorage-e2e-client unmount <mount-point>... [--confirm]
//   anchorage-e2e-client suggest <device>        mount-point suggestions (no helper; prints SUGGEST lines)
//   anchorage-e2e-client list-shares <host> smb|nfs [user]   (no helper; prints SHARE and VERSIONS lines)
//   anchorage-e2e-client save-login <server> <user> [domain] (writecredentials; prints path=)
//   anchorage-e2e-client install-tool smb|nfs                (PackageKit, not the helper)
// Passwords come from the ANCHORAGE_E2E_PASSWORD environment variable, never the command
// line (which every user can see in the process list).
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
#include "binds.h"
#include "drives.h"
#include "locations.h"
#include "clienttools.h"
#include "packagekit.h"
#include "shares.h"
#include "mounts.h"
#include "helperclient.h"
#include "requests.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QFile>
#include <QTextStream>
#include <QThread>

#include <cstdlib>

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
    QList<binds::Bind> wantedBinds;
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
        } else if (key == QLatin1String("bind")) {
            wantedBinds.append({value.section(QLatin1Char(':'), 1), value.section(QLatin1Char(':'), 0, 0)});
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
    // The same code the editor uses for "Also show it at…".
    if (!wantedBinds.isEmpty() && !binds::apply(file, existing >= 0 ? mountPoint : QString(), e, wantedBinds, error)) {
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
    // With ANCHORAGE_E2E_HANDSHAKE set: announce our bus name and wait until the test has
    // registered a polkit agent for this process (before anything that may need a password).
    const auto waitForAgent = [] {
        const QString handshake = qEnvironmentVariable("ANCHORAGE_E2E_HANDSHAKE");
        if (!handshake.isEmpty()) {
            QFile name(handshake + QStringLiteral(".name"));
            if (!name.open(QIODevice::WriteOnly)) {
                out << "RESULT error usage cannot write " << name.fileName() << "\n";
                out.flush();
                std::exit(2);
            }
            name.write(QDBusConnection::systemBus().baseService().toUtf8());
            name.close();
            for (int i = 0; i < 300 && !QFile::exists(handshake + QStringLiteral(".go")); ++i) {
                QThread::msleep(100);
            }
        }
    };
    // mount-entry / unmount: every argument after the command that isn't a --flag.
    const auto listArgs = [&args] {
        QStringList l;
        for (qsizetype k = 2; k < args.size(); ++k) {
            if (!args.at(k).startsWith(QLatin1String("--"))) {
                l.append(args.at(k));
            }
        }
        return l;
    };
    const QString envPassword = qEnvironmentVariable("ANCHORAGE_E2E_PASSWORD");
    if (cmd == QLatin1String("list-shares") && args.size() >= 4) {
        const QString host = args.at(2);
        if (args.at(3) == QLatin1String("smb")) {
            const shares::Login login{args.value(4), envPassword, QString(), false};
            const shares::SmbList l = shares::listSmb(host, login);
            if (!l.ok()) {
                out << "RESULT error smb" << int(l.error) << " " << l.message << "\n";
                return 1;
            }
            for (const auto &sh : l.shares) {
                out << "SHARE " << sh.name << "\n";
            }
        } else {
            const auto l = shares::listNfs(host);
            if (!l) {
                out << "RESULT error nfs " << l.error() << "\n";
                return 1;
            }
            for (const auto &sh : *l) {
                out << "SHARE " << sh.name << "\n";
            }
            QStringList v;
            for (int n : shares::nfsVersions(host)) {
                v.append(QString::number(n));
            }
            out << "VERSIONS " << v.join(QLatin1Char(',')) << "\n";
        }
        out << "RESULT ok\n";
        return 0;
    }
    if (cmd == QLatin1String("install-tool")) {
        QFile osr(QStringLiteral("/etc/os-release"));
        const QByteArray data = osr.open(QIODevice::ReadOnly) ? osr.readAll() : QByteArray();
        const auto manager = clienttools::detectManager(clienttools::parseOsRelease(data), [](const QString &p) {
            return QFile::exists(p);
        });
        const auto protocol = args.at(2) == QLatin1String("nfs") ? clienttools::Protocol::Nfs : clienttools::Protocol::Smb;
        waitForAgent();
        int code = 1;
        packagekit::install(clienttools::packageName(manager, protocol), &app, [&code, &app](const packagekit::Result &r) {
            out << (r.ok ? "RESULT ok" : r.cancelled ? "RESULT error cancelled" : "RESULT error ") << (r.ok ? QString() : r.message) << "\n";
            code = r.ok ? 0 : 1;
            app.quit();
        });
        app.exec();
        return code;
    }
    if (cmd == QLatin1String("suggest")) {
        // The editor's suggestions for a real device (SPEC §8), from lsblk and mountinfo.
        const auto volumes = drives::scan();
        const auto current = mounts::readCurrent();
        if (!volumes || !current) {
            out << "RESULT error scan failed\n";
            return 1;
        }
        for (const auto &v : *volumes) {
            if (v.path == args.at(2)) {
                for (const QString &s : locations::suggestions(v, qEnvironmentVariable("USER"), *current, [](const QString &p) {
                         return QFile::exists(p);
                     })) {
                    out << "SUGGEST " << s << "\n";
                }
                out << "RESULT ok\n";
                return 0;
            }
        }
        out << "RESULT error no such device\n";
        return 1;
    }
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
    } else if (cmd == QLatin1String("mount-entry")) {
        action = QStringLiteral("mountentry");
        request = requests::encodeMountPoints(listArgs());
    } else if (cmd == QLatin1String("mount-temp") && args.size() >= 5) {
        action = QStringLiteral("mounttemporary");
        const QString opts = args.size() >= 6 && !args.at(5).startsWith(QLatin1String("--")) ? args.at(5) : QString();
        request = requests::encodeTemporaryMount({{args.at(2), args.at(3), args.at(4), opts.split(QLatin1Char(','), Qt::SkipEmptyParts), 0, 0}, confirm});
    } else if (cmd == QLatin1String("unmount")) {
        action = QStringLiteral("unmount");
        request = requests::encodeUnmount({listArgs(), confirm});
    } else if (cmd == QLatin1String("save-login") && args.size() >= 4) {
        action = QStringLiteral("writecredentials");
        request = requests::encodeCredentials({args.at(2), args.at(3), envPassword, args.size() >= 5 && !args.at(4).startsWith(QLatin1String("--")) ? args.at(4) : QString()});
    } else if (cmd == QLatin1String("restore")) {
        action = QStringLiteral("restorebackup");
        request = requests::encodeRestore({args.at(2), confirm});
    } else {
        return usage();
    }

    waitForAgent();
    const QHash<QString, client::Action> actions = {{QStringLiteral("writefstab"), client::Action::WriteFstab},
                                                    {QStringLiteral("addsharedmount"), client::Action::AddSharedMount},
                                                    {QStringLiteral("removesharedmount"), client::Action::RemoveSharedMount},
                                                    {QStringLiteral("restorebackup"), client::Action::RestoreBackup},
                                                    {QStringLiteral("writesettings"), client::Action::WriteSettings},
                                                    {QStringLiteral("mountentry"), client::Action::MountEntry},
                                                    {QStringLiteral("mounttemporary"), client::Action::MountTemporary},
                                                    {QStringLiteral("unmount"), client::Action::Unmount},
                                                    {QStringLiteral("writecredentials"), client::Action::WriteCredentials}};
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
