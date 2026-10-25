// SPDX-License-Identifier: GPL-3.0-or-later
// Party phantoms (gpu/shim/party/party_phantom, plus party_travel's guest redirect choice), the
// parts without the game: the EMEVD skip-rule syntax "event:bank:id[@index]", the confinement-wall
// rules, the EVENT "phantom" JSON, rest detection, boss-Insight pairing and grants, the Insight
// drip, the refill scheduler and the lamp announcer.
// Build and run: ninja -C out/gpu party-phantom-test && out/gpu/party-phantom-test.exe
#include "party/party_phantom.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace coop;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

static void test_skip_rules() {
    std::vector<EmevdSkipRule> r;
    std::vector<std::string> err;
    CHECK(ParseEmevdSkipRules("7600:2005:3@6, 7600:2006:2@7,*:2003:12,0x1e:1:2", &r, &err) == 4);
    CHECK(err.empty());
    CHECK(r.size() == 4);
    CHECK(r[0].event == 7600 && r[0].bank == 2005 && r[0].id == 3 && r[0].index == 6 && !r[0].needs_travel);
    CHECK(r[1].event == 7600 && r[1].bank == 2006 && r[1].id == 2 && r[1].index == 7);
    CHECK(r[2].event == -1 && r[2].bank == 2003 && r[2].id == 12 && r[2].index == -1);
    CHECK(r[3].event == 30 && r[3].bank == 1 && r[3].id == 2);

    // Bad items are reported and skipped; good ones around them still count.
    r.clear();
    err.clear();
    CHECK(ParseEmevdSkipRules("7600:2005,1:2:3@x,abc:1:2,1:2:3:4,,5:6:7@", &r, &err) == 0);
    CHECK(err.size() == 5);
    r.clear();
    err.clear();
    CHECK(ParseEmevdSkipRules("1:2:3@-1,9:8:7", &r, &err) == 1);
    CHECK(err.size() == 1 && r.size() == 1 && r[0].event == 9);
    CHECK(ParseEmevdSkipRules("", &r, nullptr) == 0);

    // Matching: the index separates 7600's "off" write (index 1) from its "on" write (index 6).
    const EmevdSkipRule on{7600, 2005, 3, 6, false};
    CHECK(EmevdSkipMatches(on, 7600, 2005, 3, 6));
    CHECK(!EmevdSkipMatches(on, 7600, 2005, 3, 1));
    CHECK(!EmevdSkipMatches(on, 7601, 2005, 3, 6));
    CHECK(!EmevdSkipMatches(on, 7600, 2005, 4, 6));
    const EmevdSkipRule any{-1, 2003, 12, -1, false};
    CHECK(EmevdSkipMatches(any, 12411800, 2003, 12, 22));
    CHECK(EmevdSkipMatches(any, 1, 2003, 12, 0));
    CHECK(!EmevdSkipMatches(any, 1, 2003, 13, 0));

    const std::vector<EmevdSkipRule> walls = ConfinementWallRules();
    CHECK(walls.size() == 6);
    CHECK(walls[0].event == 7600 && walls[0].bank == 2005 && walls[0].id == 3 && walls[0].index == 6 &&
          walls[0].needs_travel);
    CHECK(walls[1].event == 7600 && walls[1].bank == 2006 && walls[1].id == 2 && walls[1].index == 7 &&
          walls[1].needs_travel);
    CHECK(walls[2].event == 12907600 && walls[2].index == 6 && walls[3].event == 12907600 && walls[3].index == 7);
    // m34 13400010, the wall behind Ludwig (player report 2026-10-22): wall 7, SFX 8, with the open world.
    CHECK(walls[4].event == 13400010 && walls[4].bank == 2005 && walls[4].index == 7 && walls[4].needs_travel);
    CHECK(walls[5].event == 13400010 && walls[5].bank == 2006 && walls[5].id == 2 && walls[5].index == 8);
    {
        // Party mode whatever the open world: 13400010's "not in multiplayer" condition [11]; on a
        // client, the room-open skip of the Ebrietas, Rom, Laurence and Lady Maria re-fight events.
        const std::vector<EmevdSkipRule> rooms = BossRoomRules();
        CHECK(rooms.size() == 5);
        CHECK(rooms[0].event == 13400010 && rooms[0].bank == 0 && rooms[0].id == 0 && rooms[0].index == 11 &&
              !rooms[0].needs_travel && !rooms[0].client_only);
        std::set<int> refights;
        for (std::size_t i = 1; i < rooms.size(); ++i) {
            CHECK(rooms[i].bank == 1003 && rooms[i].id == 5 && rooms[i].index == 2 && rooms[i].client_only &&
                  !rooms[i].needs_travel);
            refights.insert(rooms[i].event);
        }
        CHECK(refights == (std::set<int>{12424810, 13204830, 13404850, 13504800}));
        CHECK(DescribeSkipRule(rooms[1]) == "12424810:1003:5@2 (on a client)");
    }
    // Event 7600 as the data has it (common.emevd), and its Chalice Dungeon copy 12907600
    // (m29.emevd, the same 12 instructions): only 6 and 7 are skipped.
    const int banks[12][2] = {{2000, 2}, {2005, 3}, {2006, 1}, {3, 6}, {3, 6}, {0, 0},
                              {2005, 3}, {2006, 2}, {3, 6}, {3, 6}, {0, 0}, {1000, 4}};
    for (const int event : {7600, 12907600}) {
        for (int i = 0; i < 12; ++i) {
            bool skip = false;
            for (const EmevdSkipRule& w : walls) {
                skip = skip || EmevdSkipMatches(w, event, banks[i][0], banks[i][1], i);
            }
            CHECK(skip == (i == 6 || i == 7));
        }
    }

    CHECK(DescribeSkipRule(walls[0]) == "7600:2005:3@6 (with B1 travel)");
    CHECK(DescribeSkipRule(any) == "*:2003:12");
    // The description parses back to the same rule.
    r.clear();
    CHECK(ParseEmevdSkipRules("7600:2006:2@7", &r) == 1 && DescribeSkipRule(r[0]) == "7600:2006:2@7");
}

