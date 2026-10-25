// SPDX-License-Identifier: GPL-3.0-or-later
// ERSC parity C2 / split travel (0.7.6): enemies stand still on a lone guest's screen. The game side
// of party_enemyai_rules.h; docs/party/enemy_ai.md has the evidence.
//
// NpcIns::SetManipulator 0x18E6540 (chr, force_local) puts a client's enemies on the
// NetAIManipulator (the host's game runs their AI and sends the frames). A guest that travelled
// alone into a map the host has not loaded gets no frames, so the Cathedral Ward's enemies
// stood still until the host loaded the map. While the host is in another area this game keeps the
// local ComManipulator on its enemies (a replaced prologue forces `force_local`, and the tick
// switches the ones already on the net manipulator); once the host is in the guest's area again
// the game chooses again.
//
// Env: BB_PARTY_LONE_AI=0 turns it off (A/B); BB_PARTY_AI_LOG=1 (diagnostics) logs the manipulator,
// owner-table entry and vfunc +0x4C8 of the six nearest enemies when they change.
#pragma once

namespace coop {
struct GameSnapshot;
}

namespace coop::enemyai {

/// PartyInit (after HooksInit): installs the SetManipulator hook.
void Init();
/// The director's tick (main thread, party mode). `guest`: this game is a guest.
void Tick(const GameSnapshot& s, bool guest);

}  // namespace coop::enemyai
