// SPDX-License-Identifier: GPL-3.0-or-later
#include "drives.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

namespace anchorage::drives
{

namespace
{
QString str(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

// Older lsblk versions print numbers and booleans as strings; accept both.
qint64 number(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) {
        return static_cast<qint64>(v.toDouble());
    }
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        return ok ? n : -1;
    }
    return -1;
}

bool flag(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool()) {
        return v.toBool();
    }
    return v.isString() && v.toString() == QLatin1String("1");
}

// Recursive over lsblk's device tree; `depth` caps it, real trees are 3-4 levels deep.
void collect(const QJsonObject &node, const Volume *disk, QList<Volume> &out, int depth) // NOLINT(misc-no-recursion)
{
    if (depth > 16) {
        return; // malformed input; real trees are 3-4 levels deep
    }
    Volume v;
    v.name = str(node, "name");
    v.path = str(node, "path");
    v.type = str(node, "type");
    v.size = number(node, "size");
    v.fsType = str(node, "fstype");
    v.fsVersion = str(node, "fsver");
    v.label = str(node, "label");
    v.uuid = str(node, "uuid");
    v.partUuid = str(node, "partuuid");
    v.partLabel = str(node, "partlabel");
    v.partTypeName = str(node, "parttypename");
    v.fsSize = number(node, "fssize");
    v.fsUsed = number(node, "fsused");
    v.readOnly = flag(node, "ro");
    const QJsonValue mps = node.value(QLatin1String("mountpoints"));
    if (mps.isArray()) {
        for (const QJsonValue m : mps.toArray()) {
            if (m.isString() && !m.toString().isEmpty()) {
                v.mountPoints.append(m.toString());
            }
        }
    } else if (const QString single = str(node, "mountpoint"); !single.isEmpty()) {
        v.mountPoints.append(single);
    }
    if (disk) {
        v.diskPath = disk->diskPath;
        v.diskModel = disk->diskModel;
        v.transport = disk->transport;
        v.removable = disk->removable;
        v.rotational = disk->rotational;
    } else {
        v.diskPath = v.path;
        v.diskModel = str(node, "model").trimmed();
        v.transport = str(node, "tran");
        v.removable = flag(node, "rm") || flag(node, "hotplug");
        v.rotational = flag(node, "rota");
    }
    out.append(v);
    const Volume self = v;
    for (const QJsonValue child : node.value(QLatin1String("children")).toArray()) {
        if (child.isObject()) {
            collect(child.toObject(), disk ? disk : &self, out, depth + 1); // NOLINT(misc-no-recursion)
        }
    }
}
} // namespace

QString columns()
{
    return QStringLiteral("NAME,PATH,TYPE,SIZE,FSTYPE,FSVER,LABEL,UUID,PARTUUID,PARTLABEL,PARTTYPENAME,MOUNTPOINTS,FSSIZE,FSUSED,RM,HOTPLUG,RO,MODEL,TRAN,ROTA");
}

Result<QList<Volume>> parseLsblk(QByteArrayView json)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toByteArray(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return Result<QList<Volume>>::failure(QStringLiteral("Could not read the drive list: %1").arg(err.errorString()));
    }
    QList<Volume> out;
    for (const QJsonValue dev : doc.object().value(QLatin1String("blockdevices")).toArray()) {
        if (dev.isObject()) {
            collect(dev.toObject(), nullptr, out, 0);
        }
    }
    return out;
}

Result<QList<Volume>> scan()
{
    QProcess p;
    p.start(QStringLiteral("/usr/bin/lsblk"), {QStringLiteral("--json"), QStringLiteral("--bytes"), QStringLiteral("-o"), columns()});
    if (!p.waitForFinished(10000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        return Result<QList<Volume>>::failure(QStringLiteral("Could not list drives (lsblk): %1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed()));
    }
    return parseLsblk(p.readAllStandardOutput());
}

bool isUserVisible(const Volume &v)
{
    if (v.fsType.isEmpty()) {
        return false;
    }
    // Loop devices stay visible (attached disk images are real drives to people); snap
    // packages are loop devices too, but always squashfs, which is hidden below.
    static const QStringList hiddenTypes = {QStringLiteral("rom"), QStringLiteral("zram"), QStringLiteral("ram")};
    static const QStringList hiddenFs = {QStringLiteral("squashfs"), QStringLiteral("swap"), QStringLiteral("LVM2_member"),
                                         QStringLiteral("linux_raid_member"), QStringLiteral("crypto_LUKS"), QStringLiteral("iso9660"),
                                         QStringLiteral("udf"), QStringLiteral("zfs_member"), QStringLiteral("bcache")};
    return !hiddenTypes.contains(v.type) && !hiddenFs.contains(v.fsType);
}

} // namespace anchorage::drives
