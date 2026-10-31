// SPDX-License-Identifier: GPL-3.0-or-later
// Direct summon delivery (gpu/shim/party/party_dsummon): wire format, type-1 payload, decisions.
// Build and run: ninja -C out/gpu party-dsummon-test && out/gpu/party-dsummon-test.exe
#include "party/party_dsummon.h"

#include <cstdio>
#include <cstring>

using namespace coop::dsummon;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    CHECK(SwitchOn(nullptr, true) && !SwitchOn(nullptr, false) && !SwitchOn("0", true) && !SwitchOn("off", true) && SwitchOn("1", false));
    CHECK(KindOf(EncodeReq()) == Kind::Req && KindOf(EncodeDone(true)) == Kind::Done && KindOf("{\"op\":\"title_req\"}") == Kind::None);
    Room r;
    for (int i = 0; i < 209; ++i) r.data.push_back(std::uint8_t(i * 7));
    r.area = 0x18010000; r.pos[0] = -12.5f; r.pos[1] = 3.25f; r.pos[2] = 100.0f; r.level = 42;
    Room d; bool refused = true; std::string err;
    CHECK(DecodeRoom(EncodeRoom(r), &d, &refused, &err) && !refused);
    CHECK(d.data == r.data && d.area == r.area && d.level == 42 && d.pos[0] == -12.5f && d.pos[2] == 100.0f);
    CHECK(!DecodeRoom(EncodeRefuse(), &d, &refused, &err) && refused);
    CHECK(!DecodeRoom("{\"op\":\"dsum_room\",\"room\":\"zz\",\"area\":1,\"pos\":[0,0,0],\"level\":1}", &d, &refused, &err) && !refused);
    CHECK(!DecodeRoom("{\"op\":\"dsum_room\",\"room\":\"00112233445566778899\",\"area\":1,\"pos\":[1e9,0,0],\"level\":1}", &d, &refused, &err));
    CHECK(!DecodeRoom("{\"op\":\"dsum_room\",\"room\":\"0011\",\"area\":1,\"pos\":[0,0,0],\"level\":1}", &d, &refused, &err));
    bool ok = false;
    CHECK(DecodeDone(EncodeDone(true), &ok) && ok && DecodeDone(EncodeDone(false), &ok) && !ok);
    const std::uint8_t id[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto p = BuildPayload(id, r);
    CHECK(p.size() == kHeadSize + 209);
    std::uint32_t u = 0; std::uint64_t q = 0; float f = 0;
    CHECK(std::memcmp(p.data(), id, 8) == 0);
    std::memcpy(&u, &p[0x0c], 4); CHECK(u == 0x18010000);
    std::memcpy(&f, &p[0x10], 4); CHECK(f == -12.5f);
    std::memcpy(&u, &p[0x1c], 4); CHECK(u == 42);
    std::memcpy(&q, &p[0x28], 8); CHECK(q == 0x8000000000000000ull);
    std::memcpy(&u, &p[0x30], 4); CHECK(u == 209);
    CHECK(std::memcmp(&p[kHeadSize], r.data.data(), 209) == 0);
    HostView v; v.world_up = true;
    CHECK(HostRefusal(v) == nullptr);
    v.loading = true; CHECK(HostRefusal(v)); v.loading = false;
    // Batch 6: the host's warp is armed: refused without a wait word (the sign path follows the host).
    v.travel_pending = true; CHECK(HostRefusal(v)); CHECK(RefusalWait(v) == Busy::None);
    v.host_ready = false; CHECK(RefusalWait(v) == Busy::None); v.host_ready = true; v.travel_pending = false;
    v.cooperators = 3; CHECK(HostRefusal(v)); v.cooperators = 1;
    v.ms_since_sign_summon = 5000; CHECK(HostRefusal(v)); v.ms_since_sign_summon = 30000;
    v.other_hold_ms = 100; CHECK(HostRefusal(v)); v.other_hold_ms = 0; CHECK(!HostRefusal(v));
    // A11: a boss fight / cutscene holds every join (direct summon included), with the reason
    CHECK(HostBusy(false, false) == Busy::None && HostBusy(true, false) == Busy::Boss &&
          HostBusy(false, true) == Busy::Cutscene && HostBusy(true, true) == Busy::Boss);
    // Title join (verification replay): the host's prologue refuses it too, with the wait word.
    CHECK(TitleJoinBusy(false, false, true) == Busy::None && TitleJoinBusy(false, false, false) == Busy::Prologue &&
          TitleJoinBusy(true, false, false) == Busy::Boss && TitleJoinBusy(false, true, true) == Busy::Cutscene);
    CHECK(WaitOf(std::string("{\"op\":\"title_room\",\"room\":\"\",\"wait\":\"") + BusyWord(Busy::Prologue) + "\"}") ==
          Busy::Prologue);
    v.busy = Busy::Boss; CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "boss"));
    v.busy = Busy::Cutscene; CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "cutscene"));
    v.busy = Busy::None; CHECK(!HostRefusal(v));
    CHECK(!DecodeRoom(EncodeRefuse(Busy::Boss), &d, &refused, &err) && refused);
    CHECK(WaitOf(EncodeRefuse(Busy::Boss)) == Busy::Boss && WaitOf(EncodeRefuse()) == Busy::None);
    CHECK(KindOf(EncodeBusy(Busy::Cutscene)) == Kind::Busy && WaitOf(EncodeBusy(Busy::Cutscene)) == Busy::Cutscene);
    CHECK(KindOf(EncodeBusy(Busy::None)) == Kind::Busy && WaitOf(EncodeBusy(Busy::None)) == Busy::None);
    CHECK(BusyText(Busy::Boss, false) == "Waiting: the host is in a boss fight" && BusyText(Busy::None, true).empty());
    // guest: no answer -> failed quietly
    Guest g; OnAsked(g, 0);
    CHECK(Step(g, 1, false, false) == Action::None && g.phase == Phase::Asked);
    CHECK(Step(g, kAnswerWait + 1, false, false) == Action::None && g.phase == Phase::Failed);
    // refused
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, true); CHECK(g.phase == Phase::Failed);
    // late room ignored
    OnRoom(g, 0.2, false); CHECK(g.phase == Phase::Failed);
    // room -> wait for the entry -> deliver -> joined
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, false);
    CHECK(Step(g, 1, false, false) == Action::None);
    CHECK(Step(g, 2, true, false) == Action::Deliver);
    OnDelivered(g, 2, true); CHECK(g.phase == Phase::Delivered);
    CHECK(Step(g, 3, false, false) == Action::None);
    CHECK(Step(g, 4, false, true) == Action::ReportOk && g.phase == Phase::Done);
    // delivered but no join
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false); OnDelivered(g, 1, true);
    CHECK(Step(g, 1 + kJoinWait + 0.1, false, false) == Action::ReportFail && g.phase == Phase::Failed);
    // the entry never comes
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false);
    CHECK(Step(g, kRearmAfter - 0.1, false, false) == Action::None);
    CHECK(Step(g, kRearmAfter + 0.1, false, false) == Action::Rearm && g.rearmed);  // playtest 1 G4: re-ring once
    CHECK(Step(g, kRearmAfter + 1, false, false) == Action::None);                   // the wait starts again
    CHECK(Step(g, kRearmAfter + 0.1 + kEntryWait + 0.1, false, false) == Action::ReportFail);
    CHECK(std::strstr(g.fail_why, "re-ring"));
    // the re-rung bell's entry comes up: delivered
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false);
    CHECK(Step(g, 3, false, false) == Action::Rearm);
    CHECK(Step(g, 5, true, false) == Action::Deliver);
    // a new request may re-ring again
    OnAsked(g, 20); CHECK(!g.rearmed);
    // three refused deliveries
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false);
    for (int i = 0; i < 3; ++i) { CHECK(Step(g, 1, true, false) == Action::Deliver); OnDelivered(g, 1, false); }
    CHECK(Step(g, 1, true, false) == Action::ReportFail);
    // the sign path joined first
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false);
    CHECK(Step(g, 1, true, true) == Action::ReportOk);
    // A5: gather before ringing only when the direct summon cannot carry the guest across maps
    CHECK(GatherFirst(false, Phase::Idle));
    CHECK(!GatherFirst(true, Phase::Idle));
    CHECK(!GatherFirst(true, Phase::Done));
    CHECK(GatherFirst(true, Phase::Failed));
    CHECK(!GatherFirst(true, Phase::Failed, true));   // A11: a wait refusal still carries
    CHECK(GatherFirst(true, Phase::Failed, false));
    CHECK(GatherFirst(false, Phase::Failed, true));
    {
        Guest g;
        OnAsked(g, 0);
        OnRoom(g, 1, true, Busy::Cutscene);
        CHECK(g.phase == Phase::Failed && g.waited && !g.gather);
        OnAsked(g, 2);
        CHECK(!g.waited);
        OnRoom(g, 3, true, Busy::OtherMap);
        CHECK(g.gather && !g.waited);
    }
    // Playtest 1 #1: no direct summon into the host's prologue (the reason reaches the guest)
    v = HostView{}; v.world_up = true;
    v.host_ready = false;
    CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "prologue") && RefusalWait(v) == Busy::Prologue);
    CHECK(WaitOf(EncodeRefuse(RefusalWait(v))) == Busy::Prologue);
    CHECK(BusyText(Busy::Prologue, false) == "Waiting: the host is in its prologue");
    v.host_ready = true; CHECK(!HostRefusal(v) && RefusalWait(v) == Busy::None);
    // ... nor of a guest still in its own prologue
    v.guest_in_prologue = true; CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "own prologue")); v.guest_in_prologue = false;
    // #19: another member still in its prologue: nobody is summoned until everybody is ready
    v.member_in_prologue = true;
    CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "everybody is ready") && RefusalWait(v) == Busy::MemberPrologue);
    CHECK(WaitOf(EncodeRefuse(RefusalWait(v))) == Busy::MemberPrologue);
    CHECK(WaitOf(EncodeRefuse(Busy::Prologue)) == Busy::Prologue);  // "prologue" is not "member_prologue"
    CHECK(BusyText(Busy::MemberPrologue, false) == "Waiting: a party member is in the prologue");
    CHECK(TitleJoinBusy(false, false, true, true) == Busy::MemberPrologue &&
          TitleJoinBusy(false, false, false, true) == Busy::Prologue);
    v.member_in_prologue = false;
    // fp5 (0.7.8): a loading screen right after the room came: the entry wait does not run (no failure at 20 s)
    {
        Guest g;
        OnAsked(g, 10.0);
        OnRoom(g, 10.1, false);
        CHECK(g.phase == Phase::HaveRoom);
        for (double t = 10.5; t < 30.0; t += 0.5) {
            HoldWhileLoading(g, t, true);
            CHECK(Step(g, t, false, false) != Action::ReportFail && g.phase == Phase::HaveRoom);
        }
        CHECK(Step(g, 32.1, false, false) == Action::Rearm);     // the load ended: the normal wait begins
        HoldWhileLoading(g, 33.0, false);                         // not loading: the timer is untouched
        CHECK(Step(g, 40.4, false, false) == Action::ReportFail); // and still gives up when the entry never comes
    }
    // fp5 (0.7.8): the cooperator cap follows BB_PARTY_MAX (host included): 5 players take 4 cooperators
    {
        CHECK(MaxCooperators(2) == 1 && MaxCooperators(3) == 2 && MaxCooperators(4) == 3 && MaxCooperators(5) == 4);
        HostView w; w.world_up = true; w.host_ready = true; w.ms_since_sign_summon = 1000000;
        w.cooperators = 3; w.max_cooperators = MaxCooperators(5);
        CHECK(HostRefusal(w) == nullptr);                       // the 4th guest is served at 5 players
        w.cooperators = 4;
        CHECK(HostRefusal(w) && std::strstr(HostRefusal(w), "world is full"));
        w.cooperators = 3; w.max_cooperators = MaxCooperators(4);
        CHECK(HostRefusal(w) && std::strstr(HostRefusal(w), "world is full"));  // still full at 4 players
    }
    // F5: a member left this host's room moments ago and its game is ending that session: the
    // guest asks again (Settle) instead of falling back to the bell sign
    {
        HostView w; w.world_up = true; w.host_ready = true; w.settling = true;
        w.max_cooperators = 3; w.ms_since_sign_summon = 1000000;
        CHECK(HostRefusal(w) && std::strstr(HostRefusal(w), "ending that session") && RefusalWait(w) == Busy::Settle);
        CHECK(WaitOf(EncodeRefuse(Busy::Settle)) == Busy::Settle && BusyText(Busy::Settle, false).empty());
        w.busy = Busy::Boss; CHECK(RefusalWait(w) == Busy::Boss);  // an earlier reason wins
        Guest g;
        OnAsked(g, 10.0);
        OnRoom(g, 10.2, true, Busy::Settle);
        CHECK(g.phase == Phase::Asked && Step(g, 11.0, false, false) == Action::None);
        CHECK(Step(g, 11.8, false, false) == Action::Reask && g.reasks == 1 && g.phase == Phase::Asked);
        CHECK(Step(g, 14.0, false, false) == Action::None && g.phase == Phase::Asked);  // waits for the answer again
        OnRoom(g, 14.1, false);
        CHECK(g.phase == Phase::HaveRoom);
        Guest h;
        OnAsked(h, 0.0);
        for (int i = 0; i < kMaxReasks; ++i) {
            OnRoom(h, 1.0 + i * 2.0, true, Busy::Settle);
            CHECK(Step(h, 1.0 + i * 2.0 + kReaskWait, false, false) == Action::Reask);
        }
        OnRoom(h, 30.0, true, Busy::Settle);  // too often: the sign path
        CHECK(h.phase == Phase::Failed);
    }
    // A failed join locked the host (smoke 2026-10-21): its game summoned the failed guest's new
    // sign every ~30 s and every direct summon landed in the 20 s sign hold, for minutes. A sign
    // summon that brought nobody into the room ends its hold after kSignSummonNoJoin; the guest
    // asks again (Sign) and is served then.
    {
        HostView w; w.world_up = true; w.cooperators = 0;
        w.ms_since_sign_summon = 200; w.ms_since_member_joined = 1 << 30;
        CHECK(SignSummonBusy(w) && HostRefusal(w) && std::strstr(HostRefusal(w), "summoning a sign") &&
              RefusalWait(w) == Busy::Sign);
        w.ms_since_sign_summon = kSignSummonNoJoin - 1; CHECK(SignSummonBusy(w));
        w.ms_since_sign_summon = kSignSummonNoJoin + 500; CHECK(!SignSummonBusy(w) && !HostRefusal(w));  // nobody came
        w.ms_since_member_joined = 3000;  // its guest joined the room 3 s ago: a real summon, held
        CHECK(SignSummonBusy(w) && RefusalWait(w) == Busy::Sign);
        w.ms_since_member_joined = kSignSummonNoJoin + 900;  // joined before the request: not this summon
        CHECK(!SignSummonBusy(w));
        w.ms_since_member_joined = 3000; w.ms_since_sign_summon = kSignSummonHold; CHECK(!SignSummonBusy(w));
        w.ms_since_sign_summon = 1000; w.cooperators = 3; CHECK(RefusalWait(w) == Busy::None);  // full: no re-ask
        w.cooperators = 0; w.settling = true; CHECK(RefusalWait(w) == Busy::Sign);  // the sign hold is named first
        CHECK(WaitOf(EncodeRefuse(Busy::Sign)) == Busy::Sign && BusyText(Busy::Sign, false).empty());
        // The guest: asks again every kReaskWait; the hold ends within the re-asks (8 x 1.5 s > 10 s).
        CHECK(kMaxReasks * kReaskWait * 1000.0 > double(kSignSummonNoJoin));
        Guest g;
        OnAsked(g, 0.0);
        OnRoom(g, 0.2, true, Busy::Sign);
        CHECK(g.phase == Phase::Asked && Step(g, 0.2 + kReaskWait, false, false) == Action::Reask);
        OnRoom(g, 2.0, false);
        CHECK(g.phase == Phase::HaveRoom);
        Guest h;
        OnAsked(h, 0.0);
        for (int i = 0; i < kMaxReasks; ++i) {
            OnRoom(h, 1.0 + i * 2.0, true, Busy::Sign);
            CHECK(Step(h, 1.0 + i * 2.0 + kReaskWait, false, false) == Action::Reask);
        }
        OnRoom(h, 30.0, true, Busy::Sign);  // still summoning: the bell sign path, not a wait
        CHECK(h.phase == Phase::Failed && !h.waited);
        // The host's sign summon brought the guest in while it was asking: no more asks.
        Guest j;
        OnAsked(j, 0.0);
        OnRoom(j, 0.2, true, Busy::Sign);
        CHECK(Step(j, 0.5, false, true) == Action::ReportOk && j.phase == Phase::Done);
        CHECK(Step(j, 0.2 + kReaskWait, false, true) == Action::None);
        OnRoom(j, 2.0, true, Busy::Sign);  // a late refusal: ignored
        CHECK(j.phase == Phase::Done && Step(j, 4.0, false, true) == Action::None);
    }
    // An unreachable joiner (its last join failed, nothing heard since) is not summoned again
    CHECK(!UnreachableHold(0, 0, 100.0));                       // never failed
    CHECK(UnreachableHold(50.0, 0, 60.0));                      // failed, never heard
    CHECK(UnreachableHold(50.0, 40.0, 60.0));                   // heard only before the failure
    CHECK(!UnreachableHold(50.0, 50.5, 60.0));                  // heard after it (a probe's answer)
    CHECK(!UnreachableHold(50.0, 0, 50.0 + kUnreachableMax));   // the hold runs out
    // Playtest 1 #9: the guest in another map gathers first (refused with "wait":"map")
    CHECK(MapKnown(0x15000000) && !MapKnown(0) && !MapKnown(0xffffffffu));
    CHECK(SameMap(0x18010000, 0x18010000) && SameMap(0x18010000, 0x18010001) && !SameMap(0x18010000, 0x18000000));
    CHECK(!SameMap(0x15000000, 0xffffffffu) && !SameMap(0, 0));
    v.host_map = 0x18010000; v.guest_map = 0x15000000;
    CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "another map") && RefusalWait(v) == Busy::OtherMap);
    CHECK(WaitOf(EncodeRefuse(Busy::OtherMap)) == Busy::OtherMap);
    v.guest_map = 0xffffffffu; CHECK(!HostRefusal(v));  // not known: no map check (the guest checks the room)
    v.guest_map = 0x18010000; CHECK(!HostRefusal(v));
    v.guest_map = 0x15000000; v.carry = true; CHECK(!HostRefusal(v)); v.carry = false;
    CHECK(CarriesAcrossMaps(0x1d000000, false) && !CarriesAcrossMaps(0x18010000, false) && CarriesAcrossMaps(0x18010000, true));
    // the boss / prologue reasons come before the map; the map before "full"
    v.busy = Busy::Boss; CHECK(RefusalWait(v) == Busy::Boss); v.busy = Busy::None;
    v.host_ready = false; CHECK(RefusalWait(v) == Busy::Prologue); v.host_ready = true;
    v.cooperators = 3; CHECK(RefusalWait(v) == Busy::OtherMap); v.cooperators = 0;
    v.host_map = v.guest_map = 0;
    // guest: a refusal for another map asks for the gather
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, true, Busy::OtherMap);
    CHECK(g.phase == Phase::Failed && g.gather && std::strlen(g.fail_why) > 0);
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, true, Busy::Prologue); CHECK(g.phase == Phase::Failed && !g.gather);
    OnAsked(g, 1); CHECK(!g.gather && g.fail_why[0] == 0);
    // guest: a room of another map is never delivered (the Dream / Central Yharnam race)
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, false);
    CHECK(RoomElsewhere(g, 0.2, 0x18010000, 0x15000000, false) && g.phase == Phase::Failed && g.gather);
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, false);
    CHECK(RoomElsewhere(g, 0.2, 0x18010000, 0xffffffffu, false) && g.phase == Phase::Failed && !g.gather);  // own map unknown
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, false);
    CHECK(!RoomElsewhere(g, 0.2, 0x18010000, 0x18010000, false) && g.phase == Phase::HaveRoom);
    CHECK(!RoomElsewhere(g, 0.2, 0x1d000000, 0x15000000, true) && g.phase == Phase::HaveRoom);  // chalice quick join
    g.phase = Phase::Delivered; CHECK(!RoomElsewhere(g, 0.2, 0x18010000, 0x15000000, false));   // only before delivery
    // every failure says why (logged with FAILED)
    g = Guest{}; OnAsked(g, 0); Step(g, kAnswerWait + 1, false, false); CHECK(std::strstr(g.fail_why, "no answer"));
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false); Step(g, 3, false, false); Step(g, 3 + kEntryWait + 1, false, false); CHECK(std::strstr(g.fail_why, "entry"));
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false); OnDelivered(g, 1, true); Step(g, 1 + kJoinWait + 1, false, false);
    CHECK(std::strstr(g.fail_why, "did not join"));
    // Playtest 1 #2: the host's bell is the direct summon's fallback only
    HostBellView b;
    CHECK(!HostBellHeld(b));                                   // direct summon off: ring as before
    b.direct_on = true;
    CHECK(HostBellHeld(b));                                    // a member just started waiting: its ring first
    b.waiting_s = kHostBellGrace + 1; CHECK(!HostBellHeld(b)); // no request came: ring (sign path)
    b.since_req = 0.5; b.since_served = 0.4; CHECK(HostBellHeld(b));  // room on its way
    b.since_done = 0.1; b.done_ok = true; CHECK(HostBellHeld(b));     // it joined
    b.done_ok = false; CHECK(!HostBellHeld(b));                       // it failed: the fallback rings now
    b = HostBellView{}; b.direct_on = true; b.waiting_s = 100; b.since_req = 1;
    CHECK(HostBellHeld(b));                                    // batch 6: the request is not answered yet (live: rang 0.03 s after it)
    b.since_refused = 0.9;
    CHECK(!HostBellHeld(b));                                   // refused (hold / sign summon): sign path
    b.since_refused = 30;
    CHECK(HostBellHeld(b));                                    // an old refusal does not answer a new request
    b.since_refused = 0.9; b.refused_wait = true; CHECK(HostBellHeld(b));             // refused to wait / gather: no bell
    b.since_req = kHostBellHold + 1; CHECK(!HostBellHeld(b));  // long ago
    b = HostBellView{}; b.direct_on = true; b.waiting_s = 100; b.since_served = kHostBellHold + 5; b.since_done = kHostBellHold + 1;
    b.done_ok = true; CHECK(!HostBellHeld(b));                 // an old join does not hold a new wait
    // Batch 6: the host's lamp travel waits for a member it just served a room (H1:3630).
    HostTravelHoldView h;
    CHECK(!HostTravelHeld(h));                                     // off / nothing served
    h.direct_on = true;
    CHECK(!HostTravelHeld(h));                                     // nothing served lately
    h.since_served = 2;
    CHECK(HostTravelHeld(h));                                      // room on its way: wait
    h.since_done = 1; h.done_ok = true;
    CHECK(HostTravelHeld(h));                                      // joined, chr not in the world yet: wait
    h.joiner_in_world = true;
    CHECK(!HostTravelHeld(h));                                     // in the world: travel (it follows)
    h.joiner_in_world = false; h.done_ok = false;
    CHECK(!HostTravelHeld(h));                                     // the join failed: travel now
    h.since_done = 5; h.done_ok = false;
    CHECK(HostTravelHeld(h));                                      // an older failure does not release a new room
    h.since_served = kHostTravelHoldMax;
    CHECK(!HostTravelHeld(h));                                     // waited long enough
    // par_c6: a title joiner (no dsum_done) holds the travel until its chr is here, direct summon on or off.
    h = HostTravelHoldView{};
    h.since_title = 3;
    CHECK(HostTravelHeld(h));                                      // room sent to a title joiner: wait
    h.joiner_in_world = true; h.joiner_loading = true;
    CHECK(HostTravelHeld(h));                                      // its chr is here but its loading screen is up: wait
    h.joiner_loading = false;
    CHECK(!HostTravelHeld(h));                                     // its chr is here: travel (it follows)
    h.joiner_loading = true; h.since_title = kHostTravelHoldMax;
    CHECK(!HostTravelHeld(h));                                     // waited long enough, even if still loading
    h.joiner_loading = false;
    h.joiner_in_world = false; h.since_title = kHostTravelHoldMax;
    CHECK(!HostTravelHeld(h));                                     // waited long enough
    // A11: no beside-the-host warp while the host watches a cutscene or its chr reads at the origin.
    {
        const float origin[3] = {0.0f, 0.0f, 0.0f}, lamp[3] = {-10.8f, -7.0f, -21.0f};
        CHECK(JoinBesideWaits(Busy::Cutscene, lamp));
        CHECK(JoinBesideWaits(Busy::Boss, lamp));
        CHECK(JoinBesideWaits(Busy::None, origin));
        CHECK(!JoinBesideWaits(Busy::None, lamp));
        CHECK(!JoinBesideWaits(Busy::OtherMap, lamp));
    }
    // Chalice Dungeons (after PR #8, live): the direct summon's message carries the host's ChannelId,
    // else the guest's game builds no dungeon and the guest falls through an empty m29.
    {
        Room c = r;
        c.area = 0x1d0a5a00;
        c.channel = 1110692594440089ull;  // 0x3f22b458bd799, the shared fixed Pthumeru dungeon
        Room cd;
        CHECK(DecodeRoom(EncodeRoom(c), &cd, &refused, &err) && cd.channel == c.channel && cd.area == c.area);
        const auto cp = BuildPayload(id, c);
        std::memcpy(&q, &cp[0x28], 8); CHECK(q == 1110692594440089ull);
        // An older host (no "channel" key) decodes as none.
        CHECK(DecodeRoom("{\"op\":\"dsum_room\",\"room\":\"00112233445566778899\",\"area\":486539264,\"pos\":[0,0,0],\"level\":0}",
                         &cd, &refused, &err) && cd.channel == kNoChannel);
        CHECK(ChannelSet(1110692594440089ull) && !ChannelSet(0) && !ChannelSet(kNoChannel) && !ChannelSet(0x8000000000000001ull));
        CHECK(ChaliceMap(0x1d000000) && ChaliceMap(0x1d0a5a00) && !ChaliceMap(0x15000000) && !ChaliceMap(0xffffffffu));
        // The guest keeps a dungeon room without a ChannelId out of its game (waited, no gather).
        Guest cg; OnAsked(cg, 0); OnRoom(cg, 1, false);
        CHECK(RoomLacksDungeon(cg, 2, cd) && cg.phase == Phase::Failed && cg.waited && !cg.gather);
        Guest cg2; OnAsked(cg2, 0); OnRoom(cg2, 1, false);
        CHECK(!RoomLacksDungeon(cg2, 2, c) && cg2.phase == Phase::HaveRoom);
        Room plain = r;  // a room outside the dungeons needs none
        CHECK(!RoomLacksDungeon(cg2, 2, plain) && cg2.phase == Phase::HaveRoom);
        // The host refuses while its dungeon's ChannelId is not ready, with the Chalice wait (the guest asks again).
        HostView hv; hv.world_up = true; hv.ms_since_sign_summon = 30000;
        hv.chalice_unready = true;
        CHECK(HostRefusal(hv) && std::strstr(HostRefusal(hv), "ChannelId") && RefusalWait(hv) == Busy::Chalice);
        hv.chalice_unready = false; CHECK(!HostRefusal(hv));
        CHECK(WaitOf(EncodeRefuse(Busy::Chalice)) == Busy::Chalice);
        Guest rg; OnAsked(rg, 0); OnRoom(rg, 1, true, Busy::Chalice);
        CHECK(rg.phase == Phase::Asked && rg.reask_at > 1);
        CHECK(Step(rg, 1 + kReaskWait + 0.1, false, false) == Action::Reask);
        // Title join: only a joiner whose own slot holds the host's dungeon comes in from its title.
        CHECK(!TitleJoinChaliceElsewhere(0x15000000, kNoChannel, 0));
        CHECK(TitleJoinChaliceElsewhere(0x1d000000, 1110692594440089ull, 0));
        CHECK(TitleJoinChaliceElsewhere(0x1d000000, 1110692594440089ull, 42));
        CHECK(TitleJoinChaliceElsewhere(0x1d000000, kNoChannel, kNoChannel));
        CHECK(!TitleJoinChaliceElsewhere(0x1d000000, 1110692594440089ull, 1110692594440089ull));
        CHECK(TitleJoinBusy(false, false, true, false, true) == Busy::Chalice &&
              TitleJoinBusy(true, false, true, false, true) == Busy::Boss);
        CHECK(!BusyText(Busy::Chalice, false).empty() && std::string(BusyWord(Busy::Chalice)) == "chalice");
    }
    std::printf("party-dsummon-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
