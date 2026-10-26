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

static void TestFallBackAndBeside() {
    using namespace coop::deathrules;
    // lamprule run 7: placed, never stood still, no safe spot: back beside the host.
    CHECK(FallBackTarget(false, false, true) == FallBack::Host);
    CHECK(FallBackTarget(false, false, false) == FallBack::None);
    CHECK(FallBackTarget(true, true, true) == FallBack::GameSafe);
    CHECK(FallBackTarget(false, true, true) == FallBack::Stable);
    CHECK(FallBackTarget(true, false, true) == FallBack::GameSafe);
    // The title join's move beside the host: not when already there; else only on loaded ground.
    CHECK(PlanBesideMove(1.6f, false, false, 0.0) == BesideMove::Skip);  // run 7: 1.6 m from the target
    CHECK(PlanBesideMove(20.0f, false, false, 0.5) == BesideMove::Wait);  // map "-": wait
    CHECK(PlanBesideMove(20.0f, true, false, 2.0) == BesideMove::Wait);   // no ground under it yet
    CHECK(PlanBesideMove(20.0f, true, true, 2.0) == BesideMove::Warp);
    CHECK(PlanBesideMove(20.0f, false, false, 12.0) == BesideMove::Skip); // gave up: the game's placement
    CHECK(PlanBesideMove(-1.0f, true, true, 1.0) == BesideMove::Warp);    // position unknown: on ground only
}

