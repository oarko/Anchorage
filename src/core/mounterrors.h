// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// Plain-language explanations for common mount errors (SPEC §11), shown under the error
// mount itself reported. Empty when there is nothing useful to add.
namespace anchorage::mounterrors
{

QString explain(const QString &fsType, const QString &message);

} // namespace anchorage::mounterrors