static void test_events() {
    PhantomEvent lamp;
    lamp.kind = PhantomEventKind::Lamp;
    lamp.seq = 0x1234567890abcdefull;
    lamp.lamp = 2412952;
    PhantomEvent out;
    std::string err;
    CHECK(PhantomEventFromJsonText(PhantomEventToJsonText(lamp), &out, &err));
    CHECK(out.kind == PhantomEventKind::Lamp && out.seq == lamp.seq && out.lamp == 2412952u);

    PhantomEvent ins;
    ins.kind = PhantomEventKind::Insight;
    ins.seq = 7;
    ins.insight = 3;
    ins.source_event = 12411800;
    CHECK(PhantomEventFromJsonText(PhantomEventToJsonText(ins), &out, &err));
    CHECK(out.kind == PhantomEventKind::Insight && out.insight == 3 && out.source_event == 12411800u && out.seq == 7);
    CHECK(out.boss);
    ins.boss = false;  // G2: a first encounter / story Insight
    CHECK(PhantomEventFromJsonText(PhantomEventToJsonText(ins), &out) && !out.boss && out.insight == 3);
    CHECK(!PhantomEventFromJsonText("{\"kind\":\"insight\",\"seq\":\"0x1\",\"n\":2,\"event\":5,\"boss\":1}", &out));
    ins.boss = true;

    // Statsaudit finding 4: the host's boss clear bonus.
    PhantomEvent bonus;
    bonus.kind = PhantomEventKind::BossBonus;
    bonus.seq = 11;
    bonus.bonus = 128000;
    bonus.source_event = 2100800;
    CHECK(PhantomEventFromJsonText(PhantomEventToJsonText(bonus), &out, &err));
    CHECK(out.kind == PhantomEventKind::BossBonus && out.bonus == 128000 && out.source_event == 2100800u);
    CHECK(!PhantomEventFromJsonText("{\"kind\":\"boss_bonus\",\"seq\":\"0x1\",\"echoes\":-5,\"event\":1}", &out));
    CHECK(!PhantomEventFromJsonText("{\"kind\":\"boss_bonus\",\"seq\":\"0x1\",\"echoes\":99999999,\"event\":1}", &out));
    CHECK(!PhantomEventFromJsonText("{\"kind\":\"boss_bonus\",\"seq\":\"0x1\",\"event\":1}", &out));

    PhantomEvent rest;
    rest.kind = PhantomEventKind::Rested;
    rest.seq = 9;
    CHECK(PhantomEventFromJsonText(PhantomEventToJsonText(rest), &out, &err));
    CHECK(out.kind == PhantomEventKind::Rested && out.seq == 9);

    CHECK(!PhantomEventFromJsonText("{\"kind\":\"dance\",\"seq\":\"0x1\"}", &out, &err));
    CHECK(!PhantomEventFromJsonText("{\"kind\":\"lamp\",\"seq\":\"0x1\"}", &out, &err)); // no lamp
    CHECK(!PhantomEventFromJsonText("{\"kind\":\"insight\",\"seq\":\"0x1\",\"n\":500,\"event\":1}", &out, &err));
    CHECK(!PhantomEventFromJsonText("not json", &out, &err));
    CHECK(!PhantomEventFromJsonText("[1,2]", &out, &err));
}

