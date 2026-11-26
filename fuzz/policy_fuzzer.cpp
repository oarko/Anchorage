// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target for the helper's permission rules (src/core/mountpolicy.cpp).
// Input layout: [flags byte][current fstab] '\0' [proposed fstab]
// The fuzzer checks that no input lets a caller do more than their tier allows.
#include "mountpolicy.h"
#include "mountplan.h"
#include "requests.h"

#include <cstdint>

using namespace anchorage;
using namespace anchorage::policy;

namespace
{
[[noreturn]] void fail()
{
    __builtin_trap();
}

Caller callerFrom(uint8_t flags)
{
    Caller c;
    c.uid = 1000;
    c.gid = 1000;
    c.name = QStringLiteral("fuzz");
    c.home = QStringLiteral("/home/fuzz");
    c.sudo = flags & 1;
    c.driveAdmin = flags & 2;
    c.netMount = flags & 4;
    return c;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) {
        return 0;
    }
    const uint8_t flags = data[0];
    const QByteArrayView rest(reinterpret_cast<const char *>(data + 1), static_cast<qsizetype>(size - 1));
    const qsizetype split = rest.indexOf('\0');
    const QByteArrayView current = split < 0 ? rest : rest.first(split);
    const QByteArrayView proposed = split < 0 ? QByteArrayView() : rest.sliced(split + 1);
    const Caller caller = callerFrom(flags);
    const bool confirm = flags & 8;

    const Plan plan = planFstabWrite(current, proposed, caller, confirm);
    if (plan.ok()) {
        for (const fstab::Entry &e : plan.added) {
            // Every written entry is valid, whoever asked.
            if (fstab::hasErrors(fstab::validate(e))) {
                fail();
            }
            // Only full-access callers may add or change system entries.
            if (isSystemEntry(e) && !caller.fullAccess()) {
                fail();
            }
        }
        for (const fstab::Entry &e : plan.removed) {
            if (isSystemEntry(e) && !caller.fullAccess()) {
                fail();
            }
        }
        for (const QString &mp : plan.newMountPoints) {
            const Zone z = classifyMountPoint(mp).zone;
            if (z == Zone::Invalid) {
                fail();
            }
            if (z == Zone::Override && !caller.fullAccess()) {
                fail();
            }
            if (z == Zone::Blocked && !(caller.sudo && confirm)) {
                fail();
            }
        }
        // New unparseable lines never get through.
        const fstab::File after = fstab::File::parse(proposed);
        const fstab::File before = fstab::File::parse(current);
        for (const auto &l : after.lines()) {
            if (l.kind != fstab::LineKind::Invalid) {
                continue;
            }
            bool existed = false;
            for (const auto &o : before.lines()) {
                existed = existed || o.raw == l.raw;
            }
            if (!existed) {
                fail();
            }
        }
    }

    // Shared mounts: build an entry from the proposed file's first entry, if any.
    const fstab::File file = fstab::File::parse(proposed);
    const auto entries = file.entryLines();
    if (!entries.empty()) {
        const fstab::Entry requested = *file.lines().at(static_cast<std::size_t>(entries.front())).entry;
        const SharedMountPlan shared = planSharedMount(requested, caller, current, QStringLiteral("/etc/anchorage/credentials"));
        if (shared.ok()) {
            const fstab::Entry &e = shared.entry;
            if (!caller.netMount && !caller.fullAccess()) {
                fail();
            }
            if (!isNetworkType(e.type) || !e.options.contains(QStringLiteral("nosuid")) || !e.options.contains(QStringLiteral("nodev"))
                || !e.options.contains(QStringLiteral("nofail"))) {
                fail();
            }
            if (classifyMountPoint(e.mountPoint).zone != Zone::Safe || !e.mountPoint.startsWith(QStringLiteral("/mnt/"))
                || e.mountPoint.count(QLatin1Char('/')) != 2) {
                fail();
            }
            for (const QString &o : e.options) {
                if (o == QLatin1String("suid") || o == QLatin1String("dev") || o == QLatin1String("exec")) {
                    fail();
                }
            }
            if (fstab::hasErrors(fstab::validate(e))) {
                fail();
            }
        }
        // Mounting (SPEC §6.1). `current` doubles as the mount table (mountinfo format).
        const QList<mounts::Mount> now = mounts::parseMountInfo(current);
        const auto temp = mounting::planTemporaryMount(requested, caller, confirm, current, now);
        if (temp.ok()) {
            if (!caller.fullAccess() || fstab::hasErrors(fstab::validate(requested)) || requested.spec.startsWith(QLatin1Char('-'))) {
                fail();
            }
            for (const QString &o : requested.options) {
                const QString name = o.section(QLatin1Char('='), 0, 0);
                if (name == QLatin1String("bind") || name == QLatin1String("rbind") || name == QLatin1String("helper") || name.startsWith(QLatin1String("X-mount."))) {
                    fail();
                }
            }
            const Zone z = classifyMountPoint(requested.mountPoint).zone;
            if (z == Zone::Invalid || (z == Zone::Blocked && !(caller.sudo && confirm))) {
                fail();
            }
        }
        const auto un = mounting::planUnmount(proposed, now, requested.mountPoint, caller, confirm);
        if (un.ok()) {
            if ((un.system && !(caller.fullAccess() && confirm)) || !mounting::mountedAt(now, requested.mountPoint)) {
                fail();
            }
            const auto *m = mounting::mountedAt(now, requested.mountPoint);
            const bool inFstab = file.findMountPoint(requested.mountPoint) >= 0;
            if (!inFstab && !caller.fullAccess()) {
                fail();
            }
            if (!inFstab && m->fsType == QLatin1String("proc")) {
                fail();
            }
        }
        const auto me = mounting::planMountEntry(proposed, requested.mountPoint, caller, now);
        if (me.ok() && (me.entry.type == QLatin1String("swap") || mounting::mountedAt(now, requested.mountPoint))) {
            fail(); // swap is never mounted; nothing is mounted twice
        }

        // Requests survive encoding/decoding unchanged (the decoder refuses over 64 options).
        const auto decoded = requests::decodeEntry(requests::encodeEntry(requested));
        if (requested.options.size() <= 64 ? (!decoded || *decoded != requested) : decoded.ok()) {
            fail();
        }
    }
    return 0;
}
