// SPDX-License-Identifier: GPL-3.0-or-later
// Deaths, rests and same-map lamps in a party (gpu/shim/party/party_death_rules.h; playtest 1
// batch 1). Build and run: ninja -C out/gpu party-death-rules-test && out/gpu/party-death-rules-test.exe
#include "party/party_death_rules.h"

#include <cstdio>
#include <limits>
#include <vector>

using namespace coop::deathrules;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

int main() {
    // C5: the spectator's notice.
    CHECK(SpectateNotice("Hunter0", 1) == "Spectating Hunter0");
    CHECK(SpectateNotice("Hunter2", 2) == "Spectating Hunter2  -  Cross: next hunter");
    CHECK(SpectateNotice("", 1) == "Spectating a party member");
    CHECK(SpectateNotice("Hunter0", 0).empty());
    // Opt-in switches: on only for "1...".
    CHECK(!OptIn(nullptr));
    CHECK(!OptIn(""));
    CHECK(!OptIn("0"));
    CHECK(!OptIn("on"));
    // Guest chr type (verification replay): 0 after a B2 load / title join -> 1 again.
    CHECK(GuestChrTypeFixDue(true, 0, 500, 4, 2.5));
    CHECK(!GuestChrTypeFixDue(true, 1, 500, 4, 9.0));   // already a phantom
    CHECK(!GuestChrTypeFixDue(true, 8, 500, 4, 9.0));   // grey own body: not ours to touch
    CHECK(GuestChrTypeFixDue(true, 0, 0, 4, 0.0));      // down right after the arrival: at once
    CHECK(GuestChrTypeFixDue(true, 0, 500, 4, 0.5));    // playtest 1: a death ~1 s after a title join
    CHECK(!GuestChrTypeFixDue(true, 0, -1, 4, 9.0));    // HP unknown
    CHECK(!GuestChrTypeFixDue(true, 0, 500, 1, 9.0));   // not in flow 4 (still loading in)
    CHECK(!GuestChrTypeFixDue(true, 0, 500, 4, 0.1));   // the first frames of the arrival
    CHECK(!GuestChrTypeFixDue(false, 0, 500, 4, 9.0));  // own world / host
    CHECK(OptIn("1"));

    // Down counts.
    CHECK(PartyDown(0, 3, 3) == 0);
    CHECK(PartyDown(2, 1, 0) == 1);
    CHECK(PartyDown(2, 0, 1) == 1);
    CHECK(PartyDown(2, 5, 0) == 2);
    CHECK(!Wiped(0, 0, 0));
    CHECK(!Wiped(2, 1, 1));
    CHECK(Wiped(1, 0, 1));  // the early notice counts (bug 18: the report came 0.3 s late)
    CHECK(Wiped(1, 1, 0));

    // The host's death: a party load by default (bugs 6, 7).
    HostDeathView v;
    v.members = 1;
    v.lamp_here = true;
    CHECK(PlanHostDeath(v) == HostDeathPlan::PartyLoad);
    CHECK(!PlanTakesFunnel(PlanHostDeath(v)));
    v.party_on = false;
    CHECK(PlanHostDeath(v) == HostDeathPlan::Stock);
    v.party_on = true;
    v.members = 0;
    CHECK(PlanHostDeath(v) == HostDeathPlan::Stock);  // nobody in the world: the game's own death
    v.solo_fixture = true;
    CHECK(PlanHostDeath(v) == HostDeathPlan::PartyLoad);
    v.revive_opt_in = true;
    CHECK(PlanHostDeath(v) == HostDeathPlan::ReviveInPlace);
    v.solo_fixture = false;
    v.members = 1;
    CHECK(PlanHostDeath(v) == HostDeathPlan::ReviveInPlace);  // opt-in: the seamless revive
    CHECK(PlanTakesFunnel(PlanHostDeath(v)));
    v.lamp_here = false;
    CHECK(PlanHostDeath(v) == HostDeathPlan::AloneLoad);
    v.alone_opt = false;
    CHECK(PlanHostDeath(v) == HostDeathPlan::PartyLoad);
    v.revive_opt_in = false;
    v.boss_fight = true;
    CHECK(PlanHostDeath(v) == HostDeathPlan::BossLimbo);  // both modes keep the boss limbo
    CHECK(PlanTakesFunnel(PlanHostDeath(v)));
    v.down_seen = 1;
    CHECK(PlanHostDeath(v) == HostDeathPlan::Wipe);       // the guest is down too: a wipe
    v.boss_fight = false;
    v.down_seen = 0;
    v.down_reported = 1;
    CHECK(PlanHostDeath(v) == HostDeathPlan::Wipe);
    v.members = 2;
    CHECK(PlanHostDeath(v) == HostDeathPlan::PartyLoad);  // one of two guests down: not a wipe

    // While the host is down: recount at every frame (bug 18).
    DownView d;
    d.members = 1;
    d.down_for = 0.2;
    CHECK(PlanDownTick(d) == DownTick::Wait);
    d.down_seen = 1;
    CHECK(PlanDownTick(d) == DownTick::Wipe);  // the guest's death arrives during the delay
    d.down_seen = 0;
    d.down_for = 1.5;
    CHECK(PlanDownTick(d) == DownTick::PartyLoad);  // default: the respawn load (only the limbo gets here)
    {
        // The host in limbo, the boss at 0 HP: no respawn load until its defeat is saved.
        DownView k = d;
        k.limbo = true;
        k.boss_active = false;
        k.boss_dying = true;
        CHECK(PlanDownTick(k) == DownTick::Wait);
        k.boss_dying = false;
        CHECK(PlanDownTick(k) != DownTick::Wait);
    }
    d.revive_opt_in = true;
    CHECK(PlanDownTick(d) == DownTick::AloneLoad);
    d.lamp_here = true;
    CHECK(PlanDownTick(d) == DownTick::Revive);
    d.down_reported = 1;
    CHECK(PlanDownTick(d) == DownTick::Wipe);  // recounted at the revive tick itself
    d.down_reported = 0;
    d.limbo = true;
    d.boss_active = true;
    d.down_for = 60;
    CHECK(PlanDownTick(d) == DownTick::Wait);
    d.down_seen = 1;
    CHECK(PlanDownTick(d) == DownTick::Wipe);  // a wipe ends the limbo
    d.down_seen = 0;
    d.boss_active = false;
    d.revive_opt_in = false;
    CHECK(PlanDownTick(d) == DownTick::PartyLoad);  // boss gone: one respawn load for everyone

    // The split question closes on a load, a death or after 30 s (bug 12).
    CHECK(PlanAskClose(true, false, false, 5) == AskClose::Keep);
    CHECK(PlanAskClose(false, false, false, 100) == AskClose::Keep);  // queued: waits to open
    CHECK(PlanAskClose(true, true, false, 1) == AskClose::Load);
    CHECK(PlanAskClose(false, true, false, 1) == AskClose::Load);
    CHECK(PlanAskClose(true, false, true, 1) == AskClose::Dead);
    CHECK(PlanAskClose(true, false, false, 30.5) == AskClose::Timeout);
    CHECK(PlanAskClose(true, false, false, 29) == AskClose::Keep);

    // Whose lamp warp (bug 20): the lamp flags of events 7200 / 12107000 (EMEVD data).
    for (std::uint32_t f : {72110100u, 72200100u, 72410101u, 72410103u, 73600102u, 72102110u, 72102411u, 72103602u}) {
        CHECK(LampTriggerFlag(f));
    }
    for (std::uint32_t f : {72410201u, 72410000u, 12410202u, 72100200u, 9401u, 1001u, 72411101u, 82410101u}) {
        CHECK(!LampTriggerFlag(f));
    }
    CHECK(PlanLampOrigin(-1, -1) == LampOrigin::Other);     // no own flag: another game's warp event
    CHECK(PlanLampOrigin(-1, -1, false) == LampOrigin::Own); // no flag hook: ask as before
    CHECK(PlanLampOrigin(12, -1) == LampOrigin::Other);     // a stale own flag
    // parg: story warps are the triggering player's own use; lamp / warp-object events are not.
    CHECK(StoryWarpEvent(13201803, true));   // Blood Moon -> Yahar'gul, on the host
    CHECK(!StoryWarpEvent(13201803, false)); // a guest's stray copy stays dropped
    CHECK(StoryWarpEvent(12200131, false));  // Hemwick carriage: any player
    CHECK(StoryWarpEvent(12800431, false));  // Amygdala grab with the own Tonsil Stone
    CHECK(!StoryWarpEvent(12800430, false)); // End if Client
    CHECK(!StoryWarpEvent(7200, true));      // world lamp -> Dream
    CHECK(!StoryWarpEvent(12107000, true));  // Dream headstone
    CHECK(!StoryWarpEvent(12907020, true));  // chalice warp object
    CHECK(PlanLampOrigin(3.5, -1) == LampOrigin::Own);      // this player's lamp
    CHECK(PlanLampOrigin(-1, 3.2) == LampOrigin::Other);    // the other player's lamp
    CHECK(PlanLampOrigin(3.5, 12) == LampOrigin::Own);      // a stale peer flag
    CHECK(PlanLampOrigin(3.5, 8) == LampOrigin::Own);       // own use after the peer's
    CHECK(PlanLampOrigin(8, 3.4) == LampOrigin::Other);     // the peer's after an old own one

    // A guest's lamp (bug 10, bug 3).
    CHECK(PlanGuestLamp(false, false, true, false) == GuestLamp::Plain);
    CHECK(PlanGuestLamp(true, true, true, false) == GuestLamp::Plain);    // standing at it
    CHECK(PlanGuestLamp(true, false, true, false) == GuestLamp::SoloLoad); // same map: own load
    CHECK(PlanGuestLamp(true, true, true, true) == GuestLamp::SoloLoad);   // a Mark to the lamp here
    CHECK(PlanGuestLamp(true, false, false, false) == GuestLamp::Ask);
    CHECK(PlanGuestLamp(true, false, false, true) == GuestLamp::Ask);
    CHECK(GuestAloneRestsHostWorld(true, false));
    CHECK(!GuestAloneRestsHostWorld(false, false));  // YES: the party travel is the rest
    CHECK(GuestAloneRestsHostWorld(true, true));  // D9 (pard4): a lamp of the loaded map rests too

    // The host's rest load for a guest's rest (bug 3).
    RestView r;
    CHECK(PlanRestLoad(r) == RestLoad::NotReady);
    r.world_up = true;
    CHECK(PlanRestLoad(r) == RestLoad::Dead);
    r.alive = true;
    CHECK(PlanRestLoad(r) == RestLoad::Run);
    r.loading = true;
    CHECK(PlanRestLoad(r) == RestLoad::NotReady);
    r.loading = false;
    r.boss_fight = true;
    CHECK(PlanRestLoad(r) == RestLoad::BossFight);  // C7: a load out of the boss room
    r.travel_pending = true;
    CHECK(PlanRestLoad(r) == RestLoad::Busy);  // a travel / death under way first
    r.travel_pending = false;
    r.in_dream = true;
    CHECK(PlanRestLoad(r) == RestLoad::BossFight);  // Gehrman's fight in the Dream: out too
    r.in_dream = false;
    r.boss_fight = false;
    r.in_dream = true;
    CHECK(PlanRestLoad(r) == RestLoad::InDream);
    r.in_dream = false;
    r.travel_pending = true;
    CHECK(PlanRestLoad(r) == RestLoad::Busy);
    r.travel_pending = false;
    r.since_last = 5;
    CHECK(PlanRestLoad(r) == RestLoad::TooSoon);
    r.since_last = 25;
    CHECK(PlanRestLoad(r) == RestLoad::Run);

    // Batch 6: who a guest's rest load waits for (the resting guest went alone: nobody, 2 players).
    {
        const std::uint32_t yh = 0x18;  // area of m24_01 (the host's)
        RestMember two[] = {{1, true, 0x15}};  // the resting guest, already in the Dream
        CHECK(RestLoadExpected(two, 1, 1, yh, 0x15) == 0);  // rested to the Dream: never arrives here
        RestMember loading[] = {{1, true, 0}};  // still loading alone: it is the rest slot anyway
        CHECK(RestLoadExpected(loading, 1, 1, yh, 0x15) == 0);
        CHECK(RestLoadExpected(loading, 1, 1, yh, yh) == 1);  // rested at a lamp of the host's area: in its world again, follows
        CHECK(RestLoadExpected(loading, 1, 1, yh, 0) == 0);   // lamp unknown: not waited for
        RestMember three[] = {{1, true, 0x15}, {2, true, yh}};  // a second guest beside the host follows
        CHECK(RestLoadExpected(three, 2, 1, yh, 0x15) == 1);
        RestMember apart[] = {{1, true, 0x15}, {2, true, 0x17}, {3, false, yh}};  // elsewhere / lost: no
        CHECK(RestLoadExpected(apart, 3, 1, yh, 0x15) == 0);
        RestMember unknown[] = {{1, true, 0x15}, {2, true, 0}};  // area unknown (loading): may follow
        CHECK(RestLoadExpected(unknown, 2, 1, yh, 0x15) == 1);
    }

    // pt1polish: a guest's rest back into the host's area rides on its re-join (one load).
    {
        const std::uint32_t yh = 0x18;
        CHECK(RestRidesRejoin(true, yh, yh));
        CHECK(!RestRidesRejoin(false, yh, yh));      // a Hunter's Mark: no rest
        CHECK(!RestRidesRejoin(true, 0x15, yh));     // to another area: the host's rest load stays separate
        CHECK(!RestRidesRejoin(true, 0, yh));        // destination unknown
        RestMember loading[] = {{1, true, 0}};
        CHECK(RestLoadExpected(loading, 1, 1, yh, yh, true) == 0);   // it arrives by its own re-join
        RestMember two[] = {{1, true, 0x15}, {2, true, yh}};
        CHECK(RestLoadExpected(two, 2, 1, yh, yh, true) == 1);       // the guest beside the host still follows
        CHECK(RestCarryHeld(10.0, 9.0, false));
        CHECK(!RestCarryHeld(10.0, 9.0, true));                      // the load started: the hold ends with it
        CHECK(!RestCarryHeld(20.0, 9.0, false));                     // 8 s without a load: served anyway
        CHECK(!RestCarryHeld(10.0, 0.0, false));                     // no carried rest
        CHECK(HostServesRejoin(false, false, false));
        CHECK(!HostServesRejoin(true, false, false));                // loading
        CHECK(!HostServesRejoin(false, true, false));                // the own load's re-sync covers it
        CHECK(!HostServesRejoin(false, false, true));                // the rest load goes first
    }
    // pt1polish: banners.
    CHECK(HidePartyBanner(103100, true, true, -1));                  // guest death: it stays
    CHECK(HidePartyBanner(103300, true, true, 100));
    CHECK(HidePartyBanner(103101, true, true, -1));                  // the beckoner died: it stays too
    CHECK(!HidePartyBanner(103100, false, true, -1));                // B2 off: vanilla, it goes home
    CHECK(!HidePartyBanner(103100, true, false, -1));                // the host never sees it anyway
    CHECK(HidePartyBanner(102001, true, false, 3.0));                // host after a wipe re-join
    CHECK(HidePartyBanner(102100, true, true, 3.0));                 // guest "Entered the world" again
    CHECK(!HidePartyBanner(102001, true, false, -1));                // the first summon: shown
    CHECK(!HidePartyBanner(102001, true, false, 60.0));              // a later new join: shown
    CHECK(!HidePartyBanner(103001, true, false, 3.0));               // "Cooperator ... has died": shown
    CHECK(!HidePartyBanner(105200, true, true, 3.0));                // boss-fight banners untouched
    // pt1polish: an empty stain is not placed.
    CHECK(ClearEmptyStain(true, 1, 0));
    CHECK(!ClearEmptyStain(true, 1, 48));
    CHECK(!ClearEmptyStain(true, 0, 0));
    CHECK(!ClearEmptyStain(false, 1, 0));
    CHECK(SkipEmptyStainRecord(true, 1, 0));
    CHECK(!SkipEmptyStainRecord(true, 1, 1670));                     // echoes to recover: the stain as usual
    CHECK(!SkipEmptyStainRecord(true, 1, -1));                       // unknown: the game decides
    CHECK(!SkipEmptyStainRecord(true, 0, 0));                        // no stain set
    CHECK(!SkipEmptyStainRecord(false, 1, 0));                       // solo: vanilla
    // pt1polish: test steps.
    CHECK(DeadWatchdogFires(15.0, 15.0, false, false));              // dead 15 s, no death state: redirect
    CHECK(!DeadWatchdogFires(14.0, 15.0, false, false));
    CHECK(!DeadWatchdogFires(20.0, 15.0, true, false));              // the game's death state ran (held)
    CHECK(!DeadWatchdogFires(20.0, 15.0, false, true));              // following the host's respawn travel (wipe)
    CHECK(TestStepWaits(true, false, true, false));                  // guest_obj while dead
    CHECK(!TestStepWaits(true, true, true, false));
    CHECK(!TestStepWaits(false, false, true, false));                // a press may run while dead
    CHECK(TestStepWaits(false, true, true, true));                   // inside the last wipe's re-join
    CHECK(TestStepWaits(false, true, false, false));                 // not in the world

    // Own bloodstain record changes (bug 13 logging).
    CHECK(PlanStainChange(0, -1, 1, 4843) == StainChange::Recorded);
    CHECK(PlanStainChange(1, 4843, 1, -1) == StainChange::Taken);
    CHECK(PlanStainChange(1, 4843, 0, 4843) == StainChange::Taken);
    CHECK(PlanStainChange(1, 4843, 1, 2341) == StainChange::Replaced);  // a second death overwrote it
    CHECK(PlanStainChange(1, 4843, 1, 4843) == StainChange::None);
    CHECK(PlanStainChange(0, -1, 0, -1) == StainChange::None);

    // Bug 22: nobody to travel with = single player (no in-world lamp move, no redirect).
    CHECK(PartyTravelActive(true, false, true, false));              // guest in the host's world
    CHECK(!PartyTravelActive(true, false, false, false));            // Join-a-party guest, no host
    CHECK(!PartyTravelActive(true, false, false, true));             // own world: a host's members do not count
    CHECK(PartyTravelActive(false, true, false, true));              // host with members
    CHECK(!PartyTravelActive(false, true, false, false));            // host alone
    CHECK(!PartyTravelActive(false, false, true, true));             // no party role
    CHECK(GuestInHostWorld(6, 0));                                   // client
    CHECK(GuestInHostWorld(7, 1));                                   // leaving, still on the overlay (Mark)
    CHECK(GuestInHostWorld(0, 1));
    CHECK(!GuestInHostWorld(0, 0));                                  // own world, idle session
    CHECK(!GuestInHostWorld(-1, -1));                                // no world / no store
    CHECK(!GuestInHostWorld(3, 0));                                  // hosting in its own world

    // P4: the same scripted warp asks on the lower slot only.
    CHECK(DropSameWarpQuestion(true, 3202958, 1, 3202958, 0));   // guest: the host asks the same
    CHECK(!DropSameWarpQuestion(true, 3202958, 0, 3202958, 1));  // host keeps its question
    CHECK(!DropSameWarpQuestion(true, 3202958, 1, 3202959, 0));  // another warp
    CHECK(!DropSameWarpQuestion(false, 3202958, 1, 3202958, 0)); // nothing pending here
    CHECK(DropSameWarpQuestion(true, 2502959, 2, 2502959, 1));   // guest 2 vs guest 1
    CHECK(!DropSameWarpQuestion(true, 2502959, -1, 2502959, 0)); // own slot unknown
    CHECK(!DropSameWarpQuestion(true, 0, 1, 0, 0));
    // B5: a living guest apart stays at the host's death; in its area / dead / busy it follows.
    CHECK(StayApartAtHostDeath(false, true, 24, 21));
    CHECK(!StayApartAtHostDeath(false, true, 24, 24));
    CHECK(!StayApartAtHostDeath(true, true, 24, 21));
    CHECK(!StayApartAtHostDeath(false, false, 24, 21));
    CHECK(!StayApartAtHostDeath(false, true, 0, 21));

    {
        // A11: the host's chr at the origin (first sync waiting) is not where the host is.
        const float origin[3] = {0, 0, 0}, host[3] = {-193.4f, -28.6f, 68.5f}, near[3] = {-193.0f, -28.6f, 68.1f};
        CHECK(!HostChrPlaced(origin, true, host));
        CHECK(!HostChrPlaced(origin, false, host));
        CHECK(HostChrPlaced(near, true, host));
        CHECK(HostChrPlaced(near, false, origin));
        CHECK(HostChrPlaced(host, true, host));
    }
    for (int i = 0; i < 6; ++i) {
        CHECK(HostDeathPlanName(HostDeathPlan(i))[0] != '?');
    }
    CHECK(DownTickName(DownTick::Wipe)[0] != '?');
    // Issue #4 item 6: the guest died first and spectated > 90 s, then the host died in the boss
    // fight. The down record lasts (it used to expire after 90 s: "1 member(s), 0 down", limbo).
    {
        DownLedger led;
        std::vector<MemberNow> m{{1, true, true, false}};  // slot 1 in the roster, its vitals: HP 0
        led.Down(1, 100.0);
        for (double t = 100.0; t <= 100.0 + 400.0; t += 0.5) led.Reconcile(m, t);
        CHECK(led.Count() == 1);
        HostDeathView v;
        v.members = 1;
        v.down_reported = led.Count();
        v.down_seen = SeenDown(m);
        v.boss_fight = true;
        CHECK(PlanHostDeath(v) == HostDeathPlan::Wipe);
        // Even with no vitals at all (stale: a spectator that stopped sending), the notice counts.
        std::vector<MemberNow> stale{{1, true, false, false}};
        CHECK(led.Reconcile(stale, 600.0) == 0 && led.Count() == 1);
        v.down_seen = SeenDown(stale);
        CHECK(v.down_seen == 0 && PlanHostDeath(v) == HostDeathPlan::Wipe);
        // A host already in limbo: the recount each frame also wipes.
        DownView dv;
        dv.members = 1, dv.down_reported = led.Count(), dv.limbo = true, dv.boss_active = true, dv.down_for = 5;
        CHECK(PlanDownTick(dv) == DownTick::Wipe);
        // Seen at HP 0 alone (the notice lost) is a wipe too.
        dv.down_reported = 0, dv.down_seen = SeenDown(m);
        CHECK(PlanDownTick(dv) == DownTick::Wipe);
    }
    // A confirmed revival / a rejoin (new chr: "arrived") / a leave clears the old death.
    {
        DownLedger led;
        led.Down(1, 10.0);
        led.Down(2, 10.0);
        led.Down(0, 10.0);   // the host's own slot is never a member
        led.Down(16, 10.0);  // out of range
        CHECK(led.Count() == 2);
        led.Clear(1);  // member_revived / arrived
        CHECK(!led.Has(1) && led.Has(2));
        std::vector<MemberNow> gone;  // slot 2 left the roster (replaced by nobody)
        int slot = -1;
        CHECK(led.Reconcile(gone, 11.0, &slot) == 1 && slot == 2 && led.Count() == 0);
        // A member that comes back alive without a notice (a lost member_revived): its own vitals
        // alive for kDownAliveConfirm s, once the notice is kDownNoticeGrace s old.
        led.Down(1, 20.0);
        std::vector<MemberNow> alive{{1, true, true, true}};
        led.Reconcile(alive, 20.5);  // vitals lag the notice: not yet
        led.Reconcile(alive, 22.9);
        CHECK(led.Has(1));
        led.Reconcile(alive, 23.0);  // grace over: the confirm starts
        led.Reconcile(alive, 25.0);
        CHECK(led.Has(1));
        led.Reconcile(std::vector<MemberNow>{{1, true, true, false}}, 25.5);  // dead again: the confirm restarts
        led.Reconcile(alive, 26.0);
        led.Reconcile(alive, 28.9);
        CHECK(led.Has(1));
        led.Reconcile(alive, 29.0);
        CHECK(!led.Has(1));
        // A lost-but-kept member (not connected: no fresh vitals) keeps its death until it rejoins.
        led.Down(1, 40.0);
        std::vector<MemberNow> lost{{1, true, false, false}};
        CHECK(led.Reconcile(lost, 200.0) == 0 && led.Has(1));
        // A new death after the revival is counted again.
        led.ClearAll();
        led.Down(1, 300.0);
        CHECK(led.Count() == 1);
    }
    // Issue #4 item 7: a guest's respawn at a lamp of the loaded map.
    // Fall state only (user decision): the distance no longer decides.
    CHECK(PlanGuestRevive(0.0, true, false) == GuestRevive::InPlace);        // a normal death
    CHECK(PlanGuestRevive(2.0, true, false) == GuestRevive::InPlace);        // a stumble is no fall
    CHECK(PlanGuestRevive(6.0, true, false) == GuestRevive::LoadFall);       // a fall
    CHECK(PlanGuestRevive(30.0, false, false) == GuestRevive::InPlace);      // no history, no fall state
    CHECK(PlanGuestRevive(0.0, false, true) == GuestRevive::LoadFallState);  // no history, the game's state
    // The issue's bundles: the 27 m fall (157 m from its lamp) loads; the normal deaths 92-191 m
    // from the lamp (+0x1E1 0001001098, hp+0x200 0000 or 8000) stay in place.
    CHECK(PlanGuestRevive(26.8, true, FallDeathState(0x04010010d8ull, 0xf800)) == GuestRevive::LoadFall);
    CHECK(FallDeathState(0x04010010d8ull, 0xf800) && FallDeathState(0x00030010d8ull, 0xf800));
    CHECK(FallDeathState(0x0400011088ull, 0) && FallDeathState(0x00030010d8ull, 0));
    CHECK(!FallDeathState(0x0001001098ull, 0x0000) && !FallDeathState(0x0001001098ull, 0x8000));
    CHECK(!FallDeathState(0x0003001098ull, 0x8000));  // after a normal revive
    CHECK(PlanGuestRevive(0.0, true, FallDeathState(0x0001001098ull, 0x8000)) == GuestRevive::InPlace);
    CHECK(FallDrop(-20.0, -40.0, -80.0) == 60.0);                            // into the void: still dropping
    CHECK(FallDrop(-20.0, -21.0, -21.0) == 1.0);
    CHECK(GuestReviveName(GuestRevive::LoadFall)[0] != '?');
    CHECK(RestLoadName(RestLoad::TooSoon)[0] != '?');
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-death-rules-test: ok\n");
    return 0;
}
