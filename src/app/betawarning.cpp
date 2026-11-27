// SPDX-License-Identifier: GPL-3.0-or-later
#include "betawarning.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardGuiItem>

namespace anchorage::beta
{
QString dontShowAgainKey(const QString &version)
{
    return QStringLiteral("BetaWarning-") + version;
}

bool confirmLaunch(QWidget *parent, const QString &version)
{
    const QString text =
        xi18nc("@info",
               "<para>This is a beta version of Anchorage (%1), made for testing.</para>"
               "<para>Anchorage changes system files such as <filename>/etc/fstab</filename>, and a mistake there can stop a "
               "computer from starting. Please try it in a virtual machine or on a computer you don't depend on, and "
               "<emphasis strong='true'>never on production systems</emphasis>.</para>"
               "<para>Anchorage saves a backup before every change to <filename>/etc/fstab</filename>; you can restore one "
               "on the Backups page.</para>",
               version);
    const auto answer = KMessageBox::warningContinueCancel(parent, text, i18nc("@title:window", "Anchorage Beta"),
                                                           KGuiItem(i18nc("@action:button", "I Understand"), QStringLiteral("dialog-ok")),
                                                           KStandardGuiItem::quit(), dontShowAgainKey(version));
    return answer == KMessageBox::Continue;
}
} // namespace anchorage::beta
