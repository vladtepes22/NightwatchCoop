// SPDX-License-Identifier: GPL-3.0-or-later
// Deaths, rests and same-map lamps in a party: the decisions (playtest 1 batch 1,
// docs/party/playtest1_triage.md bugs 3, 6, 7, 10, 12, 13, 18). Pure logic, no game access
// (unit test: tests/test_party_death_rules.cpp); the game side is party_b2.cpp (HostDeathIntercept,
// HostDownTick, HostRestLoad), party_split.cpp (the question) and party_guestsvc.cpp (requests).
//
// Rule of thumb (user, after playtest 1): where a vanilla loading screen fixes a glitch, the
// loading screen is the default; the seamless (no-load) variant is an opt-in switch.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::deathrules {

/// An env switch that is ON only when set to a value starting with '1' (the opt-in switches:
/// BB_PARTY_B2_HOST_REVIVE, BB_PARTY_INWORLD, BB_PARTY_GUEST_LAMP_INWORLD).
inline bool OptIn(const char* v) {
    return v && v[0] == '1';
}

/// Members counted down: the most of what they reported ("member_down") and what is seen (their
/// chr at HP 0 / an early down notice). Never more than `members`.
inline int PartyDown(int members, int reported, int seen) {
    if (members <= 0) {
        return 0;
    }
    return std::min(members, std::max(0, std::max(reported, seen)));
}

/// Every member is down too: the party wipes (one respawn load for everyone).
inline bool Wiped(int members, int reported, int seen) {
    return members > 0 && PartyDown(members, reported, seen) >= members;
}

// ---- The host dies (SoloPlayDeath_2's funnel call) ----
enum class HostDeathPlan {
    Stock,          ///< no party / not the host: the game's own death (not taken, nothing broadcast)
    PartyLoad,      ///< the stock funnel: one respawn load, the party follows (default)
    Wipe,           ///< every member is down too: the stock funnel, everyone loads
    BossLimbo,      ///< in a boss fight: the host waits (spectates) until the boss is gone
    ReviveInPlace,  ///< opt-in BB_PARTY_B2_HOST_REVIVE=1: revived at its lamp in the loaded map, no load
    AloneLoad,      ///< opt-in revive, but the lamp is in another map: the host loads it alone
};
inline const char* HostDeathPlanName(HostDeathPlan p);
struct HostDeathView {
    bool party_on = true;      ///< B2 on, this player hosts (or the solo revive fixture)
    int members = 0;           ///< member tasks in the world
    int down_reported = 0;     ///< members that said they are down
    int down_seen = 0;         ///< members seen at HP 0 (early notices)
    bool boss_fight = false;
    bool revive_opt_in = false;
    bool lamp_here = false;    ///< the host's last lamp has a spawn spot in the loaded map
    bool alone_opt = true;     ///< BB_PARTY_HOST_DEATH_ALONE (revive mode only)
    bool solo_fixture = false; ///< BB_PARTY_B2_HOST_REVIVE_SOLO=1 (test, no members needed)
};
inline HostDeathPlan PlanHostDeath(const HostDeathView& v) {
    if (!v.party_on || (v.members <= 0 && !v.solo_fixture)) {
        return HostDeathPlan::Stock;
    }
    if (Wiped(v.members, v.down_reported, v.down_seen)) {
        return HostDeathPlan::Wipe;
    }
    if (v.boss_fight) {
        return HostDeathPlan::BossLimbo;
    }
    if (!v.revive_opt_in) {
        return HostDeathPlan::PartyLoad;
    }
    if (!v.lamp_here) {
        return v.alone_opt ? HostDeathPlan::AloneLoad : HostDeathPlan::PartyLoad;
    }
    return HostDeathPlan::ReviveInPlace;
}
/// The plan takes the game's funnel call (true) or lets it run (false: a load now).
inline bool PlanTakesFunnel(HostDeathPlan p) {
    return p == HostDeathPlan::BossLimbo || p == HostDeathPlan::ReviveInPlace;
}

