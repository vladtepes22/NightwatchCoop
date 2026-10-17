// SPDX-License-Identifier: GPL-3.0-or-later
// H4 enemy census (party_enemy_census.h).
#include "party/party_enemy_census.h"

#include <cassert>
#include <cstdio>

int main() {
    using V = std::vector<EnemyCensusEntry>;
    // Nothing changes: no diff.
    {
        const V a{{1, true}, {2, false}};
        const auto d = DiffEnemyCensus(a, a);
        assert(!d.Changed() && d.alive == 1 && d.dead == 1);
    }
    // A kill and a respawn in the same world.
    {
        const V a{{1, true}, {2, false}, {3, true}};
        const V b{{1, false}, {2, true}, {3, true}};
        const auto d = DiffEnemyCensus(a, b);
        assert(d.Changed() && d.died.size() == 1 && d.died[0] == 1);
        assert(d.respawned.size() == 1 && d.respawned[0] == 2);
        assert(d.alive == 2 && d.dead == 1 && d.gone.empty());
    }
    // Id 0 is ignored; a new entity only counts; an unloaded one is gone.
    {
        const V a{{0, false}, {5, true}};
        const V b{{0, true}, {6, true}};
        const auto d = DiffEnemyCensus(a, b);
        assert(d.respawned.empty() && d.died.empty() && d.alive == 1 && d.dead == 0);
        assert(d.gone.size() == 1 && d.gone[0] == 5);
    }
    // A dead enemy that streams out and back in alive is a respawn (the carry keeps it).
    {
        const V a{{7, false}, {8, true}};
        const V b{{8, true}};
        const V carried = CarryEnemyCensus(a, b);
        assert(carried.size() == 2);
        const V c{{7, true}, {8, true}};
        const auto d = DiffEnemyCensus(carried, c);
        assert(d.respawned.size() == 1 && d.respawned[0] == 7);
        // An alive one that left is not carried.
        assert(CarryEnemyCensus(V{{9, true}}, V{}).empty());
    }
    // host_kill target selection.
    {
        using C = std::vector<EnemyKillCandidate>;
        auto mk = [](std::uint32_t id, bool alive, float x, bool pos = true) {
            EnemyKillCandidate e;
            e.id = id ? id + 7 : 0;  // the census key differs from the map entity id
            e.entity = id;
            e.alive = alive;
            e.have_pos = pos;
            e.pos[0] = x;
            return e;
        };
        const float me[3] = {0, 0, 0};
        const C c{mk(2410700, false, 1), mk(2410800, true, 2), mk(2410740, true, 30), mk(2410710, true, 10),
                  mk(2410720, true, 3, false), mk(0, true, 1), mk(2410750, true, 600)};
        assert(PickEnemyKillTarget(c, 0, me, 500) == 3);         // nearest alive, non-boss, with a position
        assert(PickEnemyKillTarget(c, 0, me, 5) == -1);          // none in range
        assert(PickEnemyKillTarget(c, 2410740, me, 500) == 2);   // explicit, loaded, alive (range ignored)
        assert(PickEnemyKillTarget(c, 2410750, me, 5) == 6);
        assert(PickEnemyKillTarget(c, 2410800, me, 500) == 1);   // a boss by id
        assert(PickEnemyKillTarget(c, 2410700, me, 500) == -1);  // dead
        assert(PickEnemyKillTarget(c, 2411111, me, 500) == -1);  // not loaded
        assert(PickEnemyKillTarget(c, 2410747, me, 500) == 2);   // by census key too
        assert(PickEnemyKillTarget({}, 0, me, 500) == -1);
        assert(EnemyIsBossEntity(2410800) && !EnemyIsBossEntity(2410740) && !EnemyIsBossEntity(800));
    }
    {
        // H3 kill echoes: alive -> dead (dying, unpaid) is due after the grace; paid / baseline-dead
        // / alive again are not.
        KillEchoTracker k;
        const std::uint64_t dying = kKillDyingBit, paid = kKillDyingBit | 0x40000;
        assert(k.Update({{1, true, 0}, {2, true, 0}, {3, false, dying}, {4, true, 0}}, 0).empty());  // 3: dead at baseline
        assert(k.Update({{1, false, dying}, {2, false, paid}, {3, false, dying}, {4, false, 0}}, 1).empty());
        assert(k.Update({{1, false, dying}, {2, false, paid}, {3, false, dying}, {4, false, dying}}, 3).empty());
        auto d = k.Update({{1, false, dying}, {2, false, paid}, {3, false, dying}, {4, false, dying}}, 4.1);
        assert(d.size() == 2 && d[0] == 1 && d[1] == 4);  // 4: dead since t 1, dying bit later
        assert(k.Update({{1, false, paid}, {4, false, paid}}, 5).empty());  // paid by our call: never again
        KillEchoTracker n;
        n.Update({{8, true, 0}}, 0);
        n.Update({{8, false, 0}}, 1);
        assert(n.Update({{8, false, 0}}, 9).empty());  // HP 0 without the dying bit: not yet
        assert(n.Update({{8, false, dying}}, 10).size() == 1);
        KillEchoTracker r;
        r.Update({{7, true, 0}}, 0);
        r.Update({{7, false, dying}}, 1);
        r.Update({{7, true, 0}}, 2);  // alive again (revived): forgotten
        assert(r.Update({{7, false, dying}}, 3).empty() && r.Update({{7, false, dying}}, 5).empty());
        assert(r.Update({{7, false, dying}}, 6.1).size() == 1);
        r.Reset();
        assert(r.Update({{7, false, dying}}, 10).empty());
    }
    std::puts("party-enemy-census-test: ok");
    return 0;
}