static void test_rested() {
    TravelIntent t;
    t.kind = TravelKind::Lamp;
    t.respawn_mode = 2; // world lamp -> Hunter's Dream
    CHECK(RestedFromTravel(t));
    t.respawn_mode = 1; // headstone -> world lamp
    CHECK(!RestedFromTravel(t));
    t.kind = TravelKind::HostDeath;
    CHECK(RestedFromTravel(t));
    t.kind = TravelKind::HuntersMark;
    CHECK(RestedFromTravel(t));
    t.kind = TravelKind::ScriptedWarp;
    CHECK(!RestedFromTravel(t));
    t.kind = TravelKind::LuaBonfireWarp;
    CHECK(!RestedFromTravel(t));
}

static void test_insight() {
    CHECK(InsightModeFromString(nullptr) == InsightMode::Parity);
    CHECK(InsightModeFromString("") == InsightMode::Parity);
    CHECK(InsightModeFromString("0") == InsightMode::Off);
    CHECK(InsightModeFromString("off") == InsightMode::Off);
    CHECK(InsightModeFromString("full") == InsightMode::Full);
    CHECK(InsightModeFromString("parity") == InsightMode::Parity);
    // pard4: the relay settles against what the guest's own game paid (live B2: Logarius).
    CHECK(InsightTopUp(2, 3, 1) == 2);   // only the native cooperator +1: the relay's 2
    CHECK(InsightTopUp(2, 3, 4) == 0);   // its own defeat paid the boss's 3 and the +1: nothing
    CHECK(InsightTopUp(2, 3, 2) == 1);
    CHECK(InsightTopUp(2, 3, 0) == 2);   // nothing native yet (the fallback adds the +1 later)
    CHECK(InsightTopUp(1, 1, 1) == 0);   // first encounter paid natively
    CHECK(InsightTopUp(1, 1, -2) == 1);  // Insight spent meanwhile: counts as no gain
    CHECK(InsightTopUp(0, 1, 0) == 0);
    CHECK(GuestInsightGrant(3, InsightMode::Parity) == 2);
    CHECK(GuestInsightGrant(1, InsightMode::Parity) == 0);
    CHECK(GuestInsightGrant(3, InsightMode::Full) == 3);
    CHECK(GuestInsightGrant(3, InsightMode::Off) == 0);
    CHECK(GuestInsightGrant(0, InsightMode::Full) == 0);
    CHECK(GuestInsightGrant(-2, InsightMode::Full) == 0);
    {
        PhantomEvent e;
        e.kind = PhantomEventKind::Insight;
        e.seq = 7;
        e.insight = 1;
        e.source_event = 9422;
        e.boss = false;
        PhantomEvent back;
        CHECK(PhantomEventFromJsonText(PhantomEventToJsonText(e), &back));
        CHECK(back.kind == PhantomEventKind::Insight && back.insight == 1 && !back.boss && back.source_event == 9422);
    }

    BossInsightTracker tr;
    // First encounter (no defeat marker in that event): nothing.
    CHECK(!tr.OnInitializeEvent(12411802, 9350, 1, 10.0));
    // Gascoigne: 2003[12] at 22, then 2000[0] (slot, 9350, 2) at 29.
    tr.OnBossDefeat(12411800, 100.0);
    CHECK(!tr.OnInitializeEvent(12411800, 9351, 2, 100.5)); // another event started: not Insight
    std::optional<int> n = tr.OnInitializeEvent(12411800, 9350, 2, 101.0);
    CHECK(n && *n == 2);
    CHECK(!tr.OnInitializeEvent(12411800, 9350, 2, 101.5)); // once per defeat
    // Another event's start does not pair with this defeat.
    tr.OnBossDefeat(12401800, 200.0);
    CHECK(!tr.OnInitializeEvent(12411800, 9350, 3, 201.0));
    // Too late.
    CHECK(!tr.OnInitializeEvent(12401800, 9350, 3, 200.0 + BossInsightTracker::kWindow + 1));
    // Bad N.
    tr.OnBossDefeat(1, 300.0);
    CHECK(!tr.OnInitializeEvent(1, 9350, 0, 300.0));

    // G2: Insight given by SpEffect 4680 outside 9350 (kidnap, chalice boss events).
    CHECK(DirectInsight::Allowed(9422));
    CHECK(DirectInsight::Allowed(12901588));
    CHECK(DirectInsight::Allowed(12901708));
    CHECK(DirectInsight::Allowed(12901718));
    CHECK(!DirectInsight::Allowed(9350));     // through BossInsightTracker
    CHECK(!DirectInsight::Allowed(9183));     // NPC summon refund: the player's own Insight back
    CHECK(!DirectInsight::Allowed(12404410)); // NPC summon cost (4682)
    DirectInsight di;
    CHECK(!di.OnSetSpEffect(9183, 10000, 4680, 10.0));
    CHECK(!di.OnSetSpEffect(9422, 10000, 4682, 10.0));     // another SpEffect
    CHECK(!di.OnSetSpEffect(9422, 2800100, 4680, 10.0));   // not the player
    CHECK(di.OnSetSpEffect(9422, 10000, 4680, 10.0));
    CHECK(di.Due(10.5).empty());                           // not quiet yet
    CHECK(di.Due(9.9).empty());                            // a tick clock read before the count
    // Chalice encounter table: 3 x 4680, 10 frames apart.
    CHECK(di.OnSetSpEffect(12901718, 10000, 4680, 10.6));
    CHECK(di.OnSetSpEffect(12901718, 10000, 4680, 10.77));
    CHECK(di.OnSetSpEffect(12901718, 10000, 4680, 10.94));
    std::vector<DirectInsight::Grant> due = di.Due(11.2);
    CHECK(due.size() == 1 && due[0].event == 9422 && due[0].n == 1);
    CHECK(di.Due(11.5).empty());
    due = di.Due(12.0);
    CHECK(due.size() == 1 && due[0].event == 12901718 && due[0].n == 3);
    CHECK(di.Due(30.0).empty()); // handed out once
    for (int i = 0; i < 50; ++i) {
        di.OnSetSpEffect(12901708, 10000, 4680, 40.0);
    }
    due = di.Due(41.0);
    CHECK(due.size() == 1 && due[0].n == DirectInsight::kMaxPerEvent);
    // A chalice layer boss: +2 then 2003[12] in the same event -> a boss kill at the flush.
    BossInsightTracker tr2;
    CHECK(!tr2.DefeatedRecently(12901588, 50.0));
    tr2.OnBossDefeat(12901588, 50.0);
    CHECK(tr2.DefeatedRecently(12901588, 51.0));
    CHECK(tr2.DefeatedRecently(12901588, 51.0)); // not consumed
    CHECK(!tr2.DefeatedRecently(12901708, 51.0));
    CHECK(!tr2.DefeatedRecently(12901588, 50.0 + BossInsightTracker::kWindow + 1));

    InsightDripper d;
    CHECK(!d.Step(true));
    d.Add(3);
    CHECK(d.Pending() == 3);
    CHECK(!d.Step(false)); // not allowed: waits
    CHECK(d.Step(true));
    int applied = 1, ticks = 0;
    while (d.Pending() > 0 && ticks < 1000) {
        applied += d.Step(true) ? 1 : 0;
        ++ticks;
    }
    CHECK(applied == 3);
    CHECK(ticks >= 2 * InsightDripper::kInterval - 1);
    d.Add(500);
    CHECK(d.Pending() == InsightDripper::kMaxPending);
    d.Add(-4);
    CHECK(d.Pending() == InsightDripper::kMaxPending);

    // G2: a boss's first-encounter +1 and its kill +3 a few seconds later both count (the old
    // 30 s one-event limit dropped the kill); a flood is still capped.
    InsightBudget b;
    CHECK(b.Take(1, 100.0) == 1);
    CHECK(b.Take(3, 105.0) == 3);
    CHECK(b.Take(0, 106.0) == 0);
    CHECK(b.Take(-2, 106.0) == 0);
    CHECK(b.Take(50, 107.0) == InsightBudget::kMaxPerEvent);
    CHECK(b.Take(10, 108.0) == InsightBudget::kMaxPerWindow - 14);
    CHECK(b.Take(1, 109.0) == 0);
    // The window rolls: the 100 s and 105 s grants expire first.
    CHECK(b.Take(9, 100.0 + InsightBudget::kWindow + 6) == 4);
    CHECK(b.Take(5, 107.0 + InsightBudget::kWindow + 1) == 5);
}

