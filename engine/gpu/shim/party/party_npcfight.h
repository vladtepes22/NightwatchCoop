// SPDX-License-Identifier: GPL-3.0-or-later
// ERSC parity D11 (0.7.3): scripted NPC hunter fights with the whole party, the game side. The
// rules (which events, which instructions, the assassins, the slots) are party_npcfight_rules.h;
// docs/party/npc_fights.md has the evidence.
//
//  - EMEVD dispatch filter (any role, the dispatching thread): a guest's copy of each GuestRun event
//    passes its gate and skips its progress writes while the host's rule is on; on the host, solo
//    or with the rule off the gate's argument is the data's own again.
//  - Main-thread tick (a guest in the host's world): the assassins follow the host-world flags.
//
// The host's rule: System > Party rules: World "NPC hunters fight all", bbport.ini party_npc_fights,
// BB_PARTY_NPC_FIGHTS at start (default on); a guest follows the host's (party_balance Rules).
#pragma once

namespace coop {
struct GameSnapshot;
}

namespace coop::npcfight {

/// PartyInit (after the seamless rules): installs the dispatch filter's predicate.
void Init();
/// The director's tick (main thread, party mode). `host`: this game hosts.
void Tick(const GameSnapshot& s, bool host);

}  // namespace coop::npcfight
