// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target for the option parser and the Access tab mapping (src/core/options.cpp,
// src/core/access.cpp). Input layout: [settings byte][uid byte][type] '\0' [options, comma-separated]
// Checks: nothing crashes, apply() keeps every option it doesn't own, and reading the
// result back gives the settings that were applied.
#include "access.h"
#include "options.h"

#include <cstdint>

using namespace anchorage;

namespace
{
[[noreturn]] void fail()
{
    __builtin_trap();
}

bool owned(const QString &option)
{
    static const QStringList names = {QStringLiteral("auto"), QStringLiteral("noauto"), QStringLiteral("x-systemd.automount"), QStringLiteral("user"),
                                      QStringLiteral("users"), QStringLiteral("nouser"), QStringLiteral("nofail"), QStringLiteral("_netdev"),
                                      QStringLiteral("uid"), QStringLiteral("gid"), QStringLiteral("umask"), QStringLiteral("dmask"),
                                      QStringLiteral("fmask"), QStringLiteral("file_mode"), QStringLiteral("dir_mode"), QStringLiteral("defaults")};
    return names.contains(options::split(option).name);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3) {
        return 0;
    }
    const uint8_t bits = data[0];
    const uint8_t uidByte = data[1];
    const QByteArrayView rest(reinterpret_cast<const char *>(data + 2), static_cast<qsizetype>(size - 2));
    const qsizetype nul = rest.indexOf('\0');
    const QString type = QString::fromUtf8(nul < 0 ? rest : rest.first(nul));
    const QString optText = nul < 0 ? QString() : QString::fromUtf8(rest.sliced(nul + 1));
    const QStringList opts = optText.split(QLatin1Char(','), Qt::SkipEmptyParts);

    (void)options::check(opts, type);

    access::Settings s;
    s.start = access::Start(bits % 3);
    s.who = access::Who((bits / 3) % 3);
    s.sharing = access::Sharing((bits / 9) % 4);
    s.nofail = bits & 0x40;
    s.netdev = bits & 0x80;
    if (uidByte & 1) {
        s.uid = uidByte;
        s.gid = uidByte;
    }
    const QStringList out = access::apply(opts, s, type);

    // Options the Access tab doesn't own survive, in their original order.
    QStringList keptIn;
    QStringList keptOut;
    for (const QString &o : opts) {
        if (!owned(o)) {
            keptIn.append(o);
        }
    }
    for (const QString &o : out) {
        if (!owned(o)) {
            keptOut.append(o);
        }
    }
    if (keptIn != keptOut) {
        fail();
    }

    // Reading back gives what was applied (for the parts that apply to this type, and
    // unless the input had values Anchorage leaves alone).
    const access::Settings back = access::read(out, type);
    const access::Applicable can = access::applicable(type);
    const access::Settings old = access::read(opts, type);
    if (back.nofail != s.nofail) {
        fail();
    }
    if (can.netdev && back.netdev != s.netdev) {
        fail();
    }
    if (can.who && back.who != s.who) {
        fail();
    }
    const access::Start want = (!can.automount && s.start == access::Start::WhenOpened) ? access::Start::OnRequest : s.start;
    if (s.start != old.start && back.start != want) {
        fail();
    }
    return 0;
}
