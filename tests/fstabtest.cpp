// SPDX-License-Identifier: GPL-3.0-or-later
#include "fstab.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace anchorage::fstab;

Q_DECLARE_METATYPE(anchorage::fstab::Entry)

namespace
{
QByteArray readData(const QString &name)
{
    QFile f(QStringLiteral(TEST_DATA_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("cannot open test data %s", qPrintable(name));
    }
    return f.readAll();
}

QStringList dataFiles()
{
    return QDir(QStringLiteral(TEST_DATA_DIR)).entryList({QStringLiteral("*.fstab")}, QDir::Files, QDir::Name);
}

Entry entryAt(const File &f, int line)
{
    return *f.lines().at(static_cast<std::size_t>(line)).entry;
}

// Byte column where field `n` (0-based) starts in a line.
qsizetype columnOf(const QByteArray &line, int n)
{
    return parseLine(line).columns.at(static_cast<std::size_t>(n));
}
} // namespace

class FstabTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- lossless round trip -------------------------------------------------------
    void roundTripIsByteExact_data()
    {
        QTest::addColumn<QByteArray>("data");
        for (const QString &name : dataFiles()) {
            QTest::newRow(qPrintable(name)) << readData(name);
        }
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("only newline") << QByteArray("\n");
        QTest::newRow("no final newline") << QByteArray("tmpfs /tmp tmpfs defaults 0 0");
        QTest::newRow("blank lines at end") << QByteArray("tmpfs /tmp tmpfs defaults 0 0\n\n\n");
        QTest::newRow("CRLF line endings") << QByteArray("# windows\r\ntmpfs /tmp tmpfs defaults 0 0\r\n");
        QTest::newRow("invalid UTF-8") << QByteArray("/dev/sda1 /mnt/\xff\xfe ext4 defaults 0 0\n");
        static constexpr char withNul[] = "/dev/sda1 /mnt/a\0b ext4 defaults 0 0\n";
        QTest::newRow("NUL byte") << QByteArray(withNul, sizeof(withNul) - 1);
        QTest::newRow("lone backslash") << QByteArray("/dev/sda1 /mnt/a\\ ext4 defaults 0 0\n");
    }
    void roundTripIsByteExact()
    {
        QFETCH(QByteArray, data);
        QCOMPARE(File::parse(data).serialize(), data);
    }

    // ---- parsing -------------------------------------------------------------------
    void parsesKubuntuDefault()
    {
        const File f = File::parse(readData(QStringLiteral("kubuntu-default.fstab")));
        const auto entries = f.entryLines();
        QCOMPARE(entries.size(), 6u);
        QCOMPARE(f.lines().front().kind, LineKind::Comment);

        const Entry efi = entryAt(f, entries[0]);
        QCOMPARE(efi.spec, QStringLiteral("UUID=1A2B-3C4D"));
        QCOMPARE(efi.mountPoint, QStringLiteral("/boot/efi"));
        QCOMPARE(efi.type, QStringLiteral("vfat"));
        QVERIFY(efi.options.isEmpty()); // "defaults"
        QCOMPARE(efi.pass, 2);

        const Entry root = entryAt(f, entries[1]);
        QCOMPARE(root.options, (QStringList{QStringLiteral("subvol=/@"), QStringLiteral("defaults"), QStringLiteral("noatime"), QStringLiteral("autodefrag")}));

        const Entry swap = entryAt(f, entries[4]);
        QCOMPARE(swap.mountPoint, QStringLiteral("swap"));
        QVERIFY(!hasErrors(validate(swap))); // "swap" as mount point is valid for swap
    }

