// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.7 (sharing077, NW-4ae26be): the host's fallback for a kill award the game only grants for a
// kill by entity 10000. Brain of Mensis (the Evil Eye, entity 2600129) gives the Living String through
// m26 event 12600029, whose condition is "HP 0 and damage by entity 10000" (EMEVD 3[23]); a guest's
// last hit is another entity, so the host never got the ritual material. Pure rule (tests/test_party_items.cpp);
// the tick is in party_items.cpp (KillAwardTick).
#pragma once

namespace coop::killaward {

constexpr double kGraceS = 2.5;   ///< the game's own event gets this long after the death to award by itself
constexpr double kPollS = 0.5;    ///< the host looks at the entity this often

struct State {
    double dead_since = 0;  ///< when the entity was first seen dead (0: not dead / not seen)
    bool done = false;      ///< awarded (by the game or by us) in this world: never again
};

/// True when the host should award the lot now. `seen`: the entity is loaded; `alive`: its HP > 0;
/// `lot_flag_set`: the lot's pickup flag is set in the host's world (the game's own event already ran).
inline bool Due(State* s, double now, bool seen, bool alive, bool lot_flag_set) {
    if (s->done) return false;
    if (lot_flag_set) {
        s->done = true;
        return false;
    }
    if (!seen) {  // gone from the map: it stays dead if it was seen dead (the chr is removed a while after its death)
        return s->dead_since > 0 && now - s->dead_since >= kGraceS;
    }
    if (alive) {
        s->dead_since = 0;
        return false;
    }
    if (s->dead_since <= 0) s->dead_since = now;
    return now - s->dead_since >= kGraceS;
}

}  // namespace coop::killaward
