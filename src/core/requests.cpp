// SPDX-License-Identifier: GPL-3.0-or-later
#include "requests.h"

#include <QSet>

#include <QRegularExpression>

namespace anchorage::requests
{

namespace
{
const QString kBaseline = QStringLiteral("baseline");
const QString kContent = QStringLiteral("content");
const QString kConfirm = QStringLiteral("confirmProtected");
const QString kCreate = QStringLiteral("createMountPoints");
const QString kSpec = QStringLiteral("spec");
const QString kMountPoint = QStringLiteral("mountPoint");
const QString kMountPoints = QStringLiteral("mountPoints");
const QString kType = QStringLiteral("type");
const QString kOptions = QStringLiteral("options");
const QString kDump = QStringLiteral("dump");
const QString kPass = QStringLiteral("pass");
const QString kBackup = QStringLiteral("backup");
const QString kKeepBackups = QStringLiteral("keepBackups");
const QString kNtfsDriver = QStringLiteral("ntfsDriver");
const QString kConfirmSystem = QStringLiteral("confirmSystem");
const QString kServer = QStringLiteral("server");
const QString kUser = QStringLiteral("user");
const QString kPassword = QStringLiteral("password");
const QString kDomain = QStringLiteral("domain");

// Every key present, nothing else.
QString checkKeys(const QVariantMap &args, const QStringList &required, const QStringList &optional = {})
{
    for (const QString &k : required) {
        if (!args.contains(k)) {
            return QStringLiteral("Missing \"%1\" in request.").arg(k);
        }
    }
    for (auto it = args.cbegin(); it != args.cend(); ++it) {
        if (!required.contains(it.key()) && !optional.contains(it.key())) {
            return QStringLiteral("Unexpected \"%1\" in request.").arg(it.key());
        }
    }
    return {};
}

bool is(const QVariant &v, QMetaType::Type t)
{
    return v.metaType().id() == t;
}
} // namespace

Result<WriteFstab> decodeWriteFstab(const QVariantMap &args)
{
    using R = Result<WriteFstab>;
    if (const QString e = checkKeys(args, {kBaseline, kContent, kConfirm, kCreate}); !e.isEmpty()) {
        return R::failure(e);
    }
    if (!is(args[kBaseline], QMetaType::QByteArray) || !is(args[kContent], QMetaType::QByteArray) || !is(args[kConfirm], QMetaType::Bool)
        || !is(args[kCreate], QMetaType::Bool)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    WriteFstab w;
    w.baselineFingerprint = args[kBaseline].toByteArray();
    w.content = args[kContent].toByteArray();
    w.confirmProtected = args[kConfirm].toBool();
    w.createMountPoints = args[kCreate].toBool();
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hex.match(QString::fromLatin1(w.baselineFingerprint)).hasMatch()) {
        return R::failure(QStringLiteral("The file fingerprint is malformed."));
    }
    if (w.content.size() > fstab::maxFileSize) {
        return R::failure(QStringLiteral("The new file is too large."));
    }
    return w;
}

Result<fstab::Entry> decodeEntry(const QVariantMap &args)
{
    using R = Result<fstab::Entry>;
    if (const QString e = checkKeys(args, {kSpec, kMountPoint, kType, kOptions, kDump, kPass}); !e.isEmpty()) {
        return R::failure(e);
    }
    if (!is(args[kSpec], QMetaType::QString) || !is(args[kMountPoint], QMetaType::QString) || !is(args[kType], QMetaType::QString)
        || !is(args[kOptions], QMetaType::QStringList) || !is(args[kDump], QMetaType::Int) || !is(args[kPass], QMetaType::Int)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    fstab::Entry e;
    e.spec = args[kSpec].toString();
    e.mountPoint = args[kMountPoint].toString();
    e.type = args[kType].toString();
    e.options = args[kOptions].toStringList();
    e.dump = args[kDump].toInt();
    e.pass = args[kPass].toInt();
    if (e.options.size() > 64) {
        return R::failure(QStringLiteral("Too many options."));
    }
    return e;
}

Result<QString> decodeMountPoint(const QVariantMap &args)
{
    using R = Result<QString>;
    if (const QString e = checkKeys(args, {kMountPoint}); !e.isEmpty()) {
        return R::failure(e);
    }
    if (!is(args[kMountPoint], QMetaType::QString)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    return args[kMountPoint].toString();
}

Result<Restore> decodeRestore(const QVariantMap &args)
{
    using R = Result<Restore>;
    if (const QString e = checkKeys(args, {kBackup, kConfirm}); !e.isEmpty()) {
        return R::failure(e);
    }
    if (!is(args[kBackup], QMetaType::QString) || !is(args[kConfirm], QMetaType::Bool)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    return Restore{args[kBackup].toString(), args[kConfirm].toBool()};
}

Result<settings::Settings> decodeSettings(const QVariantMap &args)
{
    using R = Result<settings::Settings>;
    if (const QString e = checkKeys(args, {kKeepBackups, kNtfsDriver}); !e.isEmpty()) {
        return R::failure(e);
    }
    if (!is(args[kKeepBackups], QMetaType::Int) || !is(args[kNtfsDriver], QMetaType::QString)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    settings::Settings s;
    s.keepBackups = args[kKeepBackups].toInt();
    s.ntfsDriver = args[kNtfsDriver].toString();
    if (const QStringList errors = settings::validate(s); !errors.isEmpty()) {
        return R::failure(errors.join(QLatin1Char(' ')));
    }
    return s;
}

Result<TemporaryMount> decodeTemporaryMount(const QVariantMap &args)
{
    using R = Result<TemporaryMount>;
    if (const QString e = checkKeys(args, {kSpec, kMountPoint, kType, kOptions, kConfirm}); !e.isEmpty()) {
        return R::failure(e);
    }
    if (!is(args[kSpec], QMetaType::QString) || !is(args[kMountPoint], QMetaType::QString) || !is(args[kType], QMetaType::QString)
        || !is(args[kOptions], QMetaType::QStringList) || !is(args[kConfirm], QMetaType::Bool)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    TemporaryMount t;
    t.entry.spec = args[kSpec].toString();
    t.entry.mountPoint = args[kMountPoint].toString();
    t.entry.type = args[kType].toString();
    t.entry.options = args[kOptions].toStringList();
    t.confirmProtected = args[kConfirm].toBool();
    if (t.entry.options.size() > 64) {
        return R::failure(QStringLiteral("Too many options."));
    }
    return t;
}

namespace
{
// A non-empty list of at most kMaxMountPoints strings without repeats. The paths themselves
// are checked by the mount rules, one by one.
Result<QStringList> mountPointList(const QVariant &v)
{
    using R = Result<QStringList>;
    if (!is(v, QMetaType::QStringList)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    const QStringList list = v.toStringList();
    if (list.isEmpty() || list.size() > kMaxMountPoints) {
        return R::failure(QStringLiteral("A request names 1 to %1 mount points.").arg(kMaxMountPoints));
    }
    if (QSet<QString>(list.cbegin(), list.cend()).size() != list.size()) {
        return R::failure(QStringLiteral("A mount point is named twice."));
    }
    return list;
}
} // namespace

Result<QStringList> decodeMountPoints(const QVariantMap &args)
{
    if (const QString e = checkKeys(args, {kMountPoints}); !e.isEmpty()) {
        return Result<QStringList>::failure(e);
    }
    return mountPointList(args[kMountPoints]);
}

Result<Unmount> decodeUnmount(const QVariantMap &args)
{
    using R = Result<Unmount>;
    if (const QString e = checkKeys(args, {kMountPoints, kConfirmSystem}); !e.isEmpty()) {
        return R::failure(e);
    }
    if (!is(args[kConfirmSystem], QMetaType::Bool)) {
        return R::failure(QStringLiteral("Request fields have the wrong type."));
    }
    auto list = mountPointList(args[kMountPoints]);
    if (!list) {
        return R::failure(list.error());
    }
    return Unmount{*list, args[kConfirmSystem].toBool()};
}

Result<Credentials> decodeCredentials(const QVariantMap &args)
{
    using R = Result<Credentials>;
    if (const QString e = checkKeys(args, {kServer, kUser, kPassword, kDomain}); !e.isEmpty()) {
        return R::failure(e);
    }
    for (const QString &k : {kServer, kUser, kPassword, kDomain}) {
        if (!is(args[k], QMetaType::QString)) {
            return R::failure(QStringLiteral("Request fields have the wrong type."));
        }
    }
    return Credentials{args[kServer].toString(), args[kUser].toString(), args[kPassword].toString(), args[kDomain].toString()};
}

QVariantMap encodeCredentials(const Credentials &c)
{
    return {{kServer, c.server}, {kUser, c.user}, {kPassword, c.password}, {kDomain, c.domain}};
}

QVariantMap encodeTemporaryMount(const TemporaryMount &t)
{
    return {{kSpec, t.entry.spec}, {kMountPoint, t.entry.mountPoint}, {kType, t.entry.type}, {kOptions, t.entry.options}, {kConfirm, t.confirmProtected}};
}

QVariantMap encodeUnmount(const Unmount &u)
{
    return {{kMountPoints, u.mountPoints}, {kConfirmSystem, u.confirmSystem}};
}

QVariantMap encodeMountPoints(const QStringList &mountPoints)
{
    return {{kMountPoints, mountPoints}};
}

QVariantMap encodeSettings(const settings::Settings &s)
{
    return {{kKeepBackups, s.keepBackups}, {kNtfsDriver, s.ntfsDriver}};
}

QVariantMap encodeWriteFstab(const WriteFstab &r)
{
    return {{kBaseline, r.baselineFingerprint}, {kContent, r.content}, {kConfirm, r.confirmProtected}, {kCreate, r.createMountPoints}};
}

QVariantMap encodeEntry(const fstab::Entry &e)
{
    return {{kSpec, e.spec}, {kMountPoint, e.mountPoint}, {kType, e.type}, {kOptions, e.options}, {kDump, e.dump}, {kPass, e.pass}};
}

QVariantMap encodeMountPoint(const QString &mountPoint)
{
    return {{kMountPoint, mountPoint}};
}

QVariantMap encodeRestore(const Restore &r)
{
    return {{kBackup, r.backupName}, {kConfirm, r.confirmProtected}};
}

} // namespace anchorage::requests
