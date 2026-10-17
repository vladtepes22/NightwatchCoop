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

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("lamp-travel-test: ok\n");
    return 0;
}
