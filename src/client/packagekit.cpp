// SPDX-License-Identifier: GPL-3.0-or-later
#include "packagekit.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QPointer>

namespace anchorage::packagekit
{

namespace
{
const QString kService = QStringLiteral("org.freedesktop.PackageKit");
const QString kTransaction = QStringLiteral("org.freedesktop.PackageKit.Transaction");
// PkBitfield values: a filter or flag enum value n is the bit 1 << n.
constexpr quint64 kFilterNotInstalled = quint64(1) << 3; // PK_FILTER_ENUM_NOT_INSTALLED
constexpr quint64 kFilterArch = quint64(1) << 18; // PK_FILTER_ENUM_ARCH: this machine's architecture
constexpr quint64 kFlagOnlyTrusted = quint64(1) << 1; // PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED
constexpr uint kExitSuccess = 1; // PK_EXIT_ENUM_SUCCESS
constexpr uint kExitCancelled = 3; // PK_EXIT_ENUM_CANCELLED

// One PackageKit transaction: collects Package/ErrorCode signals until Finished.
class Transaction : public QObject
{
    Q_OBJECT

public:
    explicit Transaction(QObject *parent)
        : QObject(parent)
    {
    }

    bool start(const std::function<bool(QDBusInterface &)> &call)
    {
        QDBusInterface pk(kService, QStringLiteral("/org/freedesktop/PackageKit"), kService, QDBusConnection::systemBus());
        const QDBusReply<QDBusObjectPath> path = pk.call(QStringLiteral("CreateTransaction"));
        if (!path.isValid()) {
            error = path.error().message();
            return false;
        }
        m_path = path.value().path();
        auto bus = QDBusConnection::systemBus();
        bus.connect(kService, m_path, kTransaction, QStringLiteral("Package"), this, SLOT(onPackage(uint, QString, QString)));
        bus.connect(kService, m_path, kTransaction, QStringLiteral("ErrorCode"), this, SLOT(onError(uint, QString)));
        bus.connect(kService, m_path, kTransaction, QStringLiteral("Finished"), this, SLOT(onFinished(uint, uint)));
        QDBusInterface t(kService, m_path, kTransaction, bus);
        t.call(QStringLiteral("SetHints"), QStringList{QStringLiteral("interactive=true")});
        if (!call(t)) {
            return false;
        }
        return true;
    }

    std::function<void()> finished; // NOLINT(readability-redundant-access-specifiers): members, then slots
    QStringList packageIds;
    QString error;
    uint exit = 0;

public Q_SLOTS:
    void onPackage(uint, const QString &id, const QString &)
    {
        packageIds.append(id);
    }
    void onError(uint, const QString &details)
    {
        error = details;
    }
    void onFinished(uint exitCode, uint)
    {
        exit = exitCode;
        if (finished) {
            finished();
        }
    }

private:
    QString m_path;
};

QString callError(QDBusInterface &t, const QString &method, const QVariantList &args)
{
    const QDBusMessage reply = t.callWithArgumentList(QDBus::Block, method, args);
    return reply.type() == QDBusMessage::ErrorMessage ? reply.errorMessage() : QString();
}
} // namespace

bool available()
{
    const auto *iface = QDBusConnection::systemBus().interface();
    if (!iface) {
        return false;
    }
    if (iface->isServiceRegistered(kService)) {
        return true;
    }
    const QDBusReply<QStringList> activatable = iface->activatableServiceNames();
    return activatable.isValid() && activatable.value().contains(kService);
}

void install(const QString &packageName, QObject *context, std::function<void(const Result &)> done) // NOLINT(performance-unnecessary-value-param): stored in a lambda
{
    QPointer<QObject> guard(context);
    auto finish = [guard, done](const Result &r) {
        if (guard) {
            done(r);
        }
    };
    auto *resolve = new Transaction(context);
    resolve->finished = [resolve, packageName, context, finish] {
        resolve->deleteLater();
        if (resolve->exit != kExitSuccess || resolve->packageIds.isEmpty()) {
            finish({false, false,
                    resolve->error.isEmpty() ? QStringLiteral("The package %1 was not found, or is already installed.").arg(packageName) : resolve->error});
            return;
        }
        auto *inst = new Transaction(context);
        inst->finished = [inst, finish] {
            inst->deleteLater();
            finish({inst->exit == kExitSuccess, inst->exit == kExitCancelled, inst->error});
        };
        const QStringList ids = {resolve->packageIds.first()};
        if (!inst->start([ids, inst](QDBusInterface &t) {
                inst->error = callError(t, QStringLiteral("InstallPackages"), {QVariant::fromValue(kFlagOnlyTrusted), ids});
                return inst->error.isEmpty();
            })) {
            inst->deleteLater();
            finish({false, false, inst->error});
        }
    };
    if (!resolve->start([packageName, resolve](QDBusInterface &t) {
            resolve->error = callError(t, QStringLiteral("Resolve"), {QVariant::fromValue(kFilterNotInstalled | kFilterArch), QStringList{packageName}});
            return resolve->error.isEmpty();
        })) {
        resolve->deleteLater();
        finish({false, false, resolve->error.isEmpty() ? QStringLiteral("PackageKit is not available.") : resolve->error});
    }
}

} // namespace anchorage::packagekit

#include "packagekit.moc"