    void parsesMessyFile()
    {
        const File f = File::parse(readData(QStringLiteral("messy.fstab")));
        const int usb = f.findMountPoint(QStringLiteral("/media/My USB Stick"));
        QVERIFY(usb >= 0);
        QCOMPARE(entryAt(f, usb).spec, QStringLiteral("/dev/sdc1"));

        const int odd = f.findMountPoint(QStringLiteral("/mnt/odd"));
        QCOMPARE(entryAt(f, odd).spec, QStringLiteral("LABEL=Back\\slash"));

        // 4 fields: dump and pass default to 0
        const Entry lin = entryAt(f, f.findMountPoint(QStringLiteral("/mnt/lingames")));
        QCOMPARE(lin.options, (QStringList{QStringLiteral("compress=zstd"), QStringLiteral("nofail")}));
        QCOMPARE(lin.dump, 0);
        QCOMPARE(lin.pass, 0);

        // 3 fields: no options at all
        QVERIFY(entryAt(f, f.findMountPoint(QStringLiteral("/mnt/x"))).options.isEmpty());

        // Text after the 6th field is ignored (like libmount) but kept
        const int proc = f.findMountPoint(QStringLiteral("/proc"));
        QVERIFY(f.lines().at(static_cast<std::size_t>(proc)).trailing.contains("# comments after field 6"));

        // Commented-out entries are comments, not entries
        QCOMPARE(f.findMountPoint(QStringLiteral("/mnt/disabled")), -1);
    }

    void brokenLinesAreKeptAndExplained()
    {
        const File f = File::parse(readData(QStringLiteral("broken.fstab")));
        QCOMPARE(f.lines().at(2).kind, LineKind::Invalid);
        QVERIFY(f.lines().at(2).error.contains(QStringLiteral("at least 3 fields")));
        QCOMPARE(f.lines().at(3).kind, LineKind::Invalid);
        QVERIFY(f.lines().at(3).error.contains(QStringLiteral("dump")));
        // libmount accepts a negative pass, so it parses; validation rejects it
        QCOMPARE(f.lines().at(4).kind, LineKind::Entry);
        QVERIFY(hasErrors(validate(*f.lines().at(4).entry)));
        QCOMPARE(f.entryLines().size(), 3u);
    }

    void invalidUtf8IsAnInvalidLine()
    {
        const File f = File::parse(QByteArray("/dev/sda1 /mnt/\xff ext4 defaults 0 0\n"));
        QCOMPARE(f.lines().front().kind, LineKind::Invalid);
    }

    // ---- escaping ------------------------------------------------------------------
    void escapingRoundTrips_data()
    {
        QTest::addColumn<QString>("value");
        QTest::newRow("space") << QStringLiteral("/mnt/My Games");
        QTest::newRow("tab") << QStringLiteral("/mnt/a\tb");
        QTest::newRow("backslash") << QStringLiteral("LABEL=Back\\slash");
        QTest::newRow("backslash digits") << QStringLiteral("/mnt/\\040literal");
        QTest::newRow("unicode") << QStringLiteral("/mnt/Spiele für Kinder 🎮");
    }
    void escapingRoundTrips()
    {
        QFETCH(QString, value);
        const QByteArray enc = encodeField(value);
        QVERIFY(!enc.contains(' '));
        QVERIFY(!enc.contains('\t'));
        QCOMPARE(decodeField(enc), std::optional<QString>(value));
    }

    // ---- editing -------------------------------------------------------------------
    void replaceTouchesOnlyThatLineAndKeepsColumns()
    {
        const QByteArray original = readData(QStringLiteral("kubuntu-default.fstab"));
        File f = File::parse(original);
        const int home = f.findMountPoint(QStringLiteral("/home"));
        Entry e = entryAt(f, home);
        e.options.append(QStringLiteral("compress=zstd"));
        QVERIFY(f.replace(home, e));

        const QList<QByteArray> before = original.split('\n');
        const QList<QByteArray> after = f.serialize().split('\n');
        QCOMPARE(after.size(), before.size());
        for (qsizetype i = 0; i < before.size(); ++i) {
            if (i == home) {
                QVERIFY(after[i] != before[i]);
                for (int field = 0; field < 4; ++field) {
                    QCOMPARE(columnOf(after[i], field), columnOf(before[i], field));
                }
                QVERIFY(after[i].contains("autodefrag,compress=zstd"));
            } else {
                QCOMPARE(after[i], before[i]);
            }
        }
        QCOMPARE(entryAt(File::parse(f.serialize()), home), e);
    }

