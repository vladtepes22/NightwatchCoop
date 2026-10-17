// SPDX-License-Identifier: GPL-3.0-or-later
// H4 "no unnecessary respawns": a census of the loaded map's enemies (entity id, alive) and the
// difference between two censuses. Pure logic (unit test: tests/test_party_enemy_census.cpp); the
// game side is CensusTick in party_balance.cpp (BB_PARTY_ENEMY_CENSUS=1).
//
// A regular enemy that was dead in one census and alive in a later one WITHOUT a load in between
// respawned: the only thing that may do that in a party is a rest (a load) or a wipe (a load), so
// a respawn seen in the same loaded world after a member's death is an H4 bug.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

struct EnemyCensusEntry {
    std::uint32_t id = 0;  ///< entity id (chr +0x3B8); 0 = no entity id (skipped)
    bool alive = false;    ///< HP > 0
};

struct EnemyCensusDiff {
    int alive = 0;                       ///< alive in the new census
    int dead = 0;                        ///< dead in the new census
    std::vector<std::uint32_t> respawned;  ///< dead before, alive now (same loaded world)
    std::vector<std::uint32_t> died;       ///< alive before, dead now
    std::vector<std::uint32_t> gone;       ///< in the old census, not in the new one (unloaded)
    bool Changed() const { return !respawned.empty() || !died.empty() || !gone.empty(); }
};

/// before / after: two censuses of the same loaded world (no load in between). Entities without
/// an id are ignored; an entity new in `after` only counts (it streamed in).
inline EnemyCensusDiff DiffEnemyCensus(const std::vector<EnemyCensusEntry>& before,
                                       const std::vector<EnemyCensusEntry>& after) {
    EnemyCensusDiff d;
    for (const auto& a : after) {
        if (!a.id) continue;
        a.alive ? ++d.alive : ++d.dead;
        const auto it = std::find_if(before.begin(), before.end(), [&](const EnemyCensusEntry& b) { return b.id == a.id; });
        if (it == before.end()) continue;
        if (!it->alive && a.alive) d.respawned.push_back(a.id);
        if (it->alive && !a.alive) d.died.push_back(a.id);
    }
    for (const auto& b : before) {
        if (!b.id) continue;
        const bool still = std::any_of(after.begin(), after.end(), [&](const EnemyCensusEntry& a) { return a.id == b.id; });
        if (!still) d.gone.push_back(b.id);
    }
    return d;
}

/// The census to compare the next one with: the new census, plus the dead entities of the old one
/// that left it (unloaded chrs of the same world stay dead: they must not come back alive when they
/// stream in again).
inline std::vector<EnemyCensusEntry> CarryEnemyCensus(const std::vector<EnemyCensusEntry>& before,
                                                      const std::vector<EnemyCensusEntry>& after) {
    std::vector<EnemyCensusEntry> out = after;
    for (const auto& b : before) {
        if (!b.id || b.alive) continue;
        const bool in = std::any_of(after.begin(), after.end(), [&](const EnemyCensusEntry& a) { return a.id == b.id; });
        if (!in) out.push_back(b);
    }
    return out;
}

/// A loaded enemy as the census sees it, with its position (the host_kill / guest_kill test
/// steps pick their target from this list: an entity the census found is loaded and resolvable).
struct EnemyKillCandidate {
    std::uint32_t id = 0;      ///< the census key (chr +0x3B8; differs between games)
    std::uint32_t entity = 0;  ///< the map entity id (chr +0x1F0, e.g. 2410740; the same on every game)
    bool alive = false;     ///< HP > 0
    bool have_pos = false;
    float pos[3] = {};
    std::uint64_t chr = 0;  ///< game side only
};

/// A boss entity (AAB08xx): left to its defeat flags, never a "regular enemy" pick.
inline bool EnemyIsBossEntity(std::uint32_t id) { return id >= 1000000 && id % 10000 / 100 == 8; }

/// The kill target: `want` != 0: that entity (map entity id, or the census key) if it is loaded and
/// alive (-1 if not: dead or not loaded); `want` == 0: the nearest living enemy that is not a boss
/// entity, with a position within `max_m` of `me`.
/// Returns the index into `c`, -1 for none.
inline int PickEnemyKillTarget(const std::vector<EnemyKillCandidate>& c, std::uint32_t want, const float me[3],
                               float max_m) {
    if (want) {
        for (size_t i = 0; i < c.size(); ++i)
            if (c[i].entity == want || c[i].id == want) return c[i].alive ? int(i) : -1;
        return -1;
    }
    int best = -1;
    float bd = max_m * max_m;
    for (size_t i = 0; i < c.size(); ++i) {
        const auto& e = c[i];
        if (!e.id || !e.alive || !e.have_pos || EnemyIsBossEntity(e.entity)) continue;
        const float dx = e.pos[0] - me[0], dy = e.pos[1] - me[1], dz = e.pos[2] - me[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d <= bd) {
            bd = d;
            best = int(i);
        }
    }
    return best;
}

/// H3 kill echoes in a party. The game's kill-echo payer (0x18C5070) pays the LOCAL player (no
/// attacker check; full for clients with the parity patch) and marks the chr paid (+0x1E1 0x40000).
/// In a party session the per-frame death path (0x18BD9D0) leaves a dead enemy's payer to the
/// network death handler, and live (2026-10-13) a host kill paid nobody until the guest had left:
/// the host's 48 echoes came 6 s after the kill, at the guest's disconnect; the guest got none.
/// KillEchoTracker names the enemies seen alive and then dead (dying bit 0x10000) whose payer has
/// not run after `grace` s; the game side then calls the payer itself on every player's game. The
/// payer's own paid bit keeps a later native call from paying twice.
struct KillEchoObs {
    std::uint64_t chr = 0;
    bool alive = false;
    std::uint64_t flags = 0;  ///< chr +0x1E1, 5 bytes
};
constexpr std::uint64_t kKillDyingBit = 0x10000, kKillPaidBits = 0x240000;

class KillEchoTracker {
public:
    /// One observation of the loaded enemies at `now` (s); returns the chrs whose echoes are due.
    std::vector<std::uint64_t> Update(const std::vector<KillEchoObs>& cur, double now, double grace = 3.0) {
        std::vector<std::uint64_t> due;
        std::map<std::uint64_t, bool> alive;
        std::map<std::uint64_t, double> dead_since;
        for (const auto& o : cur) {
            if (!o.chr) continue;
            alive[o.chr] = o.alive;
            if (o.alive || (o.flags & kKillPaidBits)) continue;
            const auto was = alive_.find(o.chr);
            const auto since = dead_since_.find(o.chr);
            double t = -1;
            if (since != dead_since_.end()) t = since->second;
            else if (was != alive_.end() && was->second) t = now;  // alive before, dead now: a kill
            if (t < 0) continue;  // dead since before we looked (a load, a baseline): not ours
            if (now - t >= grace && (o.flags & kKillDyingBit)) due.push_back(o.chr);
            else dead_since[o.chr] = t;
        }
        alive_.swap(alive);
        dead_since_.swap(dead_since);
        return due;
    }
    void Reset() {
        alive_.clear();
        dead_since_.clear();
    }

private:
    std::map<std::uint64_t, bool> alive_;
    std::map<std::uint64_t, double> dead_since_;
};
