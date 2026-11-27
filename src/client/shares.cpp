// SPDX-License-Identifier: GPL-3.0-or-later
#include "shares.h"

#include "logging.h"

#include <QProcess>
#include <QRegularExpression>

#include <libsmbclient.h>

#include <algorithm>
#include <cerrno>
#include <functional>

namespace anchorage::shares
{

namespace
{
bool validHost(const QString &h)
{
    static const QRegularExpression name(QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,251}[A-Za-z0-9])?$"));
    static const QRegularExpression v6(QStringLiteral("^\\[[0-9A-Fa-f:.]{2,45}\\]$"));
    return name.match(h).hasMatch() || v6.match(h).hasMatch();
}
} // namespace

Result<Address> parseAddress(const QString &text)
{
    using R = Result<Address>;
    QString t = text.trimmed();
    Address a;
    if (t.startsWith(QLatin1String("smb://"), Qt::CaseInsensitive)) {
        t = t.mid(6);
        a.protocolGiven = true;
    } else if (t.startsWith(QLatin1String("nfs://"), Qt::CaseInsensitive)) {
        t = t.mid(6);
        a.protocol = Protocol::Nfs;
        a.protocolGiven = true;
    } else if (t.startsWith(QLatin1String("//")) || t.startsWith(QLatin1String("\\\\"))) {
        t = t.mid(2);
        a.protocolGiven = true; // Windows-style: SMB
    }
    t.replace(QLatin1Char('\\'), QLatin1Char('/'));
    a.host = t.section(QLatin1Char('/'), 0, 0);
    if (a.host.endsWith(QLatin1Char(':'))) {
        a.host.chop(1); // "nas:/export" typed as an address
        if (!a.protocolGiven) {
            a.protocol = Protocol::Nfs;
            a.protocolGiven = true;
        }
    }
    if (!validHost(a.host)) {
        return R::failure(QStringLiteral("Type a server name or address, e.g. nas.local, 192.168.1.10 or smb://nas.local."));
    }
    return a;
}

QList<Share> parseShowmount(QByteArrayView output)
{
    QList<Share> out;
    for (const QByteArray &raw : output.toByteArray().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (!line.startsWith(QLatin1Char('/'))) {
            continue; // header lines, errors
        }
        // The export may contain spaces; the clients are the last field.
        const qsizetype sp = line.lastIndexOf(QLatin1Char(' '));
        out.append(sp < 0 ? Share{line, QString()} : Share{line.left(sp).trimmed(), line.mid(sp + 1)});
    }
    return out;
}

Result<QList<Share>> listNfs(const QString &host)
{
    using R = Result<QList<Share>>;
    if (!validHost(host)) {
        return R::failure(QStringLiteral("Not a server name."));
    }
    qCInfo(logging::network).noquote() << "Starting network scan of" << host << "(NFS)";
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("/usr/sbin/showmount"), {QStringLiteral("-e"), QStringLiteral("--no-headers"), host});
    if (!p.waitForStarted(5000)) {
        return R::failure(QStringLiteral("showmount is not installed (it comes with the NFS client tools)."));
    }
    if (!p.waitForFinished(20000)) {
        p.kill();
        p.waitForFinished();
        return R::failure(QStringLiteral("%1 didn't answer.").arg(host));
    }
    const QByteArray out = p.readAll();
    if (p.exitCode() != 0) {
        // NFSv4-only servers have no mount service to ask; the path can still be typed.
        const QString why = QString::fromUtf8(out).trimmed().isEmpty() ? QStringLiteral("%1 didn't list any NFS exports.").arg(host)
                                                                      : QString::fromUtf8(out).trimmed();
        qCInfo(logging::network).noquote() << "NFS scan of" << host << "failed:" << why;
        return R::failure(why);
    }
    const QList<Share> shares = parseShowmount(out);
    for (const Share &share : shares) {
        qCInfo(logging::network).noquote() << "Found share at" << deviceFor(host, Protocol::Nfs, share) << "for" << share.comment;
    }
    qCInfo(logging::network).noquote() << "Found" << shares.size() << "NFS exports on" << host;
    return shares;
}

namespace
{
void authCallback(SMBCCTX *ctx, const char *, const char *, char *wg, int wglen, char *un, int unlen, char *pw, int pwlen)
{
    const auto *login = static_cast<const Login *>(smbc_getOptionUserData(ctx));
    if (!login) {
        return;
    }
    const auto copy = [](char *dst, int len, const QString &value) {
        if (len > 0) {
            qstrncpy(dst, value.toUtf8().constData(), static_cast<size_t>(len)); // always NUL-terminated
        }
    };
    if (login->guest) {
        copy(un, unlen, QStringLiteral("guest"));
        copy(pw, pwlen, QString());
        return;
    }
    if (!login->domain.isEmpty()) {
        copy(wg, wglen, login->domain);
    }
    copy(un, unlen, login->user);
    copy(pw, pwlen, login->password);
}
} // namespace

