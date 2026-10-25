// SPDX-License-Identifier: GPL-3.0-or-later
// A player's own world must be built with a normal character (chr type 0). Pure logic, no game access
// (unit test tests/test_party_ownworld_rules.cpp); the game side is party_b2.cpp (OwnWorldTypeHook).
//
// Player reports (0.7.5, Nightwatch 0.2.4 beta 1; an older save "stuck in own world"): a guest that had
// been summoned could not talk, use headstones or lamps or pick anything up in its own Hunter's Dream.
// Logs: the load report's `player numbers: own 0 (type 1`. RE: the game builds the local PlayerIns in
// MoveMapStep's per-frame step (0x1939470) with the chr type PlayerGameData +0xA4 ("remembered type":
// 0x13CDE30 stores the current chr's type there at every leave / death, the save keeps it). A summoned
// guest's value is 1 (white phantom). A save written (or a game closed / crashed) while summoned keeps 1,
// and the next own-world load then builds a phantom: the game's host-only EMEVD branches, talk scripts
// (IsClientPlayer) and the action-button hold treat it as a client in its own world.
#pragma once

namespace coop::ownworld {

/// GSM flow +0x84 value of a summoned (client) load.
constexpr int kSummonFlow = 3;

/// Whether the remembered chr type must be cleared before the step builds the chr. `flow` is the network
/// flow state (+0x84: 3 = this load builds another player's world), `remembered` PlayerGameData +0xA4,
/// `pending` GameDataMan +0x8C (-1 = none; the step copies a pending type over +0xA4 itself, the game's own
/// value wins).
inline bool ClearRememberedType(int flow, int remembered, int pending) {
    return flow != kSummonFlow && remembered > 0 && pending == -1;
}

}  // namespace coop::ownworld