static ReplayState Up() {
    ReplayState s;
    s.world_up = true;
    s.loading = false;
    s.transition_requested = false;
    s.session_role = 0;
    return s;
}

static void test_refill() {
    RefillScheduler r;
    double t = 0;
    CHECK(!r.Step(Up(), t));

    // No load expected: after kStableTicks steady frames.
    r.Request(RefillReason::HostRested, false, t);
    int fired = -1;
    for (int i = 0; i < 100; ++i) {
        if (r.Step(Up(), t += 0.016)) {
            fired = i;
            break;
        }
    }
    CHECK(fired == RefillScheduler::kStableTicks - 1);
    CHECK(!r.Pending());

    // Load expected: nothing before it, then after the world is steady again.
    r.Request(RefillReason::GuestRespawn, true, t);
    for (int i = 0; i < 100; ++i) {
        CHECK(!r.Step(Up(), t += 0.016));
    }
    ReplayState loading = Up();
    loading.loading = true;
    for (int i = 0; i < 20; ++i) {
        CHECK(!r.Step(loading, t += 0.016));
    }
    ReplayState pending = Up();
    pending.transition_requested = true;
    CHECK(!r.Step(pending, t += 0.016));
    ReplayState joining = Up();
    joining.session_role = 4;
    for (int i = 0; i < 40; ++i) {
        CHECK(!r.Step(joining, t += 0.016)); // the bell's join reload: not a safe point
    }
    fired = -1;
    for (int i = 0; i < 100; ++i) {
        if (r.Step(Up(), t += 0.016)) {
            fired = i;
            break;
        }
    }
    CHECK(fired == RefillScheduler::kStableTicks - 1);

    // The expected load never comes: refill after kLoadWait anyway.
    r.Request(RefillReason::HostRested, true, t);
    t += RefillScheduler::kLoadWait + 1;
    bool any = false;
    for (int i = 0; i < 40; ++i) {
        any = any || r.Step(Up(), t += 0.016);
    }
    CHECK(any);

    // Never a safe moment: dropped after kTimeout.
    r.Request(RefillReason::HostRested, false, t);
    ReplayState down;
    for (int i = 0; i < 10; ++i) {
        CHECK(!r.Step(down, t += 1.0));
    }
    t += RefillScheduler::kTimeout;
    CHECK(!r.Step(down, t));
    CHECK(!r.Pending());

    CHECK(std::string(RefillReasonName(RefillReason::GuestRespawn)) == "guest respawn");
}