SmbList listSmb(const QString &host, const Login &login)
{
    SmbList out;
    if (!validHost(host)) {
        out.error = SmbError::Other;
        out.message = QStringLiteral("Not a server name.");
        return out;
    }
    SMBCCTX *ctx = smbc_new_context();
    if (!ctx) {
        out.error = SmbError::Other;
        out.message = QStringLiteral("Could not start the SMB client.");
        return out;
    }
    qCInfo(logging::network).noquote() << "Starting network scan of" << host << "(SMB),"
                                       << (login.guest ? QStringLiteral("as guest")
                                                       : login.user.isEmpty() ? QStringLiteral("without a login") : QStringLiteral("as %1").arg(login.user));
    Login copy = login;
    smbc_setDebug(ctx, 0);
    smbc_setTimeout(ctx, 10000);
    smbc_setOptionUserData(ctx, &copy);
    smbc_setFunctionAuthDataWithContext(ctx, authCallback);
    smbc_setOptionNoAutoAnonymousLogin(ctx, login.guest ? 0 : 1);
    if (!smbc_init_context(ctx)) {
        smbc_free_context(ctx, 0);
        out.error = SmbError::Other;
        out.message = QStringLiteral("Could not start the SMB client.");
        return out;
    }
    const QByteArray url = QStringLiteral("smb://%1").arg(host).toUtf8();
    errno = 0;
    SMBCFILE *dir = smbc_getFunctionOpendir(ctx)(ctx, url.constData());
    if (!dir) {
        const int e = errno;
        switch (e) {
        case EACCES:
        case EPERM:
            out.error = (login.user.isEmpty() && !login.guest) ? SmbError::NeedsLogin : SmbError::LoginRefused;
            out.message = out.error == SmbError::NeedsLogin ? QStringLiteral("%1 needs a user name and password.").arg(host)
                                                            : QStringLiteral("%1 refused this user name or password.").arg(host);
            break;
        case ENOENT:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ETIMEDOUT:
        case ECONNREFUSED:
            out.error = SmbError::Unreachable;
            out.message = QStringLiteral("%1 can't be reached, or doesn't share files over SMB.").arg(host);
            break;
        default:
            out.error = SmbError::Other;
            out.message = QStringLiteral("Listing the shares of %1 failed: %2").arg(host, QString::fromLocal8Bit(strerror(e)));
        }
        smbc_free_context(ctx, 1);
        qCInfo(logging::network).noquote() << "SMB scan of" << host << "failed:" << out.message << QStringLiteral("(errno %1)").arg(e);
        return out;
    }
    while (const smbc_dirent *d = smbc_getFunctionReaddir(ctx)(ctx, dir)) {
        if (d->smbc_type == SMBC_FILE_SHARE) {
            // `name` is declared char[1] but really holds namelen bytes: pass the length, or
            // Qt's array overload reads only the first character.
            const char *name = d->name;
            const qsizetype len = qsizetype(qstrnlen(name, d->namelen));
            const char *comment = d->comment;
            out.shares.append({QString::fromUtf8(name, len), comment ? QString::fromUtf8(comment, qsizetype(qstrnlen(comment, d->commentlen))) : QString()});
            qCInfo(logging::network).noquote() << "Found share at" << deviceFor(host, Protocol::Smb, out.shares.last());
        }
    }
    smbc_getFunctionClosedir(ctx)(ctx, dir);
    smbc_free_context(ctx, 1);
    qCInfo(logging::network).noquote() << "Found" << out.shares.size() << "SMB shares on" << host;
    return out;
}

QList<int> parseRpcinfo(QByteArrayView output)
{
    QList<int> out;
    for (const QByteArray &raw : output.toByteArray().split('\n')) {
        const QList<QByteArray> f = raw.simplified().split(' ');
        // program version(s) netid(s) service owner; NFS is program 100003.
        if (f.size() >= 2 && f.at(0) == "100003") {
            for (const QByteArray &v : f.at(1).split(',')) {
                bool ok = false;
                const int n = v.toInt(&ok);
                if (ok && !out.contains(n)) {
                    out.append(n);
                }
            }
        }
    }
    std::sort(out.begin(), out.end(), std::greater<int>());
    return out;
}

QList<int> nfsVersions(const QString &host)
{
    if (!validHost(host)) {
        return {};
    }
    QProcess p;
    p.start(QStringLiteral("/usr/sbin/rpcinfo"), {QStringLiteral("-s"), host});
    if (!p.waitForStarted(5000) || !p.waitForFinished(15000)) {
        p.kill();
        p.waitForFinished();
        return {};
    }
    const QList<int> versions = parseRpcinfo(p.readAllStandardOutput());
    qCInfo(logging::network).noquote() << host << "offers NFS versions" << versions;
    return versions;
}

QString deviceFor(const QString &host, Protocol protocol, const Share &share)
{
    if (protocol == Protocol::Smb) {
        return QStringLiteral("//%1/%2").arg(host, share.name);
    }
    return host + QLatin1Char(':') + share.name;
}

} // namespace anchorage::shares