// ---- Each frame while the host is down (a taken death) ----
enum class DownTick {
    Wait,           ///< not yet (the revive delay, or the boss still fights)
    Wipe,           ///< everyone is down now: the stock funnel (a wipe load)
    Revive,         ///< revive in place (opt-in) at the lamp here
    PartyLoad,      ///< the stock funnel: the respawn load for everyone
    AloneLoad,      ///< the lamp is in another map: the host loads it alone (revive mode)
};
inline const char* DownTickName(DownTick d);
struct DownView {
    int members = 0;
    int down_reported = 0;
    int down_seen = 0;
    bool limbo = false;        ///< the death came in a boss fight
    bool boss_active = false;  ///< the boss fight is still on
    bool revive_opt_in = false;
    bool lamp_here = false;
    double down_for = 0;       ///< s since the death was taken
    double delay = 1.0;        ///< BB_PARTY_B2_HOST_REVIVE_DELAY
};
inline DownTick PlanDownTick(const DownView& v) {
    // Recounted every frame: a member whose death arrives a moment after the host's turns the
    // host's lone death into a wipe (playtest 1 bug 18: "0 down" counted 0.3 s too early).
    if (Wiped(v.members, v.down_reported, v.down_seen)) {
        return DownTick::Wipe;
    }
    if (v.down_for < v.delay) {
        return DownTick::Wait;
    }
    if (v.limbo && v.boss_active) {
        return DownTick::Wait;
    }
    if (!v.revive_opt_in) {
        return DownTick::PartyLoad;
    }
    if (!v.lamp_here) {
        return DownTick::AloneLoad;
    }
    return DownTick::Revive;
}

// ---- Split travel's question ("Travel with the party?") ----
constexpr double kAskTimeout = 30.0;  ///< s an open question waits (was 120 s)
enum class AskClose { Keep, Load, Dead, Timeout };
inline const char* AskCloseName(AskClose c);
/// An asked or queued question is closed by a load (a host travel, a death load), by the local
/// player's death, or after kAskTimeout s open (playtest 1: three questions stayed open 120 s
/// across the host's load and death).
inline AskClose PlanAskClose(bool open, bool loading, bool local_dead, double open_for) {
    if (loading) {
        return AskClose::Load;
    }
    if (local_dead) {
        return AskClose::Dead;
    }
    if (open && open_for > kAskTimeout) {
        return AskClose::Timeout;
    }
    return AskClose::Keep;
}

// ---- Whose lamp / headstone use started a lamp warp (playtest 1 bug 20) ----
/// The "used" flags the lamp warp events wait for: a world lamp -> the Dream (common event 7200,
/// one instance per lamp, flag 7AAB01xx: 72410101 Central Yharnam ...), a Dream headstone -> its
/// lamp (m21 event 12107000 / common 7300, flags 72102xxx / 72103xxx). The lamp's or the
/// headstone's talk script sets it on the user's own game (SetEventFlag). The same warp event
/// also runs on every other game in that map (live splitq: the guest's 12107000 reached its warp
/// 2003[49] 0.05 s after the host's, with no flag set there), so both players were asked.
inline bool LampTriggerFlag(std::uint32_t id) {
    if (id / 1000 == 72102 || id / 1000 == 72103) {
        return true;
    }
    return id >= 70000000u && id < 80000000u && id / 1000 % 10 == 0 && id % 1000 / 100 == 1;
}
constexpr double kLampOriginWindow = 10.0;  ///< s from a lamp's flag to its warp (the event waits ~3-4 s)
enum class LampOrigin { Own, Other };
/// A lamp warp on this game: this player's own use, or another member's (`local_age` / `remote_age`:
/// s since this game set a lamp flag itself / got one from a peer (packet 0x0e / 0x0f), < 0
/// never). This player's only when its own game set the flag (or a test step used the lamp) and
/// no peer's came after it; `flag_hooks` false (the SetEventFlag hook is missing): always this
/// player's, the old behaviour.
inline LampOrigin PlanLampOrigin(double local_age, double remote_age, bool flag_hooks = true) {
    const bool remote = remote_age >= 0 && remote_age <= kLampOriginWindow;
    const bool local = local_age >= 0 && local_age <= kLampOriginWindow;
    if (!flag_hooks) {
        return LampOrigin::Own;
    }
    return local && !(remote && remote_age < local_age) ? LampOrigin::Own : LampOrigin::Other;
}

