// SPDX-License-Identifier: GPL-3.0-or-later
// A party member's own values (gpu/shim/party/party_guestvals_rules.h; statsaudit findings 1, 3, 4,
// 5). Build and run: ninja -C out/gpu party-guestvals-test && out/party-guestvals-test.exe
#include "party/party_guestvals_rules.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static void TestNgGuard() {
    using namespace coop::ngguard;
    View v;
    v.steady = true;
    // At home before any session: learn the own cycle.
    v.own_world = true;
    v.live = 0;
    CHECK(Plan(v) == Act::Learn);
    v.own = 0;
    CHECK(Plan(v) == Act::None);
    // Not steady: nothing.
    v.steady = false;
    CHECK(Plan(v) == Act::None);
    v.steady = true;
    // Summoned into an NG+1 host: the first summoned load saved the own 0 with the flag.
    View c;
    c.steady = true;
    c.client = true;
    c.live = 1;
    c.restore_flag = 1;
    c.restore_value = 0;
    c.own = -1;
    CHECK(Plan(c) == Act::Learn);  // a title join learns the own cycle from +0x159C
    c.own = 0;
    CHECK(Plan(c) == Act::None);   // restore value right
    c.restore_value = 1;           // a later summoned load re-saved the host's
    CHECK(Plan(c) == Act::KeepRestore);
    c.restore_value = 0;
    c.restore_flag = 0;            // an alone load cleared the flag
    CHECK(Plan(c) == Act::KeepRestore);
    c.alone_pending = true;        // ... while that load is set up, the game's values stay
    CHECK(Plan(c) == Act::None);
    c.alone_pending = false;
    c.restore_flag = 1;
    c.restore_value = 0;
    c.own = -1;
    c.restore_flag = 0;            // a title join before the first summoned load: nothing known yet
    CHECK(Plan(c) == Act::None);
    // Home after the session with the host's cycle in it: put back.
    View h;
    h.steady = true;
    h.own_world = true;
    h.own = 0;
    h.host = 1;
    h.live = 1;
    h.after_session = true;
    CHECK(Plan(h) == Act::RestoreHome);
    h.ending_since = true;         // an ending (NG+): the change is the player's own
    CHECK(Plan(h) == Act::AcceptChange);
    h.ending_since = false;
    h.live = 2;                    // not the host's value: the own world's own change
    CHECK(Plan(h) == Act::AcceptChange);
    h.live = 1;
    h.after_session = false;       // not right after a session (an own ending at home)
    CHECK(Plan(h) == Act::AcceptChange);
    h.live = 9;                    // out of range: nothing
    CHECK(Plan(h) == Act::None);
    for (int a = 0; a < 5; ++a) {
        CHECK(ActName(Act(a))[0] != '?');
    }
    // ngfix: in the host's world the live cycle is the host's; the own one stays in +0x159C.
    View g;
    g.host_world = true;
    g.own = 0;
    g.host = 3;
    g.live = 0;                    // a B2 load's descriptor took +0x159C (the own cycle)
    g.restore_flag = 1;
    g.restore_value = 0;
    CHECK(LiveNg(g) == 3);
    CHECK(!RestoreWrong(g));
    g.restore_value = 3;           // the MoveMapStep ctor re-saved the live value
    CHECK(RestoreWrong(g));
    g.live = 3;
    g.restore_value = 0;
    CHECK(LiveNg(g) == -1);        // already the host's
    g.live = 0;
    g.own = -1;                    // a title join before the own cycle is known: nothing written
    CHECK(LiveNg(g) == -1);
    CHECK(!RestoreWrong(g));
    g.own = 0;
    g.host = -1;                   // the host's cycle unknown
    CHECK(LiveNg(g) == -1);
    g.host = 3;
    g.alone_pending = true;        // an alone load is being set up
    CHECK(LiveNg(g) == -1);
    CHECK(!RestoreWrong(g));
    g.alone_pending = false;
    g.host_world = false;          // own world / host: never
    CHECK(LiveNg(g) == -1);
    g.restore_flag = 0;
    CHECK(!RestoreWrong(g));
    g.host_world = true;
    g.live = 8;                    // out of range: nothing
    CHECK(LiveNg(g) == -1);
}