    void replaceKeepsTabSeparators()
    {
        File f = File::parse(readData(QStringLiteral("messy.fstab")));
        const int data = f.findMountPoint(QStringLiteral("/data"));
        Entry e = entryAt(f, data);
        e.mountPoint = QStringLiteral("/srv/data");
        QVERIFY(f.replace(data, e));
        QCOMPARE(f.lines().at(static_cast<std::size_t>(data)).raw, QByteArray("\tUUID=aaaa-bbbb\t/srv/data\text4\tdefaults,noatime\t0\t2"));
    }

    void replaceKeepsShortLinesShort()
    {
        File f = File::parse(readData(QStringLiteral("messy.fstab")));
        const int lin = f.findMountPoint(QStringLiteral("/mnt/lingames"));
        Entry e = entryAt(f, lin);
        e.options.append(QStringLiteral("noatime"));
        QVERIFY(f.replace(lin, e));
        QCOMPARE(f.lines().at(static_cast<std::size_t>(lin)).raw, QByteArray("  LABEL=Lingames   /mnt/lingames btrfs compress=zstd,nofail,noatime"));
    }

    void replaceKeepsIgnoredTrailingText()
    {
        File f = File::parse(readData(QStringLiteral("messy.fstab")));
        const int proc = f.findMountPoint(QStringLiteral("/proc"));
        Entry e = entryAt(f, proc);
        e.options = {QStringLiteral("hidepid=2")};
        QVERIFY(f.replace(proc, e));
        QVERIFY(f.lines().at(static_cast<std::size_t>(proc)).raw.endsWith("   # comments after field 6 are ignored by mount"));
    }

    void appendAlignsWithLastEntry()
    {
        File f = File::parse(readData(QStringLiteral("kubuntu-default.fstab")));
        const int lastBefore = f.entryLines().back();
        const Entry e{QStringLiteral("LABEL=Lingames"), QStringLiteral("/mnt/lingames"), QStringLiteral("btrfs"), {QStringLiteral("nofail")}, 0, 0};
        const int at = f.append(e);
        QCOMPARE(at, lastBefore + 1);
        const QByteArray added = f.lines().at(static_cast<std::size_t>(at)).raw;
        const QByteArray reference = f.lines().at(static_cast<std::size_t>(lastBefore)).raw;
        for (int field = 0; field < 4; ++field) {
            QCOMPARE(columnOf(added, field), columnOf(reference, field));
        }
        QVERIFY(f.serialize().endsWith('\n'));
        QCOMPARE(entryAt(File::parse(f.serialize()), at), e);
    }

    void appendToEmptyFile()
    {
        File f = File::parse(QByteArray());
        const Entry e{QStringLiteral("tmpfs"), QStringLiteral("/tmp"), QStringLiteral("tmpfs"), {}, 0, 0};
        QCOMPARE(f.append(e), 0);
        QCOMPARE(f.serialize(), QByteArray("tmpfs  /tmp  tmpfs  defaults  0  0\n"));
    }

    void removeEntry()
    {
        File f = File::parse(readData(QStringLiteral("network.fstab")));
        const int games = f.findMountPoint(QStringLiteral("/mnt/games"));
        QVERIFY(f.remove(games));
        QCOMPARE(f.findMountPoint(QStringLiteral("/mnt/games")), -1);
        QVERIFY(!f.remove(0)); // comments can't be removed through this API
    }

