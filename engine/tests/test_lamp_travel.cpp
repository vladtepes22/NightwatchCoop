// SPDX-License-Identifier: GPL-3.0-or-later
// Direct lamp travel's rules (gpu/shim/party/lamp_travel_rules.h): the lamp ids, the warp list a
// lamp offers and which talk command it replaces.
// Build and run: ninja -C out/gpu lamp-travel-test && out/gpu/lamp-travel-test.exe
#include "party/lamp_travel_rules.h"
#include "party/party_ids.h"

#include <cstdio>
#include <set>

using namespace coop::lamptravel;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

int main() {
    // Ids (the headstones' talk scripts and live logs: 1st Floor Sickroom 2412950, Central Yharnam
    // 2412951 with Dream flag 72410101 and lit flag base 12417820 + 10).
    const Lamp sick{241, 0, Yharnam}, central{241, 1, Yharnam};
    CHECK(ReturnPoint(sick) == 2412950);
    CHECK(ReturnPoint(central) == 2412951);
    CHECK(NameFlag(central) == 70002411);
    CHECK(LitFlag(central) == 12417830);
    CHECK(DreamFlag(2412951) == 72410101);
    CHECK(DreamFlag(9999999) == 0);
    CHECK(MapOf(2412951) == 0x18010000u);  // m24_01
    CHECK(MapOf(3402950) == 0x22000000u);  // m34_00

    // Every lamp of the table is a ReturnPointParam row the game has, listed once.
    std::set<std::uint32_t> seen;
    for (const Lamp& l : kLamps) {
        CHECK(coop::ids::ReturnPointKnown(ReturnPoint(l)));
        CHECK(seen.insert(ReturnPoint(l)).second);
    }
    CHECK(kLampCount == 44);
    CHECK(coop::ids::ReturnPointKnown(kDreamReturnPoint));

    // Only the lamps' "Return to the Hunter's Dream?" question is replaced.
    const std::int32_t dream_q[5] = {2, 10010713, 3, 4, 2};
    const std::int32_t other_q[5] = {2, 10010714, 3, 4, 2};
    CHECK(IsDreamConfirm(kCmdGenericDialog, 5, dream_q));
    CHECK(!IsDreamConfirm(kCmdGenericDialog, 5, other_q));
    CHECK(!IsDreamConfirm(kCmdGenericDialog, 4, dream_q));
    CHECK(!IsDreamConfirm(kCmdAddWarpEntry, 5, dream_q));
    const std::int32_t message[5] = {7, 10010713, 1, 0, 1};
    CHECK(!IsDreamConfirm(kCmdGenericDialog, 5, message));

    // The list: the Dream first, then lit lamps in headstone order; not the lamp the player is at,
    // not blocked ones; indices 1.. unique.
    std::set<std::uint32_t> lit = {LitFlag(sick), LitFlag(central), LitFlag({240, 0, Yharnam}), LitFlag({220, 0, Frontier}),
                                   LitFlag({240, 1, Yharnam}), LitFlag({340, 0, HuntersNightmare})};
    std::set<std::uint32_t> blocked = {NameFlag({240, 1, Yharnam})};
    auto flag = [&](std::uint32_t f) { return lit.count(f) || blocked.count(f); };
    auto all = [](std::uint32_t) { return true; };
    std::vector<Entry> list = BuildList(2412951, flag, all);
    CHECK(list.size() == 5);
    if (list.size() == 5) {
        CHECK(list[0].return_point == kDreamReturnPoint && list[0].name_flag == kDreamNameFlag &&
              list[0].lit_flag == LitFlag(central) && list[0].group == Dream);
        CHECK(list[1].return_point == 2412950);
        CHECK(list[2].return_point == 2402950);
        CHECK(list[3].return_point == 2202950 && list[3].group == Frontier);
        CHECK(list[4].return_point == 3402950 && list[4].group == HuntersNightmare);
        for (std::size_t i = 0; i < list.size(); ++i) CHECK(list[i].index == i + 1);
    }
    CHECK(Chosen(list, 1) == kDreamReturnPoint);
    CHECK(Chosen(list, 4) == 2202950);
    CHECK(Chosen(list, 0) == 0);
    CHECK(Chosen(list, 99) == 0);
    // Unknown rows are left out; only the Dream left: no list (the vanilla question).
    CHECK(BuildList(2412951, flag, [](std::uint32_t rp) { return rp == 2412950; }).size() == 2);
    CHECK(BuildList(2412951, [&](std::uint32_t f) { return f == LitFlag(central); }, all).empty());
    // Not at a known lamp: no Dream entry (its flag is the lamp's own), the lamps still listed.
    CHECK(BuildList(0, flag, all).size() == 5 && BuildList(0, flag, all)[0].return_point == 2412950);

    // Without the Dream entry: lamps only.
    const std::vector<Entry> no_dream = BuildList(2412951, flag, all, false);
    CHECK(no_dream.size() == 4 && no_dream[0].return_point == 2412950 && no_dream[0].index == 1);
    CHECK(BuildList(2412951, [&](std::uint32_t f) { return f == LitFlag(central); }, all, false).empty());

    // The nearest lamp of the map within the radius.
    const std::vector<Spot> spots = {{2412950, 0, 0, 0}, {2412951, 10, 0, 0}, {2402950, 1, 0, 0}};
    const float p[3] = {9, 0, 0};
    CHECK(NearestLamp(spots, 0x18010000u, p, 15.0f) == 2412951);
    CHECK(NearestLamp(spots, 0x18000000u, p, 15.0f) == 2402950);
    CHECK(NearestLamp(spots, 0x18010000u, p, 0.5f) == 0);
    // lamprule run 5: at Central Yharnam's lamp object after an in-place warp, the drawn position still
    // at the Great Bridge lamp: the physics position names the lamp the player stands at.
    {
        std::vector<Spot> real;
        for (const Lamp& l : kLamps) {
            float sp[4];
            if (coop::ids::ReturnSpot(ReturnPoint(l), sp)) real.push_back({ReturnPoint(l), sp[0], sp[1], sp[2]});
        }
        const float phys[3] = {-196.8f, -28.6f, 66.6f}, drawn[3] = {-124.8f, -27.0f, 64.3f};
        CHECK(NearestLamp(real, 0x18010000u, StandingPos(true, phys, drawn), 15.0f) == 2412951);
        CHECK(NearestLamp(real, 0x18010000u, StandingPos(false, phys, drawn), 15.0f) == 2412952);
    }
    // chalicelamp: no table spot for a Chalice Dungeon lamp (the generated layout puts it elsewhere: the
    // template's 2902954 at (52.8, -1.0, 21.9) was the guest's revive in the air); world lamps keep theirs.
    {
        float sp[4];
        for (std::uint32_t rp = 2902950; rp <= 2902959; ++rp) CHECK(!coop::ids::ReturnSpot(rp, sp));
        CHECK(coop::ids::ReturnSpot(2412951, sp) && coop::ids::ReturnSpot(2102950, sp));
    }

    // Switch and situation.
    CHECK(SwitchOn(nullptr) && SwitchOn("") && SwitchOn("1"));
    CHECK(!SwitchOn("0") && !SwitchOn("off") && !SwitchOn("false"));
    Situation s;
    s.world_up = true;
    s.map = 0x18010000u;
    CHECK(Offer(s));
    s.map = 0x15000000u;  // the Hunter's Dream: its headstones
    CHECK(!Offer(s));
    s.map = 0x18010000u;
    s.boss = true;
    CHECK(!Offer(s));
    s.boss = false;
    s.cutscene = true;
    CHECK(!Offer(s));
    s.cutscene = false;
    s.loading = true;
    CHECK(!Offer(s));

    // The talk command switch's other observers (issue #4: shared NPC talk's choice tracker).
    {
        static int calls_a = 0, calls_b = 0;
        static std::uint64_t seen = 0;
        using Obs = void (*)(std::uint64_t, std::uint64_t);
        ObserverList<Obs, 2> list;
        const Obs a = [](std::uint64_t, std::uint64_t call) { ++calls_a; seen = call; };
        const Obs b = [](std::uint64_t, std::uint64_t) { ++calls_b; };
        CHECK(list.Add(a) && !list.Add(a) && !list.Add(nullptr));  // once each
        CHECK(list.Add(b) && list.Size() == 2);
        CHECK(!list.Add([](std::uint64_t, std::uint64_t) {}));     // full
        list.Dispatch(std::uint64_t(1), std::uint64_t(0x1234));
        CHECK(calls_a == 1 && calls_b == 1 && seen == 0x1234);
        ObserverList<Obs, 2> empty;
        empty.Dispatch(std::uint64_t(1), std::uint64_t(2));  // nothing registered: no call
        CHECK(calls_a == 1);
    }

    // Map travel: which situations refuse it, in order of what the player is told.
    {
        MapTravelSituation m;
        m.on = m.world_up = true;
        m.map = 0x18000000u;  // Cathedral Ward
        m.hp = 500;
        CHECK(MapTravelState(m) == MapTravel::Ready);
        MapTravelSituation t = m;
        t.map = 0x15000000u;  // the Hunter's Dream: its headstones run the same warp
        CHECK(MapTravelState(t) == MapTravel::Ready);
        t = m, t.on = false;
        CHECK(MapTravelState(t) == MapTravel::Off);
        t = m, t.loading = true;
        CHECK(MapTravelState(t) == MapTravel::NotInWorld);
        t = m, t.map = 0xffffffffu;
        CHECK(MapTravelState(t) == MapTravel::NotInWorld);
        t = m, t.cutscene = true, t.boss = true;
        CHECK(MapTravelState(t) == MapTravel::Cutscene);
        t = m, t.map = 0x1d0a5a00u;  // a Chalice Dungeon
        CHECK(MapTravelState(t) == MapTravel::Chalice);
        t = m, t.hp = 0, t.boss = true;
        CHECK(MapTravelState(t) == MapTravel::Dead);
        t = m, t.hp = -1;  // no chr
        CHECK(MapTravelState(t) == MapTravel::Dead);
        t = m, t.spectating = true;
        CHECK(MapTravelState(t) == MapTravel::Dead);
        t = m, t.boss = true;
        CHECK(MapTravelState(t) == MapTravel::Boss);
        t = m, t.busy = true;
        CHECK(MapTravelState(t) == MapTravel::Busy);
        // Targets: lit lamps of the table, not blocked, known; never the Hunter's Dream.
        CHECK(MapTravelTarget(2412950, flag, all));                                       // Central Yharnam, lit
        CHECK(MapTravelTarget(ReturnPoint({220, 0, Frontier}), flag, all));
        CHECK(!MapTravelTarget(ReturnPoint({240, 1, Yharnam}), flag, all));               // blocked
        CHECK(!MapTravelTarget(ReturnPoint({270, 0, Frontier}), flag, all));              // not lit
        CHECK(!MapTravelTarget(kDreamReturnPoint, [](std::uint32_t) { return true; }, all));
        CHECK(!MapTravelTarget(1234567, [](std::uint32_t) { return true; }, all));        // not a lamp
        CHECK(!MapTravelTarget(2412950, flag, [](std::uint32_t) { return false; }));      // no param row
    }
    // 0.7.6: a guest's lamp stays closed while an NPC talk plays and for a second after it.
    CHECK(!GuestLampMayOpen(0.0));                      // a talk plays
    CHECK(!GuestLampMayOpen(0.4));                      // just stopped: the talk's last press
    CHECK(!GuestLampMayOpen(kGuestLampAfterTalkGrace - 0.01));
    CHECK(GuestLampMayOpen(kGuestLampAfterTalkGrace));  // later: the player's own press at the lamp
    CHECK(GuestLampMayOpen(1e9));                       // no talk yet

    // 0.7.7: map travel (the test aid asks for it): the lit lamps of the table, never the Dream.
    {
        MapTravelSituation ok;
        ok.on = ok.world_up = true;
        ok.map = 0x18010000u;
        ok.hp = 100;
        CHECK(MapTravelState(ok) == MapTravel::Ready);
        MapTravelSituation t = ok;
        t.on = false;
        CHECK(MapTravelState(t) == MapTravel::Off);
        t = ok, t.loading = true;
        CHECK(MapTravelState(t) == MapTravel::NotInWorld);
        t = ok, t.cutscene = true;
        CHECK(MapTravelState(t) == MapTravel::Cutscene);
        t = ok, t.boss = true;
        CHECK(MapTravelState(t) == MapTravel::Boss);
        t = ok, t.hp = 0;
        CHECK(MapTravelState(t) == MapTravel::Dead);
        t = ok, t.busy = true;
        CHECK(MapTravelState(t) == MapTravel::Busy);
        t = ok, t.map = 0x1d000000u;  // area 29: a Chalice Dungeon
        CHECK(MapTravelState(t) == MapTravel::Chalice);
        const auto lit = [](std::uint32_t f) { return f == 12417830u; };
        const auto known = [](std::uint32_t) { return true; };
        CHECK(MapTravelTarget(2412951, lit, known));
        CHECK(!MapTravelTarget(2412950, lit, known));          // not lit
        CHECK(!MapTravelTarget(kDreamReturnPoint, lit, known)); // the Dream is no map target
        CHECK(!MapTravelTarget(9999999, lit, known));
    }

    // CL-48ee07: Chalice layer travel: the dungeon lamp's list (the Dream, then the other lit lamps)
    {
        const auto known = [](std::uint32_t rp) { return rp >= 2902950u && rp <= 2902958u; };
        // lamp 2 lit (flag 12907850); the player stands at lamp 2; lamp 0 (the start) is always there
        const auto lit = [](std::uint32_t f) { return f == 12907850u; };
        const std::vector<Entry> l = BuildChaliceList(2902952, lit, known);
        CHECK(l.size() == 2);
        CHECK(l[0].return_point == kDreamReturnPoint && l[0].index == 1);
        CHECK(l[1].return_point == 2902950u && l[1].index == 2 && l[1].name_flag == 70002900u && l[1].lit_flag == 12907850u);  // lamp 0 unlit: the standing lamp's flag
        CHECK(BuildChaliceList(2902952, [](std::uint32_t f) { return f == 12907850u || f == 12907810u; }, known)[1].lit_flag == 12907810u);
        CHECK(Chosen(l, 2) == 2902950u);
        // lamp 1 lit too (its registration flag 12907820 counts), lamp 3 blocked ("cannot travel" name flag):
        // ascending, the standing lamp left out
        const auto lit3 = [](std::uint32_t f) { return f == 12907820u || f == 12907850u || f == 12907870u; };
        const auto blocked = [&](std::uint32_t f) { return lit3(f) || f == 70002903u; };
        const std::vector<Entry> m = BuildChaliceList(2902952, blocked, known);
        CHECK(m.size() == 3 && m[1].return_point == 2902950u && m[2].return_point == 2902951u);
        // standing at the start lamp with nothing else lit: nothing to choose but the Dream, the vanilla question stays
        CHECK(BuildChaliceList(2902950, [](std::uint32_t) { return false; }, known).empty());
        // not a dungeon lamp
        CHECK(BuildChaliceList(2412951, lit, known).empty());
        CHECK(IsChaliceLamp(2902958) && !IsChaliceLamp(2902959) && !IsChaliceLamp(2412950));
        CHECK(ChaliceDreamFlag(4) == 72900104u && ChaliceNameFlag(2) == 70002902u && ChaliceLitFlag(8) == 12907970u);
        // the lamp the player stands at: the nearest object within 8 m
        const std::vector<Spot> spots = {{2902950, 40, 31.5f, -54}, {2902952, -48.2f, 30.6f, -93.4f}};
        const float at[3] = {-47.0f, 30.6f, -93.0f}, far_[3] = {0, 0, 0};
        CHECK(NearestChaliceLamp(spots, at, 8.0f) == 2902952u);
        CHECK(NearestChaliceLamp(spots, far_, 8.0f) == 0u);
        // a guest follows the host's dungeon-lamp warp itself only when every guard holds
        CHECK(LayerFollowRefusal(true, 2902950, 7, 7, true, false, false, false, false) == nullptr);
        CHECK(LayerFollowRefusal(false, 2902950, 7, 7, true, false, false, false, false) != nullptr);
        CHECK(LayerFollowRefusal(true, 2412950, 7, 7, true, false, false, false, false) != nullptr);
        CHECK(LayerFollowRefusal(true, 2902959, 7, 7, true, false, false, false, false) != nullptr);
        CHECK(LayerFollowRefusal(true, 2902950, 7, 8, true, false, false, false, false) != nullptr);  // another dungeon
        CHECK(LayerFollowRefusal(true, 2902950, 0, 0, true, false, false, false, false) != nullptr);  // none known
        CHECK(LayerFollowRefusal(true, 2902950, 7, 7, true, true, false, false, false) != nullptr);   // stay home
        CHECK(LayerFollowRefusal(true, 2902950, 7, 7, false, false, false, false, false) != nullptr);
        CHECK(LayerFollowRefusal(true, 2902950, 7, 7, true, false, true, false, false) != nullptr);
        CHECK(LayerFollowRefusal(true, 2902950, 7, 7, true, false, false, true, false) != nullptr);
        CHECK(LayerFollowRefusal(true, 2902950, 7, 7, true, false, false, false, true) != nullptr);
        // the rule: the host (or a solo game) only
        CHECK(ChaliceListOffered(true, false) && !ChaliceListOffered(false, false) && !ChaliceListOffered(true, true));
    }

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("lamp-travel-test: ok\n");
    return 0;
}
