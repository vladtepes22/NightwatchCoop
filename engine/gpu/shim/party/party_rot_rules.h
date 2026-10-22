// SPDX-License-Identifier: GPL-3.0-or-later
// C4 rot, what cleanses part of it (0.7.2, user request 2026-10-22): the pure rules, header only
// (unit test: tests/test_party_rot.cpp, cmake --build out/gpu --target party-rot-test). The game
// side is party_balance.cpp; the rules (on / off) are the host's (party_balance_rules.h Rules).
//
//   boss     a boss's death clears all the rot of every party member in the host's world, alive
//            or dead in that fight (with the boss's other party rewards).
//   killer   the enemy that killed you dies (killed by anyone in the party): the stack that death
//            added goes. The killer is the attacker of the lethal hit (the game's kill log call
//            0x1FEB2A0(victim, attacker), made by the damage step 0x1A2A600 when a hit leaves the
//            victim at HP 0). A fall, the environment (a trap object: 0x1FEBFF0), a scripted death,
//            yourself or another player: no killer, nothing to kill. A rest or a wipe respawns the
//            killer: the link is gone.
//   echoes   picking up your bloodstain (the Blood Echoes came back) removes the stack of the
//            death that dropped it. There is one stain: the last death's. A later death replaces
//            it (its echoes are lost, the stack stays); a stack the killer's death removed first
//            is not removed twice.
// One stack per death: the Ledger keeps, for each death that added a stack, its kind and what
// can still cleanse it. A rest (a lamp, a level up) clears everything, as before.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coop::rot {

constexpr int kKinds = 5;      ///< party_rot.txt's kinds: health, stamina, stamina cost, rally, discovery
constexpr int kMaxStacks = 5;

/// What killed a player (the attacker of the lethal hit). Nothing (all zero): no killer.
struct Killer {
    std::uint32_t entity = 0;  ///< map entity id (chr +0x1F0; the same on every game; 0 none)
    std::uint64_t chr = 0;     ///< the chr instance on this game (0 none / gone after a load)
    std::uint32_t key = 0;     ///< its census key (chr +0x3B8): another instance has another key
    bool Any() const { return entity != 0 || chr != 0; }
};

/// The lethal hit's attacker as a killer: only an enemy counts (a map chr of the enemy census).
/// `seen_s`: seconds from the kill log call to the HP 0 seen by the tick (negative: no call);
/// `self` the attacker is the local player, `enemy` it is in the enemy census.
constexpr double kKillerWindowS = 3.0;
inline bool KillerCounts(double seen_s, bool self, bool enemy) {
    return seen_s >= 0 && seen_s <= kKillerWindowS && !self && enemy;
}
/// A map entity id the whole party knows the enemy by (players are 10000.., no id is 0 / -1).
inline bool PartyEntity(std::uint32_t entity) { return entity >= 100000 && entity != 0xFFFFFFFFu; }

/// One death that added a stack.
struct Death {
    int kind = -1;            ///< the stack's kind (0..4)
    Killer killer;
    bool killer_link = false; ///< its killer still lives (this world)
    bool stain_link = false;  ///< its bloodstain is still there (the last death's)
};

/// What a cleanse removed: the kinds of the stacks (one per death).
using Removed = std::vector<int>;

