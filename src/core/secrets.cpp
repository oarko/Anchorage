// SPDX-License-Identifier: GPL-3.0-or-later
#include "secrets.h"

#include <QSet>

#include <algorithm>

namespace anchorage::secrets
{

namespace
{
QString name(const QString &o)
{
    return o.section(QLatin1Char('='), 0, 0);
}
QString value(const QString &o)
{
    const qsizetype eq = o.indexOf(QLatin1Char('='));
    return eq < 0 ? QString() : o.mid(eq + 1);
}
bool isSmb(const fstab::Entry &e)
{
    return e.type == QLatin1String("cifs") || e.type == QLatin1String("smb3");
}
bool isPasswordOption(const QString &o)
{
    return (name(o) == QLatin1String("password") || name(o) == QLatin1String("pass")) && o.contains(QLatin1Char('='));
}
bool isLoginOption(const QString &o)
{
    static const QSet<QString> login = {QStringLiteral("username"), QStringLiteral("user"), QStringLiteral("password"), QStringLiteral("pass"),
                                        QStringLiteral("domain"), QStringLiteral("dom"), QStringLiteral("workgroup")};
    // "user" without a value is the "any user may mount" flag, not a login.
    return login.contains(name(o)) && o.contains(QLatin1Char('='));
}

QSet<QString> passwordsIn(QByteArrayView content)
{
    QSet<QString> out;
    const fstab::File f = fstab::File::parse(content);
    for (const int i : f.entryLines()) {
        const auto &e = f.lines().at(static_cast<std::size_t>(i)).entry;
        if (!e || !isSmb(*e)) {
            continue;
        }
        for (const QString &o : e->options) {
            // An empty password is no secret (and would match every "password=").
            if (isPasswordOption(o) && !value(o).isEmpty() && value(o) != QLatin1String(redactedValue)) {
                out.insert(value(o));
            }
        }
    }
    return out;
}
} // namespace

QList<Plaintext> find(const fstab::File &file)
{
    QList<Plaintext> out;
    for (const int i : file.entryLines()) {
        const auto &e = file.lines().at(static_cast<std::size_t>(i)).entry;
        if (!e || !isSmb(*e)) {
            continue;
        }
        Plaintext p;
        p.line = i;
        p.server = e->spec.startsWith(QLatin1String("//")) ? e->spec.mid(2).section(QLatin1Char('/'), 0, 0) : QString();
        for (const QString &o : e->options) {
            const QString n = name(o);
            if (!o.contains(QLatin1Char('='))) {
                continue;
            }
            if (n == QLatin1String("username") || n == QLatin1String("user")) {
                p.user = value(o);
            } else if (n == QLatin1String("password") || n == QLatin1String("pass")) {
                p.password = value(o);
            } else if (n == QLatin1String("domain") || n == QLatin1String("dom") || n == QLatin1String("workgroup")) {
                p.domain = value(o);
            }
        }
        if (std::any_of(e->options.cbegin(), e->options.cend(), isPasswordOption) && p.password != QLatin1String(redactedValue)) {
            out.append(p);
        }
    }
    return out;
}

fstab::Entry withCredentials(const fstab::Entry &entry, const QString &path)
{
    fstab::Entry e = entry;
    QStringList opts;
    bool placed = false;
    for (const QString &o : entry.options) {
        if (isLoginOption(o)) {
            if (!placed) {
                opts.append(QStringLiteral("credentials=") + path);
                placed = true;
            }
            continue;
        }
        if (name(o) == QLatin1String("credentials") || name(o) == QLatin1String("cred")) {
            continue; // replaced by the new one
        }
        opts.append(o);
    }
    if (!placed) {
        opts.append(QStringLiteral("credentials=") + path);
    }
    e.options = opts;
    return e;
}

QStringList removedPasswords(QByteArrayView before, QByteArrayView after)
{
    const QSet<QString> still = passwordsIn(after);
    QStringList out;
    for (const QString &p : passwordsIn(before)) {
        if (!still.contains(p)) {
            out.append(p);
        }
    }
    out.sort();
    return out;
}

QByteArray redact(QByteArrayView content, const QStringList &passwords)
{
    // Works on the raw bytes of each line's options field (located by the parser, split on
    // commas, each value decoded exactly as the parser does), so it also scrubs lines the
    // fstab editor would refuse to rewrite and values with unusual escapes (both found by
    // fuzzing), and every byte that isn't the password stays as it was.
    if (passwords.isEmpty()) {
        return content.toByteArray();
    }
    QByteArray out;
    out.reserve(content.size());
    qsizetype pos = 0;
    while (pos <= content.size()) {
        const qsizetype nl = content.indexOf('\n', pos);
        const qsizetype end = nl < 0 ? content.size() : nl;
        QByteArray line = content.sliced(pos, end - pos).toByteArray();
        const fstab::Line parsed = fstab::parseLine(line);
        if (parsed.kind == fstab::LineKind::Entry && parsed.entry && isSmb(*parsed.entry) && parsed.fieldCount >= 4) {
            const qsizetype start = parsed.columns.at(3);
            qsizetype stop = start;
            const auto blank = [](char c) {
                return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
            };
            while (stop < line.size() && !blank(line.at(stop))) {
                ++stop;
            }
            QByteArray field = line.mid(start, stop - start);
            QList<QByteArray> parts = field.split(',');
            bool hit = false;
            for (QByteArray &part : parts) {
                const qsizetype eq = part.indexOf('=');
                if (eq < 0) {
                    continue;
                }
                const QByteArray key = part.left(eq);
                if (key != "password" && key != "pass") {
                    continue;
                }
                const auto value = fstab::decodeField(QByteArrayView(part).sliced(eq + 1));
                if (value && passwords.contains(*value)) {
                    part = key + '=' + redactedValue;
                    hit = true;
                }
            }
            if (hit) {
                line.replace(start, stop - start, parts.join(','));
            }
        }
        out += line;
        if (nl < 0) {
            break;
        }
        out += '\n';
        pos = nl + 1;
    }
    return out;
}

} // namespace anchorage::secrets
