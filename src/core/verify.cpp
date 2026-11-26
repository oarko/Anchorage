// SPDX-License-Identifier: GPL-3.0-or-later
#include "verify.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

namespace anchorage::verify
{

QList<Finding> parseFindmntOutput(const QByteArray &output)
{
    QList<Finding> out;
    QString target;
    for (const QByteArray &raw : output.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        if (line.isEmpty()) {
            continue;
        }
        if (!line.startsWith(QLatin1Char(' '))) {
            // A target heading, or the summary line ("0 parse errors, 2 errors, ...").
            target = line.contains(QLatin1String("parse error")) ? QString() : line.trimmed();
            continue;
        }
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1String("[E]")) || t.startsWith(QLatin1String("[W]"))) {
            out.append({target, t.startsWith(QLatin1String("[E]")), t.mid(3).trimmed()});
        }
    }
    return out;
}

Result<QList<Finding>> findmnt(const QByteArray &content)
{
    using R = Result<QList<Finding>>;
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return R::failure(QStringLiteral("Could not create a temporary folder."));
    }
    const QString path = dir.filePath(QStringLiteral("fstab"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(content) != content.size()) {
        return R::failure(QStringLiteral("Could not write a temporary file."));
    }
    f.close();
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("/usr/bin/findmnt"), {QStringLiteral("--verify"), QStringLiteral("--verbose"), QStringLiteral("--tab-file"), path});
    if (!p.waitForFinished(15000)) {
        p.kill();
        p.waitForFinished();
        return R::failure(QStringLiteral("findmnt did not finish."));
    }
    return parseFindmntOutput(p.readAll());
}

} // namespace anchorage::verify
