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

} // namespace anchorage::linediff
