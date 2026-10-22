// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel (gpu/shim/party/party_travel, phase B1), the parts without the game: the funnel
// caller -> kind table, the EVENT "travel" JSON round trip, the host queue and the guest's
// newest-seq / replay-gate / under-way logic with an injected game state.
// Build and run: ninja -C out/gpu party-travel-test && out/gpu/party-travel-test.exe
#include "party/party_chalice.h"
#include "party/party_travel.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

static void test_call_sites() {
    // travel.md 1.6 (return address - 5).
    CHECK(TravelKindFromCallSite(0x13ce002) == TravelKind::Lamp);
    CHECK(TravelKindFromCallSite(0x17c0bed) == TravelKind::ScriptedWarp);
    CHECK(TravelKindFromCallSite(0x132e03e) == TravelKind::LuaStageWarp);
    CHECK(TravelKindFromCallSite(0x132e0b4) == TravelKind::LuaBonfireWarp);
    CHECK(TravelKindFromCallSite(0x138c5f0) == TravelKind::LuaBonfireWarp);
    CHECK(TravelKindFromCallSite(0x1332b84) == TravelKind::LuaStageKick);
    CHECK(TravelKindFromCallSite(0x1381ede) == TravelKind::HostDeath);
    CHECK(TravelKindFromCallSite(0x18ffa1d) == TravelKind::HuntersMark); // the Hunter's Mark item
    // Gather target: the host's lamp when it is in the host's map, else a lamp of that map.
    CHECK(GatherLampFor(2412951, 0x18010000) == 2412951);
    CHECK(GatherLampFor(2412959, 0x15000000) / 10000 == 210); // fresh host in the Dream: a Dream lamp
    CHECK(GatherLampFor(2412959, 0) == 2412959);
    CHECK(GatherLampFor(kTravelNone, 0x18000000) / 10000 == 240);
    CHECK(GatherLampFor(2412951, 0x7f000000) == 2412951); // no lamp in that map: as it is
    CHECK(TravelKindFromCallSite(0x1381a93) == TravelKind::GuestSentHome);
    CHECK(TravelKindFromCallSite(0x138a51c) == TravelKind::GuestSentHome);
    CHECK(TravelKindFromCallSite(0x1382663) == TravelKind::GuestDied);
    CHECK(TravelKindFromCallSite(0x13856f4) == TravelKind::BossCleared);
    CHECK(TravelKindFromCallSite(0x138b513) == TravelKind::MissionOrPk);
    CHECK(TravelKindFromCallSite(0x132e11e) == TravelKind::TitleOrDebug);
    CHECK(TravelKindFromCallSite(0x1317632) == TravelKind::TitleOrDebug);
    CHECK(TravelKindFromCallSite(0x13ce007) == TravelKind::Unknown); // the return address itself
    CHECK(TravelKindFromCallSite(0) == TravelKind::Unknown);

    CHECK(TravelKindBroadcast(TravelKind::Lamp));
    CHECK(TravelKindBroadcast(TravelKind::ScriptedWarp));
    CHECK(TravelKindBroadcast(TravelKind::HostDeath));
    CHECK(TravelKindBroadcast(TravelKind::HuntersMark));
    CHECK(!TravelKindBroadcast(TravelKind::GuestSentHome));
    CHECK(!TravelKindBroadcast(TravelKind::BossCleared));
    CHECK(!TravelKindBroadcast(TravelKind::TitleOrDebug));
    CHECK(!TravelKindBroadcast(TravelKind::Unknown));

    for (int k = 0; k <= static_cast<int>(TravelKind::TitleOrDebug); ++k) {
        const TravelKind kind = static_cast<TravelKind>(k);
        CHECK(TravelKindFromName(TravelKindName(kind)) == kind);
    }
    CHECK(TravelKindFromName("nonsense") == TravelKind::Unknown);
    // Player-facing labels (pt1polish: the loading screen said "Travelling: host_death").
    CHECK(std::string(TravelKindLabel(TravelKind::HostDeath)) == "Respawning with the party");
    CHECK(std::string(TravelKindLabel(TravelKind::Transform)) == "Travelling with the party");
    for (int k = 0; k <= static_cast<int>(TravelKind::TitleOrDebug); ++k) {
        const std::string label = TravelKindLabel(static_cast<TravelKind>(k));
        CHECK(!label.empty() && label.find('_') == std::string::npos);  // never an internal name
    }
}

