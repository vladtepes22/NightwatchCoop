// SPDX-License-Identifier: GPL-3.0-or-later
// Boss fights after a party load (gpu/shim/party/party_bossfog_rules.h): which overlay groups the
// guard restores after a load, and when a guest's boss fog counts as walked through - for every
// boss fog of the game (worldact::BossGates).
// cmake --build out/gpu --target party-bossfog-rules-test && out/gpu/party-bossfog-rules-test
#include "party/party_bossfog_rules.h"
#include "party/party_progress.h"
#include "party/party_worldact.h"

#include <cstdio>
#include <map>
#include <set>

using namespace coop::bossfog;
namespace pg = coop::progress;
namespace wa = coop::worldact;

static int g_fail = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

namespace {

// The guard over a fake overlay: block -> its 1000 flags (only the set ones).
using Overlay = std::map<std::uint32_t, std::set<std::uint32_t>>;
void Guard(const Overlay& saved, Overlay* now) {
    for (auto& [block, bits] : *now) {
        const auto it = saved.find(block);
        if (GuardRestores(block, bits.empty(), it != saved.end() && !it->second.empty())) {
            bits = it->second;
        }
    }
}
bool Has(const Overlay& o, std::uint32_t id) {
    const auto it = o.find(id / 1000);
    return it != o.end() && it->second.count(id);
}

// The guest's fog watch over one stay (party_worldact GuestTick): `reads` the guest-entered flag per
// scan; returns how many reports went to the host.
int Stay(std::initializer_list<bool> reads) {
    bool seen_out = false, handled = false, stale = false;
    int sent = 0;
    for (bool in : reads) {
        if (!in) {
            seen_out = true;
            stale = false;
        }
        if (StaleFogFlag(seen_out, in, handled || stale)) {
            stale = true;
        }
        if (stale) {
            continue;
        }
        if (FogWalkedThrough(seen_out, in, handled)) {
            handled = true;
            ++sent;
        } else if (!in) {
            handled = false;
        }
    }
    return sent;
}

// m34 13400010 as the data has it (instruction: bank, id), and a model of it: is the wall up after
// a while in a session, for Ludwig alive / killed during the stay, with the party's skips?
const int kM34Wall[15][2] = {{2000, 2}, {2005, 3}, {2006, 1}, {3, 6}, {3, 6}, {3, 0}, {0, 0}, {2005, 3},
                             {2006, 2}, {3, 6}, {3, 6}, {0, 0}, {3, 0}, {0, 0}, {1000, 4}};
bool WallSkipped(int index, bool open_world) {
    return ConfinementWallSkip(13400010, kM34Wall[index][0], kM34Wall[index][1], index, open_world);
}
// The wall after Ludwig is killed during a session stay: up from the stay's start ([7], waiting at
// [13]), then 9471 comes on. `party`: with the party's skips (else the data as it is).
bool M34WallAfterKill(bool party, bool open_world, bool multiplayer) {
    if (!multiplayer) {
        return false;  // the first wait ([3..6]) holds: never a wall alone
    }
    const bool wall = !(party && WallSkipped(7, open_world));
    const bool alone_cond = (party && WallSkipped(11, open_world)) || !multiplayer;
    if (alone_cond) {
        return false;  // [13] passes with 9471 on: [14] restart, [1] wall off, [6] waits for good
    }
    return wall;
}

} // namespace

