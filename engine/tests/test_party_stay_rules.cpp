// SPDX-License-Identifier: GPL-3.0-or-later
// A guest the party keeps where it stands (gpu/shim/party/party_stay_rules.h).
// cmake --build out/gpu --target party-stay-rules-test && out/gpu/party-stay-rules-test
#include "party/party_stay_rules.h"

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

    if (g_fail) {
        std::printf("party-stay-rules-test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("party-stay-rules-test: all passed\n");
    return 0;
}