static void TestStain() {
    using namespace coop::gueststain;
    Stain s;
    s.live = true;
    s.slot = 0;
    s.map = 0x18010000;
    s.pos[0] = -192.8f, s.pos[1] = -28.6f, s.pos[2] = 66.1f;
    s.echoes = 5000;
    CHECK(InMap(s, 0x18010000));
    CHECK(InMap(s, 0x18010203));          // same block
    CHECK(!InMap(s, 0x18000000));         // another block
    CHECK(!InMap(s, 0xffffffffu));
    Stain c = s;
    c.map = 0x1d000000;
    CHECK(!InMap(c, 0x1d000000));         // Chalice Dungeons: not ours
    c = s;
    c.echoes = 0;
    CHECK(!InMap(c, 0x18010000));
    c.echoes = 5000;
    c.live = false;
    CHECK(!InMap(c, 0x18010000));
    const float on[3] = {-192.0f, -28.0f, 66.5f}, near[3] = {-190.0f, -28.6f, 66.1f}, far[3] = {-185.0f, -28.6f, 66.1f},
                above[3] = {-192.8f, -24.0f, 66.1f};
    CHECK(Touching(on, s.pos));
    CHECK(!Touching(near, s.pos));        // 2.8 m
    CHECK(!Touching(above, s.pos));       // 4.6 m above
    CHECK(!Arms(on, s.pos));
    CHECK(!Arms(near, s.pos));
    CHECK(Arms(far, s.pos));              // 7.8 m away
    CHECK(Arms(above, s.pos));
    // party_stain.txt lines.
    const std::string line = Format(s);
    Stain r;
    std::string rest;
    CHECK(Parse(line, &r, &rest) && rest.empty());
    CHECK(r.live && r.slot == 0 && r.map == 0x18010000 && r.echoes == 5000);
    CHECK(Horizontal(r.pos, s.pos) < 0.01f);
    CHECK(Parse(line + " 00ff10\n", &r, &rest) && rest == "00ff10");
    CHECK(!Parse("", &r));
    CHECK(!Parse("0 402718720 1 2 3", &r));          // no echoes
    CHECK(!Parse("0 402718720 1 2 3 0", &r));        // 0 echoes
    CHECK(!Parse("-1 402718720 1 2 3 5", &r));       // bad slot
    CHECK(!Parse("0 402718720 1e9 2 3 5", &r));      // absurd position
    CHECK(!Parse("0 402718720 nan 2 3 5", &r));
}

static void TestLampShare() {
    using namespace coop::lampshare;
    const auto f = LitFlags(241, 1);
    CHECK(f.size() == 3 && f[0] == 12417830u && f[1] == 12417831u && f[2] == 72410201u);
    const auto g = LitFlags(230, 0);
    CHECK(g[0] == 12307810u && g[1] == 12307811u && g[2] == 72300200u);
    // Mine lit (base+10 and the 7AAB02xx), theirs nothing: both go.
    auto mine = [](std::uint32_t id) { return id == 12417830u || id == 72410201u; };
    auto none = [](std::uint32_t) { return false; };
    auto all = [](std::uint32_t) { return true; };
    const auto m = Missing(241, 1, mine, none);
    CHECK(m.size() == 2 && m[0] == 12417830u && m[1] == 72410201u);
    CHECK(Missing(241, 1, mine, all).empty());         // theirs lit already
    CHECK(Missing(241, 2, mine, none).empty());        // not lit in mine
    // Only the lamp-lit flag without the headstone flag: not a lit lamp.
    auto half = [](std::uint32_t id) { return id == 72410201u; };
    CHECK(Missing(241, 1, half, none).empty());
    // Lamps past a boss the other world has not beaten stay out of it.
    CHECK(BlockBossFlag(240) == 12401800u && BlockBossFlag(320) == 13201800u);
    CHECK(HeldForBoss(1, true, false));    // the Grand Cathedral, Amelia alive in the host's world
    CHECK(!HeldForBoss(0, true, false));   // the block's first lamp: the way in
    CHECK(!HeldForBoss(1, true, true));    // beaten there too
    CHECK(!HeldForBoss(1, false, false));  // nobody beat it: the lamp is not past it
}

static void TestBossFall() {
    using namespace coop::bossfall;
    Seen none, both;
    both.echoes = both.insight = true;
    Seen echoes_only;
    echoes_only.echoes = true;
    CHECK(Plan(0.5, true, none) == Pay::Wait);
    CHECK(Plan(kWaitS, true, none) == Pay::Both);
    CHECK(Plan(0.1, true, both) == Pay::Nothing);      // the native defeat ran: never twice
    CHECK(Plan(9.0, true, both) == Pay::Nothing);
    CHECK(Plan(9.0, false, none) == Pay::Nothing);     // not in the host's world for the kill
    CHECK(Plan(9.0, true, echoes_only) == Pay::Insight);
    Seen insight_only;
    insight_only.insight = true;
    CHECK(Plan(9.0, true, insight_only) == Pay::Echoes);
    for (int p = 0; p < 5; ++p) {
        CHECK(PayName(Pay(p))[0] != '?');
    }
}

static void TestFirstEncounter() {
    using namespace coop::firstenc;
    CHECK(Guard(12401802) == 9301u);   // Vicar Amelia
    CHECK(Guard(13301802) == 9302u);   // Amygdala
    CHECK(Guard(12401800) == 0u);      // a kill, not a first encounter
    CHECK(Grant(9301, false));
    CHECK(!Grant(9301, true));         // this player had it already
    CHECK(!Grant(0, false));
}

int main() {
    TestFirstEncounter();
    TestNgGuard();
    TestStain();
    TestLampShare();
    TestBossFall();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-guestvals-test: ok\n");
    return 0;
}