static void test_lamp_announcer() {
    LampAnnouncer a;
    CHECK(!a.ShouldSend(kTravelNone, 1));
    CHECK(!a.ShouldSend(0, 1));
    CHECK(a.ShouldSend(2412952, 1));
    CHECK(!a.ShouldSend(2412952, 1));
    CHECK(a.ShouldSend(2412952, 2)); // a member joined: tell again
    CHECK(!a.ShouldSend(2412952, 2));
    CHECK(a.ShouldSend(2412951, 2)); // the host lit another lamp
}

static void test_guest_redirect() {
    // party_travel's choice for the guest-side funnel calls.
    CHECK(ChooseGuestRedirect(TravelKind::GuestDied, false, 2412952) == GuestRedirect::HostLamp);
    CHECK(ChooseGuestRedirect(TravelKind::GuestDied, true, 2412952) == GuestRedirect::TravelDestination);
    CHECK(ChooseGuestRedirect(TravelKind::GuestDied, true, kTravelNone) == GuestRedirect::TravelDestination);
    CHECK(ChooseGuestRedirect(TravelKind::GuestDied, false, kTravelNone) == GuestRedirect::None);
    CHECK(ChooseGuestRedirect(TravelKind::GuestDied, false, 0) == GuestRedirect::None);
    CHECK(ChooseGuestRedirect(TravelKind::HuntersMark, false, 2102950) == GuestRedirect::HostLamp);
    // The stock send-home kinds keep their own retarget (party_travel RetargetSendHome).
    CHECK(ChooseGuestRedirect(TravelKind::GuestSentHome, false, 2412952) == GuestRedirect::None);
    CHECK(ChooseGuestRedirect(TravelKind::BossCleared, true, 2412952) == GuestRedirect::None);
    CHECK(ChooseGuestRedirect(TravelKind::Lamp, false, 2412952) == GuestRedirect::None);

    TravelIntent t;
    CHECK(HostLampFromTravel(t) == kTravelNone);
    t.last_lamp = (101163ull << 32) | 2412952u;
    CHECK(HostLampFromTravel(t) == 2412952u);
    t.last_lamp = 0xffffffff00000000ull;
    CHECK(HostLampFromTravel(t) == kTravelNone);
}

int main() {
    test_skip_rules();
    test_events();
    test_rested();
    test_insight();
    test_refill();
    test_lamp_announcer();
    test_guest_redirect();
    std::printf("party-phantom-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
