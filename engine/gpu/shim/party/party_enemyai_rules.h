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

/// The enemies' target search (SprjTargetBankManager *0x553B098, a vector of every chr's target
/// entry, read by 0x12ADC50) has no HP check, and a party member's dead chr stays in it: the host's
/// own chr lies dead in its boss-fight limbo (and, spectating, is carried onto the followed member's
/// spot, 0.2 m from it), a dead guest's copy stays at its death spot. The boss then picks a dead
/// player as its nearest target and idles over it. On the AI owner's game a player chr at HP 0 in a
/// boss fight (or the host's limbo) leaves the bank until it gets up or the fight ends.
inline bool DeadPlayerOutOfBank(bool enabled, bool ai_owner, bool fight_or_limbo, int hp) {
    return enabled && ai_owner && fight_or_limbo && hp <= 0;
}

/// The bank vector edits (as the chr teardown 0x18BC110 does them): remove `x` from [b, e), shifting
/// the rest down; returns true when it was there (the caller stores e - 1).
template <class T>
inline bool BankRemove(T* b, T* e, T x) {
    for (T* p = b; p != e; ++p) {
        if (*p == x) {
            for (T* q = p; q + 1 != e; ++q) *q = q[1];
            return true;
        }
    }
    return false;
}

/// Put `x` back at the end of [b, e) (capacity `cap`): false when it is already there (a load built a
/// new entry) or there is no room (the vector grew meanwhile: the game's own allocator owns that).
template <class T>
inline bool BankInsert(T* b, T* e, T* cap, T x) {
    for (T* p = b; p != e; ++p) {
        if (*p == x) return false;
    }
    if (e >= cap) return false;
    *e = x;
    return true;
}

/// Boss attention sharing (0.7.7): the boss's own target search keeps the player it first took (or the
/// nearest one), so a guest who fights from a few metres more than the host is ignored. On the AI
/// owner's game, in a boss fight with two or more living players within `reach` of the boss, the
/// target is handed on every `hold` seconds to the one who was the target longest ago (never targeted
/// = oldest). `d2` squared distances to the boss (negative: dead / unreadable), `last` the time each
/// was last handed the target (0 = never), `current` the index handed it last (-1 none), `since` the
/// time it was handed. Returns the index to hand the target now, -1 for no change.
inline int PickAttention(const float* d2, const double* last, int n, float reach, int current, double since,
                         double now, double hold) {
    int live = 0, best = -1;
    for (int i = 0; i < n; ++i) {
        if (d2[i] < 0 || d2[i] > reach * reach) continue;
        ++live;
        if (best < 0 || last[i] < last[best] || (last[i] == last[best] && d2[i] < d2[best])) best = i;
    }
    if (live < 2) return -1;
    if (current >= 0 && current < n && d2[current] >= 0 && d2[current] <= reach * reach && now - since < hold) return -1;
    return best == current ? -1 : best;
}

}  // namespace enemyai_rules