static TravelIntent lamp_intent(std::uint64_t seq) {
    TravelIntent t;
    t.seq = seq;
    t.kind = TravelKind::Lamp;
    t.call_site = 0x13ce002;
    t.lamp_id = 2412952; // Great Bridge (shadp2p capture)
    t.packed_map = 0x18010000;
    t.warp_point = 0xffffffffu;
    t.respawn_mode = 1;
    t.respawn_record = (101163ull << 32) | 2412952ull;
    t.last_lamp = (101163ull << 32) | 2412952ull;
    return t;
}

static void test_json() {
    TravelIntent t = lamp_intent(1728550000123ull);
    std::string text = TravelToJsonText(t);
    TravelIntent back;
    std::string err;
    CHECK(TravelFromJsonText(text, &back, &err));
    CHECK(back.seq == t.seq);
    CHECK(back.kind == t.kind);
    CHECK(back.call_site == t.call_site);
    CHECK(back.lamp_id == t.lamp_id);
    CHECK(back.packed_map == t.packed_map);
    CHECK(back.warp_point == t.warp_point);
    CHECK(back.respawn_mode == t.respawn_mode);
    CHECK(back.respawn_record == t.respawn_record);
    CHECK(back.last_lamp == t.last_lamp);
    CHECK(!back.has_pos);

    // Exact transform: the float bits survive (-0.0, a NaN-free odd value, a large one).
    TravelIntent p;
    p.seq = 0xfedcba9876543210ull; // beyond 2^53: must travel as hex
    p.kind = TravelKind::Transform;
    p.has_pos = true;
    p.pos_map = 0x18010000;
    const float pos[4] = {-0.0f, 123.456789f, -3.0e7f, 1.0f};
    const float rot[4] = {0.0f, 0.70710677f, 0.0f, 0.70710677f};
    std::memcpy(p.pos, pos, sizeof pos);
    std::memcpy(p.rot, rot, sizeof rot);
    CHECK(TravelFromJsonText(TravelToJsonText(p), &back, &err));
    CHECK(back.seq == p.seq);
    CHECK(back.kind == TravelKind::Transform);
    CHECK(back.has_pos && back.pos_map == p.pos_map);
    CHECK(std::memcmp(back.pos, p.pos, sizeof p.pos) == 0);
    CHECK(std::memcmp(back.rot, p.rot, sizeof p.rot) == 0);
    CHECK(std::signbit(back.pos[0]));

    // Malformed payloads leave *out alone.
    TravelIntent untouched = lamp_intent(7);
    CHECK(!TravelFromJsonText("{\"seq\":\"0x5\"}", &untouched, &err)); // members missing
    CHECK(!err.empty());
    CHECK(!TravelFromJsonText("not json", &untouched, &err));
    CHECK(!TravelFromJsonText("[1,2]", &untouched, &err));
    TravelIntent zero = lamp_intent(1);
    zero.seq = 0;
    CHECK(!TravelFromJsonText(TravelToJsonText(zero), &untouched, &err)); // seq 0 is never sent
    CHECK(untouched.seq == 7);
}