/// Story warps (EMEVD `2003[49]` outside the lamp / warp-object events). Each is that player's own
/// use when its game runs it, so the warp gets the travel question instead of being dropped as
/// "another member's lamp" (no lamp flag is set). Live (parg, 2026-10-14): the Blood Moon warp
/// 13201803 -> 2802958 was dropped on the host, leaving it at Moonside Lake.
/// Host-only ones (chr type 0 / `End if Client`): 3503 DLC cleanup -> Dream, 12405263 Amygdala
/// grab -> DLC, 12800430 Yahar'gul -> Lecture Building after the One Reborn, 13201803 Rom -> Blood
/// Moon -> Yahar'gul. A guest's copy of these runs only by accident (its chr type reads 0 for a
/// moment after a load, live parg re-run: the guest got its own Blood Moon question) and stays
/// dropped there; the host's travel brings the guest. Any player's: 12200131 Hemwick carriage
/// (action button), 12800431 Amygdala grab (own Tonsil Stone), 13200310 / 13200311 Lecture
/// Building doors (ObjAct).
inline bool StoryWarpEvent(std::int32_t event, bool host) {
    switch (event) {
    case 3503:
    case 12405263:
    case 12800430:
    case 13201803:
        return host;
    case 12200131:
    case 12800431:
    case 13200310:
    case 13200311:
        return true;
    default:
        return false;
    }
}
/// After a story warp's cutscene the travel question waits this long once the scene is over (live
/// parg re-run: opened in the frame the Blood Moon scene ended, the box never showed and six Cross
/// presses did nothing).
constexpr double kAskAfterSceneDelay = 2.0;

// ---- P4 (quest_audit.md): one question for a scripted warp that runs on two games ----
/// This game's question for warp `mine` (pending: queued or open) closes when a member with a
/// lower party slot asks for the same warp (the host first). Slots < 0 / id 0: unknown, keep it.
inline bool DropSameWarpQuestion(bool pending, std::uint32_t mine, int my_slot, std::uint32_t theirs,
                                 int their_slot) {
    return pending && mine != 0 && mine == theirs && my_slot >= 0 && their_slot >= 0 && their_slot < my_slot;
}

// ---- B5: the host dies while a guest is apart ----
/// A living, idle guest in another area than the host (split up) does not follow the host's
/// death load: it plays on where it is (ERSC: each respawns at their own grace). Areas 0 =
/// unknown: follow as before. A dead guest (a wipe) follows.
inline bool StayApartAtHostDeath(bool guest_dead, bool guest_idle, std::uint32_t host_area, std::uint32_t own_area) {
    return !guest_dead && guest_idle && host_area != 0 && own_area != 0 && host_area != own_area;
}

// ---- A11 (live a11c): the host's chr is not placed yet ----
/// A joiner is put beside the host's chr only when that chr stands where the host is: a host
/// whose first sync waits (a cutscene started as the guest loaded in) leaves its chr on the
/// guest's game at the origin (0, 0, 0); a warp beside it dropped the guest into the void.
/// `vitals` (the host's own reported position) when known: within 10 m; else not at the origin.
inline bool HostChrPlaced(const float chr[3], bool have_vitals, const float vitals[3]) {
    if (have_vitals) {
        const float dx = chr[0] - vitals[0], dy = chr[1] - vitals[1], dz = chr[2] - vitals[2];
        return dx * dx + dy * dy + dz * dz <= 100.0f;
    }
    return !(chr[0] > -1.0f && chr[0] < 1.0f && chr[1] > -1.0f && chr[1] < 1.0f && chr[2] > -1.0f && chr[2] < 1.0f);
}

