// SPDX-License-Identifier: GPL-3.0-or-later
// H4 enemy census (party_enemy_census.h).
#include "party/party_enemy_census.h"
#include "party/party_kill_record.h"

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
    {
        // The party's kill record (party_kill_record.h, streamsync).
        using namespace coop::killrec;
        using L = std::vector<Obs>;
        std::vector<std::uint32_t> died;
        // A neighbour map's enemy killed here, streamed out, back in as a new chr: silent re-kill.
        KillRecord h;
        h.Reset(1);
        assert(h.Update(L{{2400125, true, 0xA0, 11}, {2410247, true, 0xB0, 12}}, 0, &died).empty() && died.empty());
        assert(h.Update(L{{2400125, false, 0xA0, 11}, {2410247, true, 0xB0, 12}}, 1, &died).empty());
        assert(died.size() == 1 && died[0] == 2400125 && h.PaidHere(2400125));
        died.clear();
        assert(h.Update(L{{2410247, true, 0xB0, 12}}, 5, &died).empty());  // streamed out
        auto a = h.Update(L{{2400125, true, 0xC0, 13}, {2410247, true, 0xB0, 12}}, 20, &died);
        assert(a.size() == 1 && a[0].entity == 2400125 && a[0].chr == 0xC0 && a[0].silent && a[0].respawn);
        assert(died.empty());
        assert(h.Update(L{{2400125, true, 0xC0, 13}}, 20.5, &died).empty());  // acted on: not twice
        assert(h.Update(L{{2400125, false, 0xC0, 13}}, 21, &died).empty() && died.empty());  // dead again: no new kill
        // The same chr alive again without leaving: a revive by the game, forgotten.
        KillRecord r;
        r.Update(L{{2410300, true, 0xD0, 1}}, 0, &died);
        r.Update(L{{2410300, false, 0xD0, 1}}, 1, &died);
        assert(r.Update(L{{2410300, true, 0xD0, 1}}, 2, &died).empty() && !r.Dead(2410300));
        died.clear();
        // A party member's kill: loaded and alive here at the kill -> after the grace, with echoes.
        KillRecord g;
        g.Reset(4);
        g.Update(L{{2400125, true, 0xE0, 2}}, 0, &died);
        assert(g.AddRemote(2400125, 1) && !g.AddRemote(2400125, 1));
        assert(g.Update(L{{2400125, true, 0xE0, 2}}, 2, &died).empty());  // grace: the game's own sync first
        a = g.Update(L{{2400125, true, 0xE0, 2}}, 4.5, &died);
        assert(a.size() == 1 && !a[0].silent && !a[0].respawn);
        assert(g.Update(L{{2400125, false, 0xE0, 2}}, 5, &died).empty() && died.empty() && g.PaidHere(2400125));
        // Not loaded here at the kill: put to death silently when it streams in.
        assert(g.AddRemote(2400200, 6));
        a = g.Update(L{{2400200, true, 0xF0, 3}}, 30, &died);
        assert(a.size() == 1 && a[0].silent && !a[0].respawn);
        // Died through the game's own sync within the grace: nothing to do.
        g.Update(L{{2410400, true, 0x100, 4}}, 31, &died);
        g.AddRemote(2410400, 31.2);
        assert(g.Update(L{{2410400, false, 0x100, 4}}, 32, &died).empty() && died.empty());
        // Bosses and enemies without an entity id are never recorded.
        assert(!g.AddRemote(2410800, 40) && !g.AddRemote(0, 40) && !g.AddRemote(0xFFFFFFFFu, 40));
        // This game's own load: a hold first (a party load's reset may come later), then silent.
        g.LocalLoad(50);
        assert(g.Update(L{{2400125, true, 0x200, 9}}, 51, &died).empty());
        a = g.Update(L{{2400125, true, 0x200, 9}}, 61, &died);
        assert(a.size() == 1 && a[0].silent && a[0].respawn);
        // The host's next epoch: everything forgotten.
        g.Reset(5);
        assert(g.size() == 0 && g.epoch() == 5 && g.Update(L{{2400125, true, 0x300, 7}}, 70, &died).empty());
        // Two chrs of one id: left alone.
        KillRecord d;
        d.Update(L{{2400300, true, 1, 1}, {2400300, true, 2, 2}}, 0, &died);
        assert(d.Update(L{{2400300, false, 1, 1}, {2400300, true, 2, 2}}, 1, &died).empty() && !d.Dead(2400300));
    }
    std::puts("party-enemy-census-test: ok");
    return 0;
}