static void test_replay_plan() {
    TravelIntent t = lamp_intent(1);
    ReplayPlan p = ChooseReplay(t);
    CHECK(p.method == ReplayMethod::LampWarp && p.id == 2412952u);
    t.lamp_id = kTravelNone; // no key seen: the stored record's id
    p = ChooseReplay(t);
    CHECK(p.method == ReplayMethod::LampWarp && p.id == 2412952u);
    t.respawn_record = ~0ull;
    CHECK(ChooseReplay(t).method == ReplayMethod::None);

    TravelIntent d;
    d.kind = TravelKind::HostDeath;
    d.last_lamp = (5ull << 32) | 2102950ull;
    p = ChooseReplay(d);
    CHECK(p.method == ReplayMethod::LampWarp && p.id == 2102950u);
    d.kind = TravelKind::HuntersMark;
    CHECK(ChooseReplay(d).method == ReplayMethod::LampWarp);
    d.last_lamp = ~0ull;
    CHECK(ChooseReplay(d).method == ReplayMethod::None);
    d.has_pos = true; // the exact transform is the fallback
    d.pos_map = 0x18010000;
    CHECK(ChooseReplay(d).method == ReplayMethod::Transform);

    TravelIntent s;
    s.kind = TravelKind::ScriptedWarp;
    s.packed_map = 0x1c000000; // m28 (Hypogean Gaol)
    s.warp_point = 2800960;
    CHECK(ChooseReplay(s).method == ReplayMethod::StageWarp);
    CHECK(s.Area() == 28 && s.Block() == 0 && s.Region() == 0 && s.Index() == 0);
    s.packed_map = 0x18020301;
    CHECK(s.Area() == 24 && s.Block() == 2 && s.Region() == 3 && s.Index() == 1);
    TravelIntent b;
    b.kind = TravelKind::LuaBonfireWarp;
    b.warp_point = 2412951;
    p = ChooseReplay(b);
    CHECK(p.method == ReplayMethod::BonfireWarp && p.id == 2412951u);
    TravelIntent u;
    CHECK(ChooseReplay(u).method == ReplayMethod::None);
    u.kind = TravelKind::GuestSentHome;
    CHECK(ChooseReplay(u).method == ReplayMethod::None);
}

static void test_host_queue() {
    HostTravelQueue q;
    TravelIntent out;
    CHECK(!q.Pop(&out));
    CHECK(q.Push(lamp_intent(10)));
    CHECK(!q.Push(lamp_intent(10))); // duplicate seq
    CHECK(!q.Push(lamp_intent(9)));  // older
    CHECK(q.Push(lamp_intent(11)));
    CHECK(q.Size() == 2);
    CHECK(q.Pop(&out) && out.seq == 10);
    CHECK(q.Pop(&out) && out.seq == 11);
    CHECK(!q.Pop(&out));
    for (std::uint64_t s = 100; s < 100 + HostTravelQueue::kMax + 5; ++s) {
        CHECK(q.Push(lamp_intent(s)));
    }
    CHECK(q.Size() == HostTravelQueue::kMax);
    CHECK(q.Pop(&out) && out.seq == 105); // the oldest five were dropped
}

static ReplayState in_world() {
    ReplayState s;
    s.world_up = true;
    s.loading = false;
    s.transition_requested = false;
    s.session_role = 6; // client in the host's world
    return s;
}

static void test_gate() {
    CHECK(ReplayAllowedNow(in_world()));
    ReplayState s = in_world();
    s.loading = true;
    CHECK(!ReplayAllowedNow(s));
    s = in_world();
    s.world_up = false;
    CHECK(!ReplayAllowedNow(s));
    s = in_world();
    s.transition_requested = true;
    CHECK(!ReplayAllowedNow(s));
    s = in_world();
    s.session_role = 4;
    CHECK(!ReplayAllowedNow(s));
    s.session_role = 7;
    CHECK(!ReplayAllowedNow(s));
    s.session_role = 0;
    CHECK(ReplayAllowedNow(s));
    s.session_role = 3;
    CHECK(ReplayAllowedNow(s));
}

static int ticks_until_replay(GuestTravel& g, const ReplayState& s, double& now, int limit) {
    for (int i = 1; i <= limit; ++i) {
        now += 1.0 / 60;
        if (g.Step(s, now)) {
            return i;
        }
    }
    return -1;
}

