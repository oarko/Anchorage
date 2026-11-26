// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fstab.h"
#include "result.h"

#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// The option catalogue: every mount option the entry editor knows, with a plain-language
// description, default and valid values (SPEC §8, §11). The data lives in
// src/core/data/{filesystems,options}.json, compiled in as a Qt resource.
namespace anchorage::options
{

enum class Family { Local, Network, Virtual, Swap };
// How file ownership is chosen for a file system (Access tab, "Who owns the files").
enum class OwnerModel {
    Mask, // FAT, exFAT, NTFS: uid=, gid=, umask=
    Cifs, // SMB: uid=, gid=, file_mode=, dir_mode=
    Posix, // the files keep their own owners
    None, // swap
};

struct FileSystem {
    QString type;
    QString label;
    QString description;
    Family family = Family::Local;
    OwnerModel owner = OwnerModel::Posix;
    int pass = 0;
    bool testable = true;
};

enum class Kind { Flag, Value, FlagOrValue };
enum class ValueType { None, Number, Octal, Size, Text, Choice, Path, Seconds };
enum class Category { General, Boot, Desktop, FileSystem };

struct Option {
    QString name;
    QStringList fs; // types or groups (@all, @local, @network)
    QStringList notFs; // types excluded from a group (mount.cifs refuses some general options)
    Category category = Category::General;
    Kind kind = Kind::Flag;
    ValueType valueType = ValueType::None;
    QStringList choices; // allowed values (Choice) or suggestions (Text)
    QString example;
    std::optional<double> min;
    std::optional<double> max;
    QString defaultText;
    QString exclusive; // options sharing this group exclude each other
    bool access = false; // set on the Access tab
    QString alias; // same as this other option
    QString warning;
    QString description;
    // Mount sweep (tests/mounttest.cpp)
    QString testSkip; // reason, if not tested
    QStringList testWith; // extra options needed
    QString testValue; // value to test with (default: example or first choice)

    bool takesValue() const { return kind != Kind::Flag; }
    bool needsValue() const { return kind == Kind::Value; }
};

// One item of an option list, split at the first '='.
struct Item {
    QString name;
    std::optional<QString> value;
};
Item split(const QString &option);
QString join(const QString &name, const std::optional<QString> &value);

class Catalogue
{
public:
    // The built-in catalogue (loaded once; aborts if the compiled-in data is broken,
    // which the unit tests make sure can't ship).
    static const Catalogue &builtIn();
    static Result<Catalogue> fromJson(QByteArrayView fileSystems, QByteArrayView options);

    const QList<FileSystem> &fileSystems() const { return m_fileSystems; }
    const FileSystem *fileSystem(const QString &type) const;
    const QList<Option> &all() const { return m_options; }

    bool appliesTo(const Option &option, const QString &type) const;
    // Options that apply to `type`, in catalogue order. Unknown types get the general ones.
    QList<const Option *> optionsFor(const QString &type) const;
    // The catalogue entry for one option item on `type`, or nullptr if unknown.
    const Option *find(const QString &type, const Item &item) const;
    const Option *find(const QString &type, const QString &option) const { return find(type, split(option)); }

    Family family(const QString &type) const;
    OwnerModel ownerModel(const QString &type) const;

private:
    QList<FileSystem> m_fileSystems;
    QList<Option> m_options;
};

// Empty if `value` is acceptable for `option` (nullopt: written without '=value').
QString checkValue(const Option &option, const std::optional<QString> &value);

// Checks a whole option list for `type`: invalid values and contradictions are errors;
// unknown options, risky options and repeated options are warnings.
QList<fstab::Issue> check(const QStringList &options, const QString &type, const Catalogue &catalogue = Catalogue::builtIn());

} // namespace anchorage::options
