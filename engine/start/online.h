// SPDX-License-Identifier: GPL-3.0-or-later
// The online profile (was scripts/online.py): The Hunter's Dream's server for the engine's thd
// backend, the host overrides the game's requests follow, or offline.
#pragma once
#include "common.h"

namespace start {

/// Clears the engine's online environment and sets BB_ONLINE=0; always false (offline).
bool ConfigureOnline(const std::map<std::string, std::string>& settings, const fs::path& data);

} // namespace start
