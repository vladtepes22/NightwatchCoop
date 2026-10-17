// SPDX-License-Identifier: GPL-3.0-or-later
// Party campaign start (gpu/shim/party/party_start, C1): BB_PARTY_START parsing, the start step
// and readiness from the flags, the bell grant plan and the lobby's ring decisions.
// Build and run: ninja -C out/gpu party-start-test && out/gpu/party-start-test.exe
#include "party/party_start.h"

#include <cstdio>
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

static StartFlags Flags(bool opening, bool dream, bool right, bool left, bool cutscene = false) {
    StartFlags f;
    f.flags_ok = true;
    f.opening_done = opening;
    f.first_dream = dream;
    f.weapon_right = right;
    f.weapon_left = left;
    f.cutscene = cutscene;
    f.beckoning_count = 0;
    f.resonant_count = 0;
    f.map_id = 0x18010000u;  // Central Yharnam (m24_01): out of the first Dream
    return f;
}

static void test_mode() {
    bool known = false;
    CHECK(ParseStartMode(nullptr, &known) == StartMode::PrologueSolo && known);
    CHECK(ParseStartMode("", &known) == StartMode::PrologueSolo && known);
    CHECK(ParseStartMode("prologue_solo", &known) == StartMode::PrologueSolo && known);
    CHECK(ParseStartMode("Immediate", &known) == StartMode::Immediate && known);
    CHECK(ParseStartMode("clinic", &known) == StartMode::Immediate && known);
    CHECK(ParseStartMode("bogus", &known) == StartMode::PrologueSolo && !known);
    CHECK(std::string(StartModeName(StartMode::Immediate)) == "immediate");
    CHECK(std::string(StartModeName(StartMode::PrologueSolo)) == "prologue_solo");
}

static void test_steps() {
    // The vanilla timeline (campaign_start.md §1.5).
    CHECK(ClassifyStart(StartFlags{}, false) == StartStep::NoWorld);
    CHECK(ClassifyStart(Flags(true, true, true, true), false) == StartStep::NoWorld);
    CHECK(ClassifyStart(StartFlags{}, true) == StartStep::Unknown); // flags unreadable
    CHECK(ClassifyStart(Flags(false, false, false, false), true) == StartStep::Opening);
    CHECK(ClassifyStart(Flags(true, false, false, false), true) == StartStep::Clinic);
    CHECK(ClassifyStart(Flags(true, true, false, false), true) == StartStep::FirstDream);
    CHECK(ClassifyStart(Flags(true, true, true, false), true) == StartStep::FirstDream);
    CHECK(ClassifyStart(Flags(true, true, false, true), true) == StartStep::FirstDream);
    CHECK(ClassifyStart(Flags(true, true, true, true), true) == StartStep::Ready);
    // Playtest 1 #19: both weapons taken but still in the FIRST Hunter's Dream visit (Notebook,
    // headstone to the sickroom next): not ready; ready once out of the Dream (latched), or with
    // the Doll awake (Insight, well past the prologue). An unknown map (first ticks) is not ready.
    {
        StartFlags f = Flags(true, true, true, true);
        f.map_id = 0x15000000u;  // m21_00 Hunter's Dream
        CHECK(ClassifyStart(f, true) == StartStep::FirstDream);
        CHECK(!StartReady(StartMode::PrologueSolo, f, true));
        CHECK(DescribeStart(f, true).find("headstone") != std::string::npos);
        f.map_id = 0xffffffffu;
        CHECK(ClassifyStart(f, true) == StartStep::FirstDream);
        f.map_id = 0;
        CHECK(ClassifyStart(f, true) == StartStep::FirstDream);
        f.map_id = 0x15000000u;
        f.left_first_dream = true;  // back in the Dream after the headstone
        CHECK(ClassifyStart(f, true) == StartStep::Ready);
        f.left_first_dream = false;
        f.doll_awake = true;
        CHECK(ClassifyStart(f, true) == StartStep::Ready);
        f.doll_awake = false;
        f.map_id = 0x15010000u;  // m21_01 (Abandoned Old Workshop) is not the Dream proper
        CHECK(ClassifyStart(f, true) == StartStep::Ready);
        f.map_id = 0x18010000u;  // the sickroom lamp in Central Yharnam
        CHECK(StartReady(StartMode::PrologueSolo, f, true));
        CHECK(InHuntersDream(0x15000000u) && !InHuntersDream(0x18010000u) && !InHuntersDream(0xffffffffu));
    }
    CHECK(std::string(StartStepName(StartStep::Clinic)) == "clinic");
}