// ---- Playtest 1 bug 22: nobody to travel with = single player ----
/// The party's own lamp / headstone / Hunter's Mark handling (the in-world lamp move without a
/// load, "already standing at it", the guest's Mark to the host's lamp) applies only when there
/// is a party to travel with: a guest standing in the host's world (session client, or the flag
/// store still on the host-world overlay, load mode 1) or a host with members. A Join-a-party
/// guest whose host never answered (or between sessions, in its own world) and a host alone use
/// lamps, headstones and the Mark exactly as in single player (the vanilla loading screen).
inline bool PartyTravelActive(bool guest, bool host, bool guest_in_host_world, bool host_has_members) {
    return (guest && guest_in_host_world) || (host && host_has_members);
}
/// A guest is in the host's world: its session role is client (6), or its flag store still holds
/// the host-world overlay (load mode 1; a phantom's Hunter's Mark leaves the session before the
/// funnel runs). Its own world: load mode 0 and no client role.
inline bool GuestInHostWorld(int session_role, int flag_load_mode) {
    return session_role == 6 || flag_load_mode == 1;
}

// ---- C5: the spectator's on-screen notice ----
/// "Spectating Hunter0" (+ "  -  Cross: next hunter" when another living member can be chosen);
/// empty when there is nobody to follow. `name` empty: the member is not matched yet.
inline std::string SpectateNotice(const std::string& name, int living) {
    if (living <= 0) {
        return {};
    }
    std::string t = "Spectating " + (name.empty() ? std::string("a party member") : name);
    if (living > 1) {
        t += "  -  Cross: next hunter";
    }
    return t;
}

// ---- A guest uses a lamp / headstone / Hunter's Mark in the host's world ----
enum class GuestLamp {
    Plain,     ///< nothing to decide here (not a guest in the host's world, or standing at it)
    SoloLoad,  ///< the lamp is in the loaded map: this guest's own loading screen, no party move
    Ask,       ///< another map: the question (YES party travel, NO alone + a rest load for the host)
};
inline const char* GuestLampName(GuestLamp g);
inline GuestLamp PlanGuestLamp(bool guest_in_host_world, bool at_lamp_spot, bool lamp_in_loaded_map, bool mark) {
    if (!guest_in_host_world || (at_lamp_spot && !mark)) {
        return GuestLamp::Plain;
    }
    // Playtest 1 bug 10: a guest's warp to a lamp of the map the party stands in moved the host
    // too (an in-world party move). Now only the guest moves, with a vanilla loading screen.
    if (lamp_in_loaded_map) {
        return GuestLamp::SoloLoad;
    }
    return GuestLamp::Ask;
}
/// A guest that travels alone from a lamp (NO) has rested: the host's world (where the guest
/// was) resets with a rest load for everyone there. Not for a guest's death (no rest). D9 (pard4,
/// live E2): a guest's lamp of the map the party stands in (its own loading screen, `same_map_solo`)
/// is a rest too - before, its reload brought the host's kills back to life on the guest's screen
/// only (guest census 327 alive, host 325) while the host's world kept them dead.
inline bool GuestAloneRestsHostWorld(bool alone, bool same_map_solo) {
    (void)same_map_solo;
    return alone;
}

