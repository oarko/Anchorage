// SPDX-License-Identifier: GPL-3.0-or-later
// The option catalogue, the Access tab mapping, the line diff and the findmnt parser.
#include "access.h"
#include "linediff.h"
#include "options.h"
#include "verify.h"

#include <QFile>
#include <QTest>

#include <functional>

using namespace anchorage;
using access::Sharing;
using access::Start;
using access::Who;

namespace
{
QStringList L(const char *s)
{
    return s[0] ? QString::fromLatin1(s).split(QLatin1Char(',')) : QStringList{};
}
} // namespace

class OptionsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- catalogue -------------------------------------------------------------------
    void builtInCatalogueLoads()
    {
        const auto &c = options::Catalogue::builtIn();
        QVERIFY(c.fileSystems().size() >= 20);
        QVERIFY(c.all().size() >= 380);
        for (const auto &fs : c.fileSystems()) {
            QVERIFY2(!c.optionsFor(fs.type).isEmpty(), qPrintable(fs.type));
        }
    }

    void everyFileSystemHasItsOwnOptions_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("option");
        QTest::newRow("ext4") << "ext4" << "data";
        QTest::newRow("btrfs") << "btrfs" << "compress";
        QTest::newRow("xfs") << "xfs" << "logbufs";
        QTest::newRow("vfat") << "vfat" << "shortname";
        QTest::newRow("exfat") << "exfat" << "keep_last_dots";
        QTest::newRow("ntfs3") << "ntfs3" << "windows_names";
        QTest::newRow("ntfs-3g") << "ntfs" << "remove_hiberfile";
        QTest::newRow("cifs") << "cifs" << "vers";
        QTest::newRow("smb3") << "smb3" << "seal";
        QTest::newRow("nfs") << "nfs" << "nconnect";
        QTest::newRow("nfs4") << "nfs4" << "xprtsec";
        QTest::newRow("tmpfs") << "tmpfs" << "size";
        QTest::newRow("swap") << "swap" << "pri";
        QTest::newRow("bind") << "none" << "bind";
    }
    void everyFileSystemHasItsOwnOptions()
    {
        QFETCH(QString, type);
        QFETCH(QString, option);
        bool found = false;
        for (const auto *o : options::Catalogue::builtIn().optionsFor(type)) {
            found = found || (o->name == option && o->category == options::Category::FileSystem);
        }
        QVERIFY(found);
    }

    void generalOptionsDontApplyToSwap()
    {
        const auto &c = options::Catalogue::builtIn();
        QVERIFY(c.find(QStringLiteral("ext4"), QStringLiteral("noatime")));
        QVERIFY(!c.find(QStringLiteral("swap"), QStringLiteral("noatime")));
        QVERIFY(c.find(QStringLiteral("swap"), QStringLiteral("nofail")));
        // mount.cifs refuses some general options (found by tests/vm/network.sh).
        QVERIFY(c.find(QStringLiteral("nfs4"), QStringLiteral("strictatime")));
        QVERIFY(!c.find(QStringLiteral("cifs"), QStringLiteral("strictatime")));
        QVERIFY(!c.find(QStringLiteral("smb3"), QStringLiteral("nouser")));
        QVERIFY(c.find(QStringLiteral("cifs"), QStringLiteral("noatime")));
        // Unknown types get the general options.
        QVERIFY(c.find(QStringLiteral("fuse.sshfs"), QStringLiteral("noatime")));
        QCOMPARE(c.family(QStringLiteral("fuse.sshfs")), options::Family::Network);
    }

    void cifsUserWithValueIsTheUserName()
    {
        const auto &c = options::Catalogue::builtIn();
        const auto *flag = c.find(QStringLiteral("cifs"), QStringLiteral("user"));
        const auto *name = c.find(QStringLiteral("cifs"), QStringLiteral("user=anna"));
        QVERIFY(flag && name);
        QCOMPARE(flag->kind, options::Kind::Flag);
        QCOMPARE(name->alias, QStringLiteral("username"));
    }

    void catalogueLoaderRejectsMistakes_data()
    {
        QTest::addColumn<QByteArray>("opts");
        QTest::addColumn<QString>("error");
        QTest::newRow("not json") << QByteArray("{") << "options.json";
        QTest::newRow("missing desc") << QByteArray(R"({"options":[{"n":"a","fs":"@all","cat":"general","k":"flag"}]})") << "missing";
        QTest::newRow("unknown fs") << QByteArray(R"({"options":[{"n":"a","fs":["zfs"],"cat":"general","k":"flag","desc":"x"}]})") << "unknown file system";
        QTest::newRow("choice without choices")
            << QByteArray(R"({"options":[{"n":"a","fs":"@all","cat":"general","k":"value","v":{"type":"choice"},"desc":"x"}]})") << "choices";
        QTest::newRow("bad example")
            << QByteArray(R"({"options":[{"n":"a","fs":"@all","cat":"general","k":"value","v":{"type":"number","ex":"x"},"desc":"x"}]})") << "example";
        QTest::newRow("twice")
            << QByteArray(R"({"options":[{"n":"a","fs":"@all","cat":"general","k":"flag","desc":"x"},{"n":"a","fs":["ext4"],"cat":"general","k":"flag","desc":"y"}]})")
            << "twice";
        QTest::newRow("alias")
            << QByteArray(R"({"options":[{"n":"a","fs":"@all","cat":"general","k":"flag","alias":"zz","desc":"x"}]})") << "alias";
    }
    void catalogueLoaderRejectsMistakes()
    {
        QFETCH(QByteArray, opts);
        QFETCH(QString, error);
        const QByteArray fs = R"({"filesystems":[{"type":"ext4","label":"ext4","family":"local","owner":"posix","description":"d"}]})";
        const auto c = options::Catalogue::fromJson(fs, opts);
        QVERIFY(!c.ok());
        QVERIFY2(c.error().contains(error), qPrintable(c.error()));
    }

    void valuesAreChecked_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("option");
        QTest::addColumn<bool>("ok");
        QTest::newRow("number ok") << "ext4" << "commit=15" << true;
        QTest::newRow("number not a number") << "ext4" << "commit=soon" << false;
        QTest::newRow("number out of range") << "ext4" << "journal_ioprio=9" << false;
        QTest::newRow("choice ok") << "ext4" << "data=journal" << true;
        QTest::newRow("choice wrong") << "ext4" << "data=fast" << false;
        QTest::newRow("octal ok") << "vfat" << "umask=0077" << true;
        QTest::newRow("octal wrong") << "vfat" << "umask=999" << false;
        QTest::newRow("size ok") << "tmpfs" << "size=25%" << true;
        QTest::newRow("size wrong") << "tmpfs" << "size=big" << false;
        QTest::newRow("seconds ok") << "ext4" << "x-systemd.device-timeout=10s" << true;
        QTest::newRow("seconds wrong") << "ext4" << "x-systemd.device-timeout=ten" << false;
        QTest::newRow("path ok") << "cifs" << "credentials=/etc/anchorage/credentials/x" << true;
        QTest::newRow("path relative") << "cifs" << "credentials=creds" << false;
        QTest::newRow("value missing") << "ext4" << "commit" << false;
        QTest::newRow("flag with value") << "ext4" << "noatime=1" << false;
        QTest::newRow("optional value absent") << "btrfs" << "compress" << true;
        QTest::newRow("optional value given") << "btrfs" << "compress=zstd:3" << true;
    }
    void valuesAreChecked()
    {
        QFETCH(QString, type);
        QFETCH(QString, option);
        QFETCH(bool, ok);
        const auto issues = options::check({option}, type);
        QCOMPARE(!fstab::hasErrors(issues), ok);
    }

    void checkWarnsAboutUnknownContradictoryAndRisky()
    {
        auto issues = options::check(L("noatim"), QStringLiteral("ext4"));
        QCOMPARE(issues.size(), 1);
        QVERIFY(issues[0].message.contains(QLatin1String("noatim")));
        QCOMPARE(issues[0].severity, fstab::Issue::Severity::Warning);

        issues = options::check(L("ro,rw"), QStringLiteral("ext4"));
        QCOMPARE(issues.size(), 1);
        QVERIFY(issues[0].message.contains(QLatin1String("contradict")));

        issues = options::check(L("password=secret"), QStringLiteral("cifs"));
        QCOMPARE(issues.size(), 1);
        QVERIFY(issues[0].message.contains(QLatin1String("credentials file")));

        QVERIFY(options::check(L("noatime,nofail,x-systemd.automount"), QStringLiteral("ext4")).isEmpty());
    }

    // ---- Access tab --------------------------------------------------------------------
    void readsSettings_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QStringList>("opts");
        QTest::addColumn<int>("start");
        QTest::addColumn<int>("who");
        QTest::addColumn<int>("sharing");
        QTest::addColumn<bool>("nofail");
        QTest::newRow("defaults") << "ext4" << L("defaults") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::Unset) << false;
        QTest::newRow("noauto user") << "ext4" << L("noauto,user,nofail") << int(Start::OnRequest) << int(Who::AnyUser) << int(Sharing::Unset) << true;
        QTest::newRow("automount users") << "ext4" << L("x-systemd.automount,users") << int(Start::WhenOpened) << int(Who::AnyUserAnyoneUnmounts)
                                         << int(Sharing::Unset) << false;
        QTest::newRow("last who wins") << "ext4" << L("user,nouser") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::Unset) << false;
        QTest::newRow("vfat owner only") << "vfat" << L("uid=1000,umask=0077") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::OwnerOnly) << false;
        QTest::newRow("vfat dmask custom") << "vfat" << L("dmask=022,fmask=133") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::Custom) << false;
        QTest::newRow("vfat odd umask") << "vfat" << L("umask=027") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::Custom) << false;
        QTest::newRow("cifs everyone") << "cifs" << L("file_mode=0666,dir_mode=0777") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::Everyone)
                                       << false;
        QTest::newRow("cifs user name is not 'any user'") << "cifs" << L("user=anna") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::Unset) << false;
        QTest::newRow("ext4 ignores masks") << "ext4" << L("umask=077") << int(Start::AtBoot) << int(Who::Admins) << int(Sharing::Unset) << false;
    }
    void readsSettings()
    {
        QFETCH(QString, type);
        QFETCH(QStringList, opts);
        QFETCH(int, start);
        QFETCH(int, who);
        QFETCH(int, sharing);
        QFETCH(bool, nofail);
        const auto s = access::read(opts, type);
        QCOMPARE(int(s.start), start);
        QCOMPARE(int(s.who), who);
        QCOMPARE(int(s.sharing), sharing);
        QCOMPARE(s.nofail, nofail);
    }

    void applyChangesOnlyWhatChanged_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QStringList>("before");
        QTest::addColumn<QStringList>("after");
        QTest::addColumn<std::function<void(access::Settings &)>>("change");
        using S = access::Settings;
        QTest::newRow("nothing") << "ext4" << L("defaults") << L("defaults") << std::function<void(S &)>([](S &) {});
        QTest::newRow("nothing keeps odd things") << "vfat" << L("auto,uid=anna,umask=027,user=x") << L("auto,uid=anna,umask=027,user=x")
                                                  << std::function<void(S &)>([](S &) {});
        QTest::newRow("on request replaces defaults") << "ext4" << L("defaults") << L("noauto") << std::function<void(S &)>([](S &s) {
            s.start = Start::OnRequest;
        });
        QTest::newRow("automount in place") << "ext4" << L("noatime,noauto,nofail") << L("noatime,noauto,x-systemd.automount,nofail")
                                            << std::function<void(S &)>([](S &s) {
                                                   s.start = Start::WhenOpened;
                                               });
        QTest::newRow("back to boot") << "ext4" << L("noatime,noauto,x-systemd.automount,nofail") << L("noatime,nofail")
                                      << std::function<void(S &)>([](S &s) {
                                             s.start = Start::AtBoot;
                                         });
        QTest::newRow("user to users in place") << "ext4" << L("noatime,user,exec") << L("noatime,users,exec") << std::function<void(S &)>([](S &s) {
            s.who = Who::AnyUserAnyoneUnmounts;
        });
        QTest::newRow("admins removes user") << "ext4" << L("user,exec") << L("exec") << std::function<void(S &)>([](S &s) {
            s.who = Who::Admins;
        });
        QTest::newRow("cifs username untouched") << "cifs" << L("user=anna,guest") << L("user=anna,guest,users") << std::function<void(S &)>([](S &s) {
            s.who = Who::AnyUserAnyoneUnmounts;
        });
        QTest::newRow("boot safety") << "nfs4" << L("defaults") << L("nofail,_netdev") << std::function<void(S &)>([](S &s) {
            s.nofail = true;
            s.netdev = true;
        });
        QTest::newRow("vfat owner") << "vfat" << L("noatime") << L("noatime,uid=1000,gid=1000,umask=077") << std::function<void(S &)>([](S &s) {
            s.uid = 1000;
            s.gid = 1000;
            s.sharing = Sharing::OwnerOnly;
        });
        QTest::newRow("vfat custom masks replaced") << "vfat" << L("dmask=022,fmask=133,noatime") << L("umask=000,noatime")
                                                    << std::function<void(S &)>([](S &s) {
                                                           s.sharing = Sharing::Everyone;
                                                       });
        QTest::newRow("vfat owner removed") << "vfat" << L("uid=1000,gid=1000,umask=022") << L("umask=022") << std::function<void(S &)>([](S &s) {
            s.uid.reset();
            s.gid.reset();
        });
        QTest::newRow("cifs modes") << "cifs" << L("guest") << L("guest,file_mode=0600,dir_mode=0700") << std::function<void(S &)>([](S &s) {
            s.sharing = Sharing::OwnerOnly;
        });
        QTest::newRow("ext4 has no owner options") << "ext4" << L("defaults") << L("defaults") << std::function<void(S &)>([](S &s) {
            s.uid = 1000;
            s.sharing = Sharing::OwnerOnly;
        });
        QTest::newRow("swap can't automount") << "swap" << L("sw") << L("sw,noauto") << std::function<void(S &)>([](S &s) {
            s.start = Start::WhenOpened;
            s.who = Who::AnyUser;
            s.netdev = true;
        });
    }
    void applyChangesOnlyWhatChanged()
    {
        QFETCH(QString, type);
        QFETCH(QStringList, before);
        QFETCH(QStringList, after);
        QFETCH(std::function<void(access::Settings &)>, change);
        access::Settings s = access::read(before, type);
        change(s);
        QCOMPARE(access::apply(before, s, type), after);
    }

    void applyThenReadRoundTrips()
    {
        const QStringList types = {QStringLiteral("ext4"), QStringLiteral("vfat"), QStringLiteral("cifs"), QStringLiteral("nfs4"), QStringLiteral("swap")};
        for (const QString &type : types) {
            for (int start = 0; start < 3; ++start) {
                for (int who = 0; who < 3; ++who) {
                    for (int sharing = 0; sharing < 4; ++sharing) {
                        for (int flags = 0; flags < 4; ++flags) {
                            access::Settings s;
                            s.start = Start(start);
                            s.who = Who(who);
                            s.sharing = Sharing(sharing);
                            s.nofail = flags & 1;
                            s.netdev = flags & 2;
                            s.uid = 1000;
                            s.gid = 100;
                            const QStringList opts = access::apply(L("noatime"), s, type);
                            QVERIFY(!fstab::hasErrors(options::check(opts, type)));
                            const auto can = access::applicable(type);
                            // Compared field by field (copying and resetting optionals
                            // trips a GCC 15 -Wmaybe-uninitialized false positive).
                            const auto back = access::read(opts, type);
                            const QString what = type + QLatin1Char(' ') + opts.join(QLatin1Char(','));
                            const Start wantStart = (!can.automount && s.start == Start::WhenOpened) ? Start::OnRequest : s.start;
                            QVERIFY2(back.start == wantStart, qPrintable(what));
                            QVERIFY2(back.who == (can.who ? s.who : Who::Admins), qPrintable(what));
                            QVERIFY2(back.netdev == (can.netdev && s.netdev), qPrintable(what));
                            QVERIFY2(back.nofail == s.nofail, qPrintable(what));
                            QVERIFY2(back.uid.has_value() == can.owner && back.gid.has_value() == can.owner, qPrintable(what));
                            if (can.owner) {
                                QVERIFY2(*back.uid == 1000 && *back.gid == 100, qPrintable(what));
                            }
                            QVERIFY2(back.sharing == (can.owner ? s.sharing : Sharing::Unset), qPrintable(what));
                            QVERIFY(opts.contains(QStringLiteral("noatime")));
                        }
                    }
                }
            }
        }
    }

    void defaultsForNewEntries()
    {
        auto s = access::defaultsFor(QStringLiteral("ext4"));
        QVERIFY(s.nofail);
        QVERIFY(!s.netdev);
        QCOMPARE(s.start, Start::AtBoot);
        s = access::defaultsFor(QStringLiteral("cifs"));
        QVERIFY(s.nofail && s.netdev);
        QCOMPARE(s.start, Start::WhenOpened);
    }

    // ---- line diff -------------------------------------------------------------------
    void lineDiff()
    {
        const QByteArray a = "# c\nA\nB\nC\n";
        const QByteArray b = "# c\nA\nB2\nC\nD\n";
        const auto d = linediff::diff(a, b);
        QCOMPARE(linediff::changeCount(d), 3);
        QStringList kinds;
        for (const auto &l : d) {
            kinds.append(QString::number(int(l.kind)) + QString::fromUtf8(l.text));
        }
        QCOMPARE(kinds, QStringList({QStringLiteral("0# c"), QStringLiteral("0A"), QStringLiteral("1B"), QStringLiteral("2B2"), QStringLiteral("0C"),
                                     QStringLiteral("2D")}));
        QCOMPARE(linediff::changeCount(linediff::diff(a, a)), 0);
        QCOMPARE(linediff::changeCount(linediff::diff(QByteArray(), b)), 5);
    }

    void sideBySide()
    {
        using linediff::Row;
        const QByteArray now = "# fstab\nUUID=1 / btrfs defaults 0 0\n\nLABEL=new /mnt/new ext4 nofail 0 0\ntmpfs /tmp tmpfs defaults 0 0\n";
        const QByteArray backup = "# fstab\nUUID=1 / btrfs noatime 0 0\n\ntmpfs /tmp tmpfs defaults 0 0\n//nas/x /mnt/x cifs guest 0 0\n";
        const auto rows = linediff::sideBySide(now, backup);
        QStringList kinds;
        for (const auto &r : rows) {
            kinds.append(QString::number(int(r.kind)));
        }
        // same, changed (options), same (blank), removed (left only), same, added (right only)
        QCOMPARE(kinds.join(QLatin1Char(' ')), QStringLiteral("0 3 0 1 0 2"));
        const Row &changed = rows.at(1);
        QCOMPARE(changed.left.text(), QStringLiteral("UUID=1 / btrfs defaults 0 0"));
        QCOMPARE(changed.right.text(), QStringLiteral("UUID=1 / btrfs noatime 0 0"));
        QStringList leftChanged;
        QStringList rightChanged;
        for (const auto &seg : changed.left.segments) {
            if (seg.changed) {
                leftChanged.append(seg.text);
            }
        }
        for (const auto &seg : changed.right.segments) {
            if (seg.changed) {
                rightChanged.append(seg.text);
            }
        }
        QVERIFY(!leftChanged.isEmpty() && !rightChanged.isEmpty());
        QVERIFY(leftChanged.join(QString()).size() < 9); // only the differing letters, not the whole line
        // Fillers are opposite removed and added lines; real blank lines are present.
        QVERIFY(rows.at(3).left.present && !rows.at(3).right.present);
        QVERIFY(!rows.at(5).left.present && rows.at(5).right.present);
        QVERIFY(rows.at(2).left.present && rows.at(2).left.text().isEmpty());
        QCOMPARE(rows.at(5).right.line, 5);
        // Unrelated lines are not paired.
        const auto unrelated = linediff::sideBySide("aaaa bbbb\n", "zzzz yyyy\n");
        QCOMPARE(unrelated.size(), 2);
        QCOMPARE(unrelated.at(0).kind, Row::Kind::Removed);
        QCOMPARE(unrelated.at(1).kind, Row::Kind::Added);
        QCOMPARE(linediff::sideBySide(now, now).size(), 5);
    }

    // ---- findmnt ---------------------------------------------------------------------
    void parsesFindmntOutput()
    {
        const QByteArray out = "0 parse errors, 2 errors, 1 warning\n"
                               "/mnt/x\n"
                               "   [E] unreachable on boot required source: UUID=dead-beef\n"
                               "   [ ] FS options: uid=1000\n"
                               "/mnt/y\n"
                               "   [W] unreachable source: /dev/sda9: No such file or directory\n";
        const auto f = verify::parseFindmntOutput(out);
        QCOMPARE(f.size(), 2);
        QCOMPARE(f[0].target, QStringLiteral("/mnt/x"));
        QVERIFY(f[0].error);
        QCOMPARE(f[1].target, QStringLiteral("/mnt/y"));
        QVERIFY(!f[1].error);
    }

    void runsFindmnt()
    {
        if (!QFile::exists(QStringLiteral("/usr/bin/findmnt"))) {
            QSKIP("findmnt not installed");
        }
        const auto f = verify::findmnt("UUID=0000-dead /mnt/anchorage-test-none vfat nofail 0 0\n");
        QVERIFY(f.ok());
        bool sourceReported = false;
        for (const auto &x : *f) {
            sourceReported = sourceReported || (x.target == QLatin1String("/mnt/anchorage-test-none") && x.message.contains(QLatin1String("source")));
        }
        QVERIFY(sourceReported);
    }
};

QTEST_GUILESS_MAIN(OptionsTest)
#include "optionstest.moc"
