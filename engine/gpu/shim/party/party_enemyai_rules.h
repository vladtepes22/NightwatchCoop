// SPDX-License-Identifier: GPL-3.0-or-later
// Who runs an enemy's AI (ERSC parity C2 / split travel; docs/party/enemy_ai.md). Pure logic, unit
// test tests/test_party_enemyai_rules.cpp; the game side is party_enemyai.cpp.
//
// The game gives every NpcIns two manipulators: a ComManipulator (+0x3B8, the chr's own AI runs on
// this game) and a NetAIManipulator (+0x3C0, the AI runs on the owner's game and this one replays
// its frames). NpcIns::SetManipulator 0x18E6540(chr, force_local) picks one. In a client's world
// the owner is the host; a chr the host's game does not have (a map block only this guest loaded:
// split travel, "travel alone") never gets a frame, so it stands still until the host loads that
// block (player report lone_guest_split, 2026-10-25).
#pragma once

namespace enemyai_rules {

/// What the guest's game knows now.
struct View {
    bool enabled = true;     ///< BB_PARTY_LONE_AI is not 0
    bool client = false;     ///< SprjSessionManager role 6: a guest in the host's world
    bool world_up = false;   ///< a world is up, no loading screen
    int members_apart = 0;   ///< party members (the host included) in another area than this player
    bool host_apart = false; ///< the host itself is in another area (its vitals are fresh)
};

/// This game runs the AI of the enemies of the block it stands in: the host's game does not have
/// them (it is in another area), so nobody else would. Not for a host (its game is the owner), not
/// while another member of the same area could be driving them (the host is the only AI owner of a
/// shared world).
inline bool GuestRunsAi(const View& v) {
    return v.enabled && v.client && v.world_up && v.host_apart;
}

enum class Step {
    Keep,      ///< nothing changes
    Take,      ///< start running the AI: switch the loaded enemies to the local manipulator
    HandBack,  ///< the host drives again (same world, no load in between): let the game choose again
};

/// `owning`: this game runs the AI now (the last step was Take). `wants`: GuestRunsAi now.
inline Step Next(bool owning, bool wants) {
    if (wants && !owning) return Step::Take;
    if (!wants && owning) return Step::HandBack;
    return Step::Keep;
}

/// A load starts: the chrs are freed, so the next world begins with the game's own choice. Nothing
/// to hand back.
inline bool OwningAfterLoad(bool /*owning*/) { return false; }

/// Boss-fight limbo (the host lies dead, the guests fight on): the boss's AI keeps the dead host as
/// its target, so the living member nearest to the boss is forced as its target. `n` members with
/// squared distances `d2[i]` (negative: dead or unreadable); returns the index, -1 for none.
inline int PickLimboTarget(const float* d2, int n) {
    int best = -1;
    for (int i = 0; i < n; ++i) {
        if (d2[i] < 0) continue;
        if (best < 0 || d2[i] < d2[best]) best = i;
    }
    return best;
}

}  // namespace enemyai_rules