static void test_guest() {
    double now = 1000.0;
    GuestTravel g;
    CHECK(g.GetPhase() == GuestTravel::Phase::Idle);
    CHECK(!g.Destination());
    CHECK(!g.Step(in_world(), now));

    // Loading: nothing is replayed however long it takes.
    CHECK(g.Offer(lamp_intent(5), now));
    CHECK(!g.Offer(lamp_intent(5), now)); // duplicate
    CHECK(!g.Offer(lamp_intent(4), now)); // older
    CHECK(g.GetPhase() == GuestTravel::Phase::Pending);
    CHECK(g.Destination() && g.Destination()->seq == 5);
    ReplayState loading = in_world();
    loading.loading = true;
    for (int i = 0; i < 300; ++i) {
        now += 1.0 / 60;
        CHECK(!g.Step(loading, now));
    }
    // A newer intent while loading replaces the older one.
    CHECK(g.Offer(lamp_intent(6), now));
    // World up: replayed after exactly kStableTicks steady frames, once.
    const int n = ticks_until_replay(g, in_world(), now, 200);
    CHECK(n == GuestTravel::kStableTicks);
    CHECK(g.GetPhase() == GuestTravel::Phase::Underway);
    CHECK(g.Destination() && g.Destination()->seq == 6); // the send-home retarget still sees it
    CHECK(ticks_until_replay(g, in_world(), now, 100) == -1);

    // A steady gate blip mid-count restarts the count.
    GuestTravel h;
    CHECK(h.Offer(lamp_intent(1), now));
    for (int i = 0; i < GuestTravel::kStableTicks - 1; ++i) {
        now += 1.0 / 60;
        CHECK(!h.Step(in_world(), now));
    }
    ReplayState pending = in_world();
    pending.transition_requested = true;
    CHECK(!h.Step(pending, now));
    CHECK(ticks_until_replay(h, in_world(), now, 200) == GuestTravel::kStableTicks);

    // Underway -> load seen -> steady again: arrived (Idle, no destination).
    for (int i = 0; i < 10; ++i) {
        now += 1.0 / 60;
        CHECK(!g.Step(loading, now));
    }
    CHECK(g.GetPhase() == GuestTravel::Phase::Underway);
    for (int i = 0; i < GuestTravel::kStableTicks; ++i) {
        now += 1.0 / 60;
        CHECK(!g.Step(in_world(), now));
    }
    CHECK(g.GetPhase() == GuestTravel::Phase::Idle);
    CHECK(!g.Destination());

    // The send-home retarget replays it first: Step does not replay a second time.
    GuestTravel r;
    CHECK(r.Offer(lamp_intent(20), now));
    r.MarkReplayed(now);
    CHECK(r.GetPhase() == GuestTravel::Phase::Underway);
    CHECK(ticks_until_replay(r, in_world(), now, 100) == -1);
    // Underway with no load ever: times out.
    now += GuestTravel::kActiveTimeout + 1;
    CHECK(!r.Step(in_world(), now));
    CHECK(r.GetPhase() == GuestTravel::Phase::Idle);
    CHECK(!r.Offer(lamp_intent(20), now)); // still a duplicate afterwards
    CHECK(r.Offer(lamp_intent(21), now));

    // Pending forever (never in the world): dropped after kPendingTimeout.
    GuestTravel p;
    CHECK(p.Offer(lamp_intent(1), now));
    now += GuestTravel::kPendingTimeout + 1;
    CHECK(!p.Step(loading, now));
    CHECK(p.GetPhase() == GuestTravel::Phase::Idle);
    CHECK(!p.Destination());
}