// ---- The host runs a rest load for a guest's rest ----
/// BossFight (C7, ERSC: a rest removes the others from the boss room): a run, but to the host's
/// last lamp instead of where it stands - the fight resets and everyone here is taken out of it.
enum class RestLoad { Run, NotReady, Dead, BossFight, InDream, Busy, TooSoon };
inline const char* RestLoadName(RestLoad r);
constexpr double kRestLoadMinInterval = 20.0;
struct RestView {
    bool world_up = false;
    bool loading = false;
    bool alive = false;
    bool boss_fight = false;
    bool in_dream = false;          ///< the Hunter's Dream has nothing to reset
    bool travel_pending = false;    ///< a stage request is armed / the host is down
    double since_last = 1e9;        ///< s since the last rest load
};
inline RestLoad PlanRestLoad(const RestView& v) {
    if (!v.world_up || v.loading) {
        return RestLoad::NotReady;
    }
    if (!v.alive) {
        return RestLoad::Dead;  // its own death load resets the world anyway
    }
    if (v.travel_pending) {
        return RestLoad::Busy;
    }
    if (v.since_last < kRestLoadMinInterval) {
        return RestLoad::TooSoon;
    }
    if (v.boss_fight) {
        return RestLoad::BossFight;  // out of the boss room: a load at the host's last lamp
    }
    if (v.in_dream) {
        return RestLoad::InDream;
    }
    return RestLoad::Run;
}

/// How many members a guest's rest load waits for (playtest 1 batch 6): the guest that rested
/// travels alone - to another area it never arrives here; to a lamp of the host's area
/// (`rest_dest_area` == `host_area`) it is in the host's world again and follows the load (its
/// world resets too). Members known in another area stay there; a member in the host's area or
/// still loading (area 0, unknown) follows the load. 0 = nobody follows: the host's load is an
/// alone load (no re-sync, no 60 s wait for an arrival that never comes).
struct RestMember {
    int slot = -1;
    bool connected = false;
    std::uint32_t area = 0;  ///< 0: unknown (loading)
};
/// `rest_rides_rejoin`: the resting guest comes back by its own re-join, served after this load
/// (RestRidesRejoin): it does not follow the load, it arrives into the reset world.
inline int RestLoadExpected(const RestMember* m, int n, int rest_slot, std::uint32_t host_area,
                            std::uint32_t rest_dest_area, bool rest_rides_rejoin = false) {
    int expected = 0;
    for (int i = 0; i < n; ++i) {
        if (!m[i].connected) {
            continue;
        }
        if (m[i].slot == rest_slot) {
            expected += !rest_rides_rejoin && rest_dest_area != 0 && rest_dest_area == host_area ? 1 : 0;
            continue;
        }
        if (m[i].area == 0 || m[i].area == host_area) {
            ++expected;
        }
    }
    return expected;
}

// ---- A guest rests and travels alone back into the host's area (one load, pt1polish) ----
/// The playtest replay (S2): a guest at the Dream lamp chose NO to a lamp of the host's area. It
/// travelled alone (its re-join load), then the host ran the queued rest load and the guest
/// followed it: two loading screens (~15 s). Now the rest rides on the re-join request: the host
/// runs its rest load first and serves the re-join after it, so the guest sees one loading screen
/// (raised while it waits for the snapshot) and arrives into the reset world. Only for a rest
/// (not a Hunter's Mark) into the area the host stands in.
inline bool RestRidesRejoin(bool rest, std::uint32_t dest_area, std::uint32_t host_area) {
    return rest && dest_area != 0 && dest_area == host_area;
}
/// Host: how long a re-join that carries a rest waits for the rest load to start. Past it the
/// re-join is served (the rest load, if it still runs, is a second load as before).
constexpr double kRestCarryHoldS = 8.0;
inline bool RestCarryHeld(double now, double carry_at, bool rest_load_started) {
    return carry_at > 0 && !rest_load_started && now - carry_at < kRestCarryHoldS;
}
/// Host: a pending re-join request (a guest's respawn / alone travel into this world) is served
/// now: in the world, not inside an own load's settle (its re-sync covers the request), and not
/// held for a rest load that is about to start.
inline bool HostServesRejoin(bool loading, bool own_load_settling, bool rest_carry_held) {
    return !loading && !own_load_settling && !rest_carry_held;
}

