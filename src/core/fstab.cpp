// SPDX-License-Identifier: GPL-3.0-or-later
#include "fstab.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>

namespace anchorage::fstab
{

namespace
{
// Same whitespace set as isspace() in the C locale, which libmount uses.
constexpr bool isBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

constexpr bool isOctal(char c)
{
    return c >= '0' && c <= '7';
}

// Parses a decimal number of at most 9 digits with an optional leading '-'.
// libmount accepts negative values here, so we parse them too (validate() rejects them).
std::optional<int> parseNumber(QByteArrayView text)
{
    const bool negative = text.startsWith('-');
    if (negative) {
        text = text.sliced(1);
    }
    if (text.isEmpty() || text.size() > 9) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return negative ? -value : value;
}

bool hasControlCharacter(const QString &s)
{
    for (const QChar c : s) {
        const auto cat = c.category();
        if (cat == QChar::Other_Control || cat == QChar::Separator_Line || cat == QChar::Separator_Paragraph) {
            return true;
        }
    }
    return false;
}

void checkText(QList<Issue> &issues, const QString &field, const QString &value, const QString &label)
{
    if (hasControlCharacter(value)) {
        issues.append({Issue::Severity::Error, field, QStringLiteral("%1 contains a control character or line break.").arg(label)});
    }
    if (value.contains(QLatin1Char('#'))) {
        issues.append({Issue::Severity::Error, field, QStringLiteral("%1 must not contain '#'.").arg(label)});
    }
}
} // namespace

QByteArray encodeField(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    QByteArray out;
    out.reserve(utf8.size());
    for (const char c : utf8) {
        switch (c) {
        case ' ':
            out.append("\\040");
            break;
        case '\t':
            out.append("\\011");
            break;
        case '\n':
            out.append("\\012");
            break;
        case '\\':
            out.append("\\134");
            break;
        default:
            out.append(c);
        }
    }
    return out;
}

std::optional<QString> decodeField(QByteArrayView raw)
{
    QByteArray bytes;
    bytes.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\\' && i + 3 < raw.size() && isOctal(raw[i + 1]) && isOctal(raw[i + 2]) && isOctal(raw[i + 3])) {
            const int value = (raw[i + 1] - '0') * 64 + (raw[i + 2] - '0') * 8 + (raw[i + 3] - '0');
            if (value <= 0377) {
                bytes.append(static_cast<char>(value));
                i += 3;
                continue;
            }
        }
        bytes.append(c);
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        return std::nullopt;
    }
    return text;
}

bool hasErrors(const QList<Issue> &issues)
{
    return std::any_of(issues.cbegin(), issues.cend(), [](const Issue &i) {
        return i.severity == Issue::Severity::Error;
    });
}

QList<Issue> validate(const Entry &entry)
{
    QList<Issue> issues;
    const auto error = [&issues](const QString &field, const QString &msg) {
        issues.append({Issue::Severity::Error, field, msg});
    };

    if (entry.spec.isEmpty()) {
        error(QStringLiteral("spec"), QStringLiteral("Device or share is empty."));
    }
    checkText(issues, QStringLiteral("spec"), entry.spec, QStringLiteral("Device or share"));

    const bool isSwap = entry.type == QLatin1String("swap");
    if (entry.mountPoint.isEmpty()) {
        error(QStringLiteral("mountPoint"), QStringLiteral("Mount point is empty."));
    } else if (isSwap && (entry.mountPoint == QLatin1String("none") || entry.mountPoint == QLatin1String("swap"))) {
        // fine: swap has no mount point
    } else if (!entry.mountPoint.startsWith(QLatin1Char('/'))) {
        error(QStringLiteral("mountPoint"), QStringLiteral("Mount point must be a full path starting with /."));
    }
    checkText(issues, QStringLiteral("mountPoint"), entry.mountPoint, QStringLiteral("Mount point"));

    static const QRegularExpression typeRe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$"));
    if (!typeRe.match(entry.type).hasMatch()) {
        error(QStringLiteral("type"), QStringLiteral("Filesystem type must be letters, digits and . _ + - (e.g. ext4, nfs4, fuse.sshfs)."));
    }

    for (const QString &opt : entry.options) {
        if (opt.isEmpty()) {
            error(QStringLiteral("options"), QStringLiteral("Options contain an empty item (two commas in a row?)."));
            break;
        }
        if (opt.contains(QLatin1Char(','))) {
            error(QStringLiteral("options"), QStringLiteral("A single option must not contain a comma."));
            break;
        }
    }
    const QString joined = entry.options.join(QLatin1Char(','));
    checkText(issues, QStringLiteral("options"), joined, QStringLiteral("Options"));

    if (entry.dump < 0 || entry.dump > 9) {
        error(QStringLiteral("dump"), QStringLiteral("Dump must be 0 to 9 (normally 0)."));
    }
    if (entry.pass < 0 || entry.pass > 9) {
        error(QStringLiteral("pass"), QStringLiteral("Check order must be 0 to 9."));
    } else if (entry.pass > 2) {
        issues.append({Issue::Severity::Warning, QStringLiteral("pass"), QStringLiteral("Check order is normally 0, 1 (root) or 2.")});
    }

    if (!hasErrors(issues) && formatEntry(entry).size() > maxLineLength) {
        error(QStringLiteral("line"), QStringLiteral("The entry is too long (over %1 bytes).").arg(maxLineLength));
    }
    return issues;
}