    // ---- validation and injection --------------------------------------------------
    void rejectsUnsafeEntries_data()
    {
        QTest::addColumn<Entry>("entry");
        QTest::addColumn<QString>("field");
        const Entry ok{QStringLiteral("/dev/sdb1"), QStringLiteral("/mnt/x"), QStringLiteral("ext4"), {}, 0, 0};
        auto with = [&ok](auto change) {
            Entry e = ok;
            change(e);
            return e;
        };
        QTest::newRow("newline injects a 2nd line") << with([](Entry &e) { e.spec = QStringLiteral("/dev/sdb1 /mnt/x ext4 defaults 0 0\n/dev/evil"); }) << QStringLiteral("spec");
        QTest::newRow("# starts a comment") << with([](Entry &e) { e.mountPoint = QStringLiteral("/mnt/#x"); }) << QStringLiteral("mountPoint");
        QTest::newRow("relative mount point") << with([](Entry &e) { e.mountPoint = QStringLiteral("mnt/x"); }) << QStringLiteral("mountPoint");
        QTest::newRow("empty spec") << with([](Entry &e) { e.spec.clear(); }) << QStringLiteral("spec");
        QTest::newRow("bad type") << with([](Entry &e) { e.type = QStringLiteral("ext4 defaults"); }) << QStringLiteral("type");
        QTest::newRow("empty option") << with([](Entry &e) { e.options = {QStringLiteral("rw"), QString()}; }) << QStringLiteral("options");
        QTest::newRow("comma in option") << with([](Entry &e) { e.options = {QStringLiteral("rw,exec")}; }) << QStringLiteral("options");
        QTest::newRow("C1 control NEL") << with([](Entry &e) { e.options = {QStringLiteral("rw\u0085x")}; }) << QStringLiteral("options");
        QTest::newRow("Unicode line separator") << with([](Entry &e) { e.spec = QStringLiteral("/dev/a b"); }) << QStringLiteral("spec");
        QTest::newRow("carriage return") << with([](Entry &e) { e.mountPoint = QStringLiteral("/mnt/x\r"); }) << QStringLiteral("mountPoint");
        QTest::newRow("negative pass") << with([](Entry &e) { e.pass = -1; }) << QStringLiteral("pass");
        QTest::newRow("huge dump") << with([](Entry &e) { e.dump = 10; }) << QStringLiteral("dump");
        QTest::newRow("over-long line") << with([](Entry &e) { e.options = {QString(5000, QLatin1Char('a'))}; }) << QStringLiteral("line");
    }
    void rejectsUnsafeEntries()
    {
        QFETCH(Entry, entry);
        QFETCH(QString, field);
        const QList<Issue> issues = validate(entry);
        QVERIFY(hasErrors(issues));
        QVERIFY(std::any_of(issues.cbegin(), issues.cend(), [&](const Issue &i) { return i.field == field; }));

        const QByteArray original = readData(QStringLiteral("kubuntu-default.fstab"));
        File f = File::parse(original);
        QCOMPARE(f.append(entry), -1);
        QVERIFY(!f.replace(f.entryLines().front(), entry));
        QCOMPARE(f.serialize(), original); // nothing changed
    }

