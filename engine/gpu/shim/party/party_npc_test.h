// SPDX-License-Identifier: GPL-3.0-or-later
// Single-instance NPC-summon test fixture (docs/party/npc_peer.md sections 2 and 5): summons an
// NPC phantom ("AI player", CSMultiNPCPlayerInsTask) from the director's main-thread tick and logs
// the session/cooperator machinery it goes through. One game instance, no network: it covers
// CSMultiPlayMan's NPC vector, the NetworkFlow 5-entry slot table and its cap, the cooperator
// count 0x15bdc20, the SOS request filter 0x1874710 (with its boss-cleared rejection 0x18749E8),
// event flag 6009 and the send-home path.
//
// Env:
//   BB_PARTY_TEST_NPC=<entity>[:<session type>][,...] | auto
//       entities to summon (session type 26..33; default: the type the map's EMEVD template uses
//       for a known entity, else 27). "auto": every known NPC summon entity (npc_peer.md 1.1)
//       that exists in the loaded maps (the first one only, except in cap mode).
//   BB_PARTY_TEST_NPC_MODE=summon (default) | cap | filter | log
//       summon: the faithful path (SetDisable(0) + SosSel_BuildRequestFromChr 0x1878d90) for each
//               entity, once per load.
//       cap:    one entity every 3 s through the direct path (EnsureNpcTask 0x1e54c30 +
//               NetFlowSlots_Register 0x15bc590; BB_PARTY_TEST_NPC_PATH=a for the faithful one),
//               logs Register's answer, the slot count against the member cap and the cooperator
//               count after each one, then a summary line.
//       filter: A/B/C of the request filters, one faithful request each, 4 s apart, until a task appears:
//               A with the 1.09 bytes; B with the boss-cleared rejections 0x18749E8 / 0x18749F0 NOP'd
//               ("Party: Bells after boss defeated"); C with B plus the status producer's area
//               restriction 0x18700D3 -> 0 (part of "Party: Bells anywhere"). Written at runtime,
//               restored afterwards. A wrapper on 0x1874710 logs each request's fate ("SOS filter:").
//       log:    no summon; logs the state only (call-path and slot-table check in any map).
//   BB_PARTY_TEST_NPC_DELAY=<s>   seconds in a steady world before the first summon (default 5)
//   BB_PARTY_TEST_NPC_RETURN=<s>  send every summoned NPC home (0x15be2a0) after <s> seconds
//   BB_PARTY_TEST_NPC_LOG=0       no per-second state line
//   BB_PARTY_TEST_NPC_PATCH=1     summon/cap modes: both patch groups of filter mode C on for the run
//   BB_PARTY_TEST_NPC_WARP=<id>   once, before anything else: lamp warp 0x13cdf30(<WarpParam id>) when
//                                 the world is not in that map (e.g. 2412951 Central Yharnam, m24_01)
//
// Log lines start with "Party NPC:"; the once-a-second line is "Party NPC: state ...".
#pragma once

#include "game_state.h"

namespace coop {

/// BB_PARTY_TEST_NPC is set (the party layer must then start, see PartyRequested).
bool NpcTestRequested();
/// Main-thread tick (the director's), with the state the director just read.
void NpcTestTick(const GameSnapshot& s);

} // namespace coop