static void test_ready() {
    const StartMode solo = StartMode::PrologueSolo, now = StartMode::Immediate;
    // prologue_solo: only at the end of the prologue, in the world, no cutscene.
    CHECK(!StartReady(solo, Flags(false, false, false, false), true));
    CHECK(!StartReady(solo, Flags(true, false, false, false), true)); // clinic: no summons
    CHECK(!StartReady(solo, Flags(true, true, true, false), true));   // one weapon
    CHECK(StartReady(solo, Flags(true, true, true, true), true));
    CHECK(!StartReady(solo, Flags(true, true, true, true), false));       // title / loading
    CHECK(!StartReady(solo, Flags(true, true, true, true, true), true));  // 9180: cutscene
    CHECK(!StartReady(solo, StartFlags{}, true));                         // unreadable
    // immediate: from the end of the opening cutscene on.
    CHECK(!StartReady(now, Flags(false, false, false, false), true));
    CHECK(!StartReady(now, Flags(false, false, false, false, true), true));
    CHECK(StartReady(now, Flags(true, false, false, false), true));
    CHECK(!StartReady(now, Flags(true, false, false, false, true), true));
    CHECK(StartReady(now, Flags(true, true, true, true), true));
    CHECK(!StartReady(now, StartFlags{}, true));
    // The log line names the step and the flags.
    const std::string d = DescribeStart(Flags(true, false, false, false), true);
    CHECK(d.find("step clinic") == 0 && d.find("12410000 1") != std::string::npos &&
          d.find("9401 0") != std::string::npos && d.find("goods 200 x0") != std::string::npos);
    CHECK(DescribeStart(StartFlags{}, true).find("unreadable") != std::string::npos);
}

static void test_grants() {
    StartFlags f = Flags(true, true, true, true);
    BellGrant g = PlanBellGrant(f); // a fresh character: neither bell
    CHECK(g.beckoning_lot && !g.beckoning_goods && g.resonant && g.any());
    f.beckoning_lot = true; // lot flag set, but the bell is gone
    g = PlanBellGrant(f);
    CHECK(!g.beckoning_lot && g.beckoning_goods && g.resonant);
    f.beckoning_count = 1;
    f.resonant_count = 1;
    CHECK(!PlanBellGrant(f).any()); // owns both
    f.beckoning_count = -1;         // inventory unreadable: nothing
    f.resonant_count = -1;
    CHECK(!PlanBellGrant(f).any());
    CHECK(!PlanBellGrant(StartFlags{}).any());
}

static party::RosterEntry Member(int slot, party::MemberState st, bool connected = true) {
    party::RosterEntry e;
    e.slot = slot;
    e.name = "m" + std::to_string(slot);
    e.state = st;
    e.connected = connected;
    return e;
}

