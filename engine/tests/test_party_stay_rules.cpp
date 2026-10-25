// SPDX-License-Identifier: GPL-3.0-or-later
// A guest the party keeps where it stands (gpu/shim/party/party_stay_rules.h).
// cmake --build out/gpu --target party-stay-rules-test && out/gpu/party-stay-rules-test
#include "party/party_stay_rules.h"
#include "party/party_death_rules.h"

#include <cstdio>

using namespace coop::stayrules;

static int g_fail = 0;
#define CHECK(x)                                                     \
    do {                                                             \
        if (!(x)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static HoldView Held(Keep k) {
    HoldView v;
    v.party_guest = true;
    v.kept = k;
    v.since_kept = 1.0;
    v.world_up = true;
    v.player = true;
    v.load_wait = 1;
    v.hold = 1;
    return v;
}

int main() {
    // The report: the host travelled alone out of the Dream, OnLeave_Limit's send-home was dropped,
    // the leave state's load wait / hold stayed: lifted at once, the headstones work.
    CHECK(PlanActionHold(Held(Keep::HostApart)) == HoldAction::Lift);
    {
        HoldView v = Held(Keep::HostApart);
        v.load_wait = 0;  // only the copy left (it comes back next frame from +0x8D anyway)
        CHECK(PlanActionHold(v) == HoldAction::Lift);
        v.load_wait = 1;
        v.hold = 0;
        CHECK(PlanActionHold(v) == HoldAction::Lift);
        v.load_wait = 0;
        CHECK(PlanActionHold(v) == HoldAction::None);  // nothing held: nothing written
        v.load_wait = -1;
        v.hold = -1;
        CHECK(PlanActionHold(v) == HoldAction::None);  // unreadable: nothing written
    }
    // 0.7.5 guest Dream report: a world lamp's Dream flag -> that lamp's event warp point; a guest's flag still
    // set 1.5 s later with no warp = the lamp's event 7200 is used up.
    CHECK(WorldLampDreamPoint(72200100) == 2102951);  // Hemwick
    CHECK(WorldLampDreamPoint(72200101) == 2102951);
    CHECK(WorldLampDreamPoint(72410100) == 2102950);  // 1st Floor Sickroom
    CHECK(WorldLampDreamPoint(72410103) == 2102950);
    CHECK(WorldLampDreamPoint(72500102) == 2102952);  // Nightmare Frontier lamps (common.emevd: 2102952)
    CHECK(WorldLampDreamPoint(72600103) == 2102953);
    CHECK(WorldLampDreamPoint(72800102) == 2102952);
    CHECK(WorldLampDreamPoint(72800103) == 2102953);
    CHECK(WorldLampDreamPoint(73200101) == 2102953);
    CHECK(WorldLampDreamPoint(73200102) == 2102951);
    CHECK(WorldLampDreamPoint(72102410) == 0);  // a headstone flag
    CHECK(WorldLampDreamPoint(72100100) == 0);  // the Dream's talk state
    CHECK(WorldLampDreamPoint(2202951) == 0);   // a lamp id, not a flag
    CHECK(WorldLampDreamPoint(72200110) == 0);
    CHECK(WorldLampWarpFallback(true, true, 1.6, false, true, false));
    CHECK(!WorldLampWarpFallback(true, true, 1.0, false, true, false));   // the event may still clear it
    CHECK(!WorldLampWarpFallback(true, true, 2.0, false, false, false));  // cleared: the event runs
    CHECK(!WorldLampWarpFallback(true, true, 2.0, true, true, false));    // a warp followed
    CHECK(!WorldLampWarpFallback(false, true, 2.0, false, true, false));  // not in a party (a guest or the host)
    CHECK(!WorldLampWarpFallback(true, false, 2.0, false, true, false));  // in the Dream: headstones
    CHECK(!WorldLampWarpFallback(true, true, 2.0, false, true, true));    // once
    CHECK(!WorldLampWarpFallback(true, true, 9.6, false, true, false));   // past the own-use window
    // Never without a keep: the game's own hold (a guest on its way home, a boss clear before K10).
    {
        HoldView v = Held(Keep::None);
        CHECK(PlanActionHold(v) == HoldAction::None);
        v.party_guest = false;
        CHECK(PlanActionHold(v) == HoldAction::None);
    }
    // The session ended (or this game is no guest any more): the keep is forgotten.
    {
        HoldView v = Held(Keep::HostApart);
        v.party_guest = false;
        CHECK(PlanActionHold(v) == HoldAction::End);
        v = Held(Keep::BossClear);
        v.party_guest = false;
        CHECK(PlanActionHold(v) == HoldAction::End);
    }
    // A load: the host-apart lift ends (the load builds a new Lua context); the boss-clear one
    // stays for the session but writes nothing during the load.
    {
        HoldView v = Held(Keep::HostApart);
        v.loading = true;
        CHECK(PlanActionHold(v) == HoldAction::End);
        v = Held(Keep::BossClear);
        v.loading = true;
        CHECK(PlanActionHold(v) == HoldAction::None);
        CHECK(KeepEndsAtLoad(Keep::HostApart));
        CHECK(!KeepEndsAtLoad(Keep::BossClear));
        CHECK(!KeepEndsAtLoad(Keep::None));
    }
    // Not in the world / no player: wait.
    {
        HoldView v = Held(Keep::HostApart);
        v.world_up = false;
        CHECK(PlanActionHold(v) == HoldAction::None);
        v = Held(Keep::HostApart);
        v.player = false;
        CHECK(PlanActionHold(v) == HoldAction::None);
    }
    // The boss-clear test aids (issue #4) apply to it only; the host-apart lift is a fix, no switch.
    {
        HoldView v = Held(Keep::BossClear);
        v.keep_env = true;
        CHECK(PlanActionHold(v) == HoldAction::None);
        v.keep_env = false;
        v.delay = 40.0;
        v.since_kept = 10.0;
        CHECK(PlanActionHold(v) == HoldAction::None);
        v.since_kept = 41.0;
        CHECK(PlanActionHold(v) == HoldAction::Lift);
        HoldView a = Held(Keep::HostApart);
        a.keep_env = true;  // ignored for the host-apart keep
        a.delay = 40.0;
        a.since_kept = 0.0;
        CHECK(PlanActionHold(a) == HoldAction::Lift);
    }
    CHECK(KeepName(Keep::HostApart)[0] != 'n');
    CHECK(KeepName(Keep::BossClear)[0] != 'n');

    // YES at a headstone while the host is in another area.
    constexpr std::uint32_t kM24_01 = 24u << 24 | 1u << 16, kM24_00 = 24u << 24, kM21 = 21u << 24;
    // 1st Floor Sickroom (2412950, m24_01) with the host standing in m24_01 (region bits set): the
    // host would ignore the request, so the guest goes there itself.
    CHECK(PlanApartGuestYes(true, kM24_01, kM24_01 | 0x0203u) == ApartYes::OwnTrip);
    // Another block of the host's area: the host's party travel (it loads, the guest follows).
    CHECK(PlanApartGuestYes(true, kM24_01, kM24_00) == ApartYes::PartyRequest);
    // Another area: the host's party travel, as before.
    CHECK(PlanApartGuestYes(true, kM24_01, 22u << 24) == ApartYes::PartyRequest);
    // Together (the Dream with the host): unchanged.
    CHECK(PlanApartGuestYes(false, kM24_01, kM24_01) == ApartYes::PartyRequest);
    CHECK(PlanApartGuestYes(false, kM21, kM21) == ApartYes::PartyRequest);
    // The host's map unknown (loading, stale vitals) or no lamp map: the request, as before.
    CHECK(PlanApartGuestYes(true, kM24_01, 0) == ApartYes::PartyRequest);
    CHECK(PlanApartGuestYes(true, kM24_01, 0xffffffffu) == ApartYes::PartyRequest);
    CHECK(PlanApartGuestYes(true, 0, kM24_01) == ApartYes::PartyRequest);

    // Headstone choice flags (live: 72102410 "1st Floor Sickroom" -> 2412950).
    CHECK(HeadstoneFlagLamp(72102410) == 2412950);
    CHECK(HeadstoneFlagLamp(72102411) == 2412951);
    CHECK(HeadstoneFlagLamp(72103210) == 3212950);
    CHECK(HeadstoneFlagLamp(72100110) == 0);  // talk state (721001xx), not a headstone
    CHECK(HeadstoneFlagLamp(72102100) == 0);  // the Dream itself
    CHECK(HeadstoneFlagLamp(72410201) == 0);  // a lamp-lit flag
    CHECK(HeadstoneFlagLamp(12107000) == 0);
    // The fallback: only a guest in the Dream whose choice brought no warp for 8 s (not before the
    // kneel's ~4 s, not past 9.5 s: the own-use window, once).
    CHECK(HeadstoneWarpFallback(true, true, 8.5, false, false));
    CHECK(!HeadstoneWarpFallback(true, true, 4.0, false, false));
    CHECK(!HeadstoneWarpFallback(true, true, 25.0, false, false));
    CHECK(!HeadstoneWarpFallback(true, true, 8.5, true, false));   // the event warped
    CHECK(!HeadstoneWarpFallback(true, true, 8.5, false, true));   // already run
    CHECK(!HeadstoneWarpFallback(false, true, 8.5, false, false)); // the host: its own events
    CHECK(!HeadstoneWarpFallback(true, false, 8.5, false, false)); // left the Dream


    // The two lamp fixes together (W1 slice 5). 1) A peer's lamp flag packet (0x0f) or lamp event
    // state (0x0e) is dropped before the game reads it (deathrules::PeerLampPacket, our df59c37e):
    // this game's event 7200 for that lamp stays ready, so the other player's later Dream choice
    // there warps through the event (the fallback sees the warp and stays quiet). 2) If the event was
    // used up anyway (a path the drop does not see), the player's own flag is still set 1.5 s later
    // with no warp: WorldLampWarpFallback (visy 64686779 / 30a6ff7e) clears it and warps, host or guest.
    // Every flag the fallback watches is one the drop covers:
    for (std::uint32_t aab : {220u, 230u, 240u, 241u, 242u, 250u, 260u, 270u, 280u, 320u, 330u, 340u, 350u, 360u}) {
        for (std::uint32_t n = 0; n < 10; ++n) {
            const std::uint32_t flag = 70000100u + aab * 10000u + n;
            if (WorldLampDreamPoint(flag)) {
                CHECK(coop::deathrules::LampTriggerFlag(flag));
                const std::uint32_t pkt[1] = {flag};
                CHECK(coop::deathrules::PeerLampPacket(0xf, 4, pkt));
            }
        }
    }
    // Guest at a lamp after the host's trip from it (or the host after a guest's): the host's packet
    // was dropped, the guest's own choice runs the event, a warp follows: no fallback.
    CHECK(!WorldLampWarpFallback(true, true, 2.0, true, false, false));
    // The same lamp with its event used up: the flag stays set, nothing warped: the fallback warps.
    CHECK(WorldLampWarpFallback(true, true, 2.0, false, true, false));
    CHECK(!WorldLampWarpFallback(true, true, 1.0, false, true, false));  // the event still has time

    if (g_fail) {
        std::printf("party-stay-rules-test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("party-stay-rules-test: all passed\n");
    return 0;
}
