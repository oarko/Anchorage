// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config-anchorage.h"

#include <string_view>

// Fixed system paths (CLAUDE.md security rule 2).
//
// The root helper only ever writes to these. They are compile-time constants on
// purpose: no setting, environment variable or request from the GUI can change
// them. Code that needs a different location for testing takes paths as function
// parameters instead; the shipped helper passes these constants.
namespace anchorage::paths
{
inline constexpr std::string_view fstab = "/etc/fstab";
inline constexpr std::string_view configDir = ANCHORAGE_CONFIG_DIR;
inline constexpr std::string_view backupDir = ANCHORAGE_CONFIG_DIR "/backups";
inline constexpr std::string_view credentialsDir = ANCHORAGE_CONFIG_DIR "/credentials";
inline constexpr std::string_view configFile = ANCHORAGE_CONFIG_DIR "/config";
inline constexpr std::string_view blockedPathsFile = ANCHORAGE_CONFIG_DIR "/blocked-paths";
inline constexpr std::string_view polkitRulesFile = "/etc/polkit-1/rules.d/60-" ANCHORAGE_APP_BINARY ".rules";
}