static void test_lobby() {
    using MS = party::MemberState;
    CHECK(OwnWorldState(true) == MS::Home);
    CHECK(OwnWorldState(false) == MS::Prologue);

    const std::vector<party::RosterEntry> host_only = {Member(0, MS::Home)};
    const std::vector<party::RosterEntry> guest_prologue = {Member(0, MS::Home), Member(1, MS::Prologue)};
    const std::vector<party::RosterEntry> guest_ready = {Member(0, MS::Home), Member(1, MS::Home)};
    const std::vector<party::RosterEntry> guest_joining = {Member(0, MS::Home), Member(1, MS::Joining)};
    const std::vector<party::RosterEntry> guest_lost = {Member(0, MS::Home), Member(1, MS::Home, false)};
    const std::vector<party::RosterEntry> guest_in = {Member(0, MS::Home), Member(1, MS::InHostWorld)};
    const std::vector<party::RosterEntry> mixed = {Member(0, MS::Home), Member(1, MS::InHostWorld),
                                                   Member(2, MS::Prologue), Member(3, MS::Home)};

    CHECK(!MemberWaits(host_only));
    CHECK(!MemberWaits(guest_prologue)); // still in its prologue: not summoned
    CHECK(MemberWaits(guest_ready));
    CHECK(MemberWaits(guest_joining));
    CHECK(!MemberWaits(guest_lost));
    CHECK(!MemberWaits(guest_in));
    CHECK(MemberWaits(mixed));

    // Host: only when itself ready and a ready member waits, with room left.
    CHECK(HostMayRing(true, 0, 3, guest_ready));
    CHECK(!HostMayRing(false, 0, 3, guest_ready)); // host still in its prologue
    CHECK(!HostMayRing(true, 0, 3, guest_prologue));
    CHECK(!HostMayRing(true, -1, 3, guest_ready)); // cooperator count unknown
    CHECK(!HostMayRing(true, 2, 3, guest_ready));  // full
    CHECK(!HostMayRing(true, 1, 4, mixed));  // #19: member 2 still in its prologue: nobody is summoned yet

    // Session roles: the host rings idle or already hosting (2nd / 3rd cooperator), a guest idle only.
    CHECK(SessionMayRing(true, 0));
    CHECK(SessionMayRing(true, 3));
    CHECK(SessionMayRing(false, 0));
    CHECK(!SessionMayRing(false, 3));
    for (int r : {1, 2, 4, 5, 6, 7, -1}) {
        CHECK(!SessionMayRing(true, r));
        CHECK(!SessionMayRing(false, r));
    }
    // A 3-player party: the host with 1 cooperator in rings again for the 2nd; with 2 it stops.
    const std::vector<party::RosterEntry> three = {Member(0, MS::Home), Member(1, MS::InHostWorld),
                                                   Member(2, MS::Home)};
    CHECK(HostMayRing(true, 1, 3, three));
    CHECK(!HostMayRing(true, 2, 3, three));
    // 4 players: up to 3 cooperators.
    const std::vector<party::RosterEntry> four = {Member(0, MS::Home), Member(1, MS::InHostWorld),
                                                  Member(2, MS::InHostWorld), Member(3, MS::Joining)};
    CHECK(HostMayRing(true, 2, 4, four));
    CHECK(!HostMayRing(true, 3, 4, four));
    CHECK(!HostMayRing(true, 1, 2, mixed));
    std::vector<party::RosterEntry> mixed_ready = mixed;
    mixed_ready[2].state = MS::Home;
    CHECK(HostMayRing(true, 1, 4, mixed_ready));
    mixed_ready[2].state = MS::Prologue;
    mixed_ready[2].connected = false;  // a lost member does not hold the party
    CHECK(HostMayRing(true, 1, 4, mixed_ready));
    CHECK(AnyMemberInPrologue(mixed) && !AnyMemberInPrologue(guest_ready));
    CHECK(MembersInPrologue(mixed) == "m2");
    CHECK(MembersInPrologue(mixed, "m2").empty());
    const std::vector<party::RosterEntry> host_prologue = {Member(0, MS::Prologue), Member(1, MS::Home)};
    CHECK(HostInPrologue(host_prologue) && !HostInPrologue(guest_prologue));
    CHECK(MembersInPrologue(host_prologue, "m1") == "m0");

    // Guest: ready and connected.
    CHECK(GuestMayRing(true, true));
    CHECK(!GuestMayRing(false, true));
    CHECK(!GuestMayRing(true, false));
    CHECK(!GuestMayRing(true, true, false));  // #19: the host (or another member) in its prologue

    // Two members finishing the prologue at different times: the host rings once both are ready.
    StartFlags host = Flags(true, true, true, true), guest = Flags(true, false, false, false);
    const StartMode solo = StartMode::PrologueSolo;
    std::vector<party::RosterEntry> roster = {Member(0, OwnWorldState(StartReady(solo, host, true))),
                                              Member(1, OwnWorldState(StartReady(solo, guest, true)))};
    CHECK(!HostMayRing(StartReady(solo, host, true), 0, 2, roster));
    CHECK(!GuestMayRing(StartReady(solo, guest, true), true));
    guest = Flags(true, true, true, true);
    roster[1].state = OwnWorldState(StartReady(solo, guest, true));
    CHECK(HostMayRing(StartReady(solo, host, true), 0, 2, roster));
    CHECK(GuestMayRing(StartReady(solo, guest, true), true, !AnyMemberInPrologue(roster)));

    // #19, both orders. Guest first: the guest is ready (out of the Dream), the host still in its
    // clinic: the guest neither rings nor gathers, the host does not ring; then the host finishes.
    for (int order = 0; order < 2; ++order) {
        StartFlags h = Flags(true, false, false, false), g = Flags(true, false, false, false);
        StartFlags& first = order == 0 ? g : h;
        StartFlags& second = order == 0 ? h : g;
        first = Flags(true, true, true, true);
        std::vector<party::RosterEntry> r = {Member(0, OwnWorldState(StartReady(solo, h, true))),
                                             Member(1, OwnWorldState(StartReady(solo, g, true)))};
        CHECK(!HostMayRing(StartReady(solo, h, true), 0, 2, r));
        CHECK(!GuestMayRing(StartReady(solo, g, true), true, !AnyMemberInPrologue(r)));
        // The second one takes its weapons in the Dream: still the prologue (the #19 case).
        second = Flags(true, true, true, true);
        second.map_id = 0x15000000u;
        r[0].state = OwnWorldState(StartReady(solo, h, true));
        r[1].state = OwnWorldState(StartReady(solo, g, true));
        CHECK(AnyMemberInPrologue(r));
        CHECK(!HostMayRing(StartReady(solo, h, true), 0, 2, r));
        CHECK(!GuestMayRing(StartReady(solo, g, true), true, !AnyMemberInPrologue(r)));
        // The headstone: out of the Dream. Now both ring.
        second.map_id = 0x18010000u;
        r[0].state = OwnWorldState(StartReady(solo, h, true));
        r[1].state = OwnWorldState(StartReady(solo, g, true));
        CHECK(!AnyMemberInPrologue(r));
        CHECK(HostMayRing(StartReady(solo, h, true), 0, 2, r));
        CHECK(GuestMayRing(StartReady(solo, g, true), true, !AnyMemberInPrologue(r)));
    }
}