// Chalice Dungeons: a guest follows a warp into a dungeon only into the one its game has.
static void test_chalice() {
    TravelIntent t;
    t.seq = 9;
    t.kind = TravelKind::ScriptedWarp;
    t.packed_map = 0x18010000u;  // m24_01: not a dungeon
    CHECK(!TravelIntoChalice(t) && ChaliceReplayAllowed(t, 0));
    t.packed_map = 0x1D010A0Cu;  // m29_01_10_12: a dungeon piece
    CHECK(TravelIntoChalice(t));
    CHECK(!ChaliceReplayAllowed(t, 0) && !ChaliceReplayAllowed(t, 77));
    t.chalice_channel = 77;
    CHECK(ChaliceReplayAllowed(t, 77) && !ChaliceReplayAllowed(t, 78) && !ChaliceReplayAllowed(t, 0));
    // The channel travels in the event (hex) and is optional.
    TravelIntent back;
    CHECK(TravelFromJsonText(TravelToJsonText(t), &back) && back.chalice_channel == 77);
    t.chalice_channel = 0;
    CHECK(TravelFromJsonText(TravelToJsonText(t), &back) && back.chalice_channel == 0);
    // Split travel: "alone" is optional (absent = the party travels together) and round-trips.
    CHECK(!back.alone && TravelToJsonText(t).find("alone") == std::string::npos);
    t.alone = true;
    CHECK(TravelFromJsonText(TravelToJsonText(t), &back) && back.alone);
    t.alone = false;
    // Lamps 29xxxxx, a dungeon respawn, an exact position in area 29.
    TravelIntent l;
    l.kind = TravelKind::Lamp;
    l.lamp_id = 2902950;
    CHECK(TravelIntoChalice(l));
    l.lamp_id = 2102950;
    CHECK(!TravelIntoChalice(l));
    l.kind = TravelKind::HostDeath;
    l.lamp_id = kTravelNone;
    l.last_lamp = 2902951;
    CHECK(TravelIntoChalice(l));
    TravelIntent p;
    p.kind = TravelKind::Transform;
    p.has_pos = true;
    p.pos_map = 0x1D000000u;
    CHECK(TravelIntoChalice(p));
}


// The local chalice slot and the party's chalices (party_chalice.h).
static void test_chalice_state() {
    using namespace coop::chalice;
    CHECK(SlotFromBits(0) == -1);
    CHECK(SlotFromBits(0x40) == 0);  // flag 9020 (the first of 7, the value's bit 6)
    CHECK(SlotFromBits(0x01) == 6);  // flag 9026
    CHECK(SlotFromBits(0x04) == 4);
    // The current-slot read while summoned into the host's dungeon (quick-joined slot 0, own slot 1).
    for (int k = 0; k < kSlots; ++k) CHECK(SlotFromBits(BitsForSlot(k)) == k);
    CHECK(BitsForSlot(-1) == 0 && BitsForSlot(7) == 0);
    CHECK(InDungeonSlotBits(0x20, 0) == 0x40);   // own slot 1 -> the joined slot 0 (the host's dungeon)
    CHECK(InDungeonSlotBits(0x40, 0) == 0x40);   // already the joined slot
    CHECK(InDungeonSlotBits(0x20, -1) == 0x20);  // no quick join carried (home, the host): own slot
    CHECK(InDungeonSlotBits(0, -1) == 0 && InDungeonSlotBits(0, 0) == 0x40);
    CHECK(InDungeonSlotBits(0x20, 9) == 0x20);
    CHECK(ChannelFromDwords(0, 0x80000000u) == 0);  // the game's "never uploaded"
    CHECK(ChannelFromDwords(0x12345678u, 0x9u) == 0x912345678ull);
    CHECK(IsChaliceGoods(6109) && IsChaliceGoods(6539) && !IsChaliceGoods(7000) && !IsChaliceGoods(200));
    std::vector<std::uint32_t> ids;
    CHECK(GoodsFromJsonText(GoodsToJsonText({6109, 6209}), &ids) && ids == (std::vector<std::uint32_t>{6109, 6209}));
    // A peer's list: only chalices, once each.
    CHECK(GoodsFromJsonText(R"({"goods":[6109,6109,200,7000,-5,1e300,"x",6209]})", &ids) &&
          ids == (std::vector<std::uint32_t>{6109, 6209}));
    // Root Chalices are used up by their ritual: never shared (a used one would come back), also
    // not when an older peer still sends them; every fixed chalice (id ending in 9) is.
    for (std::uint32_t root : {6100u, 6200u, 6210u, 6300u, 6302u, 6310u, 6312u, 6401u, 6420u, 6500u, 6502u, 6520u,
                               6522u, 6530u, 6532u})
        CHECK(!IsChaliceGoods(root));
    for (std::uint32_t fixed : {6109u, 6209u, 6219u, 6309u, 6319u, 6409u, 6429u, 6509u, 6529u, 6539u})
        CHECK(IsChaliceGoods(fixed) && fixed % 10 == 9);
    CHECK(GoodsFromJsonText(R"({"goods":[6100,6309,6532,6539]})", &ids) &&
          ids == (std::vector<std::uint32_t>{6309, 6539}));
    CHECK(!GoodsFromJsonText("[]", &ids) && !GoodsFromJsonText(R"({"goods":5})", &ids));
}

