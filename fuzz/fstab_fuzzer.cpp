// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target for the fstab parser and writer.
// Build with the "fuzz" preset, then run e.g.:
//   build/fuzz/bin/fstab_fuzzer -max_total_time=300 fuzz/corpus/fstab tests/data
#include "fstab.h"

#include <cstdint>
#include <cstdlib>

using namespace anchorage::fstab;

namespace
{
[[noreturn]] void fail(const char *what)
{
    __builtin_trap();
    (void)what;
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArrayView input(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));

    // 1. Lossless: whatever the input, serialize() gives it back byte for byte.
    File file = File::parse(input);
    if (file.serialize() != input) {
        fail("round trip changed the file");
    }

    for (const int index : file.entryLines()) {
        const Line &line = file.lines().at(static_cast<std::size_t>(index));
        const Entry &entry = *line.entry;

        // 2. A valid entry rendered with or without its layout parses back to itself.
        if (!hasErrors(validate(entry))) {
            const Line again = parseLine(formatEntry(entry, &line));
            if (again.kind != LineKind::Entry || *again.entry != entry) {
                fail("formatEntry(layout) did not round trip");
            }
            const Line fresh = parseLine(formatEntry(entry));
            if (fresh.kind != LineKind::Entry || *fresh.entry != entry) {
                fail("formatEntry() did not round trip");
            }
        }

        // 3. Escaping round trips for every decoded field.
        for (const QString &field : {entry.spec, entry.mountPoint, entry.type}) {
            if (decodeField(encodeField(field)) != field) {
                fail("escape round trip");
            }
        }
    }

    // 4. Edits never produce a file whose entries differ from what was asked,
    //    and never add or lose lines other than the one edited.
    File edited = file;
    const auto entries = edited.entryLines();
    if (!entries.empty()) {
        const int target = entries.front();
        Entry e = *edited.lines().at(static_cast<std::size_t>(target)).entry;
        e.options.append(QStringLiteral("nofail"));
        const std::size_t before = edited.lines().size();
        if (edited.replace(target, e)) {
            const File reparsed = File::parse(edited.serialize());
            if (reparsed.lines().size() != before) {
                fail("replace changed the number of lines");
            }
            const Line &l = reparsed.lines().at(static_cast<std::size_t>(target));
            if (l.kind != LineKind::Entry || *l.entry != e) {
                fail("replace result differs after re-parse");
            }
        }
    }
    return 0;
}
