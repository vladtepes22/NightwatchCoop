// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.5 "a guest's blows turn a friendly NPC hostile": whose blow the host's damage records name.
// The EMEVD damage conditions (3[23]: Alfred's three hits, Eileen, ...) read the records of
// SprjEventMan +0x38 (written by 0x13D5190 from the damage step) for attacker entity 10000, the
// host's player. Pure rule (tests/test_party_npchit.cpp); the hook is in party_b2.cpp.
#pragma once

#include <cstdint>

namespace coop::npchit {

/// The chr a damage record names as attacker. `attacker_remote`: a guest's chr on the host's game;
/// `victim_player`: the victim is a player (the host or a guest: party PvP stays a duel, never a
/// credit). Only the host's game credits, only a guest's blow on a non-player.
inline std::uint64_t RecordedAttacker(bool host, std::uint64_t attacker, bool attacker_remote, std::uint64_t victim,
                                      bool victim_player, std::uint64_t local_player) {
    if (!host || !attacker || !victim || !local_player || !attacker_remote || victim_player) return attacker;
    return local_player;
}

}  // namespace coop::npchit