static StartFlags Fresh() {
    StartFlags f;
    f.flags_ok = true;
    f.opening_done = true; // in the clinic after the opening
    return f;
}

static void test_catchup() {
    // A fresh guest, host past the prologue: steady 3 s, apply, warp, load, done.
    Catchup c;
    CatchupInput in;
    in.guest = true;
    in.host_known = true;
    in.host_ready = true;
    in.in_world = true;
    in.own_idle = true;
    in.flags = Fresh();
    in.t = 10;
    CHECK(c.Step(in).kind == CatchupAction::None);
    in.t = 12;
    CHECK(c.Step(in).kind == CatchupAction::None);
    in.t = 13.1;
    CatchupAction a = c.Step(in);
    CHECK(a.kind == CatchupAction::ApplyStart && c.Busy());
    in.t = 13.5;
    CHECK(c.Step(in).kind == CatchupAction::None);
    in.t = 14.2;
    CHECK(c.Step(in).kind == CatchupAction::Warp);
    in.in_world = false; // loading
    in.t = 15;
    CHECK(c.Step(in).kind == CatchupAction::None && c.Busy());
    in.in_world = true;
    in.t = 20;
    CHECK(c.Step(in).kind == CatchupAction::None); // steady again from 20
    in.t = 23.5;
    CHECK(c.Step(in).kind == CatchupAction::Finished && !c.Busy() && c.Applied());
    CHECK(c.Step(in).kind == CatchupAction::None);

    // Ready already: nothing applied.
    Catchup r;
    in = CatchupInput{};
    in.guest = in.host_known = in.host_ready = in.in_world = in.own_idle = true;
    in.flags = Fresh();
    in.flags.first_dream = in.flags.weapon_left = in.flags.weapon_right = true;
    in.flags.map_id = 0x18010000u;  // out of the first Dream (#19)
    in.t = 1;
    CHECK(r.Step(in).kind == CatchupAction::Finished && !r.Applied());

    // Never: not a guest, no host snapshot, or the host itself in its prologue.
    for (int k = 0; k < 3; ++k) {
        Catchup n;
        CatchupInput x;
        x.guest = k != 0;
        x.host_known = k != 1;
        x.host_ready = k != 2;
        x.in_world = x.own_idle = true;
        x.flags = Fresh();
        for (int i = 0; i < 100; ++i) {
            x.t = i;
            CHECK(n.Step(x).kind == CatchupAction::None);
        }
        CHECK(n.phase() == Catchup::Phase::Idle);
    }

    // Mid-opening: skips every kSkipEvery s while the cutscene plays, then the catch-up.
    Catchup o;
    in = CatchupInput{};
    in.guest = in.host_known = in.host_ready = in.in_world = in.own_idle = true;
    in.flags.flags_ok = true;
    in.flags.cutscene = true; // 12410000 OFF, 9180 ON
    in.remo_playing = true;
    in.t = 0;
    CHECK(o.Step(in).kind == CatchupAction::SkipOpening);
    in.t = 0.5;
    CHECK(o.Step(in).kind == CatchupAction::None);
    // C2's snapshot set 12410000 while the remo still plays: still skipping, no apply yet.
    in.flags.opening_done = true;
    in.t = 1.6;
    CHECK(o.Step(in).kind == CatchupAction::SkipOpening && !o.Busy());
    in.flags.opening_done = false;
    in.t = 4.5;
    CHECK(o.Step(in).kind == CatchupAction::SkipOpening);
    in.flags.opening_done = true;
    in.flags.cutscene = false;
    in.remo_playing = false;
    in.t = 5; // the world restarts its steady clock? still idle in the own world: counted from 0
    a = o.Step(in);
    CHECK(a.kind == CatchupAction::ApplyStart);
    // Not idle in the own world (summoned / joining): never applies.
    Catchup s;
    in = CatchupInput{};
    in.guest = in.host_known = in.host_ready = in.in_world = true;
    in.own_idle = false;
    in.flags = Fresh();
    for (int i = 0; i < 50; ++i) {
        in.t = i;
        CHECK(s.Step(in).kind == CatchupAction::None);
    }
}