class Ledger {
public:
    /// A death. `kind`: the stack it added (-1: none: rot off, at the maximum, the prologue). The
    /// earlier death's bloodstain is gone either way (its stack stays).
    void OnDeath(int kind, const Killer& k) {
        for (Death& d : d_) d.stain_link = false;
        if (kind >= 0 && kind < kKinds) {
            Death d;
            d.kind = kind;
            d.killer = k;
            d.killer_link = k.Any();
            d.stain_link = true;
            d_.push_back(d);
        }
        Prune();
    }
    /// An enemy died: by its entity id (the kill record, any game of the party) or its chr on this
    /// game. Every death it caused gives back its stack (`take`; else only its killer link goes:
    /// the rule is off, its bloodstain may still give the stack back).
    Removed OnKillerDied(std::uint32_t entity, std::uint64_t chr, bool take = true) {
        Removed out;
        for (auto it = d_.begin(); it != d_.end();) {
            const bool match = it->killer_link && ((entity != 0 && it->killer.entity == entity) ||
                                                   (chr != 0 && it->killer.chr == chr));
            if (match && take) {
                out.push_back(it->kind);
                it = d_.erase(it);
                continue;
            }
            if (match) it->killer_link = false;
            ++it;
        }
        Prune();
        return out;
    }
    /// The own bloodstain picked up: the stack of the death that dropped it (-1: none left).
    /// `take` false (the rule is off): only the stain link goes (its killer may still give it back).
    int OnEchoesRetrieved(bool take = true) {
        for (auto it = d_.begin(); it != d_.end(); ++it) {
            if (it->stain_link) {
                const int k = it->kind;
                if (take) {
                    d_.erase(it);
                } else {
                    it->stain_link = false;
                    Prune();
                }
                return take ? k : -1;
            }
        }
        return -1;
    }
    /// The world reset (a rest, a wipe, a death load): every killer lives again.
    void OnWorldReset() {
        for (Death& d : d_) d.killer_link = false;
        Prune();
    }
    /// This game loaded (a guest's own load in the host's world, which stays): the chr instances
    /// are new; a killer only known by its chr (no entity id) cannot be found any more.
    void OnLocalLoad() {
        for (Death& d : d_) {
            d.killer.chr = 0;
            d.killer.key = 0;
            if (!PartyEntity(d.killer.entity)) d.killer_link = false;
        }
        Prune();
    }
    /// All rot gone (a rest, a boss, another character).
    void Clear() { d_.clear(); }
    /// The stain's stack kind (party_rot.txt; -1 none).
    int StainKind() const {
        for (const Death& d : d_)
            if (d.stain_link) return d.kind;
        return -1;
    }
    /// After a restart (party_rot.txt): only the stain is still known (every killer respawned).
    void Restore(int stain_kind) {
        d_.clear();
        if (stain_kind >= 0 && stain_kind < kKinds) {
            Death d;
            d.kind = stain_kind;
            d.stain_link = true;
            d_.push_back(d);
        }
    }
    /// A stack of `kind` went another way (no longer there): no record may give it back twice.
    void Forget(const int stacks[kKinds]) {
        int have[kKinds] = {};
        for (int k = 0; k < kKinds; ++k) have[k] = std::max(0, stacks[k]);
        // The newest records keep their stacks first (the oldest are the most likely gone).
        std::vector<Death> keep;
        for (auto it = d_.rbegin(); it != d_.rend(); ++it) {
            if (have[it->kind] > 0) {
                --have[it->kind];
                keep.insert(keep.begin(), *it);
            }
        }
        d_.swap(keep);
    }
    const std::vector<Death>& deaths() const { return d_; }

private:
    void Prune() {
        d_.erase(std::remove_if(d_.begin(), d_.end(), [](const Death& d) { return !d.killer_link && !d.stain_link; }),
                 d_.end());
    }
    std::vector<Death> d_;
};

/// Takes one stack of `kind` off (false: none there).
inline bool TakeStack(int stacks[kKinds], int kind) {
    if (kind < 0 || kind >= kKinds || stacks[kind] <= 0) return false;
    --stacks[kind];
    return true;
}
inline int Stacks(const int stacks[kKinds]) {
    int n = 0;
    for (int k = 0; k < kKinds; ++k) n += stacks[k];
    return n;
}

/// The host's rules for it (party_balance Rules; an older host's rules: the cleanses off).
struct Toggles {
    bool rot = false;
    bool boss = false;
    bool killer = false;
    bool echoes = false;
};

enum class Why { Boss, Killer, Echoes };

/// What one event does to the stacks (`stacks` changed in place) and the ledger. Returns the
/// stacks taken off. The event's own links are used up whether or not its rule is on (the killer
/// is dead, the stain is gone); stacks only go while rot and that rule are on, and a death whose
/// stack stays can still be cleansed the other way.
inline int Apply(Why why, const Toggles& t, Ledger* l, int stacks[kKinds], std::uint32_t entity = 0,
                 std::uint64_t chr = 0) {
    int n = 0;
    switch (why) {
    case Why::Boss:
        if (t.rot && t.boss) {
            n = Stacks(stacks);
            for (int k = 0; k < kKinds; ++k) stacks[k] = 0;
            l->Clear();
            return n;
        }
        // Rule off: a boss that was a killer is still a killer.
        [[fallthrough]];
    case Why::Killer: {
        const Removed r = l->OnKillerDied(entity, chr, t.rot && t.killer);
        for (int k : r) n += TakeStack(stacks, k) ? 1 : 0;
        return n;
    }
    case Why::Echoes: {
        const int k = l->OnEchoesRetrieved(t.rot && t.echoes);
        if (TakeStack(stacks, k)) n = 1;
        return n;
    }
    }
    return 0;
}

/// The party notice ("" when nothing was taken off).
inline const char* NoticeText(Why why, int taken) {
    if (taken <= 0) return "";
    switch (why) {
    case Why::Boss: return "Rot cleansed: boss defeated";
    case Why::Killer: return taken > 1 ? "Rot -%d: your killer is dead" : "Rot -1: your killer is dead";
    case Why::Echoes: return "Rot -1: Blood Echoes retrieved";
    }
    return "";
}

}  // namespace coop::rot
