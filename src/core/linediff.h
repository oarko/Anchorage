// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QByteArrayList>
#include <QList>

// Line-by-line difference of two small text files (the "Preview changes" dialog).
namespace anchorage::linediff
{

struct Line {
    enum class Kind { Same, Removed, Added };
    Kind kind = Kind::Same;
    QByteArray text; // without '\n'
    int oldLine = -1; // 1-based line number in the old text, -1 for added lines
    int newLine = -1; // 1-based line number in the new text, -1 for removed lines
};

// Longest-common-subsequence diff. Inputs are limited to fstab::maxFileSize by the
// callers; texts with more than 5000 lines are compared as "all removed, all added".
QList<Line> diff(const QByteArray &oldText, const QByteArray &newText);

// Number of Removed + Added lines.
int changeCount(const QList<Line> &lines);

// ---- Side by side (the restore view) --------------------------------------------------

struct Segment {
    QString text;
    bool changed = false; // differs from the other side of a Changed row
};

struct Side {
    bool present = false; // false: a filler opposite an added or removed line
    int line = -1; // 1-based line number
    QList<Segment> segments; // the line's text, split where it differs
    QString text() const;
};

struct Row {
    enum class Kind {
        Same,
        Removed, // only on the left
        Added, // only on the right
        Changed, // similar lines, paired; segments show what differs
    };
    Kind kind = Kind::Same;
    Side left;
    Side right;
};

// Lines that are at least this similar (2 x common characters / total characters) are
// shown as one changed line rather than a removed and an added one.
inline constexpr double pairingThreshold = 0.5;

// Similarity of two lines, 0..1 (1 = equal). Very long lines count as 0.
double similarity(const QString &a, const QString &b);

QList<Row> sideBySide(const QByteArray &left, const QByteArray &right);

} // namespace anchorage::linediff