// ---- Game banners in a party (the SummonMessage dialog, EventText ids; pt1polish) ----
/// "The bell's resonance has beckoned <name>." (102001) and the arrival / "Entered the world"
/// banners (102002..102700) belong to the first summon. After a B2 re-join (a wipe or death
/// respawn load, a rest follow, an alone travel back) the member is the same one: no banner.
inline bool IsJoinBanner(int id) {
    return id >= 102001 && id <= 102799;
}
/// "You died. Returning to your world." (103100, 103200..103500) and "Your beckoner has died.
/// Returning to own world." (103101): with B2 a guest's death is held and it stays with the party.
inline bool IsReturnHomeBanner(int id) {
    return id == 103100 || id == 103101 || id == 103200 || id == 103300 || id == 103400 || id == 103500;
}
constexpr double kRejoinBannerWindowS = 15.0;
/// `since_rejoin`: s since a B2 re-join was last running on this game (negative: never).
inline bool HidePartyBanner(int id, bool b2_on, bool guest, double since_rejoin) {
    if (!b2_on) {
        return false;
    }
    if (IsReturnHomeBanner(id)) {
        return guest;
    }
    return IsJoinBanner(id) && since_rejoin >= 0 && since_rejoin < kRejoinBannerWindowS;
}

// ---- An empty bloodstain (pt1polish) ----
/// A death with 0 Blood Echoes still records a stain (flag +0x28 set, record echoes 0): the game
/// then shows "Acquire Blood Echoes" for nothing. In a party it is not placed (the flag cleared
/// before the respawn load builds it). A stain with echoes, or none, is left alone.
inline bool ClearEmptyStain(bool party_on, int flag, int echoes) {
    return party_on && flag != 0 && echoes == 0;
}
/// After any call of the stain record 0x13D2910 (`flag_after` GameDataMan +0x28, `record_echoes`
/// the record's +0x38 after the call): a stain with 0 echoes is removed at once (the function
/// builds the live stain itself). Not only the death's record call (third argument 1): with 0 it
/// skips the record but still sets the flag and builds the stain from the old record - the
/// respawn load's call put the 0-echo stain back (pt1last live: "Acquire Blood Echoes" at the
/// lamp after a wipe although the death's call had removed it). Unknown echoes (-1): left as is.
inline bool SkipEmptyStainRecord(bool party_on, int flag_after, int record_echoes) {
    return party_on && flag_after != 0 && record_echoes == 0;
}

// ---- Test steps (BB_PARTY_DIRECTOR_TEST) ----
/// A step waits (pt1polish, replay S1/S4): one that moves / acts with the local chr while that
/// chr is dead, and any step while a B2 re-join or load is in flight (a die step landed inside the
/// previous wipe's load and made no second wipe).
/// The guest's dead-without-a-death-state watchdog (party_b2 DeadAfterClearTick) redirects a guest
/// that stayed at HP 0 for `limit_s` with no death hold. Never while it follows the host's travel:
/// that load revives it (regression run 0.6.1: a wipe whose host travel came 10 s after the guest's
/// death; the redirect 4.5 s into the follow ended the session, ~50 s re-join).
inline bool DeadWatchdogFires(double dead_s, double limit_s, bool death_held, bool following_host_travel) {
    return !following_host_travel && !death_held && dead_s >= limit_s;
}

inline bool TestStepWaits(bool acts_with_own_chr, bool own_alive, bool world_up, bool rejoin_running) {
    if (!world_up || rejoin_running) {
        return true;
    }
    return acts_with_own_chr && !own_alive;
}

// ---- Own bloodstain record (GameDataMan +0x28 flag, +0x30 record +0x38 echoes) ----
enum class StainChange { None, Recorded, Taken, Replaced };
inline const char* StainChangeName(StainChange c);
/// What changed between two reads of the own bloodstain record (echoes -1 / flag 0 = none).
inline StainChange PlanStainChange(int flag_was, int echoes_was, int flag_now, int echoes_now) {
    const bool had = flag_was != 0 && echoes_was > 0, has = flag_now != 0 && echoes_now > 0;
    if (had && !has) {
        return StainChange::Taken;
    }
    if (!had && has) {
        return StainChange::Recorded;
    }
    if (had && has && echoes_was != echoes_now) {
        return StainChange::Replaced;
    }
    return StainChange::None;
}

