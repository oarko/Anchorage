// SPDX-License-Identifier: GPL-3.0-or-later
#include "access.h"

#include <QSet>

namespace anchorage::access
{

using options::OwnerModel;

namespace
{
const QString kAuto = QStringLiteral("auto");
const QString kNoauto = QStringLiteral("noauto");
const QString kAutomount = QStringLiteral("x-systemd.automount");
const QString kUser = QStringLiteral("user");
const QString kUsers = QStringLiteral("users");
const QString kNouser = QStringLiteral("nouser");
const QString kNofail = QStringLiteral("nofail");
const QString kNetdev = QStringLiteral("_netdev");

// Does `option` belong to an aspect owning `names`? "user=anna" (cifs user name) is not
// the "user" flag.
bool owns(const QSet<QString> &names, const QString &option, bool flagsOnly)
{
    const options::Item item = options::split(option);
    return names.contains(item.name) && !(flagsOnly && item.value);
}

// Replaces every option of one aspect with `replacement`, at the place of the first one.
QStringList replaceAspect(const QStringList &in, const QSet<QString> &names, const QStringList &replacement, bool flagsOnly = false)
{
    QStringList out;
    bool inserted = false;
    for (const QString &o : in) {
        if (owns(names, o, flagsOnly)) {
            if (!inserted) {
                out.append(replacement);
                inserted = true;
            }
            continue;
        }
        out.append(o);
    }
    if (!inserted) {
        out.append(replacement);
    }
    return out;
}

std::optional<QString> lastValue(const QStringList &options, const QString &name)
{
    std::optional<QString> v;
    for (const QString &o : options) {
        const auto item = options::split(o);
        if (item.name == name && item.value) {
            v = item.value;
        }
    }
    return v;
}

bool hasFlag(const QStringList &options, const QString &name)
{
    return options.contains(name);
}

std::optional<quint32> number(const std::optional<QString> &s)
{
    if (!s) {
        return std::nullopt;
    }
    bool ok = false;
    const qulonglong v = s->toULongLong(&ok, 10);
    if (!ok || v > 4294967294ULL) {
        return std::nullopt;
    }
    return static_cast<quint32>(v);
}

// "0077" -> "77", "000" -> "0"
QString normaliseOctal(const QString &s)
{
    qsizetype i = 0;
    while (i + 1 < s.size() && s.at(i) == QLatin1Char('0')) {
        ++i;
    }
    return s.mid(i);
}

Sharing readSharing(const QStringList &opts, OwnerModel model)
{
    if (model == OwnerModel::Mask) {
        if (lastValue(opts, QStringLiteral("dmask")) || lastValue(opts, QStringLiteral("fmask"))) {
            return Sharing::Custom;
        }
        const auto umask = lastValue(opts, QStringLiteral("umask"));
        if (!umask) {
            return Sharing::Unset;
        }
        const QString u = normaliseOctal(*umask);
        if (u == QLatin1String("77")) {
            return Sharing::OwnerOnly;
        }
        if (u == QLatin1String("22")) {
            return Sharing::OthersRead;
        }
        if (u == QLatin1String("0")) {
            return Sharing::Everyone;
        }
        return Sharing::Custom;
    }
    if (model == OwnerModel::Cifs) {
        const auto f = lastValue(opts, QStringLiteral("file_mode"));
        const auto d = lastValue(opts, QStringLiteral("dir_mode"));
        if (!f && !d) {
            return Sharing::Unset;
        }
        if (!f || !d) {
            return Sharing::Custom;
        }
        const QString pair = normaliseOctal(*f) + QLatin1Char('/') + normaliseOctal(*d);
        if (pair == QLatin1String("600/700")) {
            return Sharing::OwnerOnly;
        }
        if (pair == QLatin1String("644/755")) {
            return Sharing::OthersRead;
        }
        if (pair == QLatin1String("666/777")) {
            return Sharing::Everyone;
        }
        return Sharing::Custom;
    }
    return Sharing::Unset;
}
} // namespace

Applicable applicable(const QString &type, const options::Catalogue &catalogue)
{
    Applicable a;
    const auto family = catalogue.family(type);
    if (family == options::Family::Swap) {
        a.automount = false;
        a.who = false;
        a.netdev = false;
    }
    const OwnerModel m = catalogue.ownerModel(type);
    a.owner = m == OwnerModel::Mask || m == OwnerModel::Cifs;
    return a;
}

QStringList startOptions(Start start)
{
    switch (start) {
    case Start::AtBoot:
        return {};
    case Start::OnRequest:
        return {kNoauto};
    case Start::WhenOpened:
        return {kNoauto, kAutomount};
    }
    return {};
}

QStringList whoOptions(Who who)
{
    switch (who) {
    case Who::Admins:
        return {};
    case Who::AnyUser:
        return {kUser};
    case Who::AnyUserAnyoneUnmounts:
        return {kUsers};
    }
    return {};
}

QStringList sharingOptions(Sharing sharing, OwnerModel model)
{
    const bool cifs = model == OwnerModel::Cifs;
    switch (sharing) {
    case Sharing::Unset:
    case Sharing::Custom:
        return {};
    case Sharing::OwnerOnly:
        return cifs ? QStringList{QStringLiteral("file_mode=0600"), QStringLiteral("dir_mode=0700")} : QStringList{QStringLiteral("umask=077")};
    case Sharing::OthersRead:
        return cifs ? QStringList{QStringLiteral("file_mode=0644"), QStringLiteral("dir_mode=0755")} : QStringList{QStringLiteral("umask=022")};
    case Sharing::Everyone:
        return cifs ? QStringList{QStringLiteral("file_mode=0666"), QStringLiteral("dir_mode=0777")} : QStringList{QStringLiteral("umask=000")};
    }
    return {};
}

Settings read(const QStringList &opts, const QString &type, const options::Catalogue &catalogue)
{
    Settings s;
    if (hasFlag(opts, kAutomount)) {
        s.start = Start::WhenOpened;
    } else if (hasFlag(opts, kNoauto)) {
        s.start = Start::OnRequest;
    }
    for (const QString &o : opts) { // the last one wins, as in mount
        if (o == kUser) {
            s.who = Who::AnyUser;
        } else if (o == kUsers) {
            s.who = Who::AnyUserAnyoneUnmounts;
        } else if (o == kNouser) {
            s.who = Who::Admins;
        }
    }
    s.nofail = hasFlag(opts, kNofail);
    s.netdev = hasFlag(opts, kNetdev);
    const OwnerModel model = catalogue.ownerModel(type);
    if (model == OwnerModel::Mask || model == OwnerModel::Cifs) {
        s.uid = number(lastValue(opts, QStringLiteral("uid")));
        s.gid = number(lastValue(opts, QStringLiteral("gid")));
        s.sharing = readSharing(opts, model);
    }
    return s;
}

QStringList apply(const QStringList &opts, const Settings &settings, const QString &type, const options::Catalogue &catalogue)
{
    // Only aspects that differ from what the list already says are rewritten, so options
    // Anchorage can't represent (uid=name, unusual masks, explicit 'auto') stay untouched.
    const Settings old = read(opts, type, catalogue);
    const Applicable can = applicable(type, catalogue);
    QStringList out = opts;

    if (settings.start != old.start) {
        Start start = settings.start;
        if (start == Start::WhenOpened && !can.automount) {
            start = Start::OnRequest;
        }
        out = replaceAspect(out, {kAuto, kNoauto, kAutomount}, startOptions(start));
    }
    if (settings.who != old.who && can.who) {
        out = replaceAspect(out, {kUser, kUsers, kNouser}, whoOptions(settings.who), true);
    }
    if (settings.nofail != old.nofail) {
        out = replaceAspect(out, {kNofail}, settings.nofail ? QStringList{kNofail} : QStringList{});
    }
    if (settings.netdev != old.netdev && can.netdev) {
        out = replaceAspect(out, {kNetdev}, settings.netdev ? QStringList{kNetdev} : QStringList{});
    }
    if (can.owner) {
        const OwnerModel model = catalogue.ownerModel(type);
        if (settings.uid != old.uid) {
            out = replaceAspect(out, {QStringLiteral("uid")}, settings.uid ? QStringList{QStringLiteral("uid=%1").arg(*settings.uid)} : QStringList{});
        }
        if (settings.gid != old.gid) {
            out = replaceAspect(out, {QStringLiteral("gid")}, settings.gid ? QStringList{QStringLiteral("gid=%1").arg(*settings.gid)} : QStringList{});
        }
        if (settings.sharing != old.sharing && settings.sharing != Sharing::Custom) {
            const QSet<QString> names = model == OwnerModel::Cifs
                ? QSet<QString>{QStringLiteral("file_mode"), QStringLiteral("dir_mode")}
                : QSet<QString>{QStringLiteral("umask"), QStringLiteral("dmask"), QStringLiteral("fmask")};
            out = replaceAspect(out, names, sharingOptions(settings.sharing, model));
        }
    }
    // "defaults" only means "nothing else"; drop it once there is something else.
    if (out != opts && out.size() > 1) {
        out.removeAll(QStringLiteral("defaults"));
    }
    return out;
}

Settings defaultsFor(const QString &type, const options::Catalogue &catalogue)
{
    Settings s;
    const auto family = catalogue.family(type);
    s.nofail = true;
    if (family == options::Family::Network) {
        s.netdev = true;
        s.start = Start::WhenOpened;
    }
    return s;
}

} // namespace anchorage::access