    void loadRejectsOversizedFiles()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("big.fstab"));
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QByteArray(maxFileSize + 1, '#'));
        out.close();
        QString error;
        QVERIFY(!File::load(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!File::load(dir.filePath(QStringLiteral("missing")), &error));
    }

    // ---- cross-checks against the real system tools --------------------------------
    // Our idea of a parse error must match libmount's for every sample line.
    void parseErrorsMatchLibmount_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<bool>("weSayInvalid");
        for (const QString &name : dataFiles()) {
            const File f = File::parse(readData(name));
            int n = 0;
            for (const Line &l : f.lines()) {
                ++n;
                if (l.kind == LineKind::Entry || l.kind == LineKind::Invalid) {
                    QTest::addRow("%s:%d", qPrintable(name), n) << l.raw << (l.kind == LineKind::Invalid);
                }
            }
        }
    }
    void parseErrorsMatchLibmount()
    {
        const QString findmnt = QStandardPaths::findExecutable(QStringLiteral("findmnt"));
        if (findmnt.isEmpty()) {
            QSKIP("findmnt not installed");
        }
        QFETCH(QByteArray, line);
        QFETCH(bool, weSayInvalid);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("one.fstab"));
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        // A valid first line, then the line under test. (findmnt 2.41.3 crashes after
        // reporting a parse error, so only its message is used, not the exit code.)
        out.write("tmpfs /x tmpfs defaults 0 0\n" + line + '\n');
        out.close();
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(findmnt, {QStringLiteral("--tab-file"), path, QStringLiteral("-rn")});
        QVERIFY(p.waitForFinished(10000));
        const bool libmountSaysInvalid = p.readAll().contains("parse error at line 2");
        QCOMPARE(weSayInvalid, libmountSaysInvalid);
    }

    // Edited files must still be accepted by findmnt --verify without parse errors.
    void findmntAcceptsEditedFile()
    {
        const QString findmnt = QStandardPaths::findExecutable(QStringLiteral("findmnt"));
        if (findmnt.isEmpty()) {
            QSKIP("findmnt not installed");
        }
        File f = File::parse(readData(QStringLiteral("kubuntu-default.fstab")));
        f.append({QStringLiteral("//nas.local/My Games"), QStringLiteral("/mnt/my games"), QStringLiteral("cifs"),
                  {QStringLiteral("guest"), QStringLiteral("nofail"), QStringLiteral("x-systemd.automount")}, 0, 0});
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("edited.fstab"));
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(f.serialize());
        out.close();
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(findmnt, {QStringLiteral("--verify"), QStringLiteral("--tab-file"), path});
        QVERIFY(p.waitForFinished(10000));
        const QByteArray output = p.readAll();
        QVERIFY2(output.contains("0 parse errors"), output.constData());
    }

    // systemd must read our options the way we mean them (x-systemd.automount here).
    void systemdUnderstandsWrittenFile()
    {
        const QString generator = QStringLiteral("/usr/lib/systemd/system-generators/systemd-fstab-generator");
        if (!QFile::exists(generator)) {
            QSKIP("systemd-fstab-generator not available");
        }
        File f = File::parse(QByteArray());
        QVERIFY(f.append({QStringLiteral("nas.local:/export/media"), QStringLiteral("/mnt/media"), QStringLiteral("nfs4"),
                          {QStringLiteral("_netdev"), QStringLiteral("nofail"), QStringLiteral("noauto"), QStringLiteral("x-systemd.automount")}, 0, 0}) >= 0);
        QVERIFY(f.append({QStringLiteral("/dev/disk/by-label/Lingames"), QStringLiteral("/mnt/lin games"), QStringLiteral("btrfs"),
                          {QStringLiteral("nofail")}, 0, 0}) >= 0);
        QTemporaryDir dir;
        const QString fstabPath = dir.filePath(QStringLiteral("fstab"));
        QFile out(fstabPath);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(f.serialize());
        out.close();
        const QString unitDir = dir.filePath(QStringLiteral("units"));
        QVERIFY(QDir().mkpath(unitDir));

        QProcess p;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SYSTEMD_FSTAB"), fstabPath);
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(generator, {unitDir, unitDir, unitDir});
        QVERIFY(p.waitForFinished(10000));
        QVERIFY2(p.exitCode() == 0, p.readAll().constData());

        QVERIFY(QFile::exists(unitDir + QStringLiteral("/mnt-media.automount")));
        QVERIFY(QFile::exists(unitDir + QStringLiteral("/mnt-media.mount")));
        // The space in the mount point must reach systemd as a space (escaped as \x20).
        QVERIFY(QFile::exists(unitDir + QStringLiteral("/mnt-lin\\x20games.mount")));
    }
};

QTEST_GUILESS_MAIN(FstabTest)
#include "fstabtest.moc"