static void test_follow() {
    auto at = [](bool ready, int role, double ago) {
        FollowState f;
        f.start_ready = ready;
        f.session_role = role;
        f.client_ago = ago;
        return GuestFollowsHostTravel(f);
    };
    // Summoned / joining: follow (also while the flags read as not ready in the host's world).
    CHECK(at(true, 6, 0) == TravelFollow::Follow);
    CHECK(at(false, 6, -1) == TravelFollow::Follow);
    CHECK(at(true, 4, -1) == TravelFollow::Follow);
    // Sent home by the travel itself / a B2 fallback moments ago: follow.
    CHECK(at(true, 0, 0.5) == TravelFollow::Follow);
    CHECK(at(true, 7, 10) == TravelFollow::Follow);
    CHECK(at(true, 0, kFollowClientWindow) == TravelFollow::Follow);
    // The 2-PC bug: a guest in its own prologue / Dream, never summoned.
    CHECK(at(false, 0, -1) == TravelFollow::IgnorePrologue);
    CHECK(at(false, -1, -1) == TravelFollow::IgnorePrologue);
    // Ready but alone in its own world (not summoned, or summoned long ago; crash restart).
    CHECK(at(true, 0, -1) == TravelFollow::IgnoreOwnWorld);
    CHECK(at(true, 0, kFollowClientWindow + 1) == TravelFollow::IgnoreOwnWorld);
    CHECK(at(true, 3, -1) == TravelFollow::IgnoreOwnWorld); // hosting its own session
    CHECK(std::strcmp(TravelFollowName(TravelFollow::IgnorePrologue), "in own world (prologue)") == 0);
}

// Batch 6 (H1:4389, H3:2654): the empty-room check after a host travel.
static void test_empty_room() {
    EmptyRoomView v;
    CHECK(PlanEmptyRoom(v) == EmptyRoomStep::None);  // no travel
    v.since_travel = 5;
    CHECK(PlanEmptyRoom(v) == EmptyRoomStep::None);  // loading screen not seen yet
    v.load_seen = true;
    v.in_world = true;
    CHECK(PlanEmptyRoom(v) == EmptyRoomStep::Check);  // B1: arrived, close the room if empty
    v.since_travel = kEmptyRoomTimeout + 1;
    CHECK(PlanEmptyRoom(v) == EmptyRoomStep::Timeout);
    // B2 holds the room: no timeout ever, the mark is dropped once the re-join is over.
    v.b2_holds = true;
    v.b2_rejoin = true;
    CHECK(PlanEmptyRoom(v) == EmptyRoomStep::None);
    v.b2_rejoin = false;
    CHECK(PlanEmptyRoom(v) == EmptyRoomStep::Drop);
    v.in_world = false;
    CHECK(PlanEmptyRoom(v) == EmptyRoomStep::None);
    // The playtest case: B2 held through the travel (mark dropped at arrival), later the guest
    // restarts (tasks gone): nothing left to time out. Before the fix the mark was kept and the
    // first frame without B2 said "no arrival within 180 s".
    EmptyRoomView after;
    after.since_travel = -1;
    CHECK(PlanEmptyRoom(after) == EmptyRoomStep::None);
    // A guest that fell back to B1 during the load (tasks gone): the B1 close as before.
    EmptyRoomView b1;
    b1.since_travel = 12;
    b1.load_seen = true;
    b1.in_world = true;
    CHECK(PlanEmptyRoom(b1) == EmptyRoomStep::Check);
}

int main() {
    test_empty_room();
    test_follow();
    test_call_sites();
    test_json();
    test_replay_plan();
    test_host_queue();
    test_gate();
    test_guest();
    test_chalice();
    test_chalice_state();
    std::printf("party-travel-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
