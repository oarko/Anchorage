// SPDX-License-Identifier: GPL-3.0-or-later
#include "linediff.h"

#include <algorithm>
#include <vector>

namespace anchorage::linediff
{

namespace
{
QByteArrayList splitLines(const QByteArray &text)
{
    QByteArrayList lines = text.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast(); // trailing newline
    }
    return lines;
}
} // namespace

QList<Line> diff(const QByteArray &oldText, const QByteArray &newText)
{
    const QByteArrayList a = splitLines(oldText);
    const QByteArrayList b = splitLines(newText);
    const std::size_t n = static_cast<std::size_t>(a.size());
    const std::size_t m = static_cast<std::size_t>(b.size());
    QList<Line> out;

    if (n > 5000 || m > 5000) {
        for (std::size_t i = 0; i < n; ++i) {
            out.append({Line::Kind::Removed, a.at(static_cast<qsizetype>(i)), static_cast<int>(i) + 1, -1});
        }
        for (std::size_t j = 0; j < m; ++j) {
            out.append({Line::Kind::Added, b.at(static_cast<qsizetype>(j)), -1, static_cast<int>(j) + 1});
        }
        return out;
    }

    // lcs[i][j] = length of the LCS of a[i..] and b[j..]
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            lcs[i][j] = a.at(static_cast<qsizetype>(i)) == b.at(static_cast<qsizetype>(j)) ? lcs[i + 1][j + 1] + 1
                                                                                           : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a.at(static_cast<qsizetype>(i)) == b.at(static_cast<qsizetype>(j))) {
            out.append({Line::Kind::Same, a.at(static_cast<qsizetype>(i)), static_cast<int>(i) + 1, static_cast<int>(j) + 1});
            ++i;
            ++j;
        } else if (j < m && (i == n || lcs[i][j + 1] > lcs[i + 1][j])) {
            out.append({Line::Kind::Added, b.at(static_cast<qsizetype>(j)), -1, static_cast<int>(j) + 1});
            ++j;
        } else {
            out.append({Line::Kind::Removed, a.at(static_cast<qsizetype>(i)), static_cast<int>(i) + 1, -1});
            ++i;
        }
    }
    return out;
}

namespace
{
constexpr qsizetype maxPairCells = 1000000; // character LCS table limit

// Character LCS table; empty if the lines are too long to compare cheaply.
std::vector<std::vector<int>> charLcs(const QString &a, const QString &b)
{
    if (a.size() * b.size() > maxPairCells) {
        return {};
    }
    const auto n = static_cast<std::size_t>(a.size());
    const auto m = static_cast<std::size_t>(b.size());
    std::vector<std::vector<int>> t(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            t[i][j] = a.at(qsizetype(i)) == b.at(qsizetype(j)) ? t[i + 1][j + 1] + 1 : std::max(t[i + 1][j], t[i][j + 1]);
        }
    }
    return t;
}

void appendChar(QList<Segment> &segs, QChar c, bool changed)
{
    if (segs.isEmpty() || segs.last().changed != changed) {
        segs.append({QString(), changed});
    }
    segs.last().text.append(c);
}

Side plain(const QByteArray &text, int line)
{
    Side s;
    s.present = true;
    s.line = line;
    s.segments.append({QString::fromUtf8(text), false});
    return s;
}
} // namespace

QString Side::text() const
{
    QString t;
    for (const Segment &s : segments) {
        t += s.text;
    }
    return t;
}

double similarity(const QString &a, const QString &b)
{
    if (a == b) {
        return 1.0;
    }
    if (a.isEmpty() || b.isEmpty()) {
        return 0.0;
    }
    const auto t = charLcs(a, b);
    if (t.empty()) {
        return 0.0;
    }
    return 2.0 * t[0][0] / double(a.size() + b.size());
}

QList<Row> sideBySide(const QByteArray &leftText, const QByteArray &rightText)
{
    QList<Row> rows;
    const QList<Line> lines = diff(leftText, rightText);
    qsizetype i = 0;
    while (i < lines.size()) {
        const Line &l = lines.at(i);
        if (l.kind == Line::Kind::Same) {
            rows.append({Row::Kind::Same, plain(l.text, l.oldLine), plain(l.text, l.newLine)});
            ++i;
            continue;
        }
        // A run of removed and added lines: pair similar ones, in order.
        QList<const Line *> removed;
        QList<const Line *> added;
        while (i < lines.size() && lines.at(i).kind != Line::Kind::Same) {
            (lines.at(i).kind == Line::Kind::Removed ? removed : added).append(&lines.at(i));
            ++i;
        }
        qsizetype next = 0; // first added line not used yet
        for (const Line *r : std::as_const(removed)) {
            const QString rt = QString::fromUtf8(r->text);
            qsizetype match = -1;
            for (qsizetype k = next; k < added.size(); ++k) {
                if (similarity(rt, QString::fromUtf8(added.at(k)->text)) >= pairingThreshold) {
                    match = k;
                    break;
                }
            }
            if (match < 0) {
                rows.append({Row::Kind::Removed, plain(r->text, r->oldLine), Side{}});
                continue;
            }
            for (; next < match; ++next) {
                rows.append({Row::Kind::Added, Side{}, plain(added.at(next)->text, added.at(next)->newLine)});
            }
            const Line *a = added.at(match);
            next = match + 1;
            const QString at = QString::fromUtf8(a->text);
            const auto t = charLcs(rt, at);
            Row row{Row::Kind::Changed, Side{true, r->oldLine, {}}, Side{true, a->newLine, {}}};
            std::size_t x = 0;
            std::size_t y = 0;
            const auto n = static_cast<std::size_t>(rt.size());
            const auto m = static_cast<std::size_t>(at.size());
            while (x < n || y < m) {
                if (x < n && y < m && rt.at(qsizetype(x)) == at.at(qsizetype(y))) {
                    appendChar(row.left.segments, rt.at(qsizetype(x++)), false);
                    appendChar(row.right.segments, at.at(qsizetype(y++)), false);
                } else if (y < m && (x == n || t[x][y + 1] > t[x + 1][y])) {
                    appendChar(row.right.segments, at.at(qsizetype(y++)), true);
                } else {
                    appendChar(row.left.segments, rt.at(qsizetype(x++)), true);
                }
            }
            rows.append(row);
        }
        for (; next < added.size(); ++next) {
            rows.append({Row::Kind::Added, Side{}, plain(added.at(next)->text, added.at(next)->newLine)});
        }
    }
    return rows;
}

int changeCount(const QList<Line> &lines)
{
    int n = 0;
    for (const Line &l : lines) {
        n += l.kind == Line::Kind::Same ? 0 : 1;
    }
    return n;
}

} // namespace anchorage::linediff
