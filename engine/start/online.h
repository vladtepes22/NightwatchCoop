// SPDX-License-Identifier: GPL-2.0-or-later
// The online profile (was scripts/online.py): always offline in Nightwatch Co-op, whose co-op runs
// through the party's own network layer.
#pragma once
#include "common.h"

namespace start {

/// Clears the engine's online environment and sets BB_ONLINE=0; always false (offline).
bool ConfigureOnline(const std::map<std::string, std::string>& settings, const fs::path& data);

} // namespace start
