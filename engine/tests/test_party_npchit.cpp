// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.5 (gpu/shim/party/party_npchit_rules.h): a guest's blow on an NPC counts as the host's.
#include "party/party_npchit_rules.h"

#include <cassert>

using coop::npchit::RecordedAttacker;

int main() {
    const std::uint64_t host = 0x100, guest = 0x200, npc = 0x300;
    assert(RecordedAttacker(true, guest, true, npc, false, host) == host);   // guest -> NPC on the host
    assert(RecordedAttacker(true, host, false, npc, false, host) == host);   // the host's own blow
    assert(RecordedAttacker(true, guest, true, host, true, host) == guest);  // PvP on the host stays
    assert(RecordedAttacker(true, guest, true, guest, true, host) == guest);
    assert(RecordedAttacker(false, guest, true, npc, false, host) == guest); // a guest's game: unchanged
    assert(RecordedAttacker(true, 0, true, npc, false, host) == 0);          // no attacker
    assert(RecordedAttacker(true, guest, true, 0, false, host) == guest);    // no victim
    assert(RecordedAttacker(true, guest, true, npc, false, 0) == guest);     // no local player
    return 0;
}