QByteArray formatEntry(const Entry &entry, const Line *layout)
{
    const QByteArray options = entry.options.isEmpty() ? QByteArrayLiteral("defaults") : encodeField(entry.options.join(QLatin1Char(',')));
    const std::array<QByteArray, 6> fields = {
        encodeField(entry.spec),
        encodeField(entry.mountPoint),
        encodeField(entry.type),
        options,
        QByteArray::number(entry.dump),
        QByteArray::number(entry.pass),
    };

    const bool hasLayout = layout && layout->kind == LineKind::Entry;
    int count = 6;
    if (hasLayout && layout->fieldCount < 6 && entry.dump == 0 && entry.pass == 0) {
        // Keep a short line short when the omitted values are still the defaults.
        count = std::max(layout->fieldCount, entry.options.isEmpty() ? 3 : 4);
    }

    QByteArray out;
    for (int i = 0; i < count; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        if (hasLayout && i < layout->fieldCount) {
            const QByteArray &sep = layout->separators[idx];
            if (i == 0 || sep.contains('\t')) {
                out.append(sep); // indentation, or tab-separated columns: reuse as-is
            } else {
                // Space-aligned columns: pad to where this field started originally.
                const qsizetype target = layout->columns[idx];
                out.append(QByteArray(std::max<qsizetype>(1, target - out.size()), ' '));
            }
        } else if (i > 0) {
            out.append(hasLayout ? QByteArrayLiteral(" ") : QByteArrayLiteral("  "));
        }
        out.append(fields[idx]);
    }
    if (hasLayout && count == 6 && layout->fieldCount == 6) {
        out.append(layout->trailing);
    }
    return out;
}

Line parseLine(QByteArrayView raw)
{
    Line line;
    line.raw = raw.toByteArray();

    qsizetype i = 0;
    while (i < raw.size() && isBlank(raw[i])) {
        ++i;
    }
    if (i == raw.size()) {
        line.kind = LineKind::Blank;
        return line;
    }
    if (raw[i] == '#') {
        line.kind = LineKind::Comment;
        return line;
    }

    // Split into whitespace-separated fields, remembering separators and positions.
    std::array<QByteArrayView, 6> fields;
    int count = 0;
    qsizetype pos = 0;
    while (pos < raw.size()) {
        const qsizetype sepStart = pos;
        while (pos < raw.size() && isBlank(raw[pos])) {
            ++pos;
        }
        if (pos == raw.size()) {
            if (count == 6) {
                line.trailing = raw.sliced(sepStart).toByteArray();
            }
            break;
        }
        if (count == 6) {
            line.trailing = raw.sliced(sepStart).toByteArray(); // ignored by libmount; kept verbatim
            break;
        }
        const qsizetype start = pos;
        while (pos < raw.size() && !isBlank(raw[pos])) {
            ++pos;
        }
        const std::size_t idx = static_cast<std::size_t>(count);
        line.separators[idx] = raw.sliced(sepStart, start - sepStart).toByteArray();
        line.columns[idx] = start;
        fields[idx] = raw.sliced(start, pos - start);
        ++count;
    }

    const auto invalid = [&line](const QString &why) {
        line.kind = LineKind::Invalid;
        line.error = why;
        line.entry.reset();
        return line;
    };

    if (count < 3) {
        return invalid(QStringLiteral("A line needs at least 3 fields: device, mount point and type."));
    }
    line.fieldCount = count;

    Entry e;
    const auto spec = decodeField(fields[0]);
    const auto mountPoint = decodeField(fields[1]);
    const auto type = decodeField(fields[2]);
    if (!spec || !mountPoint || !type) {
        return invalid(QStringLiteral("The line is not valid UTF-8 text."));
    }
    e.spec = *spec;
    e.mountPoint = *mountPoint;
    e.type = *type;

    if (count >= 4) {
        const auto opts = decodeField(fields[3]);
        if (!opts) {
            return invalid(QStringLiteral("The options are not valid UTF-8 text."));
        }
        if (*opts != QLatin1String("defaults")) {
            e.options = opts->split(QLatin1Char(','), Qt::KeepEmptyParts);
        }
    }
    if (count >= 5) {
        const auto dump = parseNumber(fields[4]);
        if (!dump) {
            return invalid(QStringLiteral("The 5th field (dump) must be a number."));
        }
        e.dump = *dump;
    }
    if (count >= 6) {
        const auto pass = parseNumber(fields[5]);
        if (!pass) {
            return invalid(QStringLiteral("The 6th field (check order) must be a number."));
        }
        e.pass = *pass;
    }

    line.kind = LineKind::Entry;
    line.entry = std::move(e);
    return line;
}

