// SPDX-License-Identifier: GPL-3.0-or-later
#include "logging.h"

#include "config-anchorage.h"

#include <QRegularExpression>

namespace anchorage::logging
{
#if ANCHORAGE_BETA
constexpr QtMsgType defaultLevel = QtInfoMsg;
#else
constexpr QtMsgType defaultLevel = QtWarningMsg;
#endif

Q_LOGGING_CATEGORY(network, "anchorage.network", defaultLevel)
Q_LOGGING_CATEGORY(mount, "anchorage.mount", defaultLevel)
Q_LOGGING_CATEGORY(fstab, "anchorage.fstab", defaultLevel)
Q_LOGGING_CATEGORY(helper, "anchorage.helper", defaultLevel)

bool betaBuild()
{
    return ANCHORAGE_BETA != 0;
}

void setMessagePattern()
{
    qSetMessagePattern(QStringLiteral("%{if-debug}DEBUG%{endif}%{if-info}INFO%{endif}%{if-warning}WARNING%{endif}"
                                      "%{if-critical}ERROR%{endif}%{if-fatal}FATAL%{endif}: %{if-category}%{category}: %{endif}%{message}"));
}

QString scrub(const QString &text, const QStringList &secrets)
{
    QString out = text;
    for (const QString &s : secrets) {
        if (!s.isEmpty()) {
            out.replace(s, QStringLiteral("…"));
        }
    }
    // An option's value runs to the next comma or whitespace.
    static const QRegularExpression option(QStringLiteral("\\b(pass(?:word)?)=[^,\\s]*"), QRegularExpression::CaseInsensitiveOption);
    out.replace(option, QStringLiteral("\\1=…"));
    return out;
}
} // namespace anchorage::logging