int main() {
    TestFallBackAndBeside();
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

    {
        // kidnap071 (a player report): the Snatcher's kidnap moves the host's respawn to the Gaol
        // (common 9421 -> 2802959, m28) at its death; with the revive on (0.7.0 default) that was a
        // lone load and the guests stayed. A scripted death warp is always the party's load.
        HostDeathView k;
        k.members = 2;
        k.revive_opt_in = true;
        k.lamp_here = false;
        CHECK(PlanHostDeath(k) == HostDeathPlan::AloneLoad);  // a plain lamp in another map: alone
        k.scripted_warp = true;
        CHECK(PlanHostDeath(k) == HostDeathPlan::PartyLoad);
        k.lamp_here = true;
        CHECK(PlanHostDeath(k) == HostDeathPlan::PartyLoad);  // never the revive in place either
        k.alone_opt = false;
        k.revive_opt_in = false;
        CHECK(PlanHostDeath(k) == HostDeathPlan::PartyLoad);
        k.down_reported = 2;
        CHECK(PlanHostDeath(k) == HostDeathPlan::Wipe);       // a wipe stays a wipe (also everyone's load)
        k.down_reported = 0;
        k.boss_fight = true;
        CHECK(PlanHostDeath(k) == HostDeathPlan::BossLimbo);  // a fight's limbo first ...
        DownView kd;
        kd.members = 2;
        kd.down_for = 2;
        kd.limbo = true;
        kd.revive_opt_in = true;
        kd.scripted_warp = true;
        CHECK(PlanDownTick(kd) == DownTick::PartyLoad);       // ... then the party's load, not alone
        kd.lamp_here = true;
        CHECK(PlanDownTick(kd) == DownTick::PartyLoad);
        kd.boss_killed = true;
        kd.member_here = true;
        CHECK(PlanDownTick(kd) == DownTick::ArenaRevive);     // the boss killed: up in the arena as before

        // Which deaths are scripted warps.
        CHECK(KidnapDeath(kGaolReturnPoint, false, false));   // 9421 ran: the record is the Gaol
        CHECK(KidnapDeath(2402950, true, false));             // 9420 set, 9421 not yet run
        CHECK(!KidnapDeath(kGaolReturnPoint, true, true));    // woke there already (9423): a plain death
        CHECK(!KidnapDeath(2402950, false, false));
        // 9404 (the first death with Insight): the record moved from a Cathedral Ward lamp
        // (m24_00) to the Dream (m21_00) between the last frame alive and the funnel.
        CHECK(RespawnMovedAtDeath(2402950, 2102961, 0x1500, 0x1800));
        CHECK(!RespawnMovedAtDeath(2402950, 2402950, 0x1800, 0x1800));  // unchanged
        CHECK(!RespawnMovedAtDeath(2402950, 2402951, 0x1800, 0x1800));  // moved within the map
        CHECK(!RespawnMovedAtDeath(0, 2102961, 0x1500, 0x1800));        // never seen alive: unknown
        CHECK(!RespawnMovedAtDeath(0xffffffffu, 2102961, 0x1500, 0x1800));
        CHECK(ScriptedDeathWarp(true, false) && ScriptedDeathWarp(false, true) && !ScriptedDeathWarp(false, false));
    }

    {
        // 0.7.4 host_limbo_mark: the host in its boss-fight limbo, the last living member leaves the
        // fight (Hunter's Mark, lamp, Leave, disconnect): a wipe, not a limbo until a rest.
        std::vector<MemberNow> roster(2);
        roster[0].slot = 1;
        roster[1].slot = 2;
        LimboFight f;
        CHECK(f.Fighters() == -1 && !LimboFightEmpty(f.Fighters(), 0));
        f.Begin(roster);
        CHECK(f.Fighters() == 2);
        auto none = [](int) { return false; };
        CHECK(f.Out(roster, none) == 0);
        CHECK(f.Left(1, 100.0));                 // slot 1: Hunter's Mark into this world, loading until 100
        CHECK(!f.Left(1, 100.0));                // once
        CHECK(!f.Left(5, 0.0));                  // not a fighter
        CHECK(f.Out(roster, none) == 1);
        CHECK(f.Settling(50.0) && !f.Settling(100.0));
        f.Arrived(1, 50.0);
        CHECK(f.Settling(51.0) && !f.Settling(53.0));  // a moment for its re-join to end
        CHECK(!LimboFightEmpty(f.Fighters(), f.Out(roster, none)));  // slot 2 fights on
        CHECK(f.Out(roster, [](int s) { return s == 2; }) == 2);       // ... until it is down
        std::vector<MemberNow> gone(1, roster[0]);
        CHECK(f.Out(gone, none) == 2);           // slot 2 left the party (Leave / disconnect)
        f.End();
        CHECK(f.Fighters() == -1);
        CHECK(!f.Left(1, 0.0));                  // no limbo: nothing recorded

        DownView v;
        v.members = 1;
        v.down_for = 5;
        v.limbo = true;
        v.boss_active = true;
        v.fighters = 1;
        v.fighters_out = 0;
        CHECK(PlanDownTick(v) == DownTick::Wait);       // the guest fights on: the limbo
        v.fighters_out = 1;
        v.fighter_settling = true;
        CHECK(PlanDownTick(v) == DownTick::Wait);       // its Mark's load into this world still runs
        v.fighter_settling = false;
        CHECK(PlanDownTick(v) == DownTick::Wipe);       // arrived: everyone respawns as in a wipe
        v.down_for = 0.1;
        CHECK(PlanDownTick(v) == DownTick::Wipe);       // also within the revive delay
        v.down_for = 5;
        v.fighters = 0;
        v.fighters_out = 0;
        CHECK(PlanDownTick(v) == DownTick::Wipe);       // nobody was fighting (all left before the death was taken)
        v.fighters = -1;
        CHECK(PlanDownTick(v) == DownTick::Wait);       // not tracked: as before
        v.fighters = 1;
        v.fighters_out = 1;
        v.boss_active = false;                           // the boss was killed meanwhile: not a wipe
        v.boss_killed = true;
        v.arena_revive_on = true;
        v.member_here = true;
        CHECK(PlanDownTick(v) == DownTick::ArenaRevive);
    }

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
    {
        // The guests killed the boss while the host lay in limbo: it gets up in the arena, no load.
        DownView a = d;
        a.boss_killed = true;
        a.member_here = true;
        CHECK(PlanDownTick(a) == DownTick::ArenaRevive);
        a.revive_opt_in = true;
        a.lamp_here = true;
        CHECK(PlanDownTick(a) == DownTick::ArenaRevive);  // beside the member, not the lamp warp
        a.boss_dying = true;
        CHECK(PlanDownTick(a) == DownTick::Wait);  // the defeat is saved first (PR #6)
        a.boss_dying = false;
        a.down_seen = 1;
        CHECK(PlanDownTick(a) == DownTick::Wipe);  // a wipe still loads
        a.down_seen = 0;
        a.arena_revive_on = false;
        CHECK(PlanDownTick(a) == DownTick::Revive);  // the switch off: as before (here the opt-in lamp revive)
        a.revive_opt_in = false;
        CHECK(PlanDownTick(a) == DownTick::PartyLoad);
        a.arena_revive_on = true;
        a.member_here = false;
        CHECK(PlanDownTick(a) == DownTick::PartyLoad);  // nobody alive to stand beside
        a.member_here = true;
        a.boss_killed = false;
        CHECK(PlanDownTick(a) == DownTick::PartyLoad);  // the bar went without a kill
        a.boss_killed = true;
        a.limbo = false;
        CHECK(PlanDownTick(a) == DownTick::PartyLoad);  // a death outside a boss fight: unchanged
        CHECK(ArenaReviveAtCorpse(true, false, 12.0, 0.5));
        CHECK(!ArenaReviveAtCorpse(false, false, 12.0, 0.5));  // no corpse spot kept
        CHECK(!ArenaReviveAtCorpse(true, true, 12.0, 0.5));    // a fall death: the member's spot
        CHECK(!ArenaReviveAtCorpse(true, false, 41.0, 0.5));   // not in the arena with the member
        CHECK(!ArenaReviveAtCorpse(true, false, 5.0, -4.5));   // another floor
        // fallsafe: a fall death gets up on the game's safe spot (the bloodstain's), never the corpse.
        CHECK(PlanArenaReviveSpot(true, true, 12.0, 0.5, true) == ArenaSpot::SafeFloor);
        CHECK(PlanArenaReviveSpot(true, true, 12.0, 0.5, false) == ArenaSpot::Member);  // no safe spot read
        CHECK(PlanArenaReviveSpot(true, true, 2.0, -40.0, true) == ArenaSpot::SafeFloor);  // corpse far below
        CHECK(PlanArenaReviveSpot(true, false, 12.0, 0.5, true) == ArenaSpot::Corpse);  // no fall: as before
        CHECK(PlanArenaReviveSpot(true, false, 41.0, 0.5, true) == ArenaSpot::Member);
        CHECK(PlanArenaReviveSpot(false, false, 1.0, 0.0, true) == ArenaSpot::Member);
        // The game's value is usable: read, finite, not the origin, near the HP 0 spot horizontally.
        const float died[3] = {-130.0f, -95.0f, 50.0f};  // 70 m below the bridge
        const float safe[3] = {-134.6f, -26.6f, 54.7f};
        CHECK(SafeSpotUsable(true, safe, died));
        CHECK(!SafeSpotUsable(false, safe, died));
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        CHECK(!SafeSpotUsable(true, zero, died));  // no tracker: the getter's zeros
        const float far[3] = {-200.0f, -26.6f, 54.7f};
        CHECK(!SafeSpotUsable(true, far, died));   // 70 m away: stale (another place)
        const float nan3[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f};
        CHECK(!SafeSpotUsable(true, nan3, died));
        CHECK(SafeSpotUsable(true, safe, nan3));   // the death spot unknown: the game's value alone
        // A revive where it lies (then the alone travel): a fall on the safe spot, or no revive here.
        CHECK(PlanLiesRevive(false, false) == LiesRevive::WhereItLies);
        CHECK(PlanLiesRevive(true, true) == LiesRevive::SafeFloor);
        CHECK(PlanLiesRevive(true, false) == LiesRevive::Load);
        // Outside an arena revive (user decision): a fall gets up at a lamp in the loaded map, no load.
        CHECK(PlanFallRespawn(true, true, true, true, true, false) == FallRespawn::OwnLamp);
        CHECK(PlanFallRespawn(true, true, false, true, true, false) == FallRespawn::HostLamp);   // guest: the host's
        CHECK(PlanFallRespawn(true, true, false, true, true, true) == FallRespawn::SafeFloor);   // the host: no "host lamp"
        CHECK(PlanFallRespawn(true, true, true, false, true, true) == FallRespawn::OwnLamp);
        CHECK(PlanFallRespawn(true, true, false, false, true, false) == FallRespawn::SafeFloor); // never a load (Dream)
        CHECK(PlanFallRespawn(true, true, false, false, false, false) == FallRespawn::Respawn);
        CHECK(PlanFallRespawn(true, false, true, true, true, false) == FallRespawn::Respawn);    // in-place revive off
        CHECK(PlanFallRespawn(false, true, true, true, true, false) == FallRespawn::Respawn);   // no fall: as before
        // chalicelamp: a guest's fall in the host's Chalice Dungeon (a living member there): beside it,
        // never a lamp's table spot nor a safe floor that was a lift car's stop; the host as before.
        CHECK(PlanFallRespawn(true, true, false, false, true, false, true) == FallRespawn::BesideMember);
        CHECK(PlanFallRespawn(true, true, true, true, true, false, true) == FallRespawn::BesideMember);
        CHECK(PlanFallRespawn(true, true, false, false, false, false, true) == FallRespawn::BesideMember);
        CHECK(PlanFallRespawn(true, true, false, false, true, true, true) == FallRespawn::SafeFloor);  // host role
        CHECK(PlanFallRespawn(true, false, false, false, true, false, true) == FallRespawn::Respawn);  // in-place off
        CHECK(PlanFallRespawn(false, true, false, false, true, false, true) == FallRespawn::Respawn);  // no fall
        CHECK(PlanFallRespawn(true, true, false, false, true, false, false) == FallRespawn::SafeFloor); // nobody alive
        {
            // The sequence of a death out of the world: HP 0 (the game's death plays), the death taken
            // (held), the body moved up, a settle, the revive in place - and no load.
            double t = 0;
            CHECK(PlanFallReviveStep(false, true, false, false, t) == FallReviveStep::Wait);  // dying
            CHECK(PlanFallRespawn(true, true, true, true, true, false) == FallRespawn::OwnLamp); // taken: no load
            CHECK(PlanFallReviveStep(true, true, false, false, t) == FallReviveStep::MoveBody);
            int frames = 0;
            for (t = 0; PlanFallReviveStep(true, true, true, true, t) == FallReviveStep::Wait; t += 1.0 / 30) ++frames;
            CHECK(frames >= 29);  // at least kFallSettle s of frames with the body on its spot first
        }
        // The order: never before the death is taken; a body out of the world moved first; then up.
        CHECK(PlanFallReviveStep(false, true, false, false, 0.0) == FallReviveStep::Wait);
        CHECK(PlanFallReviveStep(false, false, false, true, 0.0) == FallReviveStep::Wait);
        CHECK(PlanFallReviveStep(true, true, false, false, 0.0) == FallReviveStep::MoveBody);
        CHECK(PlanFallReviveStep(true, true, true, false, 0.5) == FallReviveStep::Wait);
        CHECK(PlanFallReviveStep(true, true, true, true, 0.0) == FallReviveStep::Wait);    // moved this frame
        CHECK(PlanFallReviveStep(true, true, true, true, 0.5) == FallReviveStep::Wait);    // physics settling
        CHECK(PlanFallReviveStep(true, true, true, true, kFallSettle) == FallReviveStep::Revive);
        CHECK(PlanFallReviveStep(true, true, true, false, kFallMoveWait) == FallReviveStep::Revive);
        CHECK(PlanFallReviveStep(true, false, false, false, 0.0) == FallReviveStep::Revive);  // landed near
    }
    {
        // PR #6's wait: the bar over the dead boss, or 3 s after it went; at most 20 s.
        CHECK(DefeatPending(0.5, true));
        CHECK(DefeatPending(1.0, false));
        CHECK(DefeatPending(10.0, true));
        CHECK(!DefeatPending(3.5, false));
        CHECK(!DefeatPending(21.0, true));
        // A guest dead in the fight when the boss died (user request 2026-10-21): up in the arena.
        CHECK(PlanGuestLimboEnd(true, true, false, true) == GuestLimboEnd::ArenaRevive);
        CHECK(PlanGuestLimboEnd(true, true, true, true) == GuestLimboEnd::Wait);    // the defeat saved first
        CHECK(PlanGuestLimboEnd(true, true, true, false) == GuestLimboEnd::Wait);
        CHECK(PlanGuestLimboEnd(true, true, false, false) == GuestLimboEnd::Lamp);  // nobody alive here
        CHECK(PlanGuestLimboEnd(false, true, false, true) == GuestLimboEnd::Lamp);  // the bar went, no kill
        CHECK(PlanGuestLimboEnd(true, false, false, true) == GuestLimboEnd::Lamp);  // the switch off: as before
        CHECK(PlanGuestLimboEnd(true, false, true, true) == GuestLimboEnd::Lamp);   // no wait either
        // Host and a guest dead, the other guest's kill: both up in the arena (the host by
        // PlanDownTick, the guest here); a wipe is the host's death load (PlanDownTick Wipe).
        DownView h = d;
        h.limbo = true;
        h.boss_killed = true;
        h.member_here = true;
        h.down_reported = 1;  // guest 1 down; guest 2 alive
        h.members = 2;
        CHECK(PlanDownTick(h) == DownTick::ArenaRevive);
        CHECK(PlanGuestLimboEnd(true, true, false, true) == GuestLimboEnd::ArenaRevive);
        h.down_reported = 2;
        CHECK(PlanDownTick(h) == DownTick::Wipe);
    }
    {
        // The boss killed between the host's HP 0 and its taken death (hostcheck run C: HP 0 at
        // +169.5 s, the kill at +181.4 s, the death taken at +182.2 s): the limbo, then the arena.
        CHECK(KilledAfterHp0(true, true, 169.5, 181.4, 182.2));
        CHECK(!KilledAfterHp0(false, true, 169.5, 181.4, 182.2));  // no living boss bar at HP 0
        CHECK(!KilledAfterHp0(true, false, 169.5, 181.4, 182.2));  // the bar went without a kill
        CHECK(!KilledAfterHp0(true, true, 169.5, 160.0, 182.2));   // that fight ended before HP 0
        CHECK(!KilledAfterHp0(true, true, 0.0, 181.4, 182.2));     // no HP 0 seen (a load since)
        CHECK(!KilledAfterHp0(true, true, 100.0, 120.0, 161.0));   // 61 s: not this death's fight
        HostDeathView v;
        v.members = 1;
        v.revive_opt_in = true;
        v.lamp_here = true;
        CHECK(PlanHostDeath(v) == HostDeathPlan::ReviveInPlace);   // before: the lamp revive
        v.boss_killed_late = true;
        CHECK(PlanHostDeath(v) == HostDeathPlan::BossLimbo);
        CHECK(PlanTakesFunnel(PlanHostDeath(v)));
        v.arena_revive_on = false;
        CHECK(PlanHostDeath(v) == HostDeathPlan::ReviveInPlace);   // the switch off: as before
        v.arena_revive_on = true;
        v.down_reported = 1;
        CHECK(PlanHostDeath(v) == HostDeathPlan::Wipe);            // the guest is down too: a wipe
        // Then each frame: the wait for the defeat (PR #6), then up in the arena; nobody here alive:
        // the lamp revive.
        DownView h;
        h.members = 1;
        h.limbo = true;
        h.boss_killed = true;
        h.revive_opt_in = true;
        h.lamp_here = true;
        h.down_for = 2.0;
        h.member_here = true;
        h.boss_dying = DefeatPending(0.5, true);
        CHECK(PlanDownTick(h) == DownTick::Wait);
        h.boss_dying = DefeatPending(4.0, false);
        CHECK(PlanDownTick(h) == DownTick::ArenaRevive);
        h.member_here = false;
        CHECK(PlanDownTick(h) == DownTick::Revive);
    }

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
    // Live 2026-10-20: the Dream's talk state (Doll handshake, Gehrman's lines) and 70000100 are no
    // lamp use; they stay out of the mirror as talk state.
    for (std::uint32_t f : {72100100u, 72100101u, 72100102u, 72100123u, 72100124u, 72100136u, 70000100u}) {
        CHECK(!LampTriggerFlag(f));
        CHECK(TalkStateFlag(f));
    }
    for (std::uint32_t f : {72110100u, 72410101u, 72102110u, 72410201u, 9401u}) {
        CHECK(!TalkStateFlag(f));
    }
    CHECK(PlanLampOrigin(-1, -1) == LampOrigin::Other);     // no own flag: another game's warp event
    CHECK(PlanLampOrigin(-1, -1, false) == LampOrigin::Own); // no flag hook: ask as before
    // A Chalice Dungeon's lamps stand where the generated layout puts them, not at the table's spot.
    CHECK(LampTableSpotTrusted(2412952) && LampTableSpotTrusted(2102950) && LampTableSpotTrusted(3602952));
    CHECK(!LampTableSpotTrusted(2902950) && !LampTableSpotTrusted(2902952) && !LampTableSpotTrusted(2902959));
    // mensislamp: a guest whose own last lamp is a scripted arrival spot gets up at the host's real lamp of that map
    CHECK(ScriptedArrivalSpot(2602959) && ScriptedArrivalSpot(2802958) && !ScriptedArrivalSpot(2602950));
    CHECK(!ScriptedArrivalSpot(2902959) && !ScriptedArrivalSpot(2102961));
    CHECK(ScriptedArrivalSpot(2802959) && !ScriptedArrivalSpot(2802950) && !ScriptedArrivalSpot(2802951));  // Yahar'gul
    CHECK(GuestArrivalLamp(2802958, 2802950) == 2802950);
    CHECK(GuestArrivalLamp(2602959, 2602950) == 2602950);
    CHECK(GuestArrivalLamp(2602959, 2602959) == 2602959);   // the host is at the arrival too
    CHECK(GuestArrivalLamp(2602959, 2502950) == 2602959);   // the host's lamp is in another map
    CHECK(GuestArrivalLamp(2602950, 2602953) == 2602950);   // a real own lamp stays
    CHECK(GuestArrivalLamp(2602959, 0xffffffffu) == 2602959 && GuestArrivalLamp(2602959, 0) == 2602959);
    // Option b (2026-10-21): death / forced -> home and summoned; chosen -> stays; rule off -> stays.
    CHECK(PlanChaliceApart(true, false, 2.0) == ChaliceApart::Wait);
    CHECK(PlanChaliceApart(true, false, kChaliceApartWait) == ChaliceApart::SendHome);
    CHECK(PlanChaliceApart(true, true, 600.0) == ChaliceApart::Stay);
    CHECK(PlanChaliceApart(false, false, 600.0) == ChaliceApart::Stay);
    // Rejoin into a dungeon: leave the session first only when still in it, apart, host in a dungeon.
    CHECK(ChaliceRejoinLeavesFirst(true, true, true));
    CHECK(!ChaliceRejoinLeavesFirst(false, true, true) && !ChaliceRejoinLeavesFirst(true, false, true) &&
          !ChaliceRejoinLeavesFirst(true, true, false));
    CHECK(std::string(ChaliceApartNotice()) == "The host is in a Chalice Dungeon: Party > Rejoin to go back");
    // A re-summoned guest never gets a dead boss's clear again; a real kill (later) pays as before.
    CHECK(StaleBossClear(true, 0.5) && StaleBossClear(true, 14.9));
    CHECK(!StaleBossClear(true, 15.0) && !StaleBossClear(true, 300.0));
    CHECK(!StaleBossClear(false, 0.5));        // the host's own clear
    CHECK(!StaleBossClear(true, -1.0));         // no arrival seen yet: as before
    // The dungeon lamps' "Return to the Hunter's Dream" flags (m29 event 12907020) are lamp flags.
    CHECK(LampTriggerFlag(72900100) && LampTriggerFlag(72900108));
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
    {
        // A peer's lamp packets are dropped (live 0.2.2 b1: a guest's lone Dream trip used up the
        // host's event 7200 for that lamp; the host's own Dream choice there then did nothing).
        const std::uint32_t dream_flag[5] = {72400100};     // Cathedral Ward lamp -> Dream (0x0f)
        const std::uint32_t headstone[5] = {72102950};      // a Dream headstone
        const std::uint32_t quest_flag[5] = {72400480};     // an NPC quest flag: not a lamp's
        const std::uint32_t talk_flag[5] = {72100100};      // the Dream's talk state: not a lamp's
        CHECK(PeerLampPacket(0xf, 4, dream_flag));
        CHECK(PeerLampPacket(0xf, 4, headstone));
        CHECK(!PeerLampPacket(0xf, 4, quest_flag));
        CHECK(!PeerLampPacket(0xf, 4, talk_flag));
        CHECK(!PeerLampPacket(0xf, 8, dream_flag));          // not the 4-byte flag packet
        const std::uint32_t lamp_event[5] = {10, 7200, 72400100, 2401950, 2102950};
        const std::uint32_t headstone_event[5] = {0, 12107000, 0, 0, 0};
        const std::uint32_t other_event[5] = {10, 7600, 2401999, 2403999, 0};
        CHECK(PeerLampPacket(0xe, 0x14, lamp_event));
        CHECK(PeerLampPacket(0xe, 0x14, headstone_event));
        CHECK(!PeerLampPacket(0xe, 0x14, other_event));
        CHECK(!PeerLampPacket(0xe, 4, lamp_event));          // wrong size: left alone
        CHECK(!PeerLampPacket(0x10, 4, dream_flag));         // other packet types: left alone
        CHECK(!PeerLampPacket(0xf, 4, nullptr));
    }

    // A guest's lamp (bug 10, bug 3).
    CHECK(PlanGuestLamp(false, false, true, false) == GuestLamp::Plain);
    CHECK(PlanGuestLamp(true, true, true, false) == GuestLamp::Plain);    // standing at it
    CHECK(PlanGuestLamp(true, false, true, false) == GuestLamp::SoloLoad); // same map: own load
    CHECK(PlanGuestLamp(true, true, true, true) == GuestLamp::SoloLoad);   // a Mark to the lamp here
    CHECK(PlanGuestLamp(true, false, false, false) == GuestLamp::Ask);
    CHECK(PlanGuestLamp(true, false, false, true) == GuestLamp::Ask);
    CHECK(!GuestAloneRestsHostWorld(true, false, false));  // travels away alone: no host reload (Nightwatch)
    CHECK(!GuestAloneRestsHostWorld(false, false, false));  // YES: the party travel is the rest
    CHECK(GuestAloneRestsHostWorld(true, true, false));  // D9 (pard4): a lamp of the loaded map rests too
    // 0.7.3: a guest leaving the Hunter's Dream alone (headstone, NO) is a plain travel: no rest load
    CHECK(!GuestAloneRestsHostWorld(true, false, true));
    CHECK(!GuestAloneRestsHostWorld(true, true, true));
    CHECK(!GuestAloneRestsHostWorld(false, false, true));

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
    // a die step whose chr was killed first (the boss): done, the chain goes on after the respawn
    CHECK(TestDieStepLatches(true, false, true, false, false));
    CHECK(!TestDieStepLatches(true, true, true, false, false));      // alive: the step kills it
    CHECK(!TestDieStepLatches(false, false, true, false, false));    // not a die step: waits
    CHECK(!TestDieStepLatches(true, false, true, true, false));      // in a re-join / load
    CHECK(!TestDieStepLatches(true, false, false, false, false));
    CHECK(!TestDieStepLatches(true, false, true, false, true));      // the previous die step's own death
    CHECK(TestStepWaits(false, true, true, true));                   // inside the last wipe's re-join
    CHECK(TestStepWaits(false, true, false, false));                 // not in the world
    // BB_PARTY_TEST_GUARD: invincible from the revive after a death until the next die / chr step
    {
        TestGuard g;
        CHECK(!TestGuardTick(g, 600, true));                         // never died: no guard
        CHECK(!TestGuardTick(g, 0, true));                           // killed by the enemy
        CHECK(!TestGuardTick(g, -1, false));                         // the respawn load
        CHECK(TestGuardTick(g, 600, true));                          // up again: invincible
        CHECK(TestGuardTick(g, 600, true));
        CHECK(!TestGuardStepRan(g, false));                          // a press / move: stays on
        CHECK(TestGuardTick(g, 600, true));
        CHECK(TestGuardStepRan(g, true));                            // the scripted death: off (clear)
        CHECK(!TestGuardTick(g, 600, true));
        CHECK(!TestGuardTick(g, 0, true));                           // that death
        CHECK(TestGuardTick(g, 600, true));                          // up again: on until the next one
        CHECK(TestGuardStepRan(g, true));                            // guest_chr: be killed again
        CHECK(!TestGuardStepRan(g, true));                           // nothing to clear twice
        CHECK(!TestGuardTick(g, 600, true));
    }

    // Own bloodstain record changes (bug 13 logging).
    CHECK(PlanStainChange(0, -1, 1, 4843) == StainChange::Recorded);
    CHECK(PlanStainChange(1, 4843, 1, -1) == StainChange::Taken);
    CHECK(PlanStainChange(1, 4843, 0, 4843) == StainChange::Taken);
    CHECK(PlanStainChange(1, 4843, 1, 2341) == StainChange::Replaced);  // a second death overwrote it
    CHECK(PlanStainChange(1, 4843, 1, 4843) == StainChange::None);
    CHECK(PlanStainChange(0, -1, 0, -1) == StainChange::None);

    // echodup (player report: 15k from the glowing-eyed enemy and 15k again from the stain).
    CHECK(PlanStainHold(true, 0, false, 99.0) == StainHold::Free);     // on the ground: the party stain pays
    CHECK(PlanStainHold(true, 1, true, 0.0) == StainHold::Held);       // an enemy took it: kill it
    CHECK(PlanStainHold(true, 1, false, 3.0) == StainHold::Held);      // its death still running
    CHECK(PlanStainHold(true, 1, false, 11.0) == StainHold::Void);     // hold without its enemy: recoverable
    CHECK(PlanStainHold(false, 1, true, 0.0) == StainHold::Free);      // another record (not this stain)
    // The holder's kill paid the record (0x130AB80: flag 0, +0x38 -1, echoes +15000): the party
    // stain goes, its touch no longer pays.
    CHECK(StainTakenByGame(true, -1, 15000, 15000));
    CHECK(StainTakenByGame(true, -1, 15000, 14990));                  // a kill's echoes on top, or rot
    CHECK(!StainTakenByGame(true, 15000, 15000, 15000));               // record unchanged: someone else's kill
    CHECK(!StainTakenByGame(true, -1, 15000, 120));                    // only a kill's own echoes came
    CHECK(!StainTakenByGame(false, -1, 15000, 15000));                 // the record was not this stain
    CHECK(!StainTakenByGame(true, -1, 0, 0));
    CHECK(RestoreHold(true, 0, true));                                 // eh2: the hold dropped, the enemy still glows
    CHECK(!RestoreHold(true, 1, true));
    CHECK(!RestoreHold(true, 0, false));                               // nobody has them: the ground stain stays
    CHECK(!RestoreHold(false, 0, true));                               // another record
    CHECK(RetakeHold(true, 0, true, 0.4, false));                       // echohold: the game let it go, it is alive
    CHECK(!RetakeHold(true, 1, true, 0.4, false));                      // still held
    CHECK(!RetakeHold(true, 0, false, 0.4, false));                     // the holder died / unloaded
    CHECK(!RetakeHold(true, 0, true, 5.0, false));                      // seen long ago (a new holder is the game's)
    CHECK(!RetakeHold(true, 0, true, -1.0, false));                     // never seen holding
    CHECK(!RetakeHold(true, 0, true, 0.4, true));                       // we let it go (death, pickup)
    CHECK(!RetakeHold(false, 0, true, 0.4, false));                     // another record
    // A second death before recovery: the old holder lets go (the old echoes are lost).
    CHECK(ReleaseHoldAtDeath(true, 1));
    CHECK(!ReleaseHoldAtDeath(true, 0));
    CHECK(!ReleaseHoldAtDeath(false, 1));                              // a respawn load's record call

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
    // cainhurst_death_loop: a fall death's ChrCtrl bit 1 is cleared by the revive too (03 -> 00, not 02)
    CHECK(ReviveCtrl38Clear(0x03) == 0x00);
    CHECK(ReviveCtrl38Clear(0x01) == 0x00);
    CHECK(ReviveCtrl38Clear(0x03, false) == 0x02);  // the old clear, the failure
    CHECK(ReviveCtrl38Clear(0x84) == 0x84);
    // fallcam: the fall-death camera latch is released by every revive / in-world warp that finds it
    // set; the camera is reset behind the chr whenever it was moved or the latch was on.
    {
        const ReviveCamera djura = PlanReviveCamera(true, true);  // the report: latch 1, the lamp 158 m away
        CHECK(djura.release_latch && djura.cam_reset);
        const ReviveCamera lies = PlanReviveCamera(true, false);  // up where it lies after a fall (latch on)
        CHECK(lies.release_latch && lies.cam_reset);
        const ReviveCamera lamp = PlanReviveCamera(false, true);  // a normal death at its lamp (s1 / s3)
        CHECK(!lamp.release_latch && lamp.cam_reset);
        const ReviveCamera none = PlanReviveCamera(false, false); // an arena revive in place, no fall
        CHECK(!none.release_latch && !none.cam_reset);
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(JudgeReviveView(4.5, 0.1, false) == ReviveView::Ok);           // the follow camera behind it
    CHECK(JudgeReviveView(1.0, nan, false) == ReviveView::Ok);           // a wall pushed it near; no matrix
    CHECK(JudgeReviveView(4.5, 0.1, true) == ReviveView::FallCamera);    // the latch left on
    CHECK(JudgeReviveView(158.0, 0.1, false) == ReviveView::CameraAway); // the report: the camera at the fall
    CHECK(JudgeReviveView(kReviveCamNear + 0.1, 0.0, false) == ReviveView::CameraAway);
    CHECK(JudgeReviveView(kReviveCamNear, 0.0, false) == ReviveView::Ok);
    CHECK(JudgeReviveView(5.0, 158.0, false) == ReviveView::DrawnAway);  // drawn at the old spot
    CHECK(JudgeReviveView(nan, 0.0, false) == ReviveView::NoCamera);     // headless without frames
    CHECK(JudgeReviveView(nan, 0.0, true) == ReviveView::FallCamera);    // the latch is judged without frames
    CHECK(JudgeReviveView(4.9, nan, false, -0.78) == ReviveView::Steep);   // fdj r3: the fall camera's pitch
    CHECK(JudgeReviveView(5.1, nan, false, -0.98) == ReviveView::Steep);
    CHECK(JudgeReviveView(4.0, nan, false, -0.00) == ReviveView::Ok);      // fl r1: the level follow view
    CHECK(JudgeReviveView(4.0, nan, false, -0.3) == ReviveView::Ok);
    CHECK(JudgeReviveView(4.0, nan, true, -0.9) == ReviveView::FallCamera);  // the latch first
    CHECK(JudgeReviveView(158.0, nan, false, -0.9) == ReviveView::CameraAway);
    CHECK(std::string(ReviveViewName(ReviveView::Steep)).rfind("BAD", 0) == 0);
    // the level reset after a release, a warp, or a ChrCam still in its fall-die mode
    CHECK(LevelCameraDue(true, true, 0));
    CHECK(LevelCameraDue(true, false, 0));
    CHECK(LevelCameraDue(false, true, 0));      // a normal death's lamp revive: level as after a load
    CHECK(LevelCameraDue(false, false, kChrCamFallDieMode));
    CHECK(!LevelCameraDue(false, false, 0));    // up where it lies, no fall: the camera as it is
    CHECK(!LevelCameraDue(false, false, 1));
    // fallcam 3: a ladder action state left on the chr is put back to none by the revive
    CHECK(LadderLeftover(0));              // on a ladder (state 0 .. n)
    CHECK(LadderLeftover(3));
    CHECK(!LadderLeftover(kNoLadder));     // the constructor's 0xFF: no ladder
    CHECK(!LadderLeftover(kLadderUnread)); // unreadable (no ChrCtrl)
    CHECK(WalkedToward(0.0, 3.0, 0.0, 4.0));    // 3 m of a 4 m walk north
    CHECK(WalkedToward(-2.0, 0.1, -4.0, 0.0));  // west
    CHECK(!WalkedToward(0.0, 3.0, 4.0, 0.0));   // asked east, went north: stuck on one axis
    CHECK(!WalkedToward(0.0, -0.5, 0.0, 4.0));  // backwards
    CHECK(!WalkedToward(1.0, 1.0, 0.0, 0.0));   // no target
    CHECK(std::string(ReviveViewName(ReviveView::Ok)) == "ok");
    CHECK(std::string(ReviveViewName(ReviveView::FallCamera)).rfind("BAD", 0) == 0);
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
    // revivecheck live: deaths with no drop and hp+0x200 0x7000 (an enemy's kill), 0xD000 and 0xF800
    // (ForceDeads) are no falls; only the +0x1E1 flags are.
    CHECK(!FallDeathState(0x0001001098ull, 0x7000) && !FallDeathState(0x0001001098ull, 0xd000));
    CHECK(!FallDeathState(0x0001001098ull, 0xf800));
    CHECK(PlanGuestRevive(0.0, true, FallDeathState(0x0001001098ull, 0xf800)) == GuestRevive::InPlace);
    CHECK(PlanGuestRevive(0.0, true, FallDeathState(0x0001001098ull, 0x8000)) == GuestRevive::InPlace);
    CHECK(FallDrop(-20.0, -40.0, -80.0) == 60.0);                            // into the void: still dropping
    CHECK(FallDrop(-20.0, -21.0, -21.0) == 1.0);
    // liftfall: lifts and moving objects. The report's remote chr flags: 0200001098 / 0200081098 on
    // the Mensis lift, 0001001098 off it; the game's fall flags (0x0400000040) are another bit.
    CHECK(RidingObject(0x0200001098ull) && RidingObject(0x0200081098ull) && RidingObject(0x020000108cull));
    CHECK(!RidingObject(0x0001001098ull) && !RidingObject(0x04010010d8ull) && !RidingObject(0));
    CHECK(!FallDeathState(0x0200001098ull, 0) && !FallDeathState(0x020000108cull, 0xf800));  // a body in a car
    // The recovery samples: the own-world net (1 s, 1.5 m/s) and the host's-world net (0.5 s, 1.4 m/s).
    CHECK(FallSample(4.0, 1.0, 1.5, false));          // a lift without the ride flag: a fall sample ...
    CHECK(!FallSample(4.0, 1.0, 1.5, true));          // ... with it: a ride
    CHECK(!FallSample(2.0, 0.5, 1.4, true));          // the host's-world net, the same ride
    CHECK(FallSample(30.0, 1.0, 1.5, true));          // far faster than any lift: a fall, flag or not
    CHECK(!FallSample(1.0, 1.0, 1.5, false));         // a slope
    CHECK(!FallSample(-4.0, 1.0, 1.5, false));        // up
    CHECK(!FallSample(4.0, 0.0, 1.5, false) && !FallSample(std::nan(""), 1.0, 1.5, false));
    // ... and the nets' decision: 6 s and a real fall's drop. The report: 27 m of the Mensis lift in
    // 6.5 s fired (warped to the lamp); a fall out of the world covers 100+ m in that time.
    CHECK(!FallRecoveryDue(6.5, 27.0, 30.0) && !FallRecoveryDue(15.0, 39.0, 30.0));
    CHECK(FallRecoveryDue(6.0, 150.0, 30.0) && !FallRecoveryDue(5.9, 150.0, 30.0));
    // liftfix: and only after a sample faster than any lift. A 60 m ride at ~5 m/s whose car the ride
    // test misses (a Chalice Dungeon's generated lift) drops 40 m in 8 s: no fall; a free fall from the
    // Great Bridge passes 10 m/s within a second (full smoke 0.7.1: a fall off the Mensis car, 11-23 m/s).
    CHECK(!FallRecoveryDue(8.0, 40.0, 5.1) && !FallRecoveryDue(12.0, 60.0, 12.7));  // 12.7: Cainhurst's peak
    CHECK(FallRecoveryDue(6.0, 150.0, 23.0) && FallRecoveryDue(6.0, 40.0, kLiftMaxRate));
    // frontierfall: a story warp's chr is held while its question is pending and for the grace after it.
    CHECK(StoryWarpHold(0.0, true, false, true) && StoryWarpHold(25.0, true, false, true));
    CHECK(StoryWarpHold(5.0, false, false, true) && !StoryWarpHold(kStoryHoldGrace + 1.0, false, false, true));
    CHECK(!StoryWarpHold(-1.0, true, false, true) && !StoryWarpHold(3.0, true, true, true));   // none yet / a load
    CHECK(!StoryWarpHold(3.0, true, false, false) && !StoryWarpHold(kStoryHoldMax + 1.0, true, false, true));
    // The drop test around a death: the peak counts from the last sample on a moving object.
    {
        std::vector<HeightSample> ride = {{0.0, 1003.8, true}, {1.0, 999.8, true}, {2.0, 995.8, true}, {3.0, 991.8, true}};
        CHECK(PeakSinceRide(ride, 991.6) == 991.8);                           // killed in the car: no fall
        CHECK(PlanGuestRevive(FallDropRide(PeakSinceRide(ride, 991.6), 991.6, 943.8, true), true, false) ==
              GuestRevive::InPlace);                                          // the body rides on down
        std::vector<HeightSample> off = {{0.0, 1003.8, true}, {0.5, 1001.8, true}, {1.0, 999.0, false},
                                         {2.0, 985.0, false}};                 // out of the car, down the shaft
        CHECK(PeakSinceRide(off, 960.0) == 1001.8);
        CHECK(PlanGuestRevive(FallDropRide(PeakSinceRide(off, 960.0), 960.0, 960.0, false), true, false) ==
              GuestRevive::LoadFall);
        std::vector<HeightSample> onto = {{0.0, 1003.8, false}, {1.0, 990.0, false}, {2.0, 960.0, false}};
        CHECK(PeakSinceRide(onto, 943.8) == 1003.8);                          // a fall onto the car counts
        CHECK(FallDropRide(1003.8, 943.8, 900.0, true) == 60.0);              // not followed down in the car
        CHECK(FallDropRide(-20.0, -40.0, -80.0, false) == 60.0);              // the void: as FallDrop
        std::vector<HeightSample> none;
        CHECK(PeakSinceRide(none, 5.0) == 5.0);
    }
    CHECK(GuestReviveName(GuestRevive::LoadFall)[0] != '?');
    CHECK(RestLoadName(RestLoad::TooSoon)[0] != '?');
    // K12: the guest's HostDead state (the host's death event) on a party guest.
    CHECK(SkipGuestHostDead(true, 6, 1, 1, 1));      // a summoned guest, default: skipped
    CHECK(SkipGuestHostDead(true, 6, 0, 1, 4));      // chr type 0 after a B2 load: skipped too
    CHECK(!SkipGuestHostDead(false, 6, 1, 1, 1));    // no B2 party: the game's
    CHECK(!SkipGuestHostDead(true, 3, 1, 1, 1));     // the host: the game's (it returns at once)
    CHECK(!SkipGuestHostDead(true, 6, 2, 1, 1));     // an invader: the game's PK-success path
    CHECK(!SkipGuestHostDead(true, 6, 8, 1, 1));
    CHECK(!SkipGuestHostDead(true, 6, 1, 0, 5));     // BB_PARTY_HOSTDEAD_SKIP=0: never
    CHECK(!SkipGuestHostDead(true, 6, 1, 3, 2));     // =3: the first two host deaths the game's
    CHECK(SkipGuestHostDead(true, 6, 1, 3, 3));
    // C3, the last guest down starts the wipe (player report 2026-10-21, Logarius): its follow of
    // the host's death load waits until its own death is taken.
    CHECK(PlanWipeFollow(false, 0, false, 0.0) == WipeFollow::Now);     // not a host death travel
    CHECK(PlanWipeFollow(true, 527, false, 0.0) == WipeFollow::Now);    // alive: follows at once
    CHECK(PlanWipeFollow(true, -1, false, 0.0) == WipeFollow::Now);     // HP unknown: as before
    CHECK(PlanWipeFollow(true, 0, false, 0.1) == WipeFollow::WaitDeath);  // the report: dying at the travel
    CHECK(PlanWipeFollow(true, 0, false, 8.6) == WipeFollow::WaitDeath);  // the game's death ~8.6 s after HP 0
    CHECK(PlanWipeFollow(true, 0, true, 8.6) == WipeFollow::DeadFollow);  // taken: one dead follow
    CHECK(PlanWipeFollow(true, 0, true, 0.0) == WipeFollow::DeadFollow);  // taken before the travel (s1 wipe)
    CHECK(PlanWipeFollow(true, 0, false, kWipeDeathWait) == WipeFollow::TakeDeath);  // never taken: safety
    CHECK(PlanWipeFollow(true, 0, false, 60.0) == WipeFollow::TakeDeath);
    CHECK(std::string(WipeFollowName(WipeFollow::WaitDeath)) == "wait for the death");
    // A death taken after the wipe's arrival is part of the wipe: no revive at its own lamp.
    CHECK(PlanHeldAfterWipe(5.0, true) == AfterWipe::Drop);         // the report: up at the host's lamp already
    CHECK(PlanHeldAfterWipe(5.0, false) == AfterWipe::HostLamp);    // still down: the host's lamp, not its own
    CHECK(PlanHeldAfterWipe(0.0, false) == AfterWipe::HostLamp);
    CHECK(PlanHeldAfterWipe(kWipeArrivalWindow, false) == AfterWipe::None);  // a later death of its own
    CHECK(PlanHeldAfterWipe(-1.0, false) == AfterWipe::None);       // no wipe arrival (never: now - 0 is large)
    CHECK(PlanHeldAfterWipe(1e9, true) == AfterWipe::None);
    // A dead guest waits for the host's decision as long as the host is down (64.5 s in the report).
    CHECK(GuestWaitsForHostDown(true, 64.5));
    CHECK(GuestWaitsForHostDown(true, 0.0));
    CHECK(!GuestWaitsForHostDown(true, kGuestHostDownHold));         // a stale host chr: not for good
    CHECK(!GuestWaitsForHostDown(false, 1.0));                       // the host is up: its own rules
    // The bloodstain where the guest died, not at the load's destination.
    CHECK(StainAtDeathSpot(true, true));
    CHECK(!StainAtDeathSpot(true, false));   // the usual death: the game's own record
    CHECK(!StainAtDeathSpot(false, true));   // HP 0 spot unknown
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-death-rules-test: ok\n");
    return 0;
}