File File::parse(QByteArrayView data)
{
    File file;
    if (data.isEmpty()) {
        return file;
    }
    file.m_trailingNewline = data.endsWith('\n');
    const QByteArrayView body = file.m_trailingNewline ? data.chopped(1) : data;
    qsizetype start = 0;
    while (true) {
        const qsizetype nl = body.indexOf('\n', start);
        if (nl < 0) {
            file.m_lines.push_back(parseLine(body.sliced(start)));
            break;
        }
        file.m_lines.push_back(parseLine(body.sliced(start, nl - start)));
        start = nl + 1;
    }
    return file;
}

std::optional<File> File::load(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = f.errorString();
        }
        return std::nullopt;
    }
    const QByteArray data = f.read(maxFileSize + 1);
    if (data.size() > maxFileSize) {
        if (error) {
            *error = QStringLiteral("%1 is larger than %2 bytes.").arg(path).arg(maxFileSize);
        }
        return std::nullopt;
    }
    return parse(data);
}

QByteArray File::serialize() const
{
    QByteArray out;
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        if (i > 0) {
            out.append('\n');
        }
        out.append(m_lines[i].raw);
    }
    if (m_trailingNewline) {
        out.append('\n');
    }
    return out;
}

std::vector<int> File::entryLines() const
{
    std::vector<int> out;
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        if (m_lines[i].kind == LineKind::Entry) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

int File::findMountPoint(const QString &mountPoint) const
{
    for (const int i : entryLines()) {
        if (m_lines[static_cast<std::size_t>(i)].entry->mountPoint == mountPoint) {
            return i;
        }
    }
    return -1;
}

namespace
{
// "defaults" on its own and an empty option list mean the same thing.
Entry normalized(Entry e)
{
    if (e.options == QStringList{QStringLiteral("defaults")}) {
        e.options.clear();
    }
    return e;
}

// Renders `entry` and proves the result parses back to exactly `entry`.
std::optional<Line> renderChecked(const Entry &entry, const Line *layout, QList<Issue> *issues)
{
    const QList<Issue> found = validate(entry);
    if (issues) {
        *issues = found;
    }
    if (hasErrors(found)) {
        return std::nullopt;
    }
    Line line = parseLine(formatEntry(entry, layout));
    if (line.kind != LineKind::Entry || line.entry != normalized(entry)) {
        if (issues) {
            issues->append({Issue::Severity::Error, QStringLiteral("line"), QStringLiteral("The entry could not be written safely.")});
        }
        return std::nullopt;
    }
    return line;
}
} // namespace

int File::append(const Entry &entry, QList<Issue> *issues)
{
    // Align new entries with the last existing entry, and insert right after it.
    int after = -1;
    for (const int i : entryLines()) {
        after = i;
    }
    const Line *layout = after >= 0 ? &m_lines[static_cast<std::size_t>(after)] : nullptr;
    Line hint;
    if (layout) {
        hint = *layout;
        hint.fieldCount = 6; // new entries always get all six fields
        hint.trailing.clear();
        for (std::size_t i = static_cast<std::size_t>(layout->fieldCount); i < 6; ++i) {
            hint.separators[i] = QByteArrayLiteral(" ");
            hint.columns[i] = 0;
        }
    }
    auto line = renderChecked(entry, layout ? &hint : nullptr, issues);
    if (!line) {
        return -1;
    }
    const auto pos = after >= 0 ? static_cast<std::size_t>(after) + 1 : m_lines.size();
    m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(pos), std::move(*line));
    m_trailingNewline = true;
    return static_cast<int>(pos);
}

bool File::replace(int lineIndex, const Entry &entry, QList<Issue> *issues)
{
    if (lineIndex < 0 || static_cast<std::size_t>(lineIndex) >= m_lines.size()) {
        return false;
    }
    Line &old = m_lines[static_cast<std::size_t>(lineIndex)];
    if (old.kind != LineKind::Entry) {
        return false;
    }
    auto line = renderChecked(entry, &old, issues);
    if (!line) {
        return false;
    }
    old = std::move(*line);
    return true;
}

bool File::remove(int lineIndex)
{
    if (lineIndex < 0 || static_cast<std::size_t>(lineIndex) >= m_lines.size()) {
        return false;
    }
    const auto kind = m_lines[static_cast<std::size_t>(lineIndex)].kind;
    if (kind != LineKind::Entry && kind != LineKind::Invalid) {
        return false;
    }
    m_lines.erase(m_lines.begin() + lineIndex);
    return true;
}

} // namespace anchorage::fstab