int main() {
    // Fight-state groups: T1 zones 4-6 of a map (1AAB4xxx boss fight / fog, 1AAB5xxx enemy AI,
    // 1AAB6xxx boss parts); not zone 0 / 1 (map objects, boss defeated), not the global groups.
    CHECK(FightStateFlag(12414800) && FightStateFlag(12414801) && FightStateFlag(12414802));
    CHECK(FightStateFlag(12415350) && FightStateFlag(12416000) && FightStateFlag(13204831));  // Amygdala's fog too
    CHECK(!FightStateFlag(12411800) && !FightStateFlag(12411802) && !FightStateFlag(12410000));
    CHECK(!FightStateFlag(12417810) && !FightStateFlag(12413000));
    CHECK(!FightStateFlag(9401) && !FightStateFlag(2410) && !FightStateFlag(14000) && !FightStateFlag(50002800));
    CHECK(!FightStateFlag(52114000) && !FightStateFlag(62414000) && !FightStateFlag(72414000));  // other types
    CHECK(!FightStateBlock(14) && !FightStateBlock(10004));  // global type 1 zone 4: no map

    // Every boss fog of the game (flag_tool emevd, every `ゲストがボス部屋入場` event): its host- and
    // guest-entered flags are fight state (the host's load resets them; never restored), its boss
    // and first-fight (intro) flags are not (saved: the guard and the mirror keep them).
    CHECK(wa::BossGates().size() == 20);
    for (const wa::BossGate& g : wa::BossGates()) {
        CHECK(FightStateFlag(g.host_flag) && FightStateFlag(g.guest_flag));
        CHECK(g.host_flag / 1000 == g.guest_flag / 1000);
        CHECK(!FightStateFlag(g.boss_flag));
        CHECK(g.intro_flag == 0 || !FightStateFlag(g.intro_flag));
        CHECK(pg::CategoryOf(g.host_flag) == pg::Category::boss_area);
        CHECK(!GuardRestores(g.host_flag / 1000, true, true));
        CHECK(GuardRestores(g.boss_flag / 1000, true, true));
        // the vanilla snapshot alone carries them (the world mirror leaves them out)
        CHECK(!pg::WorldMirrorKey(pg::PoolKeyOfId(g.host_flag)));
        CHECK(pg::WorldMirrorKey(pg::PoolKeyOfId(g.boss_flag)));
    }
    // One rule with the world mirror: for every pool group of a mirrored map, fight state == the
    // T1 groups the mirror leaves to the snapshot.
    int checked = 0;
    for (int key = 0; key < int(pg::kPoolBlocks); ++key) {
        const std::uint32_t block = pg::BlockOfPoolKey(key);
        if (block == 0xffffffffu || block / 10000 != 1 || block / 100 % 100 == 0 || block / 100 % 100 == 29 ||
            (block / 100 % 100 >= 40 && block / 100 % 100 <= 47)) {
            continue;
        }
        ++checked;
        CHECK(pg::WorldMirrorKey(key) == !FightStateBlock(block));
    }
    std::printf("mirror cross-check: %d map T1 groups\n", checked);
    CHECK(checked >= 17 * 3);  // at least zones 4-6 of every table map

    // The guard: only groups that came back zero with bits saved; never fight state.
    CHECK(GuardRestores(12410, true, true) && GuardRestores(9, true, true));
    CHECK(!GuardRestores(12410, false, true) && !GuardRestores(12410, true, false));
    CHECK(!GuardRestores(12414, true, true) && !GuardRestores(12415, true, true) && !GuardRestores(12416, true, true));
    CHECK(GuardRestores(12417, true, true));  // zone 7 lamps: as before

    // The 0.7.1 wipe_late run: before the wipe the guest's overlay had the running Gascoigne fight
    // (12414800 host in, 12414801 this guest in, 12414802 the fight) and the opening flags; the
    // snapshot after the host's wipe load came back with all of them zero (run 18's zeroed overlay
    // shape for the opening, the host's reset for the fight).
    {
        const Overlay saved = {{12410, {12410000}}, {9, {9401, 9402}}, {12414, {12414800, 12414801, 12414802}},
                               {12411, {12411802}}};
        Overlay now = {{12410, {}}, {9, {}}, {12414, {}}, {12411, {12411802}}};
        Guard(saved, &now);
        CHECK(Has(now, 12410000) && Has(now, 9401) && Has(now, 12411802));  // run 18: the opening stays done
        CHECK(!Has(now, 12414800) && !Has(now, 12414801) && !Has(now, 12414802));  // the fight as the host reset it
        // ... for every boss alike
        for (const wa::BossGate& g : wa::BossGates()) {
            const std::uint32_t b = g.host_flag / 1000;
            const Overlay s2 = {{b, {g.host_flag, g.guest_flag}}};
            Overlay n2 = {{b, {}}};
            Guard(s2, &n2);
            CHECK(!Has(n2, g.host_flag) && !Has(n2, g.guest_flag));
        }
        // the host's fight still runs (a lone guest's death and load): the snapshot has it, kept
        Overlay live = {{12414, {12414800}}};
        Guard(saved, &live);
        CHECK(Has(live, 12414800) && !Has(live, 12414801));
        // Every load a guest takes into the host's world while the host fights (the guard's only
        // callers: a guest's own "respawn" after a death outside the fight, its "rejoin" back from an
        // alone trip; a Party > Rejoin / title join is held by A11 during a fight, a limbo / spectate
        // and the arena revive take no load): the host re-sends the snapshot with the running fight
        // (host-entered + fight flags). It stands as sent for every boss: the bar comes up, the boss
        // fights on, and this guest is outside its fog (its own guest-entered flag is not the host's).
        for (const wa::BossGate& g : wa::BossGates()) {
            const std::uint32_t b = g.host_flag / 1000;
            const Overlay before = {{b, {g.host_flag, g.guest_flag, g.host_flag + 2}}};
            Overlay snap = {{b, {g.host_flag, g.host_flag + 2}}};
            Guard(before, &snap);
            CHECK(Has(snap, g.host_flag) && Has(snap, g.host_flag + 2) && !Has(snap, g.guest_flag));
        }
    }

    // The guest's fog report: only a walk through the fog this stay (0 -> 1), once.
    CHECK(StaleFogFlag(false, true, false) && !StaleFogFlag(true, true, false) && !StaleFogFlag(false, false, false));
    CHECK(!StaleFogFlag(false, true, true));
    CHECK(FogWalkedThrough(true, true, false) && !FogWalkedThrough(false, true, false) &&
          !FogWalkedThrough(true, false, false) && !FogWalkedThrough(true, true, true));
    CHECK(Stay({false, false, true, true, true}) == 1);  // a walk-in at the lamp after the wipe: sent once
    CHECK(Stay({true, true, true, true}) == 0);          // the 0.7.1 bug: 1 from the load on: never sent
    CHECK(Stay({true, true, false, false, true}) == 1);  // stale, then reset, then a real walk-in
    CHECK(Stay({false, true, false, true}) == 2);        // in, out (a death), in again: two walk-ins
    CHECK(Stay({}) == 0 && Stay({false}) == 0);


    // How each boss's first fight really starts (player report 2026-10-22, Laurence): one row per
    // boss fog, and only an `entry` one (the host's position) may be started by a guest's fog.
    {
        std::size_t n = 0;
        const FirstFight* rows = FirstFights(&n);
        CHECK(n == wa::BossGates().size());
        std::set<std::uint32_t> action;
        for (const wa::BossGate& g : wa::BossGates()) {
            const FirstFight* f = FirstFightFor(g.guest_event);
            CHECK(f != nullptr);
            if (!f) {
                continue;
            }
            CHECK((f->start == FirstStart::none) == (g.intro_flag == 0));
            CHECK(wa::BossGateGuestStarts(g) == (f->start == FirstStart::entry));
            if (f->start == FirstStart::action) {
                action.insert(g.guest_event);
                // The re-fight event of the gate's own map block, its client skip at [2], a notice.
                CHECK(std::uint32_t(f->refight_event) / 10000 == g.host_flag / 10000);
                CHECK(f->client_skip_index == 2 && f->notice && f->notice[0]);
                CHECK(GuestRoomOpenSkip(f->refight_event, 1003, 5, 2));
                CHECK(!GuestRoomOpenSkip(f->refight_event, 1003, 5, 1) &&
                      !GuestRoomOpenSkip(f->refight_event, 2005, 3, 2));
            } else {
                CHECK(!f->notice && f->refight_event == 0);
            }
        }
        (void)rows;
        // Ebrietas (the host's hit), Rom (anyone's hit), Laurence (the skull at the altar), Lady Maria
        // (examined).
        CHECK(action == (std::set<std::uint32_t>{12424811, 13204831, 13404851, 13504801}));
        CHECK(!GuestMayStartFirstFight(13204831) && GuestRoomOpenSkip(13204830, 1003, 5, 2));
        CHECK(!GuestMayStartFirstFight(13404851) && GuestMayStartFirstFight(13404801) &&
              GuestMayStartFirstFight(12404841) && !GuestMayStartFirstFight(12424711) &&
              !GuestMayStartFirstFight(1));
        CHECK(!GuestRoomOpenSkip(12404840, 1003, 5, 2));  // Amelia's room: the guest's fog starts it
        // The host's answer to a guest's fog report at Laurence's before his first fight: nothing
        // (no visit, no "Boss fight started"); after it (a re-fight), the fog opens the fight.
        const wa::BossGate& laurence = *wa::BossGateByEvent(13404851);
        CHECK(wa::HostFogAction(laurence, true, false, false, false, 0x22000000u) == wa::FogAction::none);
        CHECK(wa::HostFogAction(laurence, true, true, false, false, 0x22000000u) == wa::FogAction::open);
    }

    // Every co-op wall of the game (EMEVD scan, player report 2026-10-22: the wall behind Ludwig).
    {
        std::size_t n = 0;
        const ConfinementWall* walls = ConfinementWalls(&n);
        std::set<int> events;
        for (std::size_t i = 0; i < n; ++i) {
            events.insert(walls[i].event);
        }
        CHECK(n == 3 && events == (std::set<int>{7600, 12907600, 13400010}));
        // 7600 / 12907600: wall [6], SFX [7], with the open world only; nothing else.
        for (const int ev : {7600, 12907600}) {
            for (int i = 0; i < 12; ++i) {
                CHECK(!ConfinementWallSkip(ev, 0, 0, i, true) && !ConfinementWallSkip(ev, 0, 0, i, false));
            }
            CHECK(ConfinementWallSkip(ev, 2005, 3, 6, true) && ConfinementWallSkip(ev, 2006, 2, 7, true));
            CHECK(!ConfinementWallSkip(ev, 2005, 3, 6, false) && !ConfinementWallSkip(ev, 2006, 2, 7, false));
            CHECK(!ConfinementWallSkip(ev, 2005, 3, 1, true));  // the wall-off instruction stays
        }
        // 13400010: open world: [7] [8] [11]; open world off: [11] only.
        for (int i = 0; i < 15; ++i) {
            CHECK(WallSkipped(i, true) == (i == 7 || i == 8 || i == 11));
            CHECK(WallSkipped(i, false) == (i == 11));
        }
        CHECK(!ConfinementWallSkip(13400011, 2005, 3, 7, true));
        // The wall after Ludwig's death, the party still together: vanilla's data keeps it up (the
        // reported bug: until the guest left); the party: down (open world: never up at all).
        CHECK(M34WallAfterKill(false, false, true) && M34WallAfterKill(false, true, true));
        CHECK(!M34WallAfterKill(true, true, true) && !M34WallAfterKill(true, false, true));
        CHECK(!M34WallAfterKill(false, false, false) && !M34WallAfterKill(true, false, false));  // alone
    }

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("party bossfog rules: all checks passed\n");
    return 0;
}
