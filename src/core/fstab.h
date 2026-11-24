// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>
#include <vector>

// Reading and writing fstab files (format: fstab(5)).
//
// Design goals:
//  * Lossless: parse() followed by serialize() reproduces the input byte for byte,
//    including comments, blank lines, spacing and lines that don't parse.
//  * Edits only touch the line being edited, and keep its column alignment.
//  * Every edit is validated, and the rendered line is parsed again to prove it means
//    exactly what was asked (no way to inject a second line or a comment).
//
// Parsing follows libmount (what mount, findmnt and systemd use):
//  * at least 3 fields; fields 4-6 (options, dump, pass) are optional;
//  * anything after the 6th field is ignored (we keep it untouched);
//  * octal escapes such as \040 (space) are decoded;
//  * dump and pass must be numbers, otherwise the line is a parse error.
namespace anchorage::fstab
{

// Largest fstab file we accept from disk (real ones are a few KiB).
inline constexpr qsizetype maxFileSize = qsizetype(1024) * 1024;
// Longest line we will write.
inline constexpr qsizetype maxLineLength = 4096;

struct Entry {
    QString spec; // device or share: UUID=..., LABEL=..., /dev/sdb1, //server/share, host:/path
    QString mountPoint; // absolute path, or "none"/"swap" for swap
    QString type; // ext4, btrfs, nfs4, cifs, swap, auto, ...
    QStringList options; // split on ','; empty list means "defaults"
    int dump = 0;
    int pass = 0;

    bool operator==(const Entry &) const = default;
};

enum class LineKind {
    Blank,
    Comment,
    Entry,
    Invalid, // kept verbatim; `error` says why
};

struct Line {
    LineKind kind = LineKind::Blank;
    QByteArray raw; // exact bytes of the line, without the '\n'
    std::optional<Entry> entry; // set for LineKind::Entry
    QString error; // set for LineKind::Invalid

    // Layout of an Entry line, used to keep alignment when it is edited.
    int fieldCount = 0; // 3..6 fields that carry meaning
    std::array<QByteArray, 6> separators; // whitespace before each field ([0] = indentation)
    std::array<qsizetype, 6> columns{}; // byte offset where each field starts
    QByteArray trailing; // whitespace + ignored text after the 6th field, if any
};

struct Issue {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    QString field; // "spec", "mountPoint", "type", "options", "dump", "pass" or "line"
    QString message;
};

// fstab escaping: space, tab, newline and backslash become \040 \011 \012 \134.
QByteArray encodeField(const QString &value);
// Decodes \ooo octal escapes, then UTF-8. Returns nullopt for invalid UTF-8.
std::optional<QString> decodeField(QByteArrayView raw);

// Checks an entry before it is written. Errors block writing; warnings don't.
QList<Issue> validate(const Entry &entry);
bool hasErrors(const QList<Issue> &issues);

// Renders an entry as one fstab line. With `layout` (an existing Entry line), keeps that
// line's indentation, tab separators, column positions and ignored trailing text.
QByteArray formatEntry(const Entry &entry, const Line *layout = nullptr);

// Parses a single line (without '\n').
Line parseLine(QByteArrayView raw);

class File
{
public:
    static File parse(QByteArrayView data);
    // Reads a file from disk. Fails for missing/unreadable files or files over maxFileSize.
    static std::optional<File> load(const QString &path, QString *error = nullptr);

    QByteArray serialize() const;

    const std::vector<Line> &lines() const { return m_lines; }
    std::vector<int> entryLines() const; // indexes of LineKind::Entry lines
    int findMountPoint(const QString &mountPoint) const; // line index or -1

    // Edits. Each returns false (and changes nothing) if the entry has validation errors,
    // the index isn't suitable, or the rendered line wouldn't parse back to `entry`.
    // `issues` (optional) receives the validation result.
    int append(const Entry &entry, QList<Issue> *issues = nullptr); // returns new line index or -1
    bool replace(int lineIndex, const Entry &entry, QList<Issue> *issues = nullptr);
    bool remove(int lineIndex); // Entry or Invalid lines only

private:
    std::vector<Line> m_lines;
    bool m_trailingNewline = false;
};

} // namespace anchorage::fstab