static void test_travel_audience() {
    using MS = party::MemberState;
    // Both in their prologues (the 2-PC bug: the host's prologue death sent the guest home).
    CHECK(!HostTravelHasAudience(false, {Member(0, MS::Prologue), Member(1, MS::Prologue)}));
    CHECK(!HostTravelHasAudience(false, {Member(0, MS::Home), Member(1, MS::Home)}));    // own world, ready
    CHECK(!HostTravelHasAudience(false, {Member(0, MS::Home), Member(1, MS::Loading)})); // its own load
    CHECK(!HostTravelHasAudience(false, {Member(0, MS::Home), Member(1, MS::InHostWorld, false)}));
    CHECK(HostTravelHasAudience(false, {Member(0, MS::Home), Member(1, MS::InHostWorld)}));
    CHECK(HostTravelHasAudience(false, {Member(0, MS::Home), Member(1, MS::Joining)}));   // on its way in
    CHECK(HostTravelHasAudience(true, {Member(0, MS::Home), Member(1, MS::Loading)}));   // hosting
    CHECK(HostTravelHasAudience(true, {Member(0, MS::Home)}));
    CHECK(HostTravelHasAudience(false, {Member(0, MS::Home), Member(1, MS::Prologue), Member(2, MS::InHostWorld)}));
}

// Playtest 1 save audit S5: party_prologue.txt per save slot.
static void test_saved_prologue() {
    // An old unkeyed file: its value is the "last" one, for every slot (as before).
    SavedPrologueFile old = ParseSavedPrologue("1\n");
    CHECK(old.last == 1 && old.slots.empty());
    CHECK(SavedPrologueFor(old, 0) && SavedPrologueFor(old, -1));
    CHECK(!SavedPrologueFor(ParseSavedPrologue("0\n"), 3));
    CHECK(!SavedPrologueFor(ParseSavedPrologue(""), 0));  // no file: not in the prologue
    // Slot 1 in its prologue, then slot 0 past it: each keeps its own.
    SavedPrologueFile f;
    CHECK(NoteSavedPrologueFor(f, 1, true));
    CHECK(!NoteSavedPrologueFor(f, 1, true));  // unchanged: no write
    CHECK(NoteSavedPrologueFor(f, 0, false));
    CHECK(SavedPrologueFor(f, 1) && !SavedPrologueFor(f, 0));
    CHECK(!SavedPrologueFor(f, -1));          // unknown slot: the last seen (slot 0)
    CHECK(!SavedPrologueFor(f, 5));           // never seen: the last seen
    // Round trip keeps "last" even when a slot line says otherwise.
    const SavedPrologueFile g = ParseSavedPrologue(FormatSavedPrologue(f));
    CHECK(g.last == 0 && g.slots.size() == 2 && SavedPrologueFor(g, 1) && !SavedPrologueFor(g, 0));
    // The audit's case: slot 1 still marked, slot 0 continued: no title join hold.
    CHECK(!SavedPrologueFor(ParseSavedPrologue("last 1\nslot 1 1\nslot 0 0\n"), 0));
    // Junk lines and out-of-range slots are ignored.
    const SavedPrologueFile j = ParseSavedPrologue("slot 12 1\nfoo\nslot -3 1\nlast 0\n");
    CHECK(j.slots.empty() && j.last == 0);
    // An unknown slot only moves "last".
    SavedPrologueFile u;
    CHECK(NoteSavedPrologueFor(u, -1, true) && u.slots.empty() && u.last == 1);
}

int main() {
    CHECK(!ShouldAutoContinue(-1, false));
    CHECK(ShouldAutoContinue(-1, true));
    CHECK(!ShouldAutoContinue(0, false));
    CHECK(!ShouldAutoContinue(0, true));
    CHECK(ShouldAutoContinue(1, false));
    CHECK(ShouldAutoContinue(1, true));
    test_saved_prologue();
    test_travel_audience();
    test_mode();
    test_steps();
    test_ready();
    test_grants();
    test_lobby();
    test_catchup();
    std::printf("party-start-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