/// Verification replay (playtest 1, 2026-10-14): a guest's own body comes out of a B2 host-world
/// load (host death / wipe respawn, rest follow) and of a title join with PlayerIns +0x78 (chr type)
/// 0, an own-world body, instead of 1 (white phantom, what a direct summon leaves). The death
/// dispatch OnEvent_4000 0x13813D0 then takes SoloPlayDeath, which leaves the session at 0x13CDEF6
/// instead of PartyGhostDeath_2 (the B2 funnel at 0x1382663): in a wipe the guest left the party and
/// rejoined 50 s later instead of following the one load. Due: a client in the host's world, alive,
/// flow 4, type 0 for `steady_s` >= kGuestChrTypeSettle, or at once when the chr is down (HP 0):
/// playtest 1, a title-joined guest died ~1 s after its arrival, inside the old 2 s settle, so its
/// death left the session (to the title screen, a re-join 50 s later). Writing 1 where the summon
/// load would set 1 itself changes nothing, so the settle is only a few frames.
constexpr double kGuestChrTypeSettle = 0.25;
inline bool GuestChrTypeFixDue(bool client_in_world, int chr_type, int hp, int flow, double steady_s) {
    return client_in_world && chr_type == 0 && hp >= 0 && flow == 4 && (hp == 0 || steady_s >= kGuestChrTypeSettle);
}

inline const char* HostDeathPlanName(HostDeathPlan p) {
    switch (p) {
    case HostDeathPlan::Stock: return "stock";
    case HostDeathPlan::PartyLoad: return "party respawn load";
    case HostDeathPlan::Wipe: return "wipe";
    case HostDeathPlan::BossLimbo: return "boss limbo";
    case HostDeathPlan::ReviveInPlace: return "revive in place";
    case HostDeathPlan::AloneLoad: return "alone load";
    }
    return "?";
}
inline const char* DownTickName(DownTick d) {
    switch (d) {
    case DownTick::Wait: return "wait";
    case DownTick::Wipe: return "wipe";
    case DownTick::Revive: return "revive";
    case DownTick::PartyLoad: return "party load";
    case DownTick::AloneLoad: return "alone load";
    }
    return "?";
}
inline const char* AskCloseName(AskClose c) {
    switch (c) {
    case AskClose::Keep: return "keep";
    case AskClose::Load: return "a load started";
    case AskClose::Dead: return "the player died";
    case AskClose::Timeout: return "no answer in 30 s";
    }
    return "?";
}
inline const char* GuestLampName(GuestLamp g) {
    switch (g) {
    case GuestLamp::Plain: return "plain";
    case GuestLamp::SoloLoad: return "solo load";
    case GuestLamp::Ask: return "ask";
    }
    return "?";
}
inline const char* RestLoadName(RestLoad r) {
    switch (r) {
    case RestLoad::Run: return "run";
    case RestLoad::NotReady: return "the host is not in the world";
    case RestLoad::Dead: return "the host is dead (its death load resets the world)";
    case RestLoad::BossFight: return "a boss fight is on: everyone here is taken out of the boss room";
    case RestLoad::InDream: return "the host is in the Hunter's Dream";
    case RestLoad::Busy: return "a travel or death is under way";
    case RestLoad::TooSoon: return "a rest load ran less than 20 s ago";
    }
    return "?";
}
inline const char* StainChangeName(StainChange c) {
    switch (c) {
    case StainChange::None: return "none";
    case StainChange::Recorded: return "recorded";
    case StainChange::Taken: return "taken";
    case StainChange::Replaced: return "replaced";
    }
    return "?";
}

} // namespace coop::deathrules
