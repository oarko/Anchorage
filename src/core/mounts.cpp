// SPDX-License-Identifier: GPL-3.0-or-later
#include "mounts.h"

#include "fstab.h"

#include <QFile>

namespace anchorage::mounts
{

namespace
{
// mountinfo escapes space, tab, newline and backslash as \ooo, like fstab.
QString field(QByteArrayView raw)
{
    return fstab::decodeField(raw).value_or(QString::fromLatin1(raw));
}
} // namespace

QList<Mount> parseMountInfo(QByteArrayView data)
{
    QList<Mount> out;
    for (const QByteArrayView line : QByteArray(data.toByteArray()).split('\n')) {
        const QList<QByteArray> parts = line.toByteArray().split(' ');
        // id parent major:minor root target options [optional fields...] - fstype source superopts
        const qsizetype sep = parts.indexOf(QByteArray("-"));
        if (parts.size() < 10 || sep < 6 || sep + 2 >= parts.size()) {
            continue;
        }
        Mount m;
        m.root = field(parts.at(3));
        m.target = field(parts.at(4));
        m.options = QString::fromLatin1(parts.at(5));
        m.fsType = field(parts.at(sep + 1));
        m.source = field(parts.at(sep + 2));
        out.append(m);
    }
    return out;
}

Result<QList<Mount>> readCurrent()
{
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly)) {
        return Result<QList<Mount>>::failure(f.errorString());
    }
    // procfs files report size 0; readAll reads until EOF.
    return parseMountInfo(f.readAll());
}

} // namespace anchorage::mounts
