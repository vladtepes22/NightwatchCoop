// SPDX-License-Identifier: GPL-3.0-or-later
// "Stay in my world" (CL-adeb50), the game side. The rules and the wire are party_stayhome_rules.h.
#pragma once

#include "party_stayhome_rules.h"

#include <string>

namespace party {
class PartyLink;
}

namespace coop::stayhome {

/// This game's own switch (BbSettings party_stay; live).
bool Active();
/// The link's callback thread: EVENT stay_home from guest `slot` (host side).
void OnEvent(int slot, const std::string& body);
/// Host, any thread: guest `slot` reported that it stays in its own world (and the report is fresh).
bool SlotStays(int slot);
/// Main thread, once a frame (the director). A guest reports its state to the host; a host forgets
/// the members that left.
void Tick(party::PartyLink* link, bool host);

}  // namespace coop::stayhome
