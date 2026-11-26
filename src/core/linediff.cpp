// SPDX-License-Identifier: GPL-3.0-or-later
#include "linediff.h"

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

int changeCount(const QList<Line> &lines)
{
    int n = 0;
    for (const Line &l : lines) {
        n += l.kind == Line::Kind::Same ? 0 : 1;
    }
    return n;
}

} // namespace anchorage::linediff
