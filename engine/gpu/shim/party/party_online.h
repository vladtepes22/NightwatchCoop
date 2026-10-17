// SPDX-License-Identifier: GPL-3.0-or-later
// The game's FROM client going offline, logged with its reason (party_online.cpp).
#pragma once

namespace coop {

/// Hooks the drop-offline routine 0x1ea5ce0 (byte-verified prologue) to log msg id, API, result
/// code and callers each time the client marks the server offline. Call after HooksInit.
void OnlineDiagInit();

}  // namespace coop
