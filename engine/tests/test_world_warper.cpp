// SPDX-License-Identifier: GPL-3.0-or-later
// The world warper's rules (gpu/shim/party/world_warper_rules.h): options, the route through
// every ReturnPointParam row, the progress file and resume after a crash, the scripted steps.
// Build and run: ninja -C out/gpu world-warper-test && out/gpu/world-warper-test.exe
#include "party/party_ids.h"
#include "party/world_warper_rules.h"

#include <cstdio>
#include <iterator>
#include <set>

using namespace coop::warper;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static bool Spot(std::uint32_t rp) {
    float sp[4];
    return coop::ids::TemplateReturnSpot(rp, sp);
}

int main() {
    // Options.
    CHECK(ParseOptions(nullptr).mode == Mode::Off);
    CHECK(ParseOptions("").mode == Mode::Off);
    CHECK(ParseOptions("0").mode == Mode::Off);
    CHECK(ParseOptions("1").mode == Mode::Tour);
    CHECK(ParseOptions("tour").mode == Mode::Tour);
    CHECK(ParseOptions("replay").mode == Mode::Replay);
    {
        const Options o = ParseOptions("measure,lamps=2412950+2402950,replay=2102950,max=5,noenemy");
        CHECK(o.mode == Mode::Measure);
        CHECK(o.lamps.size() == 2 && o.lamps[0] == 2412950 && o.lamps[1] == 2402950);
        CHECK(o.replay.size() == 1 && o.replay[0] == 2102950);
        CHECK(o.max == 5);
        CHECK(o.act && !o.enemies && o.kill);
    }
    {
        const Options o = ParseOptions("lamps=2412950;noact;nokill");
        CHECK(o.mode == Mode::Tour);
        CHECK(o.lamps.size() == 1);
        CHECK(!o.act && o.enemies && !o.kill);
    }
    CHECK(ParseIds("1+x+3 4").size() == 3);

    // The route: every row with a spot, Dream first, then the lamps, Chalice rows never.
    const std::vector<std::uint32_t> all(std::begin(coop::ids::kPartyReturnPoints),
                                         std::end(coop::ids::kPartyReturnPoints));
    const auto route = BuildRoute(all, Spot);
    CHECK(!route.empty());
    CHECK(route.front() == coop::lamptravel::kDreamReturnPoint);
    CHECK(route[1] == coop::lamptravel::ReturnPoint(coop::lamptravel::kLamps[0]));
    std::set<std::uint32_t> uniq(route.begin(), route.end());
    CHECK(uniq.size() == route.size());
    for (std::uint32_t rp : route) {
        CHECK(!ChaliceRow(rp));
        CHECK(Spot(rp));
    }
    std::size_t lamps_in = 0;
    for (const auto& l : coop::lamptravel::kLamps) lamps_in += uniq.count(coop::lamptravel::ReturnPoint(l));
    CHECK(lamps_in == coop::lamptravel::kLampCount);  // every lamp is visited
    std::printf("route: %zu rows (%zu lamps + the Dream + %zu other rows)\n", route.size(), lamps_in,
                route.size() - lamps_in - 1);
    {
        Options o;
        o.lamps = {2402950, 2902950, 2402950, 99999999};
        const auto r = BuildRoute(all, Spot, o);
        CHECK(r.size() == 1 && r[0] == 2402950);  // chalice, duplicate and unknown rows dropped
        Options m;
        m.max = 3;
        CHECK(BuildRoute(all, Spot, m).size() == 3);
    }

    // Progress and resume.
    {
        const std::string text = "start 2102950\ndone 2102950 120 2 80 45 m21_00_00_00\nstart 2412950\r\n"
                                 "start 2412950\nstart 2412951\nskip 2412952 no load\n";
        const Progress p = ParseProgress(text);
        CHECK(p.done.count(2102950));
        CHECK(p.starts.at(2412950) == 2);
        CHECK(!p.finished);
        CHECK(SkipReason(p, 2102950) != nullptr);
        CHECK(SkipReason(p, 2412950) != nullptr);  // crashed there twice
        CHECK(SkipReason(p, 2412951) == nullptr);  // once: tried again
        CHECK(SkipReason(p, 2412952) != nullptr);
        const std::vector<std::uint32_t> r = {2102950, 2412950, 2412951, 2412952, 2412953};
        CHECK(NextIndex(r, p, 0) == 2);
        CHECK(NextIndex(r, p, 3) == 4);
        CHECK(NextIndex(r, p, 5) == 5);
        CHECK(HandledCount(r, p) == 3);
        CHECK(ParseProgress(text + "finished\n").finished);
        CHECK(ParseProgress("garbage\n\nstart x\n").starts.empty());
    }
    CHECK(DoneLine(2402950, 10, 2, 120.4, 50.2, "m24_00_00_00") == "done 2402950 10 2 120 50 m24_00_00_00");
    CHECK(ParseProgress(DoneLine(2402950, 10, 2, 1, 1, "m") + "\n").done.count(2402950));

    // Steps: the routine stays short (75 rows must fit in about an hour).
    CHECK(Seconds(LookSteps()) > 5.0f && Seconds(LookSteps()) < 15.0f);
    CHECK(Seconds(WalkSteps()) > 5.0f && Seconds(WalkSteps()) < 15.0f);
    CHECK(Seconds(ActionSteps()) < 30.0f);
    CHECK(Seconds(LoadoutSteps()) < 90.0f);
    for (const auto& st : ActionSteps()) CHECK(st.what && st.what[0]);
    CHECK(std::string(AreaName(2402950)) == "Cathedral Ward");
    CHECK(std::string(AreaName(9990000)).empty());

    // The full-coverage options (defaults on, word options off; chalice / dump opt-in).
    {
        const Options d = ParseOptions("tour");
        CHECK(d.tod && d.gear && d.remo && !d.bosses && d.items && !d.chalice && !d.dump && d.enemies_per_row == 6);
        const Options o = ParseOptions("chalice,notod,nogear,noremo,nobosses,noitems,dump,enemies=2");
        CHECK(o.mode == Mode::Tour);  // an option first: a tour
        CHECK(o.chalice && !o.tod && !o.gear && !o.remo && !o.bosses && !o.items && o.dump && o.enemies_per_row == 2);
        CHECK(ParseOptions("tour,remo").remo && ParseOptions("remo").mode == Mode::Tour);
        CHECK(ParseOptions("tour,bosses").bosses && kEnemyRange < 60.0f);
        CHECK(ParseOptions("tour,gear").gear);
        Options c;
        c.chalice = true;
        c.lamps = {2402950, 2902950};
        const auto r = BuildRoute(all, Spot, c);
        CHECK(r.size() == 2 && r[1] == 2902950);  // chalice rows when asked for
        Options full;
        full.chalice = true;
        const auto fr = BuildRoute(all, Spot, full);
        std::size_t chalice_rows = 0;
        for (std::uint32_t rp : fr) chalice_rows += ChaliceRow(rp) ? 1 : 0;
        CHECK(chalice_rows > 0 && fr.size() == route.size() + chalice_rows);
    }
    // Content seen in earlier sessions.
    {
        const Progress p = ParseProgress("todorig 6\nkind 2100\nkind 2100\nremo 21000010\ntod 4 2402950 12\n"
                                         "tod 7 2102950\neffects\nkind x\nfx 2160\n");
        CHECK(p.tod_orig == 6);
        CHECK(p.kinds.size() == 1 && p.kinds.count(2100));
        CHECK(p.remos.count(21000010));
        CHECK(p.tod_done.count(std::uint64_t(4) << 32 | 2402950) && p.tod_done.count(std::uint64_t(7) << 32 | 2102950));
        CHECK(p.effects_done && p.fx.count(2160));
        CHECK(p.done.empty() && p.starts.empty());
        CHECK(ParseProgress("").tod_orig == -1);
    }
    CHECK(EnemyKind(100010u) == 1000u && EnemyKind(10100080u) == 1010u && EnemyKind(100007000u) == 1000u);
    CHECK(EnemyRowFightable(100010u) && EnemyRowFightable(10100080u) && EnemyRowFightable(210000010u));
    CHECK(!EnemyRowFightable(902010u) && !EnemyRowFightable(905000u) && !EnemyRowFightable(6040u) && !EnemyRowFightable(0));
    CHECK(RemoMap(24010000u) == (24u << 24 | 1u << 16) && RemoMap(21000020u) == (21u << 24));
    // Time of day: the passes skip the save's own value; the route is one row per map of the
    // Yharnam areas only.
    CHECK(TodPasses(6) == (std::vector<int>{0, 4, 7}));
    CHECK(TodPasses(-1).size() == 4);
    CHECK(std::string(TodName(7)) == "Blood Moon");
    {
        const auto tr = TodRoute(route);
        std::set<std::uint32_t> maps;
        for (std::uint32_t rp : tr) {
            CHECK(TodArea(rp));
            CHECK(maps.insert(rp / 10000).second);
        }
        CHECK(!tr.empty() && tr.size() < route.size());
        CHECK(tr.front() == coop::lamptravel::kDreamReturnPoint);
        CHECK(!TodArea(3402950) && !TodArea(2902950) && TodArea(2402950));
    }
    // Gear: every armor set and weapon model comes round within one tour.
    {
        std::set<std::uint32_t> armor, right, left;
        for (std::size_t n = 0; n < route.size(); ++n) {
            const Gear g = GearFor(n);
            armor.insert(g.armor);
            right.insert(g.right), left.insert(g.left);
        }
        CHECK(armor.size() == std::size(kWarperArmorSets));
        CHECK(right.size() == std::size(kWarperRightWeapons));
        CHECK(left.size() == std::size(kWarperLeftWeapons));
        CHECK(GearFor(0).right == kWarperRightWeapons[0] && GearFor(1).right == kWarperRightWeapons[1]);
        CHECK(Seconds(LoadoutSteps(false)) < Seconds(LoadoutSteps(true)));
        for (const auto& st : GearSteps()) CHECK(st.buttons != kRight && st.buttons != kLeft);
        std::printf("gear: %zu armor sets, %zu right-hand and %zu left-hand weapons\n", armor.size(), right.size(),
                    left.size());
    }
    CHECK(kSlotRight1 == 1 && kSlotHead == 8 && kRecEquipId - kRecEquipHandle == 0x40);
    CHECK(!ItemEffectAllowed(100, 2100) && !ItemEffectAllowed(200, 9000) && !ItemEffectAllowed(1, 1));
    CHECK(ItemEffectAllowed(1500, 2160) && !ItemEffectAllowed(5, 9025));
    CHECK(!ItemEffectAllowed(1220, 150) && !ItemEffectAllowed(1101, 230) && !ItemEffectAllowed(1601, 3401));
    CHECK(!ItemEffectAllowed(1600, 3400) && !ItemEffectAllowed(4320, 4685) && ItemEffectAllowed(1300, 2200));
    CHECK(Seconds(GearSteps()) < 45.0f);

    // The cutscene pass: unplayed cutscenes with a row of their map, rows taking turns, tries capped.
    {
        const std::vector<std::uint32_t> tour = {2102950, 2402950, 2402951, 2412950};
        Progress p = ParseProgress("remo 24000000\nremook 24000000\nremo 24000020\nremo 24000030\nremo 24000030\n");
        CHECK(p.remos_ok.count(24000000) && p.remo_tries[24000030] == 2 && p.remo_tries[24000020] == 1);
        const auto r = CutsceneRoute(tour, {24000000, 24000020, 24000030, 24000060, 21000010, 36000000}, p);
        // 24000000 played, 24000030 tried twice, 36000000 has no row: 24000020, 24000060, 21000010.
        CHECK(r.size() == 3);
        CHECK(r[0].second == 24000020 && r[0].first == 2402951);  // tried once: the map's next row
        CHECK(r[1].second == 24000060 && r[1].first == 2402951);  // the map's 2nd cutscene: 2nd row
        CHECK(r[2].second == 21000010 && r[2].first == 2102950);
        const Options o = ParseOptions("tour,remoonly,remos=24000020+21000010,remoskip=20");
        CHECK(o.remo_only && o.remo && o.remos.size() == 2 && o.remo_skip == 20);
        CHECK(ParseOptions("tour").remo_skip == 150 && !ParseOptions("tour").remo_only);
    }
    // Boss spots: in front of the boss first (forward = (-sin, -cos)), then right, left, back;
    // usable only on its floor, near it, and when a walk toward it gets closer.
    {
        const float boss[3] = {10, 5, 20};
        float p[3];
        BossSpot(boss, 0.0f, 0, p);
        CHECK(std::fabs(p[0] - 10) < 1e-4f && std::fabs(p[2] - 16) < 1e-4f && std::fabs(p[1] - 5.3f) < 1e-4f);
        BossSpot(boss, 0.0f, 3, p);
        CHECK(std::fabs(p[2] - 24) < 1e-4f);
        BossSpot(boss, 0.0f, 1, p);
        CHECK(std::fabs(std::fabs(p[0] - 10) - 4) < 1e-4f && std::fabs(p[2] - 20) < 1e-4f);
        const float from[3] = {10, 5, 16};
        const float yaw = FaceYaw(from, boss);  // looking from the spot to the boss: +z
        CHECK(std::fabs(-std::sin(yaw)) < 1e-4f && std::fabs(-std::cos(yaw) - 1) < 1e-4f);
        const float settled[3] = {10, 5.1f, 16}, walked[3] = {10, 5.1f, 17}, stuck[3] = {10, 5.1f, 16.1f};
        const float fell[3] = {10, -3, 16}, far[3] = {10, 5, 9}, far2[3] = {10, 5, 10};
        CHECK(BossSpotOk(boss, settled, walked));
        CHECK(!BossSpotOk(boss, settled, stuck));   // inside a wall: no move
        CHECK(!BossSpotOk(boss, fell, fell));       // fell through
        CHECK(!BossSpotOk(boss, far, far2));        // pushed out of the arena
        float a[3];
        CHECK(BossArenaSpot(2300800, a) && std::fabs(a[0] + 88.3f) < 1e-3f && !BossArenaSpot(2300801, a));
        const float spot[3] = {0, 10, 0}, s1[3] = {0, 10.2f, 0}, w1[3] = {0.5f, 10.2f, 0}, w0[3] = {0.1f, 10.2f, 0};
        const float down[3] = {0, 3, 0};
        CHECK(ArenaSpotOk(spot, s1, w1) && !ArenaSpotOk(spot, s1, w0) && !ArenaSpotOk(spot, down, down));
    }
    // Gestures and hunter tools: once per run, resumed from the progress file.
    {
        CHECK(GesturesLeft(Progress{}).size() == 23 && ToolsLeft(Progress{}).size() == std::size(kWarperTools));
        const Progress p = ParseProgress("gesture 1\ngesture 23\ntool 1200\ntool 2120\n");
        const auto gl = GesturesLeft(p);
        CHECK(gl.size() == 21 && gl.front() == 2 && gl.back() == 22);
        CHECK(ToolsLeft(p).size() == std::size(kWarperTools) - 2);
        CHECK(std::string(GestureName(1)) == "Point Forward" && std::string(GestureName(23)) == "League Oath");
        CHECK(std::string(GestureName(0)) == "?" && std::string(GestureName(24)) == "?");
        CHECK(GestureSeen({0, 100, 0}, {104140, 100, 0}) && !GestureSeen({104140}, {104140, 0}));
        CHECK(!GestureSeen({}, {100000, 0, 3000}));
        CHECK(ToolUseSeen(3, 2, 20, 20) && ToolUseSeen(3, 3, 20, 17) && !ToolUseSeen(3, 3, 20, 20));
        CHECK(!ToolUseSeen(-1, -1, -1, -1) && !ToolUseSeen(0, 0, 0, 0));
        CHECK(ParseProgress("toolok 1200\ngestureok 3\n").tools_ok.count(1200) &&
              ParseProgress("gestureok 3\n").gestures_ok.count(3));
        const Options caps = ParseOptions("tour,gestures=1,tools=2");
        CHECK(caps.gestures && caps.tools && caps.gestures_max == 1 && caps.tools_max == 2);
        // Opt-in while unverified live.
        const Options o = ParseOptions("tour,gestures,tools,remopass");
        CHECK(o.gestures && o.tools && o.remo_pass);
        CHECK(!ParseOptions("tour").gestures && !ParseOptions("tour").tools && !ParseOptions("tour").remo_pass);
    }
    // The title stage: first-launch screens confirmed (Down, Cross, Up, Cross), no Continue = no run.
    {
        CHECK(ClassifyTitle(5000, 0, 0, 0, 6000) == TitleScreen::Brightness);
        CHECK(ClassifyTitle(5000, 6000, 0, 0, 7000) == TitleScreen::Controls);    // after brightness
        CHECK(ClassifyTitle(5000, 6000, 0, 7000, 7500) == TitleScreen::Other);    // the title built since
        CHECK(ClassifyTitle(5000, 6000, 0, 7000, 8500) == TitleScreen::NoCharacter); // ...without Continue
        CHECK(TitleUndecided(0, 7000) && !TitleUndecided(7000, 7000) && !TitleUndecided(0, 0));
        CHECK(ClassifyTitle(0, 0, 7000, 7000, 20000) == TitleScreen::Other);      // Continue: go on
        CHECK(ClassifyTitle(0, 0, 0, 0, 20000) == TitleScreen::Other);
        CHECK(FirstLaunchPress(TitleScreen::Brightness, 0) == kDown &&
              FirstLaunchPress(TitleScreen::Brightness, 1) == 0x4000u &&
              FirstLaunchPress(TitleScreen::Controls, 0) == kUp && FirstLaunchPress(TitleScreen::Controls, 1) == 0x4000u);
    }
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("world warper rules: all checks passed\n");
    return 0;
}
