// SPDX-License-Identifier: GPL-3.0-or-later
#include "options.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cstdlib>
#include <string_view>

namespace anchorage::options
{

namespace
{
template<typename E>
std::optional<E> lookup(const QHash<QString, E> &map, const QString &key)
{
    const auto it = map.constFind(key);
    return it == map.cend() ? std::nullopt : std::optional<E>(*it);
}

QStringList stringList(const QJsonValue &v)
{
    QStringList out;
    if (v.isString()) {
        out.append(v.toString());
    }
    for (const auto &x : v.toArray()) {
        out.append(x.toString());
    }
    return out;
}

} // namespace

namespace data
{
// catalogue-data.cpp, generated from src/core/data/*.json
extern const std::string_view fileSystems;
extern const std::string_view options;
}

Item split(const QString &option)
{
    const qsizetype eq = option.indexOf(QLatin1Char('='));
    if (eq < 0) {
        return {option, std::nullopt};
    }
    return {option.left(eq), option.mid(eq + 1)};
}

QString join(const QString &name, const std::optional<QString> &value)
{
    return value ? name + QLatin1Char('=') + *value : name;
}

Result<Catalogue> Catalogue::fromJson(QByteArrayView fileSystems, QByteArrayView options)
{
    using R = Result<Catalogue>;
    Catalogue c;

    QJsonParseError err{};
    const QJsonDocument fsDoc = QJsonDocument::fromJson(fileSystems.toByteArray(), &err);
    if (err.error != QJsonParseError::NoError) {
        return R::failure(QStringLiteral("filesystems.json: %1 at %2").arg(err.errorString()).arg(err.offset));
    }
    static const QHash<QString, Family> families = {{QStringLiteral("local"), Family::Local},
                                                    {QStringLiteral("network"), Family::Network},
                                                    {QStringLiteral("virtual"), Family::Virtual},
                                                    {QStringLiteral("swap"), Family::Swap}};
    static const QHash<QString, OwnerModel> owners = {{QStringLiteral("mask"), OwnerModel::Mask},
                                                      {QStringLiteral("cifs"), OwnerModel::Cifs},
                                                      {QStringLiteral("posix"), OwnerModel::Posix},
                                                      {QStringLiteral("none"), OwnerModel::None}};
    for (const auto &v : fsDoc.object().value(QStringLiteral("filesystems")).toArray()) {
        const QJsonObject o = v.toObject();
        FileSystem fs;
        fs.type = o.value(QStringLiteral("type")).toString();
        fs.label = o.value(QStringLiteral("label")).toString();
        fs.description = o.value(QStringLiteral("description")).toString();
        const auto family = lookup(families, o.value(QStringLiteral("family")).toString());
        const auto owner = lookup(owners, o.value(QStringLiteral("owner")).toString());
        if (fs.type.isEmpty() || fs.label.isEmpty() || fs.description.isEmpty() || !family || !owner) {
            return R::failure(QStringLiteral("filesystems.json: incomplete entry '%1'").arg(fs.type));
        }
        fs.family = *family;
        fs.owner = *owner;
        fs.pass = o.value(QStringLiteral("pass")).toInt();
        fs.testable = o.value(QStringLiteral("test")).toBool(true);
        if (c.fileSystem(fs.type)) {
            return R::failure(QStringLiteral("filesystems.json: '%1' is listed twice").arg(fs.type));
        }
        c.m_fileSystems.append(fs);
    }
    if (c.m_fileSystems.isEmpty()) {
        return R::failure(QStringLiteral("filesystems.json: no file systems"));
    }

    const QJsonDocument optDoc = QJsonDocument::fromJson(options.toByteArray(), &err);
    if (err.error != QJsonParseError::NoError) {
        return R::failure(QStringLiteral("options.json: %1 at %2").arg(err.errorString()).arg(err.offset));
    }
    static const QHash<QString, Kind> kinds = {{QStringLiteral("flag"), Kind::Flag},
                                               {QStringLiteral("value"), Kind::Value},
                                               {QStringLiteral("flag+value"), Kind::FlagOrValue}};
    static const QHash<QString, ValueType> types = {{QStringLiteral("number"), ValueType::Number},
                                                    {QStringLiteral("octal"), ValueType::Octal},
                                                    {QStringLiteral("size"), ValueType::Size},
                                                    {QStringLiteral("text"), ValueType::Text},
                                                    {QStringLiteral("choice"), ValueType::Choice},
                                                    {QStringLiteral("path"), ValueType::Path},
                                                    {QStringLiteral("seconds"), ValueType::Seconds}};
    static const QHash<QString, Category> categories = {{QStringLiteral("general"), Category::General},
                                                        {QStringLiteral("boot"), Category::Boot},
                                                        {QStringLiteral("desktop"), Category::Desktop},
                                                        {QStringLiteral("filesystem"), Category::FileSystem}};
    static const QSet<QString> groups = {QStringLiteral("@all"), QStringLiteral("@local"), QStringLiteral("@network")};

    for (const auto &v : optDoc.object().value(QStringLiteral("options")).toArray()) {
        const QJsonObject o = v.toObject();
        Option opt;
        opt.name = o.value(QStringLiteral("n")).toString();
        const QString where = QStringLiteral("options.json: '%1': ").arg(opt.name);
        opt.fs = stringList(o.value(QStringLiteral("fs")));
        opt.notFs = stringList(o.value(QStringLiteral("not")));
        const auto kind = lookup(kinds, o.value(QStringLiteral("k")).toString());
        const auto category = lookup(categories, o.value(QStringLiteral("cat")).toString());
        opt.description = o.value(QStringLiteral("desc")).toString();
        if (opt.name.isEmpty() || opt.fs.isEmpty() || !kind || !category || opt.description.isEmpty()) {
            return R::failure(where + QStringLiteral("missing n, fs, k, cat or desc"));
        }
        opt.kind = *kind;
        opt.category = *category;
        for (const QString &f : std::as_const(opt.notFs)) {
            if (!c.fileSystem(f)) {
                return R::failure(where + QStringLiteral("unknown file system '%1' in not").arg(f));
            }
        }
        for (const QString &f : std::as_const(opt.fs)) {
            if (f.startsWith(QLatin1Char('@')) ? !groups.contains(f) : !c.fileSystem(f)) {
                return R::failure(where + QStringLiteral("unknown file system '%1'").arg(f));
            }
        }
        const QJsonObject val = o.value(QStringLiteral("v")).toObject();
        if (opt.kind == Kind::Flag) {
            if (!val.isEmpty()) {
                return R::failure(where + QStringLiteral("a flag has no value"));
            }
        } else {
            const auto type = lookup(types, val.value(QStringLiteral("type")).toString());
            if (!type) {
                return R::failure(where + QStringLiteral("missing or unknown value type"));
            }
            opt.valueType = *type;
            opt.choices = stringList(val.value(QStringLiteral("ch")));
            opt.example = val.value(QStringLiteral("ex")).toString();
            if (val.contains(QStringLiteral("min"))) {
                opt.min = val.value(QStringLiteral("min")).toDouble();
            }
            if (val.contains(QStringLiteral("max"))) {
                opt.max = val.value(QStringLiteral("max")).toDouble();
            }
            if (opt.valueType == ValueType::Choice && opt.choices.isEmpty()) {
                return R::failure(where + QStringLiteral("a choice needs choices"));
            }
        }
        opt.defaultText = o.value(QStringLiteral("d")).toString();
        opt.exclusive = o.value(QStringLiteral("x")).toString();
        opt.access = o.value(QStringLiteral("a")).toBool();
        opt.alias = o.value(QStringLiteral("alias")).toString();
        opt.warning = o.value(QStringLiteral("warn")).toString();
        const QJsonObject t = o.value(QStringLiteral("t")).toObject();
        opt.testSkip = t.value(QStringLiteral("skip")).toString();
        opt.testWith = stringList(t.value(QStringLiteral("with")));
        opt.testValue = t.value(QStringLiteral("value")).toString();
        if (opt.takesValue() && !opt.example.isEmpty()) {
            if (const QString e = checkValue(opt, opt.example); !e.isEmpty()) {
                return R::failure(where + QStringLiteral("example is invalid: ") + e);
            }
        }
        // The same name may appear once per file system (e.g. discard, uid).
        for (const Option &other : std::as_const(c.m_options)) {
            if (other.name != opt.name) {
                continue;
            }
            for (const FileSystem &fs : std::as_const(c.m_fileSystems)) {
                if (c.appliesTo(other, fs.type) && c.appliesTo(opt, fs.type) && other.takesValue() == opt.takesValue()) {
                    return R::failure(where + QStringLiteral("listed twice for %1").arg(fs.type));
                }
            }
        }
        c.m_options.append(opt);
    }
    for (const Option &opt : std::as_const(c.m_options)) {
        if (!opt.alias.isEmpty()) {
            bool found = false;
            for (const Option &other : std::as_const(c.m_options)) {
                found = found || (other.name == opt.alias && other.alias.isEmpty());
            }
            if (!found) {
                return R::failure(QStringLiteral("options.json: '%1' is an alias of unknown '%2'").arg(opt.name, opt.alias));
            }
        }
    }
    return c;
}

const Catalogue &Catalogue::builtIn()
{
    static const Catalogue catalogue = [] {
        auto c = fromJson(QByteArrayView(data::fileSystems.data(), qsizetype(data::fileSystems.size())),
                          QByteArrayView(data::options.data(), qsizetype(data::options.size())));
        if (!c) {
            qFatal("built-in option catalogue is broken: %s", qPrintable(c.error()));
        }
        return *c;
    }();
    return catalogue;
}

const FileSystem *Catalogue::fileSystem(const QString &type) const
{
    for (const FileSystem &fs : m_fileSystems) {
        if (fs.type == type) {
            return &fs;
        }
    }
    return nullptr;
}

Family Catalogue::family(const QString &type) const
{
    if (const FileSystem *fs = fileSystem(type)) {
        return fs->family;
    }
    // fuse.sshfs, glusterfs, ... count as network; anything else as local.
    static const QRegularExpression net(QStringLiteral("^(fuse\\.sshfs|sshfs|glusterfs|ceph|9p|davfs|fuse\\.rclone)$"));
    return net.match(type).hasMatch() ? Family::Network : Family::Local;
}

OwnerModel Catalogue::ownerModel(const QString &type) const
{
    const FileSystem *fs = fileSystem(type);
    return fs ? fs->owner : OwnerModel::Posix;
}

bool Catalogue::appliesTo(const Option &option, const QString &type) const
{
    if (option.notFs.contains(type)) {
        return false;
    }
    const Family fam = family(type);
    for (const QString &f : option.fs) {
        if (f == type) {
            return true;
        }
        if (f == QLatin1String("@all") && fam != Family::Swap) {
            return true;
        }
        if (f == QLatin1String("@local") && fam == Family::Local) {
            return true;
        }
        if (f == QLatin1String("@network") && fam == Family::Network) {
            return true;
        }
    }
    return false;
}

QList<const Option *> Catalogue::optionsFor(const QString &type) const
{
    QList<const Option *> out;
    for (const Option &o : m_options) {
        if (appliesTo(o, type)) {
            out.append(&o);
        }
    }
    return out;
}

const Option *Catalogue::find(const QString &type, const Item &item) const
{
    // Prefer the entry whose kind fits (cifs "user=anna" is username, "user" is the flag).
    const Option *fallback = nullptr;
    for (const Option &o : m_options) {
        if (o.name != item.name || !appliesTo(o, type)) {
            continue;
        }
        const bool fits = item.value ? o.takesValue() : !o.needsValue();
        if (fits) {
            return &o;
        }
        fallback = fallback ? fallback : &o;
    }
    return fallback;
}

QString checkValue(const Option &option, const std::optional<QString> &value)
{
    if (!value) {
        return option.needsValue() ? QStringLiteral("%1 needs a value, e.g. %1=%2").arg(option.name, option.example.isEmpty() ? QStringLiteral("…") : option.example)
                                   : QString();
    }
    if (option.kind == Kind::Flag) {
        return QStringLiteral("%1 doesn't take a value.").arg(option.name);
    }
    const QString &v = *value;
    if (v.isEmpty() && option.valueType != ValueType::Text) {
        return QStringLiteral("%1 needs a value after '='.").arg(option.name);
    }
    if (v.contains(QLatin1Char(','))) {
        return QStringLiteral("A value can't contain a comma.");
    }
    switch (option.valueType) {
    case ValueType::None:
    case ValueType::Text:
        return {};
    case ValueType::Choice:
        return option.choices.contains(v) ? QString() : QStringLiteral("%1 must be one of: %2.").arg(option.name, option.choices.join(QStringLiteral(", ")));
    case ValueType::Number: {
        static const QRegularExpression re(QStringLiteral("^-?[0-9]{1,20}$"));
        if (!re.match(v).hasMatch()) {
            return QStringLiteral("%1 must be a whole number.").arg(option.name);
        }
        const double d = v.toDouble();
        if ((option.min && d < *option.min) || (option.max && d > *option.max)) {
            return QStringLiteral("%1 must be between %2 and %3.")
                .arg(option.name, QString::number(option.min.value_or(0), 'f', 0), QString::number(option.max.value_or(0), 'f', 0));
        }
        return {};
    }
    case ValueType::Octal: {
        static const QRegularExpression re(QStringLiteral("^[0-7]{1,4}$"));
        return re.match(v).hasMatch() ? QString() : QStringLiteral("%1 must be an octal number such as 022 or 0755.").arg(option.name);
    }
    case ValueType::Size: {
        static const QRegularExpression re(QStringLiteral("^[0-9]{1,20}([kKmMgGtTpPeE]|%)?$"));
        return re.match(v).hasMatch() ? QString() : QStringLiteral("%1 must be a size such as 4096, 64k, 2G or 25%.").arg(option.name);
    }
    case ValueType::Seconds: {
        // systemd time span: 90, 10s, 5min, 1h 30min is not possible (no spaces in fstab).
        static const QRegularExpression re(QStringLiteral("^(infinity|([0-9]+(\\.[0-9]+)?(us|ms|s|sec|m|min|h|hr|d|w)?)+)$"));
        return re.match(v).hasMatch() ? QString() : QStringLiteral("%1 must be a time such as 30, 10s, 5min or 1h.").arg(option.name);
    }
    case ValueType::Path:
        return v.startsWith(QLatin1Char('/')) ? QString() : QStringLiteral("%1 must be a full path starting with /.").arg(option.name);
    }
    return {};
}

QList<fstab::Issue> check(const QStringList &options, const QString &type, const Catalogue &catalogue)
{
    using fstab::Issue;
    QList<Issue> issues;
    const auto add = [&issues](Issue::Severity s, const QString &msg) {
        issues.append({s, QStringLiteral("options"), msg});
    };
    QHash<QString, QString> exclusive; // group -> first option seen
    QSet<QString> seen;
    QStringList unknown;
    for (const QString &raw : options) {
        const Item item = split(raw);
        const Option *opt = catalogue.find(type, item);
        if (seen.contains(item.name) && item.name != QLatin1String("device")) {
            add(Issue::Severity::Warning, QStringLiteral("%1 is set more than once; the last one wins.").arg(item.name));
        }
        seen.insert(item.name);
        if (!opt) {
            unknown.append(item.name);
            continue;
        }
        if (const QString e = checkValue(*opt, item.value); !e.isEmpty()) {
            add(Issue::Severity::Error, e);
        }
        if (!opt->warning.isEmpty()) {
            add(Issue::Severity::Warning, QStringLiteral("%1: %2").arg(item.name, opt->warning));
        }
        if (!opt->exclusive.isEmpty()) {
            const QString first = exclusive.value(opt->exclusive);
            if (!first.isEmpty() && first != item.name) {
                add(Issue::Severity::Warning, QStringLiteral("%1 and %2 contradict each other; the last one wins.").arg(first, item.name));
            } else {
                exclusive.insert(opt->exclusive, item.name);
            }
        }
    }
    if (!unknown.isEmpty()) {
        add(Issue::Severity::Warning,
            QStringLiteral("Not in Anchorage's list for %1: %2. Check the spelling; mount refuses options it doesn't know.")
                .arg(type, unknown.join(QStringLiteral(", "))));
    }
    return issues;
}

} // namespace anchorage::options
