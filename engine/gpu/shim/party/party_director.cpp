// SPDX-License-Identifier: GPL-3.0-or-later
// PartyDirector and the coop main-thread tick (see party_director.h). The test mode follows
// droogie/bbhost src/engine/np_test.cpp @8f2746c (GPL-3.0-or-later): world-up detection, the
// Insight write and the bell events.
#include "party_director.h"
#include "party_log_game.h"
#include "../boot_marks.h"
#include "../bbport_boot_watch.h"
#include "../net/netsim.h"
#include "party_hud.h"
#include "party_ids.h"
#include "party_stick_args.h"
#include "party_balance.h"
#include "party_enemyai.h"
#include "party_npcfight.h"
#include "party_async_io.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "lua_events.h"
#include "party_b2.h"
#include "party_death_rules.h"
#include "party_dsummon.h"
#include "../net/from_api.h"
#include "party_fourp.h"
#include "party_link.h"
#include "party_story.h"
#include "party_catchup.h"
#include "party_status_bridge.h"
#include "party_chalice.h"
#include "party_gestures.h"
#include "party_items.h"
#include "party_npc_test.h"
#include "party_guestsvc.h"
#include "party_split.h"
#include "party_gather.h"
#include "party_vote.h"
#include "lamp_travel.h"
#include "party_phantom.h"
#include "party_start.h"
#include "party_flow.h"
#include "party_runtime.h"
#include "party_progress.h"
#include "party_travel.h"
#include "seamless_rules.h"
#include "test_newgame.h"
#include "../bbport_game_menu.h"
#include "party_heapwatch.h"
#include "party_online.h"
#include "party_perf.h"
#include "party_watchdog.h"
#include "party_worldact.h"
#include "party_npctalk.h"
#include "party_ping.h"
#include "party_pvp.h"
#include "../bbport_game_menu.h"

#include "../net/bbnet_internal.h"
#include "../net/party_host_service.h"
#include "../net/np_session.h"
#include "../net/party_util.h"
#include "../../party_env.h"
#include "../bbport_settings.h"

#include <algorithm>

#include "../bbport_frame_state.h"
#include <array>
#include <cmath>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c
extern "C" std::uint32_t runtime_pad_last_buttons(); // src/runtime_pad.c: the last sample the game read

namespace coop {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kHostBell = "OnEvent_Call_SOS";
constexpr const char* kGuestBell = "OnEvent_SendSoulSign_NormalCoop";

// SprjFlipper::Update (bbhost 0x2434770): push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx;
// sub rsp, 0x38 - 17 bytes, no relative operands. Its one caller is the frame function 0x2018d20
// (SprjWindow update, this, then the SprjTask step 0x20512a0).
constexpr std::uint64_t kFlipperUpdate = 0x2034770;
constexpr std::uint64_t kDispatchByName = 0x1339870;

std::uint64_t ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return std::uint64_t(pthread_self());
#endif
}

double Seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party: ") + PartyLogStamp() + line + "\n"); // never blocks the caller (party_async_io.h)
}

/// A live travel test step (BB_PARTY_DIRECTOR_TEST host_warp=ID@S, host_die@S, guest_die@S):
/// runs S seconds after this player's next (re)join - the host's cooperators 0 -> >= 1, the
/// guest's client session with the world loaded - on the main thread, in list order.
// Where every member is (the party link's vitals; the director's tick writes it, the "?near=" test
// condition reads it).
std::mutex g_where_mu;
std::vector<b2::Where> g_where;
int g_where_local = -1;

/// Another member's vitals position (fresh, under 3 s) lies within `r` m of `at`; `d` its distance.
bool MemberNear(const float at[3], float r, float* d) {
    std::lock_guard<std::mutex> lk(g_where_mu);
    for (const b2::Where& w : g_where) {
        if (w.slot == g_where_local || !w.has_pos || w.age_ms > 3000) {
            continue;
        }
        const float dx = w.pos[0] - at[0], dy = w.pos[1] - at[1], dz = w.pos[2] - at[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist <= r) {
            if (d) *d = dist;
            return true;
        }
    }
    return false;
}

struct TravelStep {
    // HostMark / GuestMark (steady-world clock, as host_move; %MAP filter): the Hunter's Mark (the Lua event "OnReviveMagic", what the player's
    // special-effect update 0x18FEF50 raises for a stateInfo 0x17 effect). HostStage: Lua
    // WarpNextStage 0x132E010 (args area:block:region:index:warp point), or WarpNextStage_Bonfire
    // 0x132E050 (args bonfire:ID). HostWalk: the host's chr put at each x:y:z:yaw (separated by
    // '/') 1 s apart (repeat a spot to stay there), no load (streamed blocks). HostHeadstone: talk to the headstone t<id> (stand
    // 1.2 m in front, Cross), wait for its menu, Down N times, Cross (args id:N).
    enum Kind {
        HostWarp, HostDie, GuestDie, HostKill, HostMove, GuestMove,
        HostPress, GuestPress, HostMark, GuestMark, HostStage, HostWalk, HostHeadstone, HostFlag, GuestFlag, HostSkipRemo, GuestSkipRemo, HostLastLamp, GuestLastLamp,
        // Split travel (docs/party/split_travel.md): host_lamp=ID / guest_lamp=ID use lamp ID as the
        // player would (0x13CDF30 through the interceptor: the "Travel with the party?" question);
        // host_alone=ID: the host travels to ID alone, decided (no question).
        HostLamp, GuestLamp, HostAlone, GuestKill,
        HostOutage, GuestOutage, // a simulated total network outage of `buttons` ms (BB_NET_SIM on)
        HostSpfx, // host_spfx=ENTITY:ID: SpEffect ID on that entity's chr by its ApplySpEffect (D7 doping dedupe)
        HostPoise, // host_poise=N: the Enemy poise setting (party_enemy_poise, % of the step) set live, not saved (D7 A/B)
        HostGfx,   // host_gfx=lod:N|cap|log: graphics test aid (model LOD bias, frame record, camera values; GfxTestStep)
        HostObj, GuestObj, HostChr, GuestChr, // stand at a map object / character: ENTITY[:DIST[:ANGLE]]
        HostUse, GuestUse, // use goods ID as the item does (test aid): its behaviour's SpEffect on the own chr
        GuestFixture, // guest_fixture@S: the BB_PARTY_WORLDACT_TEST objects / pickups again (worldact::FixtureRearm)
        GuestStick, // walk by the left stick (the approach driver) to X:Z[:R[:STEPS]]: a real walk, no warp
        HostStick,  // the same on the host
        HostRoute, GuestRoute, // a stick walk along waypoints X:Z[:R]/...: a real path through the level (B1)
        GuestHome,  // the guest's own leave (LeaveGameSession: home, the link stays; its bell rings again)
        // The Party page's Leave / Rejoin as the player presses them (System > Party; party_status
        // RequestLeave / RequestRejoin): host_leave / guest_leave wait for the join like any step;
        // host_rejoin / guest_rejoin run S s after this player went out of the party (or after the
        // previous step, the later), while it is out (repeat cycles: tools/mp/smoke_party.py).
        HostLeave, GuestLeave, HostRejoin, GuestRejoin,
        // host_kick=NAME@S: the host kicks that member from the Party page (party_status KickMember); the
        // guest's own world is checked afterwards (smoke group phantom_own).
        HostKick,
        // I5: the Party page's Gather the party as the host presses it (party_status RequestGather).
        HostGather
    } kind = HostWarp;
    bool after_scene = false; // "~": the delay counts from the end of a cutscene after the previous step
    std::uint32_t id = 0; // HostWarp: the ReturnPointParam row for 0x13CDF30; HostKill: the entity id
    float pos[3] = {};    // HostMove / GuestMove: the chr warp target (map coordinates)
    float yaw = 0;        // HostMove / GuestMove: rotation about Y (radians) when has_yaw
    bool has_yaw = false;
    int warp = 0;         // HostMove / GuestMove: 0 the bare chr warp +0x5F8, 1 ":native" the game's own player warp
    double pin = 0;       // HostMove / GuestMove ":pin=S" (liftfix): held at the spot for S s (the bare chr warp every
                          // frame) while the floor's collision streams in after a long warp (else the chr falls through)
    bool ground = false;  // HostMove / GuestMove ":ground" (with pin): released once the game's safe-ground spot is
                          // there; no ground by the pin's end: held once more; a fall after the release: one re-warp
    std::uint32_t buttons = 0;  // HostPress / GuestPress: pad buttons tapped (0.15 s)
    std::string map;      // HostMove / GuestMove "%NAME": only while the current map name contains it
    std::string args;     // HostStage / HostWalk / HostHeadstone
    double delay = 15.0;
    int min_join = 0;     // "#N": not before this player's Nth join
    int min_coop = 0;     // "^N" (host): not before the host's game has N cooperators; S counts from then
    int coop_rise = 0;    // "^N*K": ... and only once the count has risen to N for the K-th time
    bool alive = false;   // "!" on a press step: waits as a step with the own chr (alive, world up)
    bool out_ok = false;  // "&" on a %MAP step: also runs out of the party (after a Leave: the own world)
    // "?FLAG=V" at the end (liftfall): not before event flag FLAG reads V on this game; S counts from
    // then (e.g. a lift's position flag at the end of its ride: no fixed wait for the ride's length)
    std::uint32_t wait_flag = 0;
    bool wait_value = true;
    // "?near=X:Y:Z:R" at the end (liftfix): not before another party member stands within R m of
    // (X, Y, Z) (its vitals' position); S counts from then (a second rider steps into a lift's car
    // once the first one stands in it: no race of two fixed delays on two games)
    bool wait_near = false;
    float near_pos[3] = {0, 0, 0};
    float near_r = 0;
    // "?obj=ENTITY:ANIM" at the end (liftfix): not before map object ENTITY shows animation ANIM and
    // it stopped (a lift car arrived: step out right at its arrival, as a player does); S counts from then
    std::int32_t wait_obj = 0, wait_obj_anim = -1;
};

// host_route / guest_route (B1): a waypoint counts as reached within this many m; still blocked after
// this many side-step slides, the walker hops (chr warp) onto the waypoint when it is this close.
constexpr float kRouteReach = 2.0f;
constexpr int kRouteSlidesBeforeHop = 2;
constexpr double kRouteHopMax = 6.0;

struct Test {
    bool on = false;
    // A11 fixture (no cooperator needed): a11_move=X:Y:Z@S moves the host's chr there S s after its
    // world is steady (into a boss arena: the fight starts); a11_kill=ENTITY@S ForceDeads ENTITY S s
    // after the boss bar came up (the fight ends) - a joiner must wait outside meanwhile.
    bool a11_move = false, a11_moved = false, a11_kill = false, a11_killed = false;
    float a11_pos[3] = {0, 0, 0};
    double a11_move_at = 0, a11_kill_after = 0;
    std::uint32_t a11_entity = 0;
    std::uint32_t a11_lamp = 0;  // a11_lamp=ID: the lamp warp first (2 s after the first steady world)
    bool a11_lamp_done = false, a11_reloaded = false;
    bool a11_mortal = false;  // a11_mortal: not invincible (the boss may kill the host)
    Clock::time_point a11_lamp_world_at{};
    Clock::time_point a11_bar_at{};
    bool a11_bar_seen = false;
    bool log_state = false;
    int insight = -1;
    int level = -1; // level=N: writes the player's level once (0.7.5 level sync A/B; test saves)
    bool level_done = false;
    int echoes = -1; // echoes=N: writes the player's Blood Echoes once (test saves)
    // armor=SET: puts on armor set SET (EquipParamProtector SET, +1000, +2000, +3000: head, chest,
    // arms, legs) once, 3 s into the first steady world, as the equipment menu does; in a session
    // the peers rebuild this chr from its new record (the mirror's path). Distinct-looking players
    // for the visual tests (all instances share one test save).
    std::uint32_t armor = 0;
    // approach=X:Z:R[:BUTTON]: walk (pad file sticks, camera-relative, closed loop on the position)
    // until within R m of (X, Z), facing it; then BUTTON (cross / triangle / square / circle) once.
    bool approach = false, approach_done = false, ap_faced = false;
    int ap_taps = 0;               // face taps made (prompt=: until its text shows, 45 deg apart)
    bool ap_prompt_seen = false;   // prompt= showed after a tap: keep that facing (no turn warp)
    std::string ap_prompt;         // prompt=TEXT: the action prompt the face taps look for
    std::int64_t ap_tap_ms = 0;
    float ap_x = 0, ap_z = 0, ap_r = 1.0f;
    bool ap_face = false;  // face=X:Z: the point the arrival tap turns toward (default: the walk's target)
    float ap_fx = 0, ap_fz = 0;
    std::uint32_t ap_button = 0;
    int ap_steps = 0, ap_phase = 0; // 0 idle, 1 moving, 2 settling, 3 arrived, 4 pressing
    int ap_max = 80;                // steps before it gives up (guest_stick: its STEPS)
    // host_route / guest_route: the waypoints {x, z, r}; the approach target is route[route_i].
    std::vector<std::array<float, 3>> route;
    std::size_t route_i = 0;
    int route_slides = 0;           // side steps taken to get off a wall
    int route_hops = 0;             // short chr warps onto the next waypoint (blocked after the side steps)
    int ap_slide = 0;               // side steps left in the current slide (sign = side)
    int ap_slides_here = 0;         // slides tried for the current waypoint
    std::uint32_t route_block = 0;  // the own chr's stage record (+0x3F8) last logged
    Clock::time_point route_started{};
    bool route_god = false;         // the route made the chr invincible (cleared at its end)
    int ap_still = 0;               // steps in a row without moving (blocked: gives up after 8)
    double ap_angle = 0;
    float ap_from[2] = {0, 0};
    Clock::time_point ap_at{};
    // stand=X:Y:Z:YAW[:BUTTON]: puts the player there in place (PlayerIns vfunc +0x5F8, no map
    // reload; yaw t faces (-sin t, -cos t), agent/guestsvc's Doll fixture), then BUTTON once 1.5 s on.
    // stand=obj:ENTITY:DIST:ANGLE[:BUTTON]: the same at a map object (worldact::StandAtObject: DIST m
    // from it at ANGLE degrees from its front, facing it), e.g. a Ritual Altar for the seq= below.
    bool stand = false, stand_done = false;
    int st_obj = 0;
    float st_obj_dist = 1.2f, st_obj_angle = 0;
    float st_pos[3] = {0, 0, 0}, st_yaw = 0;
    std::uint32_t st_button = 0;
    int st_phase = 0;
    int st_retries = 0;  // stand: warps repeated while not in place
    MenuScript st_seq; // seq=...: after stand (and its button), the menu walk (test_newgame.h)
    bool st_seq_on = false;
    std::string st_last_texts;
    Clock::time_point st_at{};
    bool god = false;     // god: the own chr kept invincible (test aid)
    bool log_pos = false; // log_pos: the player's area and position (FrpgNetMan +0xa7c / +0xaac) every 3 s
    double log_pos_every = 1.0;  // log_pos=S (liftfix): every S s (0.2..10; a lift ride's vertical speed)
    Clock::time_point pos_at{};
    // watch_text=A/B: every new appearance of a game text containing A or B (an action prompt
    // "Kick Down Ladder", a dialog) is logged with its full text (test aid, dlast).
    std::vector<std::string> watch_texts;
    std::map<std::string, std::int64_t> watch_last; // text -> last lookup ms
    std::int64_t watch_since = 0;
    Clock::time_point watch_at{};
    // log_obj=ENTITY: that map object's position (a lift car, a ladder) whenever it moved (> 5 cm).
    std::vector<std::int32_t> log_objs;
    std::map<std::int32_t, std::array<float, 3>> log_obj_pos;
    Clock::time_point log_obj_at{};
    // log_flag=ID[/ID...]: those event flags' values on this game whenever one changes (a lift's
    // state flags on every game of the party, lifts071), checked every tick.
    std::vector<std::uint32_t> log_flags;
    std::map<std::uint32_t, int> log_flag_val;
    // log_objanim=ENTITY: that map object's animation state (worldact::ReadObjAnim) whenever it changes.
    std::vector<std::int32_t> log_objanims;
    std::map<std::int32_t, std::string> log_objanim_last;
    const char* ring = nullptr; // the event to raise once
    double delay = 10.0;
    bool start = false, grant_bells = false, drop_bells = false, drop_done = false;
    int award_lot = -1; // award_lot=N: AwardItemLot(N) once (call-shape probe, test saves)
    std::vector<std::pair<int, int>> goods; // goods=ID:N: tops goods ID up to N once (test saves)
    bool goods_done = false;
    bool place = false, place_done = false; // place=MAP:X:Y:Z:YAW (hex map): one forced placement
    std::uint32_t place_map = 0;
    float place_pos[3] = {0, 0, 0}, place_yaw = 0;
    double place_at = 15.0;
    std::uint32_t warp_point = 0; // warp=MAP:POINT (hex map): one stage warp, place's timing
    std::vector<std::uint32_t> set_flags; // set_flag=ID[:ID...]: SetEventFlag(ID, 1) once (C2 live check)
    bool coop_seen = false;               // host in a party: the actions wait for a cooperator
    Clock::time_point coop_at{};
    bool insight_done = false, ring_done = false;
    const char* bell = nullptr; // bell_host / bell_guest: Ring() once ready (the real bell path)
    int bell_area = -1;         // bell_host@AA: only in a map of area AA (e.g. 29: inside a Chalice Dungeon)
    bool bell_done = false;
    Clock::time_point rung_at{};
    // Travel steps (see TravelStep): the next one waits for a join after the previous one ran.
    std::vector<TravelStep> steps;
    std::size_t next_step = 0;
    bool joined = false;       // in the party now (host: cooperators >= 1; guest: client, loaded)
    bool armed = false;        // a join since the last step ran
    int joins = 0;
    Clock::time_point joined_at{};
    Clock::time_point coop_since{};  // the next step's "^N" cooperator count reached (its clock)
    Clock::time_point flag_since{};  // the next step's "?FLAG=V" condition met (its clock)
    Clock::time_point near_since{};  // the next step's "?near=X:Y:Z:R" condition met (its clock)
    Clock::time_point obj_since{};   // the next step's "?obj=ENTITY:ANIM" condition met (its clock)
    Clock::time_point pin_until{};   // a move step's ":pin=S": the chr held at pin_pos until then
    alignas(16) float pin_pos[4] = {0, 0, 0, 1}, pin_rot[4] = {0, 0, 0, 0};
    bool pin_ground = false;          // ":ground": the pin waits for ground (the step's ground)
    double pin_secs = 0;              // the pin's length (a re-hold uses it again)
    int pin_retries = 0;              // ":ground": re-holds / re-warps done (1 at most)
    Clock::time_point pin_set{}, fall_check{};  // when the pin started; ":ground": the post-release fall check
    std::size_t obj_seen_step = ~std::size_t(0);  // "?obj=": the step whose ANIM was seen playing
    // liftfix: a step's "?FLAG=V" / "?near=" / "?obj=" condition that never comes true: after
    // kCondTimeout s (BB_PARTY_TEST_COND_TIMEOUT, default 240) the chain jumps to its last step (conditions skipped), so the run ends on its
    // marker instead of idling; logged "condition timed out".
    std::size_t cond_step = ~std::size_t(0);
    Clock::time_point cond_since{};
    bool cond_skip = false;
    std::size_t near_step = ~std::size_t(0), obj_step = ~std::size_t(0);  // the step those two were met for:
                                     // latched (an arrival's stopped animation lasts a few frames only)
    int last_coop = 0;               // the host's cooperators at the previous tick ("^N*K")
    int coop_rises[8] = {};          // how often the count rose to >= N (index N), since the start
    Clock::time_point step_at{}; // the last step's run, for "back after" times
    bool ran = false;            // a step of this role ran
    TravelStep::Kind last_kind = TravelStep::HostWarp;
    // HostKill waits for no re-join (a B2 host keeps its cooperators through a load): it runs S s
    // after the world was steady again with a cooperator, counted from here.
    Clock::time_point steady_at{};
    bool steady = false;
    Clock::time_point world_at{};  // the own world up and not loading since ("&" steps out of the party)
    bool world = false;
    // The game's cutscenes (a step ending in "~" counts from the end of one).
    bool scene_on = false;
    Clock::time_point scene_end_at{};
    // A running multi-tick step (HostWalk / HostHeadstone).
    struct Script {
        TravelStep::Kind kind = TravelStep::HostWalk;
        std::string args;
        int stage = 0, presses = 0;
        Clock::time_point at{};
        bool pressed = false;
        std::uint64_t text_seq = 0;
    } script;
    bool script_on = false;
    Clock::time_point release_at{};  // a press step's buttons are released then
    bool pressing = false;
    std::uint32_t chr_target = 0;  // the last host_chr / guest_chr entity (press logs its HP)
    Clock::time_point press_at{};  // the last host_press / guest_press tap (BB_PARTY_TARGET_ANIM)
    bool step_waiting = false;  // the next step waits (dead chr / a re-join running), logged once
    Clock::time_point left_at{};  // the last time this player went out of the party (host_rejoin / guest_rejoin)
};

struct State {
    std::mutex mu; // role / link (set from other threads)
    PartyRole role = PartyRole::None;
    int max_players = 3;
    party::PartyLink* link = nullptr;
    bool auto_ring = true;
    int auto_continue = -1; // BB_PARTY_AUTOCONTINUE: 1 on (tests, party flow Continue/Join); unset/0 off (manual play)
    double ring_every = 30.0;
    bool a11_waiting = false;  // A11: the host's bell held (boss fight / cutscene)
    int a11_kind = 0;          // 1 boss, 2 cutscene: what the guests were last told
    Clock::time_point a11_told{};
    // BB_PARTY_FAST_JOIN (default on; docs/party/join_timing.md): shorter director pauses.
    bool fast_join = true;
    double ring_delay = 0.3;        // s in a steady world before the first bell (5 without fast join)
    // The title takes no input until it has loaded (~5 s after the first tick) and then the first
    // press starts the FROM sign-in: short steps press as soon as it accepts (docs/STARTUP.md).
    double continue_settle = 0.0;   // s on the title before the first Continue press (4)
    double continue_every = 0.35;   // s between Continue presses, each held 0.15 s (2.5)
    StartMode start_mode = StartMode::PrologueSolo;
    bool grant_bells = true;
    Test test;

    // Main thread only.
    bool started = false;
    Clock::time_point start{};
    GameSnapshot last{};
    bool have_last = false;
    bool world_seen = false;
    Clock::time_point world_at{};
    Clock::time_point last_full{}, last_log{}, last_ring{}, chalice_wait_at{};
    bool rung_ever = false;
    bool poll_logged = true; // the sign polling 2.5 s after a bell (join timing)
    std::uint64_t signs_seen = 0; // host: bbnet::sign_board_changes() last acted on
    std::uint64_t summons_seen = 0; // host: bbnet::summon_requests() last seen
    Clock::time_point summon_at{};  // host: when our game last asked to summon someone
    std::vector<std::string> restarted; // host: members whose game restarted, stale peer not cleared yet
    bool member_waiting = false;        // host: HostMayRing was true last tick (since member_waiting_at)
    Clock::time_point member_waiting_at{};
    bool bell_hold_logged = false;      // host: "no Beckoning Bell while the direct summon ..." said
    std::uint64_t tick_thread = 0;
    std::uint64_t ticks = 0, ticks_at_log = 0, foreign_ticks = 0;
    party::MemberState sent_state = party::MemberState::Title;
    std::uint32_t sent_map = 0;
    bool sent_any = false;
    // Campaign start (party_start.h), refreshed with the full reads.
    StartFlags cs{};
    bool start_ready = false, start_logged = false;
    StartStep start_step = StartStep::NoWorld;
    // #19: a prologue step (opening / clinic / first Dream) by the flags - unlike !start_ready
    // not set by a scripted cutscene (9180) of a ready player, and kept over loading screens.
    bool start_prologue = false;
    bool left_first_dream = false; // seen out of the Dream with both weapons (party_start.h)
    bool granted = false; // bells granted (or found owned) in this world
    bool weapons_checked = false; // StartWeaponFlagsToClear ran in this world (own world only)
    Clock::time_point last_hosting{}; // host: when its session role last was host (3): travel audience
    // Title "Continue" (AutoContinue).
    bool continue_done = false, continue_held = false;
    int continue_presses = 0;
    Clock::time_point continue_ready{}, continue_last{};
    bool continue_waiting = false;
    bool continue_signin = false;
    // BB_BOOT_WATCHDOG: the last press or legitimate hold (sign-in, no link, title join), and
    // whether this quiet spell already asked for a snapshot.
    Clock::time_point continue_moved{};
    bool continue_watch_asked = false;
    std::string last_from_client;
    // Host session watchdog (party_watchdog.h).
    HostWatchdog watchdog;
    int watchdog_stops = 0;
    bool watchdog_check = false; // after a stop: log the session / phantoms once it is idle
    Clock::time_point watchdog_at{};
    // Summons that cannot work (summon_endpoint_unusable, UdpReachTick): host: the members with no
    // usable UDP endpoint; guest: this game (no STUN-mapped endpoint, the host is remote).
    std::vector<std::string> unreachable;
    bool self_unreachable = false;
    Clock::time_point reach_at{}, reach_said{};
};

State& S() {
    static State s;
    return s;
}

std::atomic<bool> g_local_start_ready{false};  // LocalStartReady (any thread)
std::atomic<int> g_local_start_step{0};        // LocalStartInPrologue (any thread): StartStep
// #19 (any thread): this player is in its prologue (kept over loads, cleared at the title), and
// a guest's view of the host in its prologue (the roster).
std::atomic<bool> g_local_prologue{false};
std::atomic<bool> g_host_prologue{false};
std::mutex g_prologue_names_mu;
std::string g_prologue_names;  // other connected members in their prologue (under g_prologue_names_mu)

std::atomic<bool> g_tick_installed{false};

// ---- The game's own Lua dispatches (test mode: log_state) ----
std::atomic<std::uint64_t> g_dispatch_main{0}, g_dispatch_other{0};
std::mutex g_dispatch_mu;
std::map<std::string, std::uint64_t> g_dispatch_seen; // under g_dispatch_mu

BB_COOP_SYSV void DispatchEntry(std::uint64_t ctx, std::uint64_t name_ptr, std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t) {
    char name[80] = "?";
    for (std::size_t i = 0; name_ptr && i + 1 < sizeof name; ++i) {
        char c = 0;
        if (!SafeGet(name_ptr + i, &c) || !c) {
            name[i] = 0;
            break;
        }
        name[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
        name[i + 1] = 0;
    }
    const std::uint64_t tid = ThreadId();
    const bool main = tid == S().tick_thread;
    (main ? g_dispatch_main : g_dispatch_other).fetch_add(1, std::memory_order_relaxed);
    std::uint64_t n;
    {
        std::lock_guard<std::mutex> lk(g_dispatch_mu);
        n = ++g_dispatch_seen[name];
    }
    if (n <= 3 || (n & 255) == 0) {
        Log("game Lua event %s (#%llu, ctx 0x%llx, thread %llu%s)", name, static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(ctx), static_cast<unsigned long long>(tid),
            main ? ", the tick's" : ", NOT the tick's thread");
    }
}

BB_COOP_SYSV void FlipperEntry(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                               std::uint64_t) {
    CoopTick();
}

// ss.info parser (0x1eb65e0(alloc, text, &N, &L, out)) at its one call 0x1e89a21 in the
// ss.info completion 0x1e89850: logs what the game parsed and the result (title debugging;
// BB_PARTY_TRACE_SSINFO=1).
using SsParseFn = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t);
constexpr std::uint64_t kSsParse = 0x1eb65e0, kSsParseCall = 0x1e89a21;
__attribute__((noinline)) BB_COOP_SYSV std::uint64_t SsParseHook(std::uint64_t a0, std::uint64_t text,
                                                                 std::uint64_t n, std::uint64_t l, std::uint64_t out) {
    std::uint32_t nv = 0, lv = 0;
    SafeGet(n, &nv);
    SafeGet(l, &lv);
    // An MSVC-style std::string: +8 inline buffer or pointer (capacity +0x20 > 15), size +0x18.
    std::uint64_t size = 0, cap = 0, data = text + 8;
    SafeGet(text + 0x18, &size);
    SafeGet(text + 0x20, &cap);
    if (cap > 15) {
        SafeGet(text + 8, &data);
    }
    char head[161] = {};
    SafeRead(data, head, size < 160 ? size : 160);
    for (char& c : head) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    const std::uint64_t r = reinterpret_cast<SsParseFn>(Guest(kSsParse))(a0, text, n, l, out);
    Log("ss.info parse: N %u, lang %u, %llu bytes (cap %llu) -> %s; text: %s", nv, lv,
        static_cast<unsigned long long>(size), static_cast<unsigned long long>(cap), (r & 0xff) ? "ok" : "FAILED",
        head);
    return r;
}

// BB_PARTY_TRACE_BELL=1: entry logs along the bell / sign / summon path (first 5 calls of each,
// then every 500th), to see how far a bell gets.
struct TracePoint {
    std::uint64_t off;
    const char* name;
    std::atomic<std::uint64_t> calls{0};
};
TracePoint g_trace_points[] = {
    {0x1900500, "begin sign (0x1900500)"},
    {0x130cde0, "sign Lua task op (0x130cde0)"},
    {0x1901010, "sign task body (0x1901010)"},
    {0x1901320, "SendSign"},
    {0x14b9180, "CreateSign"},
    {0x14b5e10, "SummonStepManager tick"},
    {0x1e90e30, "summon_messenger/create builder"},
    {0x1e946e0, "summon_messenger/get builder"},
    {0x14ba980, "SSM add received sign"},
    {0x14baec0, "host picks a sign (messenger target)"},
    {0x1e98650, "summon_messenger/request builder"},
};
template <int I>
BB_COOP_SYSV void TraceEntry(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t, std::uint64_t,
                             std::uint64_t) {
    TracePoint& t = g_trace_points[I];
    const std::uint64_t n = ++t.calls;
    if (n <= 5 || n % 500 == 0) {
        Log("bell trace: %s (+0x%llx) call #%llu (a0 0x%llx a1 0x%llx a2 0x%llx)", t.name,
            static_cast<unsigned long long>(t.off), static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(a0), static_cast<unsigned long long>(a1),
            static_cast<unsigned long long>(a2));
    }
}
template <int... I>
void InstallBellTrace(std::integer_sequence<int, I...>) {
    (HookPrologue(g_trace_points[I].off, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &TraceEntry<I>,
                  g_trace_points[I].name),
     ...);
}

party::MemberState LocalState(const GameSnapshot& s, bool in_prologue) {
    if (s.loading) {
        return party::MemberState::Loading;
    }
    if (!s.world_up) {
        return party::MemberState::Title;
    }
    if (s.session_role == RoleClient) {
        return party::MemberState::InHostWorld;
    }
    if (s.session_role == RoleTryingToJoin) {
        return party::MemberState::Joining;
    }
    return OwnWorldState(!in_prologue);
}

bool InWorld(const GameSnapshot& s) {
    return s.world_up && !s.loading;
}

/// #19: the last prologue state seen in the world, kept in BB_GPU_USER_DIR/party_prologue.txt, so
/// a guest at its title (no flags to read yet) knows whether the save it continues is still in its
/// prologue: then no title join (it would load that save straight into the host's world). Playtest
/// 1 save audit S5: kept per save slot (SavedPrologueFile), so one character's prologue never holds
/// another's title join; an old unkeyed file is read as the last character's state (as before).
std::mutex g_saved_prologue_mu;
SavedPrologueFile g_saved_prologue;  // under g_saved_prologue_mu
bool g_saved_prologue_loaded = false;
std::string SavedProloguePath() {
    const char* dir = std::getenv("BB_GPU_USER_DIR");
    return dir && dir[0] ? std::string(dir) + "/party_prologue.txt" : std::string();
}
/// Caller holds g_saved_prologue_mu.
SavedPrologueFile& SavedPrologueLocked() {
    if (g_saved_prologue_loaded) return g_saved_prologue;
    g_saved_prologue_loaded = true;
    const std::string path = SavedProloguePath();
    std::string text;
    if (!path.empty()) {
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            char buf[512];
            const std::size_t n = std::fread(buf, 1, sizeof buf, f);
            text.assign(buf, n);
            std::fclose(f);
        }
    }
    g_saved_prologue = ParseSavedPrologue(text);
    return g_saved_prologue;
}
bool LoadSavedPrologue() {
    const int slot = party::flow::CurrentSaveSlot();
    std::lock_guard<std::mutex> lk(g_saved_prologue_mu);
    return SavedPrologueFor(SavedPrologueLocked(), slot);
}
void NoteSavedPrologue(bool prologue) {
    const int slot = party::flow::CurrentSaveSlot();
    std::lock_guard<std::mutex> lk(g_saved_prologue_mu);
    SavedPrologueFile& f = SavedPrologueLocked();
    if (!NoteSavedPrologueFor(f, slot, prologue)) return;
    const std::string path = SavedProloguePath();
    if (!path.empty()) coop::asyncio::Replace(path, FormatSavedPrologue(f));
}

/// Live test of #19 (one PC, saves past the prologue): BB_PARTY_TEST_PROLOGUE=FLAG:0[+FLAG:0...]
/// writes those start flags at the first in-world tick, before the readiness is read (e.g.
/// 12100105:0 in the Hunter's Dream = the first Dream visit with the weapons taken; 12101021:0 =
/// one weapon not taken), so the save is in its prologue before anything could ring. Then
/// BB_PARTY_TEST_PROLOGUE_END=S[:LAMP] after S s in the world either restores them (no LAMP) or
/// lamp-warps to LAMP (2412950 = the sickroom lamp, the headstone's vanilla target): the end of
/// this player's prologue.
void TestPrologueTick(const GameSnapshot& s) {
    static const char* spec = std::getenv("BB_PARTY_TEST_PROLOGUE");
    if (!spec || !spec[0]) return;
    static bool written = false, ended = false;
    static Clock::time_point at{};
    static std::vector<std::pair<std::uint32_t, bool>> flags;
    if (!written) {
        written = true;
        at = Clock::now();
        for (const char* p = spec; *p;) {
            char* end = nullptr;
            const auto id = static_cast<std::uint32_t>(std::strtoul(p, &end, 10));
            const bool on = !(end && end[0] == ':' && end[1] == '0');
            if (id) flags.emplace_back(id, on);
            p = end && end != p ? end : p + 1;
            while (*p && *p != '+') ++p;
            if (*p == '+') ++p;
        }
        for (const auto& [id, on] : flags) {
            bool before = false, after = false;
            ReadEventFlag(id, &before);
            const bool ok = WriteEventFlag(id, on);
            ReadEventFlag(id, &after);
            Log("test: prologue: flag %u: %d -> %d (%s; %s)", id, int(before), int(after), ok ? "written" : "NOT written",
                MapName(s.map_id).c_str());
        }
        return;
    }
    const char* e = std::getenv("BB_PARTY_TEST_PROLOGUE_END");
    if (ended || !e || !e[0] || Seconds(at, Clock::now()) < std::atof(e)) return;
    ended = true;
    const char* colon = std::strchr(e, ':');
    const std::uint32_t lamp = colon ? static_cast<std::uint32_t>(std::strtoul(colon + 1, nullptr, 10)) : 0;
    if (lamp) {
        Log("test: prologue end: lamp warp to %u (the headstone's target) from %s", lamp, MapName(s.map_id).c_str());
        LampWarp(lamp);
        return;
    }
    for (const auto& [id, on] : flags) {
        const bool ok = WriteEventFlag(id, !on);
        Log("test: prologue end: flag %u back to %d (%s)", id, int(!on), ok ? "written" : "NOT written");
    }
}

/// 0.7.5 Party starts "skip the prologue": BB_PARTY_START=skip_prologue, or the host's rule
/// (party_skip_prologue; on a guest the host's once it arrived, else its own).
static bool SkipPrologueOn(StartMode env_mode) {
    return env_mode == StartMode::SkipPrologue || ::party::status::HostRuleOn(::party::status::HostRule::SkipPrologue);
}

/// The start flags (full ticks): logs step and readiness changes.
void UpdateStart(State& st, const GameSnapshot& s, StartMode mode) {
    if (s.world_up && s.loading) {
        return; // a loading screen keeps the last reading (the flags do not go back)
    }
    const bool in_world = InWorld(s);
    // In another player's world the event flags are that world's (the host snapshot / world mirror;
    // during the swap the opening flags read 0), not this save's: a guest past its prologue was
    // read as "opening" in the host's Central Yharnam (no rot, syncs held, party_prologue.txt = 1).
    // Nobody standing in the host's world is in its own prologue (a prologue save is never summoned
    // nor title-joined): ready, flags not read, party_prologue.txt untouched.
    const bool other_world = in_world && (s.session_role == RoleClient || b2::InOtherWorld());
    if (other_world) {
        if (st.start_prologue || !st.start_ready || st.start_step != StartStep::Ready)
            Log("start (%s): in the host's world (%s): not in its own prologue, ready (its flags are that "
                "world's)", StartModeName(mode), MapName(s.map_id).c_str());
        st.start_prologue = false;
        st.start_step = StartStep::Ready;
        st.start_ready = true;
        st.start_logged = true;
        g_local_prologue.store(false);
        SetLocalStartReady(true);
        g_local_start_ready.store(true);
        g_local_start_step.store(int(StartStep::Ready));
        return;
    }
    if (in_world) {
        TestPrologueTick(s);
        if (!st.weapons_checked && progress::FlagLoadMode() == 0 &&
            (s.session_role == RoleIdle || s.session_role == RoleHost)) {
            // Only the own save in view (load mode 0): leaving a host's world, the overlay is still
            // loaded for a moment (session role 7) and a write there is gone after the home load.
            // 0.7.3: a starting weapon pick synced in from another player's save (12101020 / 21
            // were npc_quest before): done without a pick of its own hides this player's
            // messengers for good. Cleared in the own save, the messengers offer the weapons at
            // the next Dream load.
            st.weapons_checked = true;
            for (const std::uint32_t id : StartWeaponFlagsToClear(&ReadEventFlag)) {
                const bool ok = WriteEventFlag(id, false);
                Log("start: starting weapon flag %u was set without a pick of this player's own (another "
                    "player's pick, synced by an older build): %s; the Dream's messengers offer the weapon again",
                    id, ok ? "cleared" : "NOT cleared");
            }
        }
        st.cs = ReadStartFlags();
    } else if (!s.world_up) {
        st.cs = StartFlags{};
        st.granted = false;
        st.weapons_checked = false;
        if (!s.loading) {
            st.left_first_dream = false;  // the title: another save may come next
            st.start_prologue = false;
        }
    }
    // #19: the first Dream visit lasts until the headstone takes the player out of the Dream.
    st.cs.map_id = in_world ? s.map_id : 0xffffffffu;
    if (in_world && st.cs.flags_ok && WeaponsDone(st.cs) && st.cs.first_dream &&
        !st.left_first_dream && s.map_id != 0 && s.map_id != 0xffffffffu && !InHuntersDream(s.map_id)) {
        st.left_first_dream = true;
        Log("start: out of the first Hunter's Dream visit (%s): the prologue is over for this player",
            MapName(s.map_id).c_str());
    }
    st.cs.left_first_dream = st.left_first_dream;
    const StartStep step = ClassifyStart(st.cs, in_world);
    const bool ready = StartReady(mode, st.cs, in_world);
    if (in_world && st.cs.flags_ok) {
        st.start_prologue = mode == StartMode::Immediate
                                ? !st.cs.opening_done
                                : (step == StartStep::Opening || step == StartStep::Clinic || step == StartStep::FirstDream);
    }
    g_local_prologue.store(st.start_prologue);
    if (in_world && st.cs.flags_ok) NoteSavedPrologue(st.start_prologue);
    if (!st.start_logged || step != st.start_step || ready != st.start_ready) {
        if (in_world || st.start_logged) {
            Log("start (%s): %s -> %s", StartModeName(mode), DescribeStart(st.cs, in_world).c_str(),
                ready ? "READY for party summons" : "not ready");
        }
        st.start_logged = st.start_logged || in_world;
    }
    st.start_step = step;
    st.start_ready = ready;
    SetLocalStartReady(ready);
    g_local_start_ready.store(ready);
    g_local_start_step.store(int(step));
}

/// The bells a ready player lacks (§4.3), once per world, in its own world only.
void GrantBells(State& st, const GameSnapshot& s) {
    if (st.granted || !st.start_ready || !InWorld(s) || s.session_role != RoleIdle || st.cs.cutscene ||
        b2::InOtherWorld()) {
        return;
    }
    const BellGrant g = PlanBellGrant(st.cs);
    if (st.cs.beckoning_count < 0 || st.cs.resonant_count < 0) {
        return; // inventory not readable yet
    }
    st.granted = true;
    if (!g.any()) {
        return;
    }
    Log("bells: before: %s", DescribeStart(st.cs, true).c_str());
    if (g.beckoning_lot) {
        Log("bells: Beckoning Bell (lot %u): %s", kLotBeckoning, AwardItemLot(kLotBeckoning) ? "awarded" : "NOT awarded");
    }
    if (g.beckoning_goods) {
        Log("bells: Beckoning Bell (goods %u, 6622 already on): %s", kGoodsBeckoning,
            GiveGoods(kGoodsBeckoning, 1) ? "given" : "NOT given");
    }
    if (g.resonant) {
        const bool ok = GiveGoods(kGoodsSmallResonant, 1);
        const bool flag = ok && WriteEventFlag(kFlagResonantShop, true);
        Log("bells: Small Resonant Bell (goods %u): %s, flag %u %s", kGoodsSmallResonant, ok ? "given" : "NOT given",
            kFlagResonantShop, flag ? "set" : "not set");
    }
    st.cs = ReadStartFlags();
    st.cs.map_id = s.map_id;
    st.cs.left_first_dream = st.left_first_dream;
    Log("bells: after: %s", DescribeStart(st.cs, true).c_str());
}

void LogTransitions(State& st, const GameSnapshot& now) {
    if (!st.have_last) {
        Log("state at the first tick: %s", Describe(now).c_str());
        bb_boot_mark("party director: first tick");
        return;
    }
    const GameSnapshot& was = st.last;
    if (now.world_up != was.world_up) {
        Log("world %s: %s", now.world_up ? "up" : "down", Describe(now).c_str());
        static bool first_world = true;
        if (now.world_up && first_world) {
            first_world = false;
            bb_boot_mark("world up (loading)");
            BbBootWatchState::Done(); // BB_BOOT_WATCHDOG: the boot is over
        }
        if (!now.world_up) BossBarReset();  // A11: a load ends any boss fight
    }
    // A guest's travel re-join shows the game's loading card well before its loading flag rises
    // (smoke s1: card at +100 s, flag at +110 s): the overlay hides from the host's travel on.
    BbFrameState::NoteGameLoading(now.loading || b2::GuestTravelPending());
    if (now.loading != was.loading) {
        Log("loading screen %s", now.loading ? "up" : "gone");
        static int load_marks = 0;
        if (load_marks < 2 && (load_marks == 0) == now.loading) {
            bb_boot_mark(load_marks++ == 0 ? "first loading screen up" : "first loading screen gone (in the world)");
        }
    }
    if (now.session_role != was.session_role || now.session_sub_state != was.session_sub_state) {
        Log("session %s (%d, sub %d) -> %s (%d, sub %d) (t %.1f s)", SessionRoleName(was.session_role), was.session_role,
            was.session_sub_state, SessionRoleName(now.session_role), now.session_role, now.session_sub_state,
            Seconds(st.start, Clock::now()));
    }
    if (now.matching_status != was.matching_status) {
        Log("matching status %d -> %d", was.matching_status, now.matching_status);
    }
    if (now.map_id != was.map_id && now.world_up) {
        Log("map %s -> %s", MapName(was.map_id).c_str(), MapName(now.map_id).c_str());
    }
    if (now.cooperators != was.cooperators && now.cooperators >= 0 && was.cooperators >= 0) {
        Log("cooperators %d -> %d (t %.1f s)", was.cooperators, now.cooperators, Seconds(st.start, Clock::now()));
        if (now.cooperators > was.cooperators && now.session_role == RoleHost) party::runtime_nr_prescan();
    }
    if (now.online_mode != was.online_mode) {
        Log("online mode %d -> %d", was.online_mode, now.online_mode);
    }
    if (now.insight != was.insight && now.insight >= 0 && was.insight >= 0) {
        Log("Insight %d -> %d", was.insight, now.insight);
    }
    if (now.echoes != was.echoes && now.echoes >= 0 && was.echoes >= 0) {
        Log("Echoes %d -> %d", was.echoes, now.echoes);  // H3 evidence: echoes earned in the host's world
    }
}

std::string DescribeFromClient();
std::string DescribeSignPolling();

// The bells as the items ring them: the goods' SpEffect on the local player (Beckoning Bell 200
// -> 9000, Small Resonant Bell 205 -> 9005). The player's update (0x18FEF50) sees the effect's
// param flag and calls 0x1900500(player, sign type), which queues the Lua task (0x1314A40, op
// 0x130CDE0) that runs SendSign 0x1901010/0x1901320 -> CreateSign -> summon_messenger/create; the
// active-bell updater 0x1506820 keeps the searching visuals (9003/9004, 9008/9009). The named Lua
// events OnEvent_Call_SOS / OnEvent_SendSoulSign_NormalCoop are what the game raises AFTER a
// summon / a sign (0x130CA87, 0x1901794): raising them only runs the Lua reaction (animation,
// RecallMenuEvent), never a sign (BB_PARTY_TRACE_BELL=1 showed no SendSign / CreateSign).
// Apply: ChrIns vtable +0x3F0 (chr, id, source chr, 0, 1, 0, 0; xmm0-4 = 1.0), the call the
// updater itself makes at 0x1506A29.
constexpr int kHostBellEffect = 9000, kGuestBellEffect = 9005;
constexpr int kChaliceWaitEffect = 9900; // the Dream bell's chalice co-op wait (stateInfo 269)
using AddSpEffectFn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                          std::uint64_t, std::uint64_t, float, float, float, float, float);

std::uint64_t LocalPlayer() {
    std::uint64_t wcm = 0, player = 0;
    SafeGet(Guest(0x553e878), &wcm);
    if (wcm) {
        SafeGet(wcm + 0x60, &player);
    }
    return player;
}

/// The player has SpEffect `id` (list at ChrIns +0x1C8: first node at +8, id +0x40, next +0x58).
bool HasSpEffect(std::uint64_t player, int id) {
    std::uint64_t list = 0, node = 0;
    if (!player || !SafeGet(player + 0x1c8, &list) || !list || !SafeGet(list + 8, &node)) {
        return false;
    }
    for (int guard = 0; node && guard < 512; ++guard) {
        std::int32_t nid = 0;
        if (!SafeGet(node + 0x40, &nid)) {
            return false;
        }
        if (nid == id) {
            return true;
        }
        if (!SafeGet(node + 0x58, &node)) {
            return false;
        }
    }
    return false;
}

/// Main thread only: removes SpEffect `id` from the local player through the game's own
/// RemoveSpEffect 0x17E1D90(list, node, 1) (EMEVD 2004[21]). False when it is not on.
bool RemoveSpEffect(int id) {
    const std::uint64_t player = LocalPlayer();
    std::uint64_t list = 0, node = 0;
    if (!player || !SafeGet(player + 0x1c8, &list) || !list || !SafeGet(list + 8, &node)) {
        return false;
    }
    for (int guard = 0; node && guard < 512; ++guard) {
        std::int32_t nid = 0;
        if (!SafeGet(node + 0x40, &nid)) {
            return false;
        }
        if (nid == id) {
            using Fn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint32_t);
            reinterpret_cast<Fn>(Guest(0x17e1d90))(list, node, 1);
            return true;
        }
        if (!SafeGet(node + 0x58, &node)) {
            return false;
        }
    }
    return false;
}

/// Main thread only (the director's tick).
bool ApplySpEffect(int id) {
    const std::uint64_t player = LocalPlayer();
    std::uint64_t vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) {
        return false;
    }
    reinterpret_cast<AddSpEffectFn>(fn)(player, static_cast<std::uint64_t>(id), player, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f,
                                        1.0f, 1.0f);
    return true;
}

/// Fast join, host: the game's peer pool (SummonStepManager [FrpgNetMan+0xc50] +0x48; used list
/// sentinel at +0x58, node {next, prev, value +0x10}, peer = [value+0x20]) keeps a peer reserved
/// for a summoned player's online id (+0x64, 16 bytes) while its keep flag (+0xe4) is set; a new
/// summon of the same id (0x147af20) renews and reuses it even when its connection belongs to a
/// crashed game process, so the summon data goes nowhere (docs/party/join_timing.md). For a
/// member that restarted, the stale peers lose the id and the keep: the next summon reserves a
/// fresh peer. Main thread only. Returns how many peers were cleared.
int ClearStalePeers(const std::string& online_id) {
    std::uint64_t fm = 0, ssm = 0, pool = 0, head = 0, node = 0;
    if (!SafeGet(Guest(0x553b120), &fm) || !fm || !SafeGet(fm + 0xc50, &ssm) || !ssm || !SafeGet(ssm + 0x48, &pool) ||
        !pool || !SafeGet(pool + 0x58, &head) || !head || !SafeGet(head, &node)) {
        return -1;
    }
    int cleared = 0;
    for (int guard = 0; node && node != head && guard < 64; ++guard) {
        std::uint64_t value = 0, peer = 0;
        char id[17] = {};
        if (SafeGet(node + 0x10, &value) && value && SafeGet(value + 0x20, &peer) && peer &&
            SafeRead(peer + 0x64, id, 16) && std::strncmp(id, online_id.c_str(), 16) == 0) {
            const char mark = '#'; // no online id starts with it
            const std::uint8_t off = 0;
            SafeWrite(peer + 0x64, &mark, 1);
            SafeWrite(peer + 0xe4, &off, 1);
            ++cleared;
        }
        if (!SafeGet(node, &node)) {
            break;
        }
    }
    return cleared;
}

void Ring(State& st, const char* event, const char* why, Clock::time_point now) {
    if (!st.rung_ever && st.world_seen) {  // A4 join speed: the first bell's wait in the world
        Log("first bell %.1f s after the world was steady", Seconds(st.world_at, now));
    }
    st.last_ring = now;
    st.rung_ever = true;
    st.poll_logged = false;
    const bool host = std::strcmp(event, kHostBell) == 0;
    const char* mode = std::getenv("BB_PARTY_BELL_MODE");
    if (mode && std::strcmp(mode, "lua") == 0) {
        if (LuaEventQueue(event)) {
            party::bridge::OnRing();
            Log("ringing %s (%s; Lua event)", event, why);
        } else {
            Log("could not queue %s (%s)", event, why);
        }
        return;
    }
    const int effect = host ? kHostBellEffect : kGuestBellEffect;
    const std::uint64_t player = LocalPlayer();
    const bool had = HasSpEffect(player, effect);
    const bool ok = ApplySpEffect(effect);
    // A guest while the host is in a Chalice Dungeon: also the vanilla Dream bell's effect 9900
    // (SpEffectParam stateInfo 269). The player's update (0x193ac10) turns it into FrpgNetMan +0xb9c
    // bit 0 -> +0xb14 bit 0; only with it does the summon handler 0x14ba340 take the message's
    // ChannelId (state 2), fetch the dungeon (get_details_info) and quick-join it before loading
    // the host's map (0x14bb200 -> 0x1aa5280). Without it the guest loads its own Dream (seen
    // live). The party's Dream bell patch (0x157F6D1) rings the normal 9005 instead.
    if (!host && ok) {
        std::uint32_t host_map = 0;
        party::PartyLink* link = nullptr;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            link = st.link;
        }
        if (link) {
            for (const party::RosterEntry& e : link->roster()) {
                if (e.slot == party::kHostSlot) {
                    host_map = e.map_id;
                }
            }
        }
        if (((host_map >> 24) & 0xff) == 29 && host_map != 0xffffffffu) {
            const bool chalice_ok = ApplySpEffect(kChaliceWaitEffect);
            Log("bell: the host is in a chalice dungeon (%s): SpEffect %d %s", MapName(host_map).c_str(),
                kChaliceWaitEffect, chalice_ok ? "applied too (quick join on summon)" : "NOT applied");
        }
    }
    if (ok) {
        party::bridge::OnRing();
        if (!host) b2::DirectSummonRing();  // party_dsummon.h
    }
    Log("bell: %s", DescribeFromClient().c_str());
    Log("bell: %s", DescribeSignPolling().c_str());
    Log("ringing the %s (%s): SpEffect %d %s%s", host ? "Beckoning Bell" : "Small Resonant Bell", why, effect,
        ok ? "applied" : "NOT applied (no player)", ok ? (HasSpEffect(player, effect) ? (had ? ", was already on" : ", now on")
                                                                              : ", not on the list after the call")
                                          : "");
}

constexpr std::uint32_t kPadCross = 0x4000;

/// The game's FROM-server client as it stands (title debugging): SprjNetworkClientMan (slot
/// 0x5540288: +0x18 parsed ss.info, its +8 <ss> status; +0x3e4 ss.info failures; +0x60 UserId,
/// +0xd0 ss.info reload timer) and FrpgNetMan (slot 0x553b120: +0xa online flag, +0xa30 queued
/// error message, +0xa50 server-offline flag).
std::string DescribeFromClient() {
    std::uint64_t cm = 0, fm = 0, ss = 0;
    SafeGet(Guest(0x5540288), &cm);
    SafeGet(Guest(0x553b120), &fm);
    std::int32_t ss_status = -1, fails = -1, msg = -1;
    std::int64_t user = -1;
    float reload = -1;
    std::uint8_t online = 0xff, offline = 0xff, b8 = 0xff, b9f6 = 0xff, b9f8 = 0xff, bb = 0xff;
    if (cm) {
        SafeGet(cm + 0x18, &ss);
        SafeGet(cm + 0x3e4, &fails);
        SafeGet(cm + 0x60, &user);
        SafeGet(cm + 0xd0, &reload);
        if (ss) {
            SafeGet(ss + 8, &ss_status);
        }
    }
    if (fm) {
        SafeGet(fm + 0xa, &online);
        SafeGet(fm + 0xa30, &msg);
        SafeGet(fm + 0xa50, &offline);
        SafeGet(fm + 8, &b8);
        SafeGet(fm + 0xb, &bb);
        SafeGet(fm + 0x9f6, &b9f6);
        SafeGet(fm + 0x9f8, &b9f8);
    }
    char b[256];
    std::snprintf(b, sizeof b,
                  "FROM client: ss.info %s (status %d, failures %d, reload in %.0f s), user id %lld; FrpgNetMan online %d, "
                  "error msg 0x%x, server-offline %d (+8 %d, +0xb %d, +0x9f6 %d, +0x9f8 %d)",
                  ss ? "parsed" : "none", ss_status, fails, reload, static_cast<long long>(user), online, msg, offline,
                  b8, bb, b9f6, b9f8);
    return b;
}

/// The sign polling as the game holds it (join timing, docs/party/join_timing.md): the parsed
/// ss.info ([cm+0x18]: +0x8a0 SummonDataGetListInterval, +0x8a8..+0x8b4 the WaitCounts) and the
/// FROM client's own countdown (+0x100) and per-type wait counters (+0xb4 normal coop, +0xbc/+0xc0
/// invasion, +0xc4 natural enemy), read by the get-list builder 0x1e946e0.
std::string DescribeSignPolling() {
    std::uint64_t cm = 0, ss = 0;
    SafeGet(Guest(0x5540288), &cm);
    if (cm) {
        SafeGet(cm + 0x18, &ss);
    }
    std::int32_t interval = -1, w0 = -1, w1 = -1, w2 = -1, w3 = -1, c0 = -1, c1 = -1, c2 = -1, c3 = -1;
    float countdown = -1;
    if (ss) {
        SafeGet(ss + 0x8a0, &interval);
        SafeGet(ss + 0x8a8, &w0);
        SafeGet(ss + 0x8ac, &w1);
        SafeGet(ss + 0x8b0, &w2);
        SafeGet(ss + 0x8b4, &w3);
    }
    if (cm) {
        SafeGet(cm + 0x100, &countdown);
        SafeGet(cm + 0xb4, &c0);
        SafeGet(cm + 0xbc, &c1);
        SafeGet(cm + 0xc0, &c2);
        SafeGet(cm + 0xc4, &c3);
    }
    char b[256];
    std::snprintf(b, sizeof b,
                  "sign polling: interval %d s (next in %.2f s), wait counts coop %d / invasion %d / enemy %d / all-area %d; "
                  "counters %d %d %d %d",
                  interval, countdown, w0, w1, w2, w3, c0, c1, c2, c3);
    return b;
}

/// Title screen: confirms "Continue" (the main menu's default item once a save exists) with the
/// game's own pad path (runtime_pad_director_press: no OS input) until the world loads. Cross is
/// pressed 0.15 s every 0.35 s (2.5 s without fast join) while no world was seen in this run, never in the world or while a
/// loading screen is up; the party link must be up first (the online login runs through it).
void AutoContinue(State& st, const GameSnapshot& s, PartyRole role, party::PartyLink* link, Clock::time_point now) {
    b2::SetTitleContinuing(!st.continue_done && !s.world_up);
    if (st.continue_done) {
        return;
    }
    // BB_BOOT_WATCHDOG=S (diagnostics, bbport_boot_watch.h): no press for S s (3 S while a load is
    // up) and nothing holding it -> the probe's watchdog writes a thread snapshot (once per spell).
    static const double watch_s = BbBootWatchState::LimitSeconds();
    auto moved = [&] {
        st.continue_moved = now;
        st.continue_watch_asked = false;
    };
    if (watch_s > 0.0) {
        if (st.continue_moved == Clock::time_point{}) {
            st.continue_moved = now;
        }
        const double quiet = Seconds(st.continue_moved, now);
        if (!st.continue_watch_asked && quiet >= (s.loading ? 3.0 * watch_s : watch_s)) {
            st.continue_watch_asked = true;
            char why[320];
            std::snprintf(why, sizeof why, "no Continue press for %.0f s after %d press(es) (%s; link %d, waiting %d)",
                          quiet, st.continue_presses, Describe(s).c_str(),
                          link ? static_cast<int>(link->state()) : -1, st.continue_waiting ? 1 : 0);
            Log("auto-continue: %s: asking for a boot snapshot (BB_BOOT_WATCHDOG)", why);
            BbBootWatchState::Request(why);
        }
    }
    auto release = [&] {
        if (st.continue_held) {
            runtime_pad_director_press(0);
            st.continue_held = false;
        }
    };
    static const bool trace = std::getenv("BB_PARTY_TITLE_TRACE") && std::getenv("BB_PARTY_TITLE_TRACE")[0] == '1';
    if (trace) {
        // The title's screens by their texts (message ids looked up in the last 0.4 s).
        static std::string last;
        std::uint32_t ids[64];
        const int n = BbGameMenu::RecentLookups(400, ids, 64);
        std::sort(ids, ids + (n < 64 ? n : 64));
        std::string text;
        for (int i = 0; i < n && i < 64; ++i) {
            char b[16];
            std::snprintf(b, sizeof b, " %#x", ids[i]);
            text += b;
        }
        if (text != last) {
            Log("title texts:%s", text.c_str());
            last = text;
        }
    }
    if (s.world_up || st.world_seen) {
        release();
        st.continue_done = true;
        Log("auto-continue: world up after %d press(es)", st.continue_presses);
        return;
    }
    if (s.loading) {
        release();
        st.continue_waiting = false;
        return;
    }
    const party::LinkState ls = link ? link->state() : party::LinkState::Idle;
    const bool link_ok = role == PartyRole::Host ? ls == party::LinkState::Hosting : ls == party::LinkState::Connected;
    if (!link_ok) {
        release();
        st.continue_waiting = false;
        moved();
        return;
    }
    if (!st.continue_waiting) {
        st.continue_waiting = true;
        st.continue_ready = now;
        st.continue_last = now;
        Log("auto-continue: party link up; confirming Continue on the title (%s)",
            party::runtime().restarted() ? "crash restart" : "party mode");
        bb_boot_mark("auto-continue: title, party link up");
    }
    // Let the title settle (logos, online login) before the first press.
    if (Seconds(st.continue_ready, now) < st.continue_settle) {
        return;
    }
    if (st.continue_held) {
        if (Seconds(st.continue_last, now) >= 0.15) {
            release();
        }
        return;
    }
    // The FROM sign-in (ss.info -> login -> sync_chara_id -> notices) starts once the title takes
    // its first press; Continue while it runs leaves the game offline: wait for it (<= 30 s).
    const bbnet::OnlineProgress op = bbnet::online_progress();
    const bool signing_in = op.ss_info > 0 && op.notice == 0 && op.since_last >= 0 && op.since_last < 30.0;
    if (signing_in != st.continue_signin) {
        st.continue_signin = signing_in;
        if (!signing_in) {
            st.continue_last = now - std::chrono::milliseconds(int(st.continue_every * 1000)); // Continue at once
        }
        Log("auto-continue: %s (ss.info %d, login %d, chara id %d, notice %d, failures %d); %s",
            signing_in ? "the FROM sign-in runs; holding Continue" : "the FROM sign-in is over", op.ss_info, op.login,
            op.chara_id, op.notice, op.failures, DescribeFromClient().c_str());
    }
    if (signing_in) {
        st.continue_last = now;
        moved();
        return;
    }
    // Guest: join the host's session from here first, so Continue loads the host's world (one load).
    if (role == PartyRole::Guest && b2::TitleJoinHold(s)) {
        st.continue_last = now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(st.continue_every));
        moved();
        return;
    }
    if (Seconds(st.continue_ready, now) > 40.0) {
        release();
        st.continue_done = true;
        Log("auto-continue: stopped after 40 s (%d presses, no world yet); the title is left to the player",
            st.continue_presses);
        return;
    }
    if (Seconds(st.continue_last, now) >= st.continue_every) {
        st.continue_last = now;
        st.continue_held = true;
        ++st.continue_presses;
        runtime_pad_director_press(kPadCross);
        if (st.continue_presses == 1) {
            bb_boot_mark("auto-continue: first Cross");
        }
        moved();
        if (st.continue_presses <= 12 || st.continue_presses % 10 == 0) {
            Log("auto-continue: Cross #%d (%s; %s)", st.continue_presses, Describe(s).c_str(), DescribeFromClient().c_str());
        }
        // Test aid BB_BOOT_TEST_EXIT=N (boot loops, tools/mp): end the game at the Nth press with
        // exit code 3, which run.bat's party crash restart turns into the next boot.
        static const int test_exit = [] {
            const char* e = std::getenv("BB_BOOT_TEST_EXIT");
            return e && e[0] ? std::atoi(e) : 0;
        }();
        if (test_exit > 0 && st.continue_presses >= test_exit) {
            Log("test: boot loop: ending the game at Cross #%d (BB_BOOT_TEST_EXIT)", st.continue_presses);
            std::fflush(stdout);
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), 3);
#else
            _exit(3);
#endif
        }
    }
}

constexpr std::uint64_t kLampWarp = 0x13cdf30; // void (u32 ReturnPointParam id): lamp / Dream / headstone
constexpr std::uint64_t kForceDead = 0x13320c0; // Lua ForceDead(ctx unused, entity id; 10000 = local player)

const char* TravelStepName(TravelStep::Kind k) {
    switch (k) {
    case TravelStep::HostWarp: return "host_warp";
    case TravelStep::HostDie: return "host_die";
    case TravelStep::GuestDie: return "guest_die";
    case TravelStep::HostKill: return "host_kill";
    case TravelStep::HostMove: return "host_move";
    case TravelStep::GuestMove: return "guest_move";
    case TravelStep::HostPress: return "host_press";
    case TravelStep::GuestPress: return "guest_press";
    case TravelStep::HostOutage: return "host_outage";
    case TravelStep::HostPoise: return "host_poise";
    case TravelStep::HostGfx: return "host_gfx";
    case TravelStep::HostSpfx: return "host_spfx";
    case TravelStep::GuestOutage: return "guest_outage";
    case TravelStep::HostMark: return "host_mark";
    case TravelStep::GuestMark: return "guest_mark";
    case TravelStep::HostStage: return "host_stage";
    case TravelStep::HostLastLamp: return "host_lastlamp";
    case TravelStep::GuestLastLamp: return "guest_lastlamp";
    case TravelStep::HostLamp: return "host_lamp";
    case TravelStep::HostWalk: return "host_walk";
    case TravelStep::HostHeadstone: return "host_headstone";
    case TravelStep::HostFlag: return "host_flag";
    case TravelStep::GuestFlag: return "guest_flag";
    case TravelStep::HostObj: return "host_obj";
    case TravelStep::GuestObj: return "guest_obj";
    case TravelStep::HostChr: return "host_chr";
    case TravelStep::GuestChr: return "guest_chr";
    case TravelStep::GuestLamp: return "guest_lamp";
    case TravelStep::GuestKill: return "guest_kill";
    case TravelStep::HostAlone: return "host_alone";
    case TravelStep::HostSkipRemo: return "host_skipremo";
    case TravelStep::GuestSkipRemo: return "guest_skipremo";
    case TravelStep::GuestStick: return "guest_stick";
    case TravelStep::HostStick: return "host_stick";
    case TravelStep::HostRoute: return "host_route";
    case TravelStep::GuestRoute: return "guest_route";
    case TravelStep::HostUse: return "host_use";
    case TravelStep::GuestUse: return "guest_use";
    case TravelStep::GuestFixture: return "guest_fixture";
    case TravelStep::GuestHome: return "guest_home";
    case TravelStep::HostKick: return "host_kick";
    case TravelStep::HostLeave: return "host_leave";
    case TravelStep::GuestLeave: return "guest_leave";
    case TravelStep::HostRejoin: return "host_rejoin";
    case TravelStep::GuestRejoin: return "guest_rejoin";
    case TravelStep::HostGather: return "host_gather";
    }
    return "?";
}

/// host_gfx=ARG (popin, test aid for graphics A/B sweeps inside one session):
///   lod:N  the game's own model LOD bias (RendMan 0x5540298 -> +0x10 -> +0x530; the LOD update
///          0x1D41D60 copies it into 0x5587D20 every frame and all three model draw paths add it to
///          the distance level, clamped to 0..2; a Model LOD patch replaces one of those reads)
///   cap    one frame analyzer record and one presented-buffer dump (creates the files named by
///          BB_CAPTURE_TRIGGER and BB_PRESENT_DUMP_TRIGGER; docs/upscaler.md)
///   log    the render camera block (RendMan config +0x50): viewport, FOV, LOD distance scale
void GfxTestStep(const std::string& arg) {
    std::uint64_t rm = 0, cfg = 0;
    if (!SafeGet(Guest(0x5540298), &rm) || !rm || !SafeGet(rm + 0x10, &cfg) || !cfg) {
        Log("test: host_gfx %s: no render manager", arg.c_str());
        return;
    }
    std::int32_t bias = 0, vp[2] = {};
    float fov = 0, scale = 0, cam[3] = {};
    SafeGet(cfg + 0x530, &bias);
    SafeRead(cfg + 0x50 + 0x110, vp, sizeof vp);
    SafeGet(cfg + 0x50 + 0x140, &fov);
    SafeGet(cfg + 0x50 + 0x150, &scale);
    SafeRead(cfg + 0x50 + 0x80, cam, sizeof cam);
    // The map collision the player stands on (chr +0x288, a 0x138-byte hit record) and its index
    // in its block's hit array (*0x553E850 +0x28: block records of 0xB8, +0x78 count, +0x80 hits):
    // the collision decides which of the map's draw groups are shown.
    std::uint64_t hit = 0, man = 0, recs = 0;
    int blocks = 0, hit_block = -1, hit_index = -1;
    if (const std::uint64_t player = LocalPlayer()) SafeGet(player + 0x288, &hit);
    if (hit && SafeGet(Guest(0x553e850), &man) && man && SafeGet(man + 0x20, &blocks) && SafeGet(man + 0x28, &recs)) {
        for (int i = 0; recs && i < blocks && i < 4096; ++i) {
            int n = 0;
            std::uint64_t base = 0;
            SafeGet(recs + std::uint64_t(i) * 0xb8 + 0x78, &n);
            SafeGet(recs + std::uint64_t(i) * 0xb8 + 0x80, &base);
            if (base && n > 0 && hit >= base && hit < base + std::uint64_t(n) * 0x138) {
                hit_block = i;
                hit_index = int((hit - base) / 0x138);
            }
        }
    }
    if (arg.rfind("lod:", 0) == 0) {
        const std::int32_t want = std::clamp(std::atoi(arg.c_str() + 4), -2, 2);
        SafeWrite(cfg + 0x530, &want, sizeof want);
        Log("test: host_gfx: model LOD bias %d -> %d", bias, want);
    } else if (arg == "cap") {
        for (const char* env : {"BB_CAPTURE_TRIGGER", "BB_PRESENT_DUMP_TRIGGER"}) {
            if (const char* path = std::getenv(env); path && *path) {
                if (FILE* f = std::fopen(path, "w")) std::fclose(f);
            }
        }
        Log("test: host_gfx: capture (LOD bias %d, camera %.2f %.2f %.2f, hit #%d of block #%d)", bias, cam[0], cam[1],
            cam[2], hit_index, hit_block);
    } else {
        Log("test: host_gfx: LOD bias %d, viewport %dx%d, fov %.4f, LOD distance scale %.4f, camera %.2f %.2f %.2f, "
            "hit #%d of block #%d",
            bias, vp[0], vp[1], fov, scale, cam[0], cam[1], cam[2], hit_index, hit_block);
    }
}

constexpr std::uint64_t kWarpNextStage = 0x132e010; // Lua WarpNextStage(ctx unused, area, block, region, index, point)
constexpr std::uint64_t kWarpNextStageBonfire = 0x132e050; // Lua WarpNextStage_Bonfire(ctx unused, id)
constexpr std::uint32_t kPadDownTest = 0x40;

/// The local chr's map stage (PlayerIns +0x3F8, what the stage-UID check 0x193C537 compares) and position.
std::string DescribeHere(const GameSnapshot& s) {
    std::uint32_t stage = 0;
    float p[4] = {};
    if (s.player) {
        // The physics position (chr +0x58 -> +8 -> +0x3B0 -> +0x68, +0x1E0; +0xC0 lags behind a warp).
        std::uint64_t a = 0;
        SafeGet(s.player + 0x3f8, &stage);
        if (SafeGet(s.player + 0x58, &a) && a && SafeGet(a + 8, &a) && a && SafeGet(a + 0x3b0, &a) && a &&
            SafeGet(a + 0x68, &a) && a) {
            SafeRead(a + 0x1e0, p, 12);
        }
    }
    char b[200];
    std::snprintf(b, sizeof b, "stage %#x pos (%.2f, %.2f, %.2f), %s", stage, p[0], p[1], p[2], Describe(s).c_str());
    return b;
}

/// Puts the local chr at `xyz` / yaw (PlayerIns vfunc +0x5F8 = 0x18CD600, the guest services
/// fixture's call): no load.
bool PutChr(const GameSnapshot& s, const float xyz[3], float yaw) {
    alignas(16) float pos[4] = {xyz[0], xyz[1], xyz[2], 1.0f};
    alignas(16) float rot[4] = {0, yaw, 0, 0};
    std::uint64_t vtbl = 0, fn = 0;
    if (!s.player || !SafeGet(s.player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) {
        return false;
    }
    using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
    return true;
}

/// HostWalk / HostHeadstone, one tick.
void RunTravelScript(State& st, const GameSnapshot& s, Clock::time_point now) {
    Test::Script& sc = st.test.script;
    auto tap = [&](std::uint32_t buttons) {
        runtime_pad_director_press(buttons);
        sc.pressed = true;
        sc.at = now;
    };
    if (sc.pressed) { // short taps: 0.12 s
        if (Seconds(sc.at, now) < 0.12) {
            return;
        }
        runtime_pad_director_press(0);
        sc.pressed = false;
    }
    const double since = Seconds(sc.at, now);
    auto done = [&](const char* how) {
        st.test.script_on = false;
        if (sc.kind == TravelStep::HostWalk && st.test.joined) {
            // No load, so no re-join follows: the next step counts from here.
            st.test.armed = true;
            st.test.joined_at = now;
            st.test.step_at = now;
        }
        Log("test: %s %s (%s)", TravelStepName(sc.kind), how, DescribeHere(s).c_str());
    };
    if (sc.kind == TravelStep::HostWalk) {
        if (!s.world_up || s.loading) {
            done("FAILED: a load started");
            return;
        }
        if (sc.stage > 0 && since < 1.0) {
            return;
        }
        const char* at = sc.args.c_str();
        for (int k = 0; k < sc.stage && at; ++k) {
            at = std::strchr(at, '/');
            at = at ? at + 1 : nullptr;
        }
        if (sc.stage > 0) {
            Log("test: host_walk hop %d settled: %s", sc.stage, DescribeHere(s).c_str());
        }
        float p[3] = {};
        float yaw = 0;
        unsigned near_id = 0;
        if (at && std::sscanf(at, "chr%u", &near_id) == 1) {
            // "chrENTITY": 2 m beside that map character (e.g. Queen Yharnam for the Blood Moon).
            const std::uint64_t chr = GuestSvcFindChr(near_id);
            float d[4] = {};
            if (!chr || !SafeRead(chr + 0xc0, d, sizeof d)) {
                done("FAILED: no character with that entity id");
                return;
            }
            p[0] = d[0] + 2.0f;
            p[1] = d[1] + 0.3f;
            p[2] = d[2];
            Log("test: host_walk: chr %u stands at (%.2f, %.2f, %.2f)", near_id, d[0], d[1], d[2]);
        } else if (!at || std::sscanf(at, "%f:%f:%f:%f", &p[0], &p[1], &p[2], &yaw) != 4) {
            done("done");
            return;
        }
        const bool ok = PutChr(s, p, yaw);
        Log("test: host_walk hop %d to (%.2f, %.2f, %.2f)%s", sc.stage + 1, p[0], p[1], p[2], ok ? "" : " FAILED");
        ++sc.stage;
        sc.at = now;
        return;
    }
    // HostHeadstone: 0 stand in front, 1 Cross (examine), 2 wait for the menu's texts, 3 Downs + Cross,
    // 4 wait for the stage request (Cross again on a confirmation).
    unsigned talk = 0;
    int downs = 0;
    std::sscanf(sc.args.c_str(), "%u:%d", &talk, &downs);
    std::uint64_t seq = 0;
    std::int64_t last_ms = 0;
    BbGameMenu::TextActivity(&seq, &last_ms);
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    switch (sc.stage) {
    case 0: {
        const std::uint64_t chr = GuestSvcFindChr(talk);
        float d[4] = {}, r[4] = {};
        if (!chr || !SafeRead(chr + 0xc0, d, sizeof d) || !SafeRead(chr + 0xd0, r, sizeof r)) {
            if (since > 6.0) {
                done("FAILED: no character with that talk id");
            }
            return;
        }
        const float yaw = r[1];
        const float p[3] = {d[0] - 1.2f * std::sin(yaw), d[1], d[2] - 1.2f * std::cos(yaw)};
        PutChr(s, p, yaw + 3.14159265f);
        Log("test: host_headstone: t%u at (%.2f, %.2f, %.2f) yaw %.3f; host 1.2 m in front", talk, d[0], d[1], d[2],
            yaw);
        sc.stage = 1;
        sc.at = now;
        return;
    }
    case 1:
        if (since >= 1.5) {
            sc.text_seq = seq;
            tap(kPadCross);
            Log("test: host_headstone: Cross (examine)");
            sc.stage = 2;
        }
        return;
    case 2:
        // The menu builds its texts: fresh lookups, then quiet for 0.6 s.
        if (seq != sc.text_seq && since >= 1.0 && last_ms > 0 && now_ms - last_ms > 600) {
            Log("test: host_headstone: menu texts up (%llu new lookups)",
                static_cast<unsigned long long>(seq - sc.text_seq));
            sc.stage = 3;
            sc.presses = 0;
            sc.at = now;
        } else if (since > 6.0) {
            if (++sc.presses > 3) {
                done("FAILED: no menu after 3 presses");
                return;
            }
            sc.text_seq = seq;
            tap(kPadCross);
            Log("test: host_headstone: no menu yet; Cross again");
        }
        return;
    case 3:
        if (since >= 0.6) {
            if (sc.presses < downs) {
                ++sc.presses;
                tap(kPadDownTest);
                Log("test: host_headstone: Down %d/%d", sc.presses, downs);
            } else {
                tap(kPadCross);
                Log("test: host_headstone: Cross (travel)");
                sc.stage = 4;
                sc.presses = 0;
            }
        }
        return;
    case 4: {
        std::uint64_t wts = 0;
        std::uint8_t req = 0;
        SafeGet(Guest(0x5556678), &wts);
        if (wts) {
            SafeGet(wts + 8, &req);
        }
        if (req || s.loading || !s.world_up) {
            done("travel started");
        } else if (since >= 1.5) {
            if (++sc.presses > 3) {
                done("FAILED: no travel after the menu");
                return;
            }
            tap(kPadCross); // a confirmation dialog
            Log("test: host_headstone: no travel yet; Cross (confirm %d)", sc.presses);
        }
        return;
    }
    default:
        done("?");
    }
}

/// host_kill / guest_kill: the target comes from the H4 census's loaded-enemy list (team 23, entity
/// id chr +0x3B8, HP +0xF8; PickEnemyKillTarget), so it is loaded and alive here. The kill: Lua
/// ForceDead by entity id (`by_id`: a boss's own defeat path), then, if HP is still above 0, the
/// death path 0x18BF9A0 on the chr itself, then HP 0 (the game's own death follows).
/// The HP a ':hp' step writes: 0 (a lethal hit), or with ':hpNN' NN % of the chr's max HP (D11
/// proof: an NPC hunter's HP below its event's ratio, e.g. Alfred's 90 % in 12500053, as a fight
/// leaves it; the chr lives on).
int TestHpValue(std::uint64_t data, int pct) {
    int max = 0;
    if (pct <= 0 || !data || !SafeGet(data + 0xfc, &max) || max <= 0) return 0;
    return std::max(1, int(std::int64_t(max) * std::min(pct, 99) / 100));
}

void KillLoadedEnemyForTest(const char* what, std::uint32_t want, bool by_id, bool hp_only = false, int hp_pct = 0) {
    const auto list = balance::LoadedEnemies();
    float me[3] = {};
    const std::uint64_t player = LocalPlayer();
    const bool have_me = player && balance::ChrPosition(player, me);
    int alive = 0;
    for (const auto& e : list) alive += e.alive ? 1 : 0;
    const int k = have_me || want ? PickEnemyKillTarget(list, want, me, 500.0f) : -1;
    if (k < 0 && want && hp_only) {
        // ':hp' on a boss outside the census: its chr by entity id (0x13C97A0, as the boss clear
        // bonus 0x1335400 finds it) and HP 0 only: the game's own death and defeat path (a "real
        // kill" without ForceDead; statsaudit finding 4). The same for an NPC hunter (D11: not team
        // 23, so not in the census: Henryk, Eileen, the DLC hunters).
        const std::uint64_t chr =
            reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::uint64_t, std::uint64_t)>(Guest(0x13c97a0))(
                std::int32_t(want), 0, 0);
        std::uint64_t ins = 0, d = 0;
        const int hp0 = chr ? balance::EnemyHp(chr) : -1;
        if (chr && SafeGet(chr + 0x3b0, &ins) && ins && SafeGet(ins + 0x20, &d) && d) {
            const int v = TestHpValue(d, hp_pct);
            SafeWrite(d + 0xf8, &v, 4);
        }
        Log("test: %s: %s entity %u (chr %#llx, not in the census list): HP %d -> %d (%s)", what,
            EnemyIsBossEntity(want) ? "boss" : "NPC", want, static_cast<unsigned long long>(chr), hp0,
            chr ? balance::EnemyHp(chr) : -1,
            hp_pct > 0 ? "HP set only: the chr lives on" : "HP 0 only: the game's own death follows");
        return;
    }
    if (k < 0 && want && by_id && EnemyIsBossEntity(want)) {
        // A boss chr is not in the census list (pard S5a: Blood-starved Beast 2300800 in its fight):
        // Lua ForceDead by id still runs its defeat path, as the campaign soak's kills did.
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kForceDead))(0, want);
        Log("test: %s: boss entity %u not in the census list (%zu loaded enemies): ForceDead by id (its defeat path)",
            what, want, list.size());
        return;
    }
    if (k < 0) {
        Log("test: %s: no target (%s %u): %zu loaded enemies, %d alive; host at (%.1f, %.1f, %.1f)", what,
            want ? "entity" : "nearest of", want, list.size(), alive, me[0], me[1], me[2]);
        if (want) {
            // G1 (parfin3): a generated Chalice layer has other entity ids than its MSB template;
            // the strongest loaded enemies name the layer bosses and where they stand.
            std::vector<std::pair<int, size_t>> by_hp;
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i].alive) by_hp.emplace_back(balance::EnemyHp(list[i].chr), i);
            }
            std::sort(by_hp.begin(), by_hp.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t n = 0; n < by_hp.size() && n < 12; ++n) {
                const auto& e = list[by_hp[n].second];
                Log("test: %s:   loaded #%zu: entity %u, HP %d, at (%.1f, %.1f, %.1f)", what, n + 1, e.entity,
                    by_hp[n].first, e.pos[0], e.pos[1], e.pos[2]);
            }
        }
        return;
    }
    const auto& t = list[size_t(k)];
    const float dx = t.pos[0] - me[0], dy = t.pos[1] - me[1], dz = t.pos[2] - me[2];
    const int hp0 = balance::EnemyHp(t.chr);
    Log("test: %s: entity %u (census key %u; loaded, HP %d) at (%.1f, %.1f, %.1f), %.1f m away; %zu loaded enemies, "
        "%d alive", what, t.entity, t.id, hp0, t.pos[0], t.pos[1], t.pos[2], std::sqrt(dx * dx + dy * dy + dz * dz), list.size(), alive);
    const int echoes0 = ReadGameState(false).echoes;
    Log("test: %s: before: own Echoes %d; %s", what, echoes0, balance::DescribeKillEchoGates(t.chr).c_str());
    const char* how = "none";
    if (hp_only) {
        // ':hp': only HP 0, the state a lethal hit leaves: the chr's own update runs its death.
        std::uint64_t ins = 0, d = 0;
        if (SafeGet(t.chr + 0x3b0, &ins) && ins && SafeGet(ins + 0x20, &d) && d) {
            const int v = TestHpValue(d, hp_pct);
            SafeWrite(d + 0xf8, &v, 4);
        }
        Log("test: %s: entity %u HP %d -> %d (%s)", what, t.entity, hp0, balance::EnemyHp(t.chr),
            hp_pct > 0 ? "HP set only: the chr lives on" : "HP 0 only: the game's own death follows");
        return;
    }
    if (by_id && t.entity) {
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kForceDead))(0, t.entity);
        how = "ForceDead by id";
    }
    if (balance::EnemyHp(t.chr) > 0) {
        // What Lua ForceDead does with the chr it finds: clear +0x1E3 bit 5, then 0x18BF9A0(chr).
        std::uint8_t b = 0;
        if (SafeGet(t.chr + 0x1e3, &b)) {
            b = std::uint8_t(b & 0xdf);
            SafeWrite(t.chr + 0x1e3, &b, 1);
        }
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t)>(Guest(0x18bf9a0))(t.chr);
        how = "the death path on the chr";
    }
    if (balance::EnemyHp(t.chr) > 0) {
        const int zero = 0;
        std::uint64_t ins = 0, d = 0;
        if (SafeGet(t.chr + 0x3b0, &ins) && ins && SafeGet(ins + 0x20, &d) && d) SafeWrite(d + 0xf8, &zero, 4);
        how = "HP 0";
    }
    Log("test: %s: entity %u killed by %s: HP %d -> %d; own Echoes %d -> %d; %s", what, t.entity, how, hp0,
        balance::EnemyHp(t.chr), echoes0, ReadGameState(false).echoes, balance::DescribeKillEchoGates(t.chr).c_str());
}

/// host_use / guest_use=GOODS (G2 test aid): what using goods GOODS does to the own chr, without the
/// animation: one is given if none is owned, its EquipParamGoods behaviorId (+0x14) names a
/// BehaviorParam_PC row whose refType (+0x9) 2 = SpEffect refId (+0xC), else the goods refId (+0x0)
/// when SpEffectParam has that row; that SpEffect is applied (Madman's Knowledge 1500: +1 Insight)
/// and a game save is requested. Not consumed. Insight / save changes show in the state lines.
void UseGoodsForTest(const char* what, std::uint32_t goods, bool take) {
    const int owned = GoodsCount(goods);
    const bool given = owned <= 0 && GiveGoods(goods, 1);
    const std::uint64_t row = GameParamRow("EquipParamGoods", "EQUIP_PARAM_GOODS_ST", goods);
    std::int32_t ref = -1, behavior = -1, beh_ref = -1;
    std::uint8_t ref_type = 0xff;
    if (row) {
        SafeGet(row + 0x0, &ref);
        SafeGet(row + 0x14, &behavior);
    }
    const std::uint64_t beh = behavior > 0 ? GameParamRow("BehaviorParam_PC", "BEHAVIOR_PARAM_ST", std::uint32_t(behavior)) : 0;
    if (beh) {
        SafeGet(beh + 0x9, &ref_type);
        SafeGet(beh + 0xc, &beh_ref);
    }
    int effect = -1;
    if (beh && ref_type == 2 && beh_ref > 0 && GameParamRow("SpEffectParam", "SP_EFFECT_PARAM_ST", std::uint32_t(beh_ref))) {
        effect = beh_ref;
    } else if (ref > 0 && GameParamRow("SpEffectParam", "SP_EFFECT_PARAM_ST", std::uint32_t(ref))) {
        effect = ref;
    }
    const int insight0 = ReadGameState(false).insight;
    const bool ok = effect > 0 && ApplySpEffect(effect);
    Log("test: %s goods %u (owned %d%s): goods row %s refId %d behaviorId %d -> BehaviorParam_PC %s refType %u refId %d; "
        "SpEffect %d %s; Insight %d -> %d", what, goods, owned, given ? ", one given" : "", row ? "found" : "MISSING",
        ref, behavior, beh ? "found" : "missing", unsigned(ref_type), beh_ref, effect, ok ? "applied" : "NOT applied",
        insight0, ReadGameState(false).insight);
    if (take) {
        // host_use=GOODS:take: the item is used up as the game's own use of an isConsume item
        // takes it (the Gold Pendant / Tear Stone become a blood gem): one fewer held.
        const int before = GoodsCount(goods);
        const bool gone = before > 0 && GiveGoods(goods, -1);
        Log("test: %s goods %u used up: held %d -> %d%s", what, goods, before, GoodsCount(goods), gone ? "" : " (NOT taken)");
    }
    if (ok || take) RequestGameSave("item use test");
}

/// The die test step (host_die / guest_die, BB_PARTY_TEST_DIE_ON_DOWN): this player dies now.
/// BB_PARTY_TEST_INVINCIBLE=1 (tests): host_move / guest_move make the local chr invincible
/// (EnableInvincible, chr +0x1E1 bit 14, as the A11 fixture does) so a boss fight lasts while the
/// test steps run; a die step clears it first.
void SetTestInvincible(bool on) {
    const std::uint64_t player = LocalPlayer();
    std::uint32_t f = 0;
    if (player && SafeRead(player + 0x1e1, &f, 4)) {
        f = on ? (f | (1u << 14)) : (f & ~(1u << 14));
        SafeWrite(player + 0x1e1, &f, 4);
    }
}
bool TestInvincibleRequested() {
    static const bool on = std::getenv("BB_PARTY_TEST_INVINCIBLE") && std::getenv("BB_PARTY_TEST_INVINCIBLE")[0] == '1';
    return on;
}

void DieForTest() {
    if (TestInvincibleRequested()) SetTestInvincible(false);
    // ForceDead's own gates (0x18BF9A0): GSM +0x176F set = HP left alone; chr +0x18 -> +0x30 ->
    // +0x18 bit 0 = no death call. Logged: a die step that "did nothing" names its gate.
    std::uint64_t gsm = 0, player = LocalPlayer(), a18 = 0, a30 = 0;
    std::uint8_t no_hp = 0xff, no_die = 0xff;
    if (SafeGet(Guest(0x5556678), &gsm) && gsm) SafeGet(gsm + 0x176f, &no_hp);
    if (player && SafeGet(player + 0x18, &a18) && a18 && SafeGet(a18 + 0x30, &a30) && a30) SafeGet(a30 + 0x18, &no_die);
    std::uint8_t f1e0[8] = {};
    if (player) SafeRead(player + 0x1e0, f1e0, sizeof f1e0);
    Log("test: ForceDead gates: GSM +0x176F %u, chr +0x18/+0x30/+0x18 %u (bit 0 blocks the death); chr +0x1e0: "
        "%02x %02x %02x %02x %02x %02x %02x %02x (+0x1e1 bit 6 = EnableInvincible)", no_hp, no_die, f1e0[0], f1e0[1],
        f1e0[2], f1e0[3], f1e0[4], f1e0[5], f1e0[6], f1e0[7]);
    auto hp_of = [] {
        std::uint64_t pl = LocalPlayer(), ins = 0, dm = 0;
        int hp = -1;
        if (pl && SafeGet(pl + 0x3b0, &ins) && ins && SafeGet(ins + 0x20, &dm) && dm) SafeGet(dm + 0xf8, &hp);
        return hp;
    };
    const int hp0 = hp_of();
    // Lua ForceDead (0x13320C0) finds the chr by entity id (0x13C97A0) and does nothing when that
    // finds none: entity 10000 is not always the local player (after a boss clear on a guest it
    // was not). Then the death path 0x18BF9A0 runs on our own chr directly.
    std::uint64_t by_id = 0;
    if (std::uint64_t lua_on = 0; SafeGet(Guest(0x553b0c8), &lua_on) && lua_on)
        by_id = reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::uint32_t, std::uint32_t, std::uint32_t)>(
            Guest(0x13c97a0))(10000, 10000, 0);
    if (by_id && by_id == player) {
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kForceDead))(0, 10000);
    } else if (player) {
        std::uint8_t b3 = 0;
        if (SafeGet(player + 0x1e3, &b3)) {
            b3 &= 0xdf;
            SafeWrite(player + 0x1e3, &b3, 1);
        }
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t)>(Guest(0x18bf9a0))(player);
    }
    Log("test: die: entity 10000 -> %#llx, local player %#llx (%s); HP %d -> %d right after",
        static_cast<unsigned long long>(by_id), static_cast<unsigned long long>(player),
        by_id == player ? "ForceDead" : "the death path on our chr", hp0, hp_of());
    std::thread([hp_of] {
        for (int i = 1; i <= 4; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            Log("test: die: HP %d (+%d s)", hp_of(), 2 * i);
        }
    }).detach();
}

/// The live travel test chain (TravelStep): joins are tracked in any state; a step runs in the
/// world, with no loading screen and no stage request pending.
/// Test aid (D7 stagger, BB_PARTY_TARGET_ANIM=1): every new animation of the host_chr / guest_chr
/// target, from its TimeAct ring ([[chr+0x3B0]+0x18]: 10 x {anim id, ...} at +0x20, the next
/// write index at +0xC0; the same ring as the player's), with its HP and the time since the last
/// press tap, so hit reactions (staggers) can be counted per hit. `=keep`: its HP is put back to
/// max whenever it falls below half (many hits on one enemy: an A/B with host_poise in one run).
void TargetAnimTick(Test& t, Clock::time_point now) {
    static const char* mode = std::getenv("BB_PARTY_TARGET_ANIM");
    if (!mode || mode[0] == '0' || !mode[0] || !t.chr_target) return;
    static std::uint32_t entity = 0;
    static std::uint64_t chr = 0;
    static Clock::time_point refresh{};
    static std::int32_t last_idx = -1;
    if (entity != t.chr_target || now >= refresh) {
        if (entity != t.chr_target) last_idx = -1;
        entity = t.chr_target;
        chr = 0;
        for (const auto& e : balance::LoadedEnemies()) {
            if (e.entity == entity) chr = e.chr;
        }
        refresh = now + std::chrono::seconds(1);
    }
    std::uint64_t cm = 0, ta = 0, dm = 0;
    std::int32_t idx = -1;
    if (!chr || !SafeGet(chr + 0x3b0, &cm) || !cm || !SafeGet(cm + 0x18, &ta) || !ta || !SafeGet(ta + 0xc0, &idx) ||
        idx < 0 || idx >= 10) {
        return;
    }
    {
        // once a second: the target's distance to this player (a boss that walks to the guest or stands at a corpse)
        static Clock::time_point next_pos{};
        float me[3], at[3];
        const std::uint64_t player = LocalPlayer();
        if (now >= next_pos && player && balance::ChrPosition(player, me) && balance::ChrPosition(chr, at)) {
            next_pos = now + std::chrono::seconds(1);
            Log("test: target pos: entity %u at (%.1f, %.1f, %.1f), %.1f m from this player", entity, double(at[0]),
                double(at[1]), double(at[2]),
                double(std::sqrt((at[0] - me[0]) * (at[0] - me[0]) + (at[2] - me[2]) * (at[2] - me[2]))));
        }
    }
    SafeGet(cm + 0x20, &dm);
    std::int32_t hp = -1, mhp = -1;
    if (dm) {
        SafeGet(dm + 0xf8, &hp);
        SafeGet(dm + 0xfc, &mhp);
    }
    if (idx != last_idx) {
        // Every entry written since the last look (a hit can start two animations in one frame).
        const int from = last_idx < 0 ? (idx + 9) % 10 : last_idx;
        for (int k = from; k != idx; k = (k + 1) % 10) {
            std::int32_t anim = 0;
            SafeGet(ta + 0x20 + std::uint64_t(k) * 0x10, &anim);
            const double since = t.press_at == Clock::time_point{} ? -1.0 : Seconds(t.press_at, now);
            Log("test: target anim: entity %u animation %d (HP %d/%d, %.2f s = %d frame(s) at 30 fps after the last press)",
                entity, anim, hp, mhp, since, since < 0 ? -1 : int(since * 30.0 + 0.5));
        }
        last_idx = idx;
    }
    if (mode[0] == 'k' && hp > 0 && mhp > 0 && hp < mhp / 2) {
        SafeWrite(dm + 0xf8, &mhp, 4);
        Log("test: target anim: entity %u HP %d -> %d (keep)", entity, hp, mhp);
    }
}

void RunTravelSteps(State& st, const GameSnapshot& s, PartyRole role, Clock::time_point now) {
    Test& t = st.test;
    TargetAnimTick(t, now);
    // BB_PARTY_TEST_GUARD=1 (test aid, smoke rt): invincible from each revive until the next die /
    // chr step (deathrules::TestGuard); set every tick (the revive and the stain warp reset it).
    static const bool guard_env = std::getenv("BB_PARTY_TEST_GUARD") && std::getenv("BB_PARTY_TEST_GUARD")[0] == '1';
    static deathrules::TestGuard guard;
    if (guard_env) {
        const bool was = guard.on;
        if (deathrules::TestGuardTick(guard, b2::LocalHp(), s.world_up && !s.loading)) {
            SetTestInvincible(true);
            if (!was) Log("test: guard (BB_PARTY_TEST_GUARD): up again after a death: invincible until the next die / chr step");
        }
    }
    // ":ground": the game's last safe ground spot (PlayerIns +0x400 -> +0x30) at the pinned spot means
    // the floor's collision is in; the chr's own position 5 m below it after the release is a fall.
    auto pin_spot_ground = [&](bool fell_check) {
        const std::uint64_t pl = LocalPlayer();
        float v[3] = {};
        std::uint64_t q = 0;
        if (fell_check) {
            float pp[3] = {};
            std::uint64_t a = 0;
            return pl && SafeGet(pl + 0x58, &a) && a && SafeGet(a + 8, &a) && a && SafeGet(a + 0x3b0, &a) && a &&
                   SafeGet(a + 0x68, &a) && a && SafeRead(a + 0x1e0, pp, 12) && pp[1] < t.pin_pos[1] - 5.0f;
        }
        if (!pl || !SafeGet(pl + 0x400, &q) || !q || !SafeRead(q + 0x30, v, 12)) return false;
        const float dx = v[0] - t.pin_pos[0], dy = v[1] - t.pin_pos[1], dz = v[2] - t.pin_pos[2];
        return dx * dx + dz * dz < 4.0f && std::fabs(dy) < 2.0f;
    };
    if (t.fall_check != Clock::time_point{} && now >= t.fall_check) {
        t.fall_check = {};
        if (s.world_up && !s.loading && pin_spot_ground(true) && t.pin_retries < 1) {
            ++t.pin_retries;
            t.pin_until = now + std::chrono::milliseconds(static_cast<int>(t.pin_secs * 1000));
            t.pin_set = now;
            Log("test: pin: fell below (%.2f, %.2f, %.2f) after the release: warped back and held again",
                double(t.pin_pos[0]), double(t.pin_pos[1]), double(t.pin_pos[2]));
        }
    }
    if (t.pin_until != Clock::time_point{} && t.pin_ground && now - t.pin_set >= std::chrono::seconds(1) &&
        now < t.pin_until && s.world_up && !s.loading && pin_spot_ground(false)) {
        Log("test: pin: ground under (%.2f, %.2f, %.2f) after %.1f s (the game's safe spot is there)",
            double(t.pin_pos[0]), double(t.pin_pos[1]), double(t.pin_pos[2]),
            std::chrono::duration<double>(now - t.pin_set).count());
        t.pin_until = now;  // released below; then the fall check
    } else if (t.pin_until != Clock::time_point{} && t.pin_ground && now >= t.pin_until && t.pin_retries < 1 &&
               s.world_up && !s.loading) {
        ++t.pin_retries;
        t.pin_until = now + std::chrono::milliseconds(static_cast<int>(t.pin_secs * 1000));
        Log("test: pin: no ground yet under (%.2f, %.2f, %.2f): held once more", double(t.pin_pos[0]),
            double(t.pin_pos[1]), double(t.pin_pos[2]));
    }
    if (t.pin_until != Clock::time_point{}) {
        // ":pin=S": the chr kept at the move's spot every frame until the floor's collision is in.
        const std::uint64_t player = LocalPlayer();
        std::uint64_t vt = 0, fn = 0;
        if (now >= t.pin_until || !s.world_up || s.loading) {
            if (t.pin_ground) t.fall_check = now + std::chrono::seconds(3);
            t.pin_until = {};
            Log("test: pin at (%.2f, %.2f, %.2f) released", double(t.pin_pos[0]), double(t.pin_pos[1]), double(t.pin_pos[2]));
        } else if (player && SafeGet(player, &vt) && vt && SafeGet(vt + 0x5f8, &fn) && fn) {
            reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint32_t)>(fn)(
                player, t.pin_pos, t.pin_rot, 1);
        }
    }
    // BB_PARTY_DIRECTOR_TEST_SOLO=1: the host steps without a cooperator, "joined" from the first
    // steady world on: in single player (no BB_PARTY) or as a party host alone (vanilla comparisons,
    // e.g. streamsync: does a neighbour map's kill stay dead when that map streams out and in).
    static const bool solo = [] {
        const char* v = std::getenv("BB_PARTY_DIRECTOR_TEST_SOLO");
        return v && v[0] == '1';
    }();
    const bool solo_now = solo && (role == PartyRole::None || role == PartyRole::Host);
    if (solo_now) {
        role = PartyRole::Host;
    }
    if (t.steps.empty() || role == PartyRole::None) {
        return;
    }
    // Guest: in once a client with the world loaded; out only when the session ends (the summon's
    // own map reload into the host world is still in).
    const bool joined = solo_now                  ? t.joined || (s.world_up && !s.loading)
                        : role == PartyRole::Host ? s.cooperators >= 1
                        : t.joined                ? s.session_role == RoleClient
                                                  : s.session_role == RoleClient && s.world_up && !s.loading;
    if (joined != t.joined) {
        t.joined = joined;
        if (joined) {
            ++t.joins;
            t.joined_at = now;
            t.armed = true;
            if (t.ran) {
                Log("test: travel steps: (re)joined #%d, %.1f s after %s", t.joins, Seconds(t.step_at, now),
                    TravelStepName(t.last_kind));
            } else {
                Log("test: travel steps: joined #%d", t.joins);
            }
        } else {
            t.left_at = now;
            Log("test: travel steps: out of the party (%s)", Describe(s).c_str());
        }
    }
    // "^N*K": count every rise of the host's cooperators to >= N (a guest's Leave and Rejoin: 2 -> 1 -> 2).
    if (s.cooperators >= 0) {
        for (int n = 1; n < 8; ++n)
            if (s.cooperators >= n && t.last_coop < n) ++t.coop_rises[n];
        t.last_coop = s.cooperators;
    }
    // The other role's steps are that instance's: skip them.
    auto mine = [&](const TravelStep& step) {
        return (step.kind == TravelStep::GuestDie || step.kind == TravelStep::GuestMove ||
                step.kind == TravelStep::GuestPress || step.kind == TravelStep::GuestMark ||
                step.kind == TravelStep::GuestLamp || step.kind == TravelStep::GuestSkipRemo || step.kind == TravelStep::GuestOutage || step.kind == TravelStep::GuestKill ||
                step.kind == TravelStep::GuestObj || step.kind == TravelStep::GuestChr ||
                step.kind == TravelStep::GuestStick || step.kind == TravelStep::GuestUse ||
                step.kind == TravelStep::GuestRoute || step.kind == TravelStep::GuestFixture ||
                step.kind == TravelStep::GuestHome || step.kind == TravelStep::GuestFlag ||
                step.kind == TravelStep::GuestLastLamp || step.kind == TravelStep::GuestLeave ||
                step.kind == TravelStep::GuestRejoin) == (role == PartyRole::Guest);
    };
    if (t.script_on) {
        RunTravelScript(st, s, now);
        return;
    }
    while (t.next_step < t.steps.size() && !mine(t.steps[t.next_step])) {
        ++t.next_step;
    }
    // Steady world with the party (HostKill's clock): restarts at every load / leave.
    // Split travel: members apart (no cooperator here) still count as the party being together.
    const bool steady = (joined || b2::MembersApart() > 0) && s.world_up && !s.loading;
    if (const bool world = s.world_up && !s.loading; world != t.world) {
        t.world = world;
        t.world_at = now;
    }
    if (steady != t.steady) {
        t.steady = steady;
        t.steady_at = now;
    }
    if (const bool scene = CutscenePlaying(); scene != t.scene_on) {
        t.scene_on = scene;
        if (!scene) {
            t.scene_end_at = now;
        }
    }
    // A guest_stick walk holds the chain: the next step's delay counts from the end of the walk.
    if (t.ran &&
        (t.last_kind == TravelStep::GuestStick || t.last_kind == TravelStep::HostStick ||
         t.last_kind == TravelStep::HostRoute || t.last_kind == TravelStep::GuestRoute) &&
        t.approach && !t.approach_done) {
        t.step_at = now;
        return;
    }
    const TravelStep::Kind next_kind = t.next_step < t.steps.size() ? t.steps[t.next_step].kind : TravelStep::HostWarp;
    if (next_kind == TravelStep::HostRejoin || next_kind == TravelStep::GuestRejoin) {
        // Rejoin (the Party page's button): waits until this player is out of the party (its own
        // Leave, the host's Leave, a kick), then presses S s after it went out (or after the
        // previous step, the later). Back in by itself before that: the step is done without a press.
        const TravelStep& step = t.steps[t.next_step];
        if (step.args == "now") {
            // guest_rejoin:now@S[%MAP]: the press S s after the previous step, in the world, also while
            // still in the session (a guest apart from a host in a Chalice Dungeon, chalamp).
            const bool map_ok = step.map.empty() || MapName(s.map_id).find(step.map) != std::string::npos;
            if (!t.ran || !s.world_up || s.loading || !map_ok || Seconds(t.step_at, now) < step.delay) {
                return;
            }
            ++t.next_step;
            t.step_at = now;
            t.last_kind = step.kind;
            ::party::status::RequestRejoin();
            Log("test: travel step %zu/%zu: %s: Party > Rejoin pressed now (%s)", t.next_step, t.steps.size(),
                TravelStepName(step.kind), Describe(s).c_str());
            return;
        }
        const bool went_out = t.left_at != Clock::time_point{} && (!t.ran || t.left_at > t.step_at);
        if (joined) {
            if (!went_out) {
                return;
            }
            ++t.next_step;
            t.step_at = now;
            t.ran = true;
            t.last_kind = step.kind;
            Log("test: travel step %zu/%zu: %s: back in the party without a press", t.next_step, t.steps.size(),
                TravelStepName(step.kind));
            return;
        }
        if (!went_out) {
            return;  // not in the party yet (never joined): nothing to rejoin
        }
        const Clock::time_point from = t.ran && t.step_at > t.left_at ? t.step_at : t.left_at;
        if (Seconds(from, now) < step.delay) {
            return;
        }
        ++t.next_step;
        t.step_at = now;
        t.ran = true;
        t.last_kind = step.kind;
        ::party::status::RequestRejoin();
        Log("test: travel step %zu/%zu: %s: Party > Rejoin pressed (%s)", t.next_step, t.steps.size(),
            TravelStepName(step.kind), Describe(s).c_str());
        return;
    }
    if (t.pressing && now >= t.release_at) {
        t.pressing = false;
        runtime_pad_director_press(0);
    }
    // "^N": a condition, not a fixed wait: the step waits until the host's game has N cooperators,
    // and its S counts from that moment (e.g. host_warp=ID@3^2: 3 s after the second guest is in).
    if (t.next_step < t.steps.size() && t.steps[t.next_step].min_coop) {
        const TravelStep& cs = t.steps[t.next_step];
        if (s.cooperators < cs.min_coop || (cs.coop_rise && t.coop_rises[cs.min_coop < 8 ? cs.min_coop : 7] < cs.coop_rise)) {
            t.coop_since = {};
            return;
        }
        if (t.coop_since == Clock::time_point{}) t.coop_since = now;
    } else {
        t.coop_since = {};
    }
    // liftfix: the conditions' timeout (see cond_step).
    if (t.next_step < t.steps.size() &&
        (t.steps[t.next_step].wait_flag || t.steps[t.next_step].wait_near || t.steps[t.next_step].wait_obj) &&
        !t.cond_skip) {
        // BB_PARTY_TEST_COND_TIMEOUT (s, default 240: above every smoke phase's longest wait, e.g. a
        // boss scene and fight; liftfix's rh / rs set 45).
        static const double kCondTimeout = [] {
            const char* e = std::getenv("BB_PARTY_TEST_COND_TIMEOUT");
            const double v = e ? std::atof(e) : 0.0;
            return v > 0.0 ? v : 240.0;
        }();
        if (t.cond_step != t.next_step) {
            t.cond_step = t.next_step;
            t.cond_since = now;
        } else if (Seconds(t.cond_since, now) > kCondTimeout) {
            Log("test: travel step %zu (%s): condition timed out after %.0f s; to the last step %zu (conditions "
                "skipped)", t.next_step + 1, TravelStepName(t.steps[t.next_step].kind), kCondTimeout, t.steps.size());
            t.next_step = t.steps.size() - 1;
            t.cond_skip = true;
            t.step_at = now;
        }
    }
    const bool cond_skip = t.cond_skip;
    // "?FLAG=V": the same, on an event flag of this game (a lift's position flag at the ride's end).
    if (!cond_skip && t.next_step < t.steps.size() && t.steps[t.next_step].wait_flag) {
        const TravelStep& fs = t.steps[t.next_step];
        bool v = false;
        if (!progress::ReadFlag(fs.wait_flag, &v) || v != fs.wait_value) {
            t.flag_since = {};
            return;
        }
        if (t.flag_since == Clock::time_point{}) {
            t.flag_since = now;
            Log("test: travel step %zu (%s): flag %u = %d now; its delay counts from here", t.next_step + 1,
                TravelStepName(fs.kind), fs.wait_flag, int(v));
        }
    } else {
        t.flag_since = {};
    }
    // "?near=X:Y:Z:R": the same, on another party member's position (the party link's vitals).
    if (!cond_skip && t.next_step < t.steps.size() && t.steps[t.next_step].wait_near &&
        !(t.near_since != Clock::time_point{} && t.near_step == t.next_step)) {
        const TravelStep& ns = t.steps[t.next_step];
        float d = 0;
        if (!MemberNear(ns.near_pos, ns.near_r, &d)) {
            t.near_since = {};
            return;
        }
        if (t.near_since == Clock::time_point{} || t.near_step != t.next_step) {
            t.near_since = now;
            t.near_step = t.next_step;
            Log("test: travel step %zu (%s): a party member %.1f m from (%.2f, %.2f, %.2f) now; its delay counts from "
                "here", t.next_step + 1, TravelStepName(ns.kind), double(d), double(ns.near_pos[0]), double(ns.near_pos[1]),
                double(ns.near_pos[2]));
        }
    } else if (!(t.next_step < t.steps.size() && t.steps[t.next_step].wait_near)) {
        t.near_since = {};
    }
    // "?obj=ENTITY:ANIM": the same, on a map object's animation (stopped).
    if (!cond_skip && t.next_step < t.steps.size() && t.steps[t.next_step].wait_obj &&
        !(t.obj_since != Clock::time_point{} && t.obj_step == t.next_step)) {
        const TravelStep& os = t.steps[t.next_step];
        worldact::ObjAnimState a;
        // liftfix: also once ANIM was seen playing and the object went on to another animation (a lift
        // whose end animation starts in the frame its ride stops: run 10 waited for good on anim 6)
        const bool have = worldact::ReadObjAnim(os.wait_obj, &a);
        if (have && a.anim == os.wait_obj_anim && a.playing) {
            t.obj_seen_step = t.next_step;
        }
        const bool stopped = have && a.anim == os.wait_obj_anim && !a.playing;
        const bool moved_on = have && a.anim != os.wait_obj_anim && t.obj_seen_step == t.next_step;
        if (!stopped && !moved_on) {
            t.obj_since = {};
            return;
        }
        if (t.obj_since == Clock::time_point{} || t.obj_step != t.next_step) {
            t.obj_since = now;
            t.obj_step = t.next_step;
            Log("test: travel step %zu (%s): object %d stopped at anim %d now; its delay counts from here", t.next_step + 1,
                TravelStepName(os.kind), os.wait_obj, os.wait_obj_anim);
        }
    } else if (!(t.next_step < t.steps.size() && t.steps[t.next_step].wait_obj)) {
        t.obj_since = {};
    }
    if (t.next_step < t.steps.size() &&
        (next_kind == TravelStep::HostKill || next_kind == TravelStep::HostMove || next_kind == TravelStep::GuestMove ||
         next_kind == TravelStep::HostPress || next_kind == TravelStep::GuestPress || next_kind == TravelStep::HostWalk ||
         next_kind == TravelStep::HostOutage || next_kind == TravelStep::GuestOutage ||
         next_kind == TravelStep::HostLastLamp || next_kind == TravelStep::GuestLastLamp || next_kind == TravelStep::HostLamp || next_kind == TravelStep::HostObj ||
         next_kind == TravelStep::GuestObj || next_kind == TravelStep::HostChr || next_kind == TravelStep::GuestChr ||
         next_kind == TravelStep::HostUse || next_kind == TravelStep::GuestUse || next_kind == TravelStep::GuestFixture ||
         next_kind == TravelStep::GuestStick || next_kind == TravelStep::HostStick ||
         next_kind == TravelStep::HostRoute || next_kind == TravelStep::GuestRoute ||
         next_kind == TravelStep::HostMark || next_kind == TravelStep::GuestMark || !t.steps[t.next_step].map.empty())) {
        const bool out = !steady && t.steps[t.next_step].out_ok && t.ran && t.world;
        if ((!steady && !out) || (!t.ran && !t.armed) || t.joins < t.steps[t.next_step].min_join) {
            return; // as the first step of a chain it still waits for the join
        }
        // S s after the later of: the last load (steady again), the previous step (or the join).
        const Clock::time_point prev = t.ran ? t.step_at : t.joined_at;
        Clock::time_point from = t.steady_at > prev ? t.steady_at : prev;
        if (out && t.world_at > from) from = t.world_at;
        if (t.coop_since > from) from = t.coop_since;
        if (t.flag_since > from) from = t.flag_since;
        if (t.near_since > from) from = t.near_since;
        if (t.obj_since > from) from = t.obj_since;
        if (t.steps[t.next_step].after_scene) {
            // "~": waits for a cutscene that ended after the previous step, then S s after its end.
            if (t.scene_on || t.scene_end_at <= prev) {
                return;
            }
            from = t.scene_end_at > from ? t.scene_end_at : from;
        }
        if (Seconds(from, now) < t.steps[t.next_step].delay) {
            return;
        }
        if (!t.steps[t.next_step].map.empty() && MapName(s.map_id).find(t.steps[t.next_step].map) == std::string::npos) {
            return;
        }
    } else if (t.next_step >= t.steps.size() || !t.armed || !joined || !s.world_up || s.loading ||
               t.joins < t.steps[t.next_step].min_join ||
               Seconds(t.coop_since > t.joined_at ? t.coop_since : t.joined_at, now) < t.steps[t.next_step].delay) {
        return;
    }
    {
        // pt1polish (replay S1 / S4): a step that moves or acts with the own chr waits while it is
        // dead, and every step waits while a B2 re-join (a wipe's load and its re-sync) runs; the
        // step's delay then counts from the end of the wait.
        const TravelStep::Kind k = t.steps[t.next_step].kind;
        const bool own_chr = !(k == TravelStep::HostPress || k == TravelStep::GuestPress || k == TravelStep::HostOutage ||
                               k == TravelStep::HostPoise || k == TravelStep::HostSpfx || k == TravelStep::HostGfx ||
                               k == TravelStep::GuestOutage || k == TravelStep::HostFlag || k == TravelStep::GuestFlag ||
                               k == TravelStep::HostSkipRemo || k == TravelStep::GuestSkipRemo ||
                               k == TravelStep::HostLastLamp || k == TravelStep::GuestLastLamp || k == TravelStep::HostKill || k == TravelStep::GuestKill) ||
                             t.steps[t.next_step].alive;
        const bool die_step = k == TravelStep::HostDie || k == TravelStep::GuestDie;
        const bool prev_die = t.ran && (t.last_kind == TravelStep::HostDie || t.last_kind == TravelStep::GuestDie);
        if (deathrules::TestDieStepLatches(die_step, b2::LocalHp() > 0, s.world_up && !s.loading, b2::RejoinRunning(),
                                           prev_die)) {
            // Killed first (an enemy, the boss): the death this step asks for happened.
            t.step_waiting = false;
            t.next_step++;
            t.armed = false;
            t.step_at = now;
            t.ran = true;
            t.last_kind = k;
            Log("test: travel step %zu/%zu: %s: the own chr is at HP 0 already (killed first): taken as done",
                t.next_step, t.steps.size(), TravelStepName(k));
            return;
        }
        if (deathrules::TestStepWaits(own_chr, b2::LocalHp() > 0, s.world_up && !s.loading, b2::RejoinRunning())) {
            if (!t.step_waiting) {
                Log("test: travel step %zu (%s) waits: %s", t.next_step + 1, TravelStepName(k),
                    b2::RejoinRunning() ? "a re-join is running" : "the own chr is dead");
            }
            t.step_waiting = true;
            if (t.ran) {
                t.step_at = now;
            }
            return;
        }
        if (t.step_waiting) {
            t.step_waiting = false;
            Log("test: travel step %zu (%s) no longer waits; its delay counts from now", t.next_step + 1, TravelStepName(k));
            return;
        }
    }
    {
        // fallcam 3: host_die:ladder / guest_die:ladder: only while the own chr climbs a ladder (its
        // ladder action state [ChrCtrl +0x38] +0x60 is set, what IsLadderAct reads); waits up to 15 s
        // for it, then is skipped (a death off the ladder would test the fall path, not the ladder).
        const TravelStep& n = t.steps[t.next_step];
        static Clock::time_point ladder_wait_from{};
        static bool ladder_waiting = false;
        if ((n.kind == TravelStep::HostDie || n.kind == TravelStep::GuestDie) && n.args == "ladder") {
            std::uint64_t pl = LocalPlayer(), ctrl = 0, act = 0;
            std::int8_t st = -1;
            const bool read = pl && SafeGet(pl + 0x58, &ctrl) && ctrl && SafeGet(ctrl + 0x38, &act) && act &&
                              SafeGet(act + 0x60, &st);
            if (!read || st < 0) {
                if (!ladder_waiting) {
                    ladder_waiting = true;
                    ladder_wait_from = now;
                    Log("test: travel step %zu (%s:ladder) waits for the ladder (state %d)", t.next_step + 1,
                        TravelStepName(n.kind), int(st));
                }
                if (Seconds(ladder_wait_from, now) < 15.0) {
                    return;
                }
                Log("test: travel step %zu/%zu: %s:ladder: NOT on a ladder after 15 s (state %d): skipped, no death",
                    t.next_step + 1, t.steps.size(), TravelStepName(n.kind), int(st));
                ladder_waiting = false;
                t.next_step++;
                t.step_at = now;
                t.ran = true;
                t.last_kind = TravelStep::HostPress;  // not a die step: the chain goes on
                return;
            }
            Log("test: %s:ladder: on the ladder (ladder state %d) after %.1f s", TravelStepName(n.kind), int(st),
                ladder_waiting ? Seconds(ladder_wait_from, now) : 0.0);
            ladder_waiting = false;
        }
    }
    std::uint64_t wts = 0;
    std::uint8_t requested = 1;
    if (!SafeGet(Guest(0x5556678), &wts) || !wts || !SafeGet(wts + 8, &requested) || requested) {
        return; // a stage request is pending
    }
    const TravelStep step = t.steps[t.next_step++];
    // A flag write is instant and moves nobody: the chain stays armed, so a travel step after it
    // still runs without another join (host_flag=1026:0@15,host_flag=1023@16,host_warp=2102961@18).
    if (step.kind != TravelStep::HostFlag && step.kind != TravelStep::GuestFlag && step.kind != TravelStep::HostGfx) {
        t.armed = false;
    }
    t.step_at = now;
    t.ran = true;
    t.last_kind = step.kind;
    if (guard_env &&
        deathrules::TestGuardStepRan(guard, step.kind == TravelStep::HostDie || step.kind == TravelStep::GuestDie ||
                                                step.kind == TravelStep::HostChr || step.kind == TravelStep::GuestChr)) {
        SetTestInvincible(false);
        Log("test: guard (BB_PARTY_TEST_GUARD): off for %s", TravelStepName(step.kind));
    }
    Log("test: travel step %zu/%zu: %s%s%u (%s)", t.next_step, t.steps.size(), TravelStepName(step.kind),
        step.kind == TravelStep::HostWarp ? " to lamp " : step.kind == TravelStep::HostKill ? " entity " : "",
        step.kind == TravelStep::HostWarp || step.kind == TravelStep::HostKill ? step.id : 0u, Describe(s).c_str());
    if (step.kind == TravelStep::HostWarp) {
        // Test setup should stay invisible: a host already standing at that lamp (within 5 m, same
        // map) only gets the lamp as its last-lamp record (+0x1528), no warp and no loading screen.
        float spot[4], me[4];
        std::uint64_t w = 0, rec = 0, pm = 0, a = 0, b = 0;
        const std::uint64_t player = LocalPlayer();
        bool at_lamp = false;
        if (ids::ReturnSpot(step.id, spot) && (ids::EntityMap(step.id) >> 16) == (s.map_id >> 16) && player &&
            SafeGet(player + 0x58, &a) && a && SafeGet(a + 8, &b) && b && SafeGet(b + 0x3b0, &pm) && pm &&
            SafeGet(pm + 0x68, &pm) && pm && SafeRead(pm + 0x1e0, me, sizeof me)) {
            const float dx = me[0] - spot[0], dy = me[1] - spot[1], dz = me[2] - spot[2];
            at_lamp = dx * dx + dy * dy + dz * dz < 25.0f;
        }
        if (at_lamp && SafeGet(Guest(0x5556678), &w) && w && SafeGet(w + 0x1528, &rec)) {
            const std::uint64_t now_rec = (rec & 0xffffffff00000000ull) | step.id;
            SafeWrite(w + 0x1528, &now_rec, 8);
            Log("test: host_warp %u skipped: already at that lamp (last-lamp record set, no load)", step.id);
        } else {
            RunLampWarpDecided(step.id, false);  // the party move (no split-travel question)
        }
    } else if (step.kind == TravelStep::HostAlone) {
        b2::HostTravelAloneNext();
        RunLampWarpDecided(step.id, true);
    } else if (step.kind == TravelStep::HostLamp || step.kind == TravelStep::GuestLamp) {
        if (step.args == "story") {
            // host_lamp=ID:story / guest_lamp=ID:story (B6): the warp as a story event's 2003[49]
            // runs it (seamless_rules: StoryWarpEvent -> NoteStoryWarp), e.g. the carriage to
            // Cainhurst 2502959 or the Lecture Building 3202958: the question after the scene.
            split::NoteStoryWarp(step.id);
            Log("test: %s %u as a story warp (2003[49])", TravelStepName(step.kind), step.id);
        } else {
            split::NoteLocalUse(step.id);  // this player's own use (no talk script sets a lamp flag)
        }
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint32_t)>(Guest(kLampWarp))(step.id);  // as the player
    } else if (step.kind == TravelStep::HostMove || step.kind == TravelStep::GuestMove) {
        // The local chr to a spot (the chr warp vfunc +0x5F8 = 0x18CD600(chr, pos, rot, 1), as the
        // NPC summon step and the B2 spacing do), keeping its rotation.
        const std::uint64_t player = LocalPlayer();
        std::uint64_t pm = 0, a = 0, b = 0, vt = 0, fn = 0;
        alignas(16) float pos[4] = {step.pos[0], step.pos[1], step.pos[2], 1.0f}, rot[4] = {0, 0, 0, 0};
        if (player && SafeGet(player + 0x58, &a) && a && SafeGet(a + 8, &b) && b && SafeGet(b + 0x3b0, &pm) && pm &&
            SafeGet(pm + 0x68, &pm) && pm && SafeRead(pm + 0x1d0, rot, sizeof rot) && SafeGet(player, &vt) && vt &&
            SafeGet(vt + 0x5f8, &fn) && fn) {
            if (step.has_yaw) {
                rot[0] = 0;
                rot[1] = step.yaw;  // the B2 "dest" event's rot is {0, yaw, 0, 0}
                rot[2] = 0;
                rot[3] = 0;
            }
            bool native = false;
            if (step.warp == 1) {
                native = b2::NativeWarpLocal(player, pos, rot, "test move");
            }
            if (!native) {
                reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint32_t)>(fn)(
                    player, pos, rot, 1);
            }
            if (TestInvincibleRequested()) SetTestInvincible(true);
            if (step.pin > 0) {
                std::memcpy(t.pin_pos, pos, sizeof pos);
                std::memcpy(t.pin_rot, rot, sizeof rot);
                t.pin_until = now + std::chrono::milliseconds(static_cast<int>(step.pin * 1000));
                t.pin_ground = step.ground;
                t.pin_secs = step.pin;
                t.pin_retries = 0;
                t.pin_set = now;
                t.fall_check = {};
            }
            Log("test: %s to (%.2f, %.2f, %.2f) rot y %.3f (%s)%s%s", TravelStepName(step.kind), double(pos[0]),
                double(pos[1]), double(pos[2]), double(rot[1]), native ? "the game's own player warp" : "bare chr warp",
                TestInvincibleRequested() ? ", invincible" : "", step.pin > 0 ? ", held there while the floor loads" : "");
        } else {
            Log("test: %s: no player / transform", TravelStepName(step.kind));
        }
    } else if (step.kind == TravelStep::HostLamp) {
        // host_lamp=ID@S: the lamp warp 0x13CDF30(ID) on the steady-world clock (host_warp waits for
        // a re-join, which a B2 host never sees): e.g. a Chalice Dungeon lamp's "back to the Dream".
        split::NoteLocalUse(step.id);
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint32_t)>(Guest(kLampWarp))(step.id);
    } else if (step.kind == TravelStep::HostLastLamp || step.kind == TravelStep::GuestLastLamp) {
        // Test fixture: the host's last-lamp record (WorldTransitionState +0x1528 {id, aux}) names
        // a lamp of another map - the state after walking into the next area without resting.
        std::uint64_t w = 0, rec = 0;
        if (SafeGet(Guest(0x5556678), &w) && w && SafeGet(w + 0x1528, &rec)) {
            const std::uint64_t now_rec = (rec & 0xffffffff00000000ull) | step.id;
            SafeWrite(w + 0x1528, &now_rec, 8);
            Log("test: %s %#llx -> %#llx", TravelStepName(step.kind), static_cast<unsigned long long>(rec),
                static_cast<unsigned long long>(now_rec));
        }
    } else if (step.kind == TravelStep::HostSpfx) {
        int entity = 0, id = 0;
        std::sscanf(step.args.c_str(), "%d:%d", &entity, &id);
        std::string what;
        const bool ok = balance::ApplySpEffectToEntity(entity, id, &what);
        Log("test: host_spfx: SpEffect %d on entity %d: %s", id, entity, ok ? what.c_str() : "FAILED: no such chr loaded");
    } else if (step.kind == TravelStep::HostGfx) {
        GfxTestStep(step.args);
    } else if (step.kind == TravelStep::HostPoise && step.args.find(':') != std::string::npos) {
        // host_rule=KEY:0|1 (smoke group rot): a party gameplay switch (BbSettings::PartySwitches,
        // e.g. party_rot_cleanse_killer) set live, not saved; the host's rules go out with it.
        const std::string key = step.args.substr(0, step.args.find(':'));
        const bool on = step.args.substr(step.args.find(':') + 1) != "0";
        bool found = false;
        for (int i = 0; i < BbSettings::PartySwitchCount; ++i) {
            const BbSettings::PartySwitch& sw = BbSettings::PartySwitches[i];
            if (key != sw.key) continue;
            found = true;
            const bool was = (BbSettings::Get().*(sw.member)).load();
            (BbSettings::Get().*(sw.member)).store(on);
            Log("test: host_rule: %s %d -> %d", sw.key, was ? 1 : 0, on ? 1 : 0);
        }
        if (!found) Log("test: host_rule: no party switch %s", key.c_str());
    } else if (step.kind == TravelStep::HostPoise) {
        // D7 stagger A/B in one session: the host's Enemy poise setting, live (not saved); the
        // balance pass rewrites row 7503 and sends the rules on.
        const int was = BbSettings::Get().party_enemy_poise.load();
        BbSettings::Get().party_enemy_poise = std::clamp(std::atoi(step.args.c_str()), 0, 300);
        Log("test: host_poise: Enemy poise %d -> %d %% of the step", was, BbSettings::Get().party_enemy_poise.load());
    } else if (step.kind == TravelStep::HostOutage || step.kind == TravelStep::GuestOutage) {
        bbnet::netsim::outage_now(double(step.buttons));
        Log("test: %s %u ms (%s)", TravelStepName(step.kind), step.buttons, Describe(s).c_str());
    } else if (step.kind == TravelStep::HostPress || step.kind == TravelStep::GuestPress) {
        // In-process pad input (runtime_pad_director_press, OR-ed into every pad sample; no OS
        // input): a tap, released 0.15 s later. ":lock": not while the lock-on (chr +0xF8) is on.
        // R3 with no target in the camera's view only turns the camera behind the player, as on
        // a pad: after a chr warp (host_chr) the camera can face away from the target (live
        // deaths071: the first R3 after host_chr missed with and without a death before it, the
        // second locked on).
        std::uint8_t locked = 0;
        if (step.args == "lock") {
            if (const std::uint64_t pl = LocalPlayer()) SafeGet(pl + 0xf8, &locked);
        }
        if (!locked) {
            runtime_pad_director_press(step.buttons);
            t.press_at = now;
            t.pressing = true;
            t.release_at = now + std::chrono::milliseconds(150);
        } else {
            Log("test: %s 0x%x:lock skipped: locked on already", TravelStepName(step.kind), step.buttons);
        }
        std::string target;
        if (t.chr_target) {
            // A weapon fight (host_chr / guest_chr then R1 taps): the target's HP and distance.
            float me[3] = {}, at[3] = {};
            const std::uint64_t player = LocalPlayer();
            for (const auto& e : balance::LoadedEnemies()) {
                if (e.entity != t.chr_target) continue;
                char b[160];
                const bool d = player && balance::ChrPosition(player, me) && balance::ChrPosition(e.chr, at);
                std::snprintf(b, sizeof b, "; target %u HP %d, %.1f m", e.entity, balance::EnemyHp(e.chr),
                              d ? double(std::sqrt((me[0] - at[0]) * (me[0] - at[0]) + (me[1] - at[1]) * (me[1] - at[1]) +
                                                   (me[2] - at[2]) * (me[2] - at[2])))
                                : -1.0);
                target = b;
            }
            if (target.empty()) {
                // An NPC hunter (D11) is not in the census list: its chr by entity id (0x13C97A0).
                const std::uint64_t chr =
                    reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::uint64_t, std::uint64_t)>(
                        Guest(0x13c97a0))(std::int32_t(t.chr_target), 0, 0);
                if (chr) {
                    char b[160];
                    const bool d = player && balance::ChrPosition(player, me) && balance::ChrPosition(chr, at);
                    std::int32_t team = -1;
                    SafeGet(chr + 0x88, &team);
                    std::snprintf(b, sizeof b, "; target %u (NPC, team %d) HP %d, %.1f m", t.chr_target, team,
                                  balance::EnemyHp(chr),
                                  d ? double(std::sqrt((me[0] - at[0]) * (me[0] - at[0]) + (me[1] - at[1]) * (me[1] - at[1]) +
                                                       (me[2] - at[2]) * (me[2] - at[2])))
                                    : -1.0);
                    target = b;
                }
            }
        }
        {
            // The own HP / max (HP module [+0x3B0] +0x20: +0xF8 / +0xFC; rally and vial checks) and
            // lock-on (chr +0xF8, the behaviour variable IsLockon in 0x1A18BF0; 0x188E900 sets it
            // and copies the target entry's first 16 bytes to +0x100: the target's position, live
            // revivecheck) with the loaded enemy nearest that point.
            const std::uint64_t player = LocalPlayer();
            std::uint64_t cm = 0, dm = 0;
            std::int32_t hp = -1, mhp = -1;
            std::uint8_t lock = 0;
            float lp[3] = {};
            if (player && SafeGet(player + 0x3b0, &cm) && cm && SafeGet(cm + 0x20, &dm) && dm) {
                SafeGet(dm + 0xf8, &hp);
                SafeGet(dm + 0xfc, &mhp);
            }
            if (player) {
                SafeGet(player + 0xf8, &lock);
                SafeRead(player + 0x100, lp, sizeof lp);
            }
            std::uint32_t lent = 0;
            int lhp = -1;
            float best = 4.0f;  // within 2 m of the point
            if (lock) {
                for (const auto& e : balance::LoadedEnemies()) {
                    float at[3] = {};
                    if (!balance::ChrPosition(e.chr, at)) continue;
                    const float d = (at[0] - lp[0]) * (at[0] - lp[0]) + (at[1] - lp[1]) * (at[1] - lp[1]) +
                                    (at[2] - lp[2]) * (at[2] - lp[2]);
                    if (d < best) {
                        best = d;
                        lent = e.entity;
                        lhp = balance::EnemyHp(e.chr);
                    }
                }
            }
            char b[220];
            std::snprintf(b, sizeof b, "; own HP %d/%d, lock-on %u (target at (%.1f, %.1f, %.1f): enemy %u, HP %d)", hp,
                          mhp, unsigned(lock), double(lp[0]), double(lp[1]), double(lp[2]), lent, lhp);
            target += b;
        }
        Log("test: %s 0x%x%s", TravelStepName(step.kind), step.buttons, target.c_str());
    } else if (step.kind == TravelStep::HostGather) {
        const bool queued = ::party::status::RequestGather();
        Log("test: host_gather: Party > Gather the party pressed%s (%s)", queued ? "" : " - NOT queued (not the host)",
            Describe(s).c_str());
    } else if (step.kind == TravelStep::HostKick) {
        const bool ok = ::party::status::KickMember(step.args);
        Log("test: host_kick: Party > Kick %s %s (%s)", step.args.c_str(), ok ? "pressed" : "refused by the board",
            Describe(s).c_str());
    } else if (step.kind == TravelStep::HostLeave || step.kind == TravelStep::GuestLeave) {
        ::party::status::RequestLeave();
        Log("test: %s: Party > Leave pressed (%s)", TravelStepName(step.kind), Describe(s).c_str());
    } else if (step.kind == TravelStep::GuestHome) {
        // Playtest 1 G4 (direct summon re-rings): the guest goes home on its own; the director's
        // bell then rings again from its own world.
        const bool left = LeaveGameSession();
        Log("test: guest_home: the game's own leave %s (%s)", left ? "run" : "NOT available", Describe(s).c_str());
    } else if (step.kind == TravelStep::GuestFixture) {
        worldact::FixtureRearm();
    } else if (step.kind == TravelStep::HostUse || step.kind == TravelStep::GuestUse) {
        UseGoodsForTest(TravelStepName(step.kind), static_cast<std::uint32_t>(std::strtoul(step.args.c_str(), nullptr, 10)),
                        step.args.find(":take") != std::string::npos);
    } else if (step.kind == TravelStep::HostMark || step.kind == TravelStep::GuestMark) {
        if (step.args == "lua") {
            std::int64_t r = -1;
            const bool ok = LuaEventRaiseNow("OnReviveMagic", &r);
            Log("test: Lua event OnReviveMagic %s (dispatcher %lld)", ok ? "raised" : "NOT raised",
                static_cast<long long>(r));
        } else {
            // The Hunter's Mark item (EquipParamGoods 100) is SpEffect 2100 (stateInfo 16): applied
            // to the local player as the item use does; the game takes it from there.
            const int effect = step.args.empty() ? 2100 : std::atoi(step.args.c_str());
            const bool ok = ApplySpEffect(effect);
            Log("test: Hunter's Mark: SpEffect %d %s (%s)", effect, ok ? "applied" : "NOT applied",
                DescribeHere(s).c_str());
        }
    } else if (step.kind == TravelStep::HostObj || step.kind == TravelStep::GuestObj ||
               step.kind == TravelStep::HostChr || step.kind == TravelStep::GuestChr) {
        int entity = 0;
        float dist = 1.2f, angle = 0, dy = 0;
        // host_obj=ENTITY[:DIST[:ANGLE[:DY]]]: DY m above the object's origin (dlast: a raised
        // ladder's top landing, its model's dummy 192).
        std::sscanf(step.args.c_str(), "%d:%f:%f:%f", &entity, &dist, &angle, &dy);
        std::string what;
        const bool obj = step.kind == TravelStep::HostObj || step.kind == TravelStep::GuestObj;
        if (!obj && entity == 0) {
            // host_chr=0: the nearest living regular enemy of the census (a real weapon fight: stand
            // at it, then host_press attacks; H3 kill echoes by the game's own kill).
            auto list = balance::LoadedEnemies();
            // Only enemies with a map entity id (StandAtChr finds the chr by it).
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [](const auto& e) { return e.entity == 0 || e.entity == 0xffffffffu; }),
                       list.end());
            float me[3] = {};
            const std::uint64_t player = LocalPlayer();
            const bool here = player && balance::ChrPosition(player, me);
            // Only enemies on the own floor (within 3 m of height): the stand spot is beside the enemy
            // at its height, so one on a ledge above or below was a warp into a fall (hostcheck: the
            // nearest enemy to the Gascoigne arena stood 28 m higher, the host fell to its death).
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const auto& e) {
                                          float at[3] = {};
                                          // HP 200 or more: the weak ones (HP 120 near the Great Bridge)
                                          // walked off and never struck (smoke s3c).
                                          return !balance::ChrPosition(e.chr, at) || std::fabs(at[1] - me[1]) > 3.0f ||
                                                 balance::EnemyHp(e.chr) < 200;
                                      }),
                       list.end());
            const int k = here ? PickEnemyKillTarget(list, 0, me, 500.0f) : -1;
            if (k < 0) {
                Log("test: %s 0: no living enemy (HP 200+) on this floor (height %.1f, +-3 m) within 500 m", TravelStepName(step.kind),
                    double(me[1]));
            }
            if (k >= 0) {
                entity = static_cast<int>(list[size_t(k)].entity);
                Log("test: %s 0: the nearest living enemy is entity %d (HP %d)", TravelStepName(step.kind), entity,
                    balance::EnemyHp(list[size_t(k)].chr));
            }
        }
        const bool ok = obj ? worldact::StandAtObject(entity, dist, angle, &what, dy)
                            : worldact::StandAtChr(entity, dist, angle, &what);
        if (!obj) t.chr_target = ok ? std::uint32_t(entity) : 0u;
        Log("test: %s %d (%.1f m, %.0f deg): %s", TravelStepName(step.kind), entity, double(dist), double(angle),
            ok ? what.c_str() : "FAILED: not loaded");
    } else if (step.kind == TravelStep::HostRoute || step.kind == TravelStep::GuestRoute) {
        std::vector<std::array<float, 3>> route;
        if (ParseRouteArgs(step.args, &route)) {
            Test& tt = t;
            tt.route = route;
            tt.route_i = 0;
            tt.route_slides = 0;
            tt.route_block = 0;
            tt.route_started = now;
            tt.approach = true;
            tt.approach_done = false;
            tt.ap_x = route[0][0];
            tt.ap_z = route[0][2];
            tt.ap_r = kRouteReach;
            tt.route_hops = 0;
            tt.ap_max = 60;
            tt.ap_button = 0;
            tt.ap_steps = 0;
            tt.ap_still = 0;
            tt.ap_slide = 0;
            tt.ap_slides_here = 0;
            tt.ap_phase = 0;
            tt.ap_at = now;
            // Invincible while it walks (EnableInvincible, chr +0x1E1 bit 14, as a11_move): enemies on the
            // way must not end the walk; fights and deaths are the steps after it.
            std::uint32_t f = 0;
            if (s.player && SafeRead(s.player + 0x1e1, &f, 4)) {
                tt.route_god = !(f & (1u << 14));
                f |= 1u << 14;
                SafeWrite(s.player + 0x1e1, &f, 4);
            }
            Log("test: %s: %zu waypoint(s), first (%.2f, %.2f) (%s)", TravelStepName(step.kind), route.size(),
                double(route[0][0]), double(route[0][2]), DescribeHere(s).c_str());
        } else {
            Log("test: %s: not X:Z[:R]/X:Z[:R]/...", TravelStepName(step.kind));
        }
    } else if (step.kind == TravelStep::GuestStick || step.kind == TravelStep::HostStick) {
        float x = 0, z = 0, r = 1.0f;
        int steps = 40;
        int entity = 0;
        float odist = 0, oangle = 0;
        bool ok = false;
        if (ParseStickObjArgs(step.args, &entity, &odist, &oangle, &r, &steps)) {
            // obj:ENTITY:DIST:ANGLE: the point DIST m from that map object (e.g. through a
            // confinement wall on a block boundary: ANGLE 180 = behind it).
            float p[3] = {};
            ok = worldact::ObjectPoint(entity, odist, oangle, p);
            x = p[0], z = p[2];
            Log("test: %s: object %d %s", TravelStepName(step.kind), entity, ok ? "found" : "NOT loaded");
        } else if (step.args.rfind("stain", 0) == 0) {
            // stain[:R[:STEPS]]: onto the own bloodstain (GameDataMan +0x28 flag, +0x30 record:
            // position +0x18), walked, so its pickup prompt comes up the way it does in play.
            std::uint64_t rec = 0;
            std::uint8_t flag = 0;
            float p[3] = {};
            std::sscanf(step.args.c_str(), "stain:%f:%d", &r, &steps);
            ok = s.game_data_man && SafeGet(s.game_data_man + 0x28, &flag) && flag &&
                 SafeGet(s.game_data_man + 0x30, &rec) && rec && SafeRead(rec, p, sizeof p);
            x = p[0], z = p[2];
            Log("test: %s: own bloodstain %s at (%.1f, %.1f, %.1f)", TravelStepName(step.kind), ok ? "found" : "NOT there",
                double(p[0]), double(p[1]), double(p[2]));
        } else {
            ok = ParseStickArgs(step.args, &x, &z, &r, &steps);
        }
        if (ok) {
            Test& tt = t;
            tt.route.clear();
            tt.approach = true;
            tt.approach_done = false;
            tt.ap_x = x;
            tt.ap_z = z;
            tt.ap_r = r;
            tt.ap_max = steps;
            tt.ap_button = 0;
            tt.ap_steps = 0;
            tt.ap_still = 0;
            tt.ap_slide = 0;
            tt.ap_slides_here = 0;
            tt.ap_phase = 0;
            tt.ap_at = now;
            Log("test: %s: walk to (%.2f, %.2f) within %.1f m, at most %d steps (%s)", TravelStepName(step.kind),
                double(x), double(z), double(r), steps, DescribeHere(s).c_str());
        } else {
            Log("test: %s '%s': not X:Z[:R[:STEPS]] / obj:ENTITY:DIST:ANGLE[:R[:STEPS]], or not loaded",
                TravelStepName(step.kind), step.args.c_str());
        }
    } else if (step.kind == TravelStep::HostSkipRemo || step.kind == TravelStep::GuestSkipRemo) {
        // The game's own cutscene skip request (what the skip input does; in a session it is this
        // player's skip vote).
        bool skippable = false;
        const bool ok = RequestRemoSkip(&skippable);
        Log("test: %s: skip request %s (skippable bit %d)", TravelStepName(step.kind), ok ? "sent" : "NOT possible",
            skippable ? 1 : 0);
    } else if (step.kind == TravelStep::HostFlag || step.kind == TravelStep::GuestFlag) {
        // A story choice the game makes in a talk menu (e.g. 72100131 = refuse Gehrman): SetEventFlag.
        // "ID:0" clears it (a test save put back into its prologue: host_flag=12101021:0).
        char* end = nullptr;
        const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(step.args.c_str(), &end, 10));
        const bool value = !(end && end[0] == ':' && end[1] == '0');
        bool before = false, after = false;
        ReadEventFlag(id, &before);
        const bool ok = WriteEventFlag(id, value);
        ReadEventFlag(id, &after);
        Log("test: %s %u: %d -> %d (%s)", TravelStepName(step.kind), id, int(before), int(after),
            ok ? "written" : "NOT written");
    } else if (step.kind == TravelStep::HostStage) {
        unsigned a = 0, b = 0, r = 0, i = 0;
        int wp = -1;
        if (step.args.rfind("bonfire:", 0) == 0) {
            const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(step.args.c_str() + 8, nullptr, 10));
            Log("test: WarpNextStage_Bonfire(%u) %s", id, DescribeHere(s).c_str());
            reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kWarpNextStageBonfire))(0, id);
        } else if (std::sscanf(step.args.c_str(), "%u:%u:%u:%u:%d", &a, &b, &r, &i, &wp) >= 4) {
            Log("test: WarpNextStage(m%02u_%02u_%02u_%02u, point %d) %s", a, b, r, i, wp, DescribeHere(s).c_str());
            reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                          std::uint32_t, std::uint32_t)>(Guest(kWarpNextStage))(
                0, a, b, r, i, static_cast<std::uint32_t>(wp));
        } else {
            Log("test: host_stage '%s' is not area:block:region:index[:point] or bonfire:ID", step.args.c_str());
        }
    } else if (step.kind == TravelStep::HostWalk || step.kind == TravelStep::HostHeadstone) {
        t.script = Test::Script{};
        t.script.kind = step.kind;
        t.script.args = step.args;
        t.script.at = now;
        t.script_on = true;
        Log("test: %s %s from %s", TravelStepName(step.kind), step.args.c_str(), DescribeHere(s).c_str());
        return;
    } else if (step.kind == TravelStep::GuestKill) {
        // guest_kill=ENTITY@S%MAP (0: the nearest living enemy within 500 m): a kill on the guest's
        // own copy (split travel: an enemy killed while the players are apart).
        // guest_kill=ENTITY:hp on a boss: its HP 0 on the guest's game (a lone guest's boss kill).
        KillLoadedEnemyForTest("guest_kill", step.id, false, step.args.rfind("hp", 0) == 0,
                               std::atoi(step.args.c_str() + (step.args.rfind("hp", 0) == 0 ? 2 : 0)));
    } else if (step.kind == TravelStep::HostKill) {
        // A boss (or any enemy) killed as the game's own death path: ForceDead on its entity id; the
        // boss-defeat EMEVD (4[00] death state -> 2003[12]) and the Lua boss clear follow.
        // host_kill=0: the nearest living regular enemy within 500 m (echoes for everyone).
        KillLoadedEnemyForTest("host_kill", step.id, true, step.args.rfind("hp", 0) == 0,
                               std::atoi(step.args.c_str() + (step.args.rfind("hp", 0) == 0 ? 2 : 0)));
    } else {
        if (step.kind == TravelStep::GuestDie && step.args == "kidnap") {
            // kidnapall: what the guest's copy of 12400780 does at a Snatcher's grab (its 9420 write
            // skipped, the host asked for the party's kidnap), then the grab's death.
            b2::TestGuestKidnapGrab();
        }
        DieForTest();
    }
    std::uint8_t after = 0;
    SafeGet(wts + 8, &after);
    Log("test: travel step %s done; stage request %s", TravelStepName(step.kind), after ? "armed" : "not armed");
}

/// The title takes scripted input (BB_TEST_NEWGAME): no world; in party mode the link is up and
/// the FROM sign-in (ss.info -> login -> notice) is over: the title menu shows after it (before,
/// "Creating save data" / "Checking network status" swallow presses).
/// The player's area and position (FrpgNetMan +0xa7c / +0xaac); false when not readable.
bool ReadPlayerPos(std::uint32_t* area, float pos[3]) {
    std::uint64_t man = 0;
    const std::uint64_t at = Guest(0x553b120);
    return at && SafeGet(at, &man) && man && SafeGet(man + 0xa7c, area) && SafeRead(man + 0xaac, pos, 3 * sizeof(float));
}

/// Test fixtures: the left stick through the BB_PAD_FILE pad script (src/runtime_pad.c re-reads
/// it when it changes); "" releases it.
void WritePadFile(const char* tokens) {
    const char* path = std::getenv("BB_PAD_FILE");
    if (!path || !path[0]) {
        return;
    }
    if (FILE* f = std::fopen(path, "w")) {
        std::fputs(tokens, f);
        std::fclose(f);
    }
}

bool TitleReadyForInput(PartyRole role, party::PartyLink* link) {
    if (role == PartyRole::None) {
        return true;
    }
    const party::LinkState ls = link ? link->state() : party::LinkState::Idle;
    const bool link_ok = role == PartyRole::Host ? ls == party::LinkState::Hosting : ls == party::LinkState::Connected;
    if (!link_ok) {
        return false;
    }
    const bbnet::OnlineProgress op = bbnet::online_progress();
    return op.notice > 0 && op.since_last >= 2.0;
}

// ---- Host session watchdog (party_watchdog.h) ----
// 0x13CDDA0: BeginLeaveSession(SprjSessionManager) 0x1ED07A0, ReloadPhase+0xf0 = chr type on
// success, then a tail jump to 0x15BED10(network flow +0x16F8) (drops the presentation members).
constexpr std::uint64_t kLeaveSessionAndDrop = 0x13cdda0;
using VoidFn = void(BB_COOP_SYSV*)();

/// SprjSessionManager peer count (+0xF8..+0x100, stride 0x20), presentation count (network flow
/// +0x16F8 -> +0x18) and the WorldChrMan phantoms (chr set 1 count, +0x850 table): for the logs.
std::string DescribeSessionMembers() {
    std::uint64_t sm = 0, flow = 0, pres = 0, b = 0, e = 0;
    SafeGet(Guest(0x5540290), &sm);
    SafeGet(Guest(0x5556678), &flow);
    if (sm) {
        SafeGet(sm + 0xf8, &b);
        SafeGet(sm + 0x100, &e);
    }
    std::int32_t pn = -1;
    if (flow && SafeGet(flow + 0x16f8, &pres) && pres) {
        SafeGet(pres + 0x18, &pn);
    }
    std::int32_t slots[4] = {-1, -1, -1, -1};
    if (pres) {
        SafeRead(pres + 0x80, slots, sizeof slots);
    }
    char out[200];
    std::snprintf(out, sizeof out, "session peers %lld, presentation count %d (handles %x %x %x %x)",
                  (b && e >= b) ? static_cast<long long>((e - b) / 0x20) : -1LL, pn, unsigned(slots[0]),
                  unsigned(slots[1]), unsigned(slots[2]), unsigned(slots[3]));
    return out;
}

} // namespace

namespace {
constexpr std::uint64_t kOnLeaveLimit = 0x138a4a0;
bool OnLeaveLimitOk() {
    static const bool ok = Matches(kOnLeaveLimit, {0x55, 0x48, 0x89, 0xe5}) && CallsTo(kOnLeaveLimit + 0x77, 0x1332bc0);
    return ok;
}
/// The game's own send-home of a phantom (OnLeave_Limit), let through party_travel's retarget.
void RunOnLeaveLimit() {
    AllowNextSendHome();
    using LimitFn = void(BB_COOP_SYSV*)(std::uint64_t);
    reinterpret_cast<LimitFn>(Guest(kOnLeaveLimit))(0);
}
std::atomic<bool> g_send_home_request{false};
std::string g_send_home_why;  // under g_send_home_mu
std::mutex g_send_home_mu;
} // namespace

void RequestGuestSendHome(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(g_send_home_mu);
        g_send_home_why = why;
    }
    g_send_home_request = true;
}

bool GuestBellEffectOn() { return HasSpEffect(LocalPlayer(), kGuestBellEffect); }
bool DropGuestBellEffect() { return RemoveSpEffect(kGuestBellEffect); }
bool ApplyGuestBellEffect() { return ApplySpEffect(kGuestBellEffect); }

bool LeaveGameSession() {
    // A guest (a phantom in the host's world): the game's own voluntary leave - OnLeave_Limit
    // (0x138A4A0, what OnLeaveMenu_Yes 0x1389FB0 runs for a phantom after the session leave): the
    // pre-summon spot, then the funnel (BeginLeave + the load home) at once. The session leave
    // alone left the game to notice a dead room 10 s later ("Session lost" 0xcc/5001, then home).
    const GameSnapshot before = ReadGameState(false);
    if (before.session_role == RoleClient && OnLeaveLimitOk()) {
        RunOnLeaveLimit();
        const GameSnapshot after = ReadGameState(false);
        Log("leave: OnLeave_Limit (the guest's own leave): role %s (%d)", SessionRoleName(after.session_role),
            after.session_role);
        return true;
    }
    static const bool ok =
        Matches(kLeaveSessionAndDrop, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x8b, 0x05,
                                       0xc7, 0x88, 0x18, 0x04, 0x48, 0x8b, 0x98, 0xf8, 0x16, 0x00, 0x00}) &&
        CallsTo(kLeaveSessionAndDrop + 0x55, 0x1ed07a0);
    if (!ok) return false;
    reinterpret_cast<VoidFn>(Guest(kLeaveSessionAndDrop))();
    const GameSnapshot after = ReadGameState(false);
    Log("leave: the game's session leave: role %s (%d); %s", SessionRoleName(after.session_role), after.session_role,
        DescribeSessionMembers().c_str());
    return true;
}

namespace {

void HostWatchdogTick(State& st, const GameSnapshot& s, party::PartyLink* link, Clock::time_point now) {
    HostWatchdogInput in;
    in.now_s = Seconds(st.start, now);
    in.in_world = InWorld(s) && st.world_seen;
    in.session_role = s.session_role;
    in.cooperators = s.cooperators;
    in.members_apart = b2::MembersApart();
    in.host_travel = b2::HostRejoinRunning();
    if (link) {
        in.roster = link->roster();
    }
    in.unreachable = st.unreachable;
    if (st.watchdog_check && s.session_role == RoleIdle && Seconds(st.watchdog_at, now) >= 2.0) {
        st.watchdog_check = false;
        Log("host watchdog: session idle %.1f s after the stop; %s; cooperators %d", Seconds(st.watchdog_at, now),
            DescribeSessionMembers().c_str(), s.cooperators);
    }
    if (!st.watchdog.Update(in)) {
        return;
    }
    static int code_ok = 0;
    if (!code_ok) {
        code_ok = Matches(kLeaveSessionAndDrop, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x8b,
                                                 0x05, 0xc7, 0x88, 0x18, 0x04, 0x48, 0x8b, 0x98, 0xf8, 0x16, 0x00, 0x00})
                      ? 1
                      : -1;
        if (code_ok < 0) {
            Log("host watchdog: 0x13CDDA0 is not the 1.09 leave helper; the watchdog cannot stop sessions");
        }
    }
    ++st.watchdog_stops;
    Log("host watchdog: stop #%d (t %.1f s): %s; %s; roster: %s", st.watchdog_stops, in.now_s, st.watchdog.why().c_str(),
        DescribeSessionMembers().c_str(), [&] {
            std::string r;
            for (const party::RosterEntry& e : in.roster) {
                char b[96];
                std::snprintf(b, sizeof b, "%s%s(slot %d, %s%s)", r.empty() ? "" : ", ", e.name.c_str(), e.slot,
                              party::member_state_name(e.state), e.connected ? "" : ", lost");
                r += b;
            }
            return r;
        }().c_str());
    if (code_ok < 0 || !CallsTo(kLeaveSessionAndDrop + 0x55, 0x1ed07a0)) {
        Log("host watchdog: no stop (code check failed)");
        return;
    }
    reinterpret_cast<VoidFn>(Guest(kLeaveSessionAndDrop))();
    const GameSnapshot after = ReadGameState(false);
    Log("host watchdog: left the session: role %s (%d); %s", SessionRoleName(after.session_role), after.session_role,
        DescribeSessionMembers().c_str());
    party::status::SetLastEvent("Summon stuck: session restarted");
    st.watchdog_check = true;
    st.watchdog_at = now;
    st.rung_ever = false; // ring again as soon as the session is idle and a member waits
}

// ---- Summons that cannot work (party_util.h summon_endpoint_unusable) ----
// 2026-10-20: a remote guest whose game had only its private LAN address (no STUN answer from the
// host's party port): 4 summons of 20 s punching that address, then the host watchdog's stop.
// Now neither side rings for it, both players get a notice, and it recovers on its own: the
// guest's keepalive retries STUN (np_session.cpp) and its signaling_update clears the hold.
// BB_PARTY_TEST_UNREACHABLE=1 (tests, loopback): every member (host) / this game (guest) counts
// as unreachable.
bool TestUnreachable() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_TEST_UNREACHABLE");
        return v && v[0] == '1';
    }();
    return on;
}

void UdpReachTick(State& st, PartyRole role, party::PartyLink* link, Clock::time_point now) {
    if (role == PartyRole::None || !link || Seconds(st.reach_at, now) < 1.0) {
        return;
    }
    st.reach_at = now;
    if (role == PartyRole::Host) {
        std::vector<std::pair<std::string, std::string>> bad = bbnet::party::PartyHostService::instance().unusable_endpoints();
        std::vector<std::string> names;
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot == party::kHostSlot || !e.connected) {
                continue;
            }
            std::string where;
            bool hit = TestUnreachable();
            for (const auto& [id, at] : bad) {
                if (id == e.name) {
                    hit = true;
                    where = at;
                }
            }
            if (!hit) {
                continue;
            }
            names.push_back(e.name);
            if (std::find(st.unreachable.begin(), st.unreachable.end(), e.name) == st.unreachable.end()) {
                Log("summon: UDP to %s is blocked: its game has only the LAN address %s (no public UDP endpoint: "
                    "the STUN on our party port gave it no answer); no bell or summon for it until it has one "
                    "(it retries in the background)",
                    e.name.c_str(), where.empty() ? "(test)" : where.c_str());
                party::status::SetLastEvent("UDP to " + e.name +
                                            " is blocked: their game has no public UDP address yet; no summon "
                                            "until it has one (retrying)");
            }
        }
        for (const std::string& n : st.unreachable) {
            if (std::find(names.begin(), names.end(), n) == names.end()) {
                Log("summon: UDP to %s works again (its public endpoint is known): summons resume", n.c_str());
                party::status::SetLastEvent("UDP to " + n + " works again: summoning");
                st.rung_ever = false;
            }
        }
        st.unreachable = std::move(names);
        return;
    }
    std::string stun_host;
    bool asked = false;
    const bool known = bbnet::session::mapped_known(&stun_host, &asked);
    const bool blocked =
        link->state() == party::LinkState::Connected && asked &&
        (TestUnreachable() ||
         (!known && bbnet::party::summon_endpoint_unusable(bbnet::party::ip_parse(stun_host),
                                                           bbnet::party::ip_parse(bbnet::session::local_addr_text()), 0, 0)));
    if (blocked != st.self_unreachable) {
        st.self_unreachable = blocked;
        st.reach_said = now;
        if (blocked) {
            Log("summon: UDP to the host is blocked: the host's party port gives no STUN answer, so this game has no "
                "public UDP address; no bell until it answers (retrying every 40 s)");
            party::status::SetLastEvent("UDP to the host is blocked: no answer from the host's party port; no summon "
                                        "until it answers (retrying)");
        } else {
            Log("summon: UDP to the host works again (STUN answered): ringing resumes");
            party::status::SetLastEvent("UDP to the host works again: joining");
            st.rung_ever = false;
        }
    }
}

/// The roster without the members nobody can summon (State::unreachable).
std::vector<party::RosterEntry> SummonableRoster(const State& st, const std::vector<party::RosterEntry>& roster) {
    std::vector<party::RosterEntry> out;
    for (const party::RosterEntry& e : roster) {
        if (e.slot == party::kHostSlot ||
            std::find(st.unreachable.begin(), st.unreachable.end(), e.name) == st.unreachable.end()) {
            out.push_back(e);
        }
    }
    return out;
}

void RunTest(State& st, const GameSnapshot& s, Clock::time_point now) {
    Test& t = st.test;
    // Test aid (batch 6, BB_PARTY_TEST_DIE_ON_DOWN=S): the host dies S s after a guest's down notice
    // (a wipe within the death grace, as in playtest 1 bug 18), without a ForceDead on an enemy
    // (host_kill raised the guest's boss-clear state in batch 1).
    {
        static const char* e = std::getenv("BB_PARTY_TEST_DIE_ON_DOWN");
        static const double delay = e ? std::atof(e) : -1;
        static double handled = 0;
        const double down = b2::LastMemberDownAt();
        if (delay >= 0 && down > handled && PartyDirector::Get().Role() == PartyRole::Host && b2::NowSeconds() - down >= delay &&
            s.world_up && !s.loading) {
            handled = down;
            Log("test: die on down: the host dies %.1f s after a member's down notice", b2::NowSeconds() - down);
            DieForTest();
        }
    }
    if (t.a11_lamp_done && st.world_at != t.a11_lamp_world_at) {
        t.a11_reloaded = true;  // the lamp warp's load: a11_move after the next steady world
    }
    if (!st.world_seen) {
        return;
    }
    if (t.a11_lamp && !t.a11_lamp_done && s.world_up && !s.loading && Seconds(st.world_at, now) >= 2.0) {
        t.a11_lamp_done = true;
        t.a11_lamp_world_at = st.world_at;
        Log("test: a11_lamp %u", t.a11_lamp);
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint32_t)>(Guest(kLampWarp))(t.a11_lamp);
        return;
    }
    if (t.a11_move && !t.a11_moved && (!t.a11_lamp || (t.a11_lamp_done && t.a11_reloaded)) && s.world_up &&
        !s.loading && Seconds(st.world_at, now) >= t.a11_move_at) {
        t.a11_moved = true;
        const std::uint64_t player = LocalPlayer();
        std::uint64_t vt = 0, fn = 0;
        alignas(16) float pos[4] = {t.a11_pos[0], t.a11_pos[1], t.a11_pos[2], 1.0f}, rot[4] = {0, 0, 0, 0};
        if (player && SafeGet(player, &vt) && vt && SafeGet(vt + 0x5f8, &fn) && fn) {
            reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint32_t)>(fn)(
                player, pos, rot, 1);
            // Invincible for the fight (EnableInvincible, chr +0x1E1 bit 14): the fight must outlast
            // the joiner's arrival in its own world.
            std::uint32_t f = 0;
            if (!t.a11_mortal && SafeRead(player + 0x1e1, &f, 4)) {
                f |= 1u << 14;
                SafeWrite(player + 0x1e1, &f, 4);
            }
            Log("test: a11_move to (%.2f, %.2f, %.2f), invincible", double(pos[0]), double(pos[1]), double(pos[2]));
        }
    }
    if (t.a11_kill && !t.a11_killed) {
        if (BossBarShown() && !t.a11_bar_seen) {
            t.a11_bar_seen = true;
            t.a11_bar_at = now;
            Log("test: a11: boss bar up; the kill in %.0f s", t.a11_kill_after);
        }
        if (t.a11_bar_seen && Seconds(t.a11_bar_at, now) >= t.a11_kill_after) {
            t.a11_killed = true;
            Log("test: a11_kill: entity %u", t.a11_entity);
            if (const std::uint64_t pl = LocalPlayer()) {
                std::uint32_t f = 0;
                if (SafeRead(pl + 0x1e1, &f, 4)) {
                    f &= ~(1u << 14);
                    SafeWrite(pl + 0x1e1, &f, 4);
                }
            }
            reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kForceDead))(0, t.a11_entity);
        }
    }
    // bell_host / bell_guest: the bell through the item path (SpEffect -> SendSign -> sign
    // create / search on the FROM server) once this player is ready, bells granted, 5 s idle.
    // bell_host@AA: the host's own map in area AA; bell_guest@AA: the host's (roster) map.
    std::uint32_t bell_map = s.map_id;
    if (t.bell_area >= 0 && t.bell && std::strcmp(t.bell, kGuestBell) == 0 && st.link) {
        bell_map = 0xffffffffu;
        for (const party::RosterEntry& e : st.link->roster()) {
            if (e.slot == party::kHostSlot) bell_map = e.map_id;
        }
    }
    if (t.bell && !t.bell_done && st.start_ready && st.granted && s.world_up && !s.loading &&
        (t.bell_area < 0 || (bell_map != 0xffffffffu && bell_map != 0 && int(bell_map >> 24) == t.bell_area)) &&
        s.session_role == RoleIdle && Seconds(st.world_at, now) >= t.delay) {
        t.bell_done = true;
        Log("test: %s once (ready: %s)", t.bell, DescribeStart(st.cs, true).c_str());
        Ring(st, t.bell, "test bell", now);
    }
    if (t.insight >= 0 && !t.insight_done && Seconds(st.world_at, now) >= 2.0 && !s.loading) {
        t.insight_done = true;
        const int before = s.insight;
        const bool ok = WritePlayerInsight(t.insight);
        Log("test: Insight %d -> %d %s", before, t.insight, ok ? "written" : "NOT written (no player record)");
    }
    if (t.level > 0 && !t.level_done && Seconds(st.world_at, now) >= 2.0 && !s.loading) {
        t.level_done = true;
        const int before = s.level;
        const bool ok = WritePlayerLevel(t.level);
        Log("test: level %d -> %d %s", before, t.level, ok ? "written" : "NOT written (no player record)");
    }
    // god: this player's chr stays invincible (chr +0x1E1 bit 14, as a11_move), e.g. a scripted
    // event walk through an area full of enemies (B6 live triggers).
    if (t.god && s.world_up && !s.loading && s.player) {
        std::uint32_t f = 0;
        if (SafeRead(s.player + 0x1e1, &f, 4) && !(f & (1u << 14))) {
            f |= 1u << 14;
            SafeWrite(s.player + 0x1e1, &f, 4);
            Log("test: god: invincible (chr +0x1E1 bit 14)");
        }
    }
    if (t.log_pos && s.world_up && !s.loading && Seconds(t.pos_at, now) >= t.log_pos_every) {
        // Every second: the physics position (as DescribeHere; the map man's +0xAAC is the last
        // grounded / area position and goes stale after a re-created PlayerIns) and the area id.
        t.pos_at = now;
        std::uint64_t man = 0;
        std::uint32_t area = 0;
        const std::uint64_t at = Guest(0x553b120);
        if (at && SafeGet(at, &man) && man) {
            SafeGet(man + 0xa7c, &area);
        }
        float p[3] = {};
        std::uint64_t a = 0;
        if (s.player && SafeGet(s.player + 0x58, &a) && a && SafeGet(a + 8, &a) && a && SafeGet(a + 0x3b0, &a) && a &&
            SafeGet(a + 0x68, &a) && a) {
            SafeRead(a + 0x1e0, p, 12);
        }
        // fallsafe: the game's last safe ground spot (PlayerIns +0x400 -> +0x30, the bloodstain's spot)
        float safe[3] = {};
        std::uint64_t tr = 0;
        if (s.player && SafeGet(s.player + 0x400, &tr) && tr) {
            SafeRead(tr + 0x30, safe, 12);
        }
        // liftfall: the chr's +0x1E1 flags (+0x1E5 bit 0x02: on a moving object, deathrules::RidingObject)
        std::uint64_t f = 0;
        if (s.player) SafeRead(s.player + 0x1e1, &f, 5);
        Log("test: pos area 0x%08x (%.2f, %.2f, %.2f) safe (%.2f, %.2f, %.2f) f %010llx", area, p[0], p[1], p[2], safe[0],
            safe[1], safe[2], static_cast<unsigned long long>(f));
    }
    if (!t.watch_texts.empty() && Seconds(t.watch_at, now) >= 0.25) {
        t.watch_at = now;
        struct Ctx {
            Test* t;
            std::int64_t newest;
        } ctx{&t, t.watch_since};
        BbGameMenu::RecentTexts(
            t.watch_since,
            [](const char16_t* text, std::int64_t ms, void* user) {
                Ctx& c = *static_cast<Ctx*>(user);
                std::string u8;
                for (const char16_t* q = text; *q && u8.size() < 160; ++q) {
                    const char32_t ch = *q;
                    if (ch < 0x80) u8 += char(ch);
                    else if (ch < 0x800) { u8 += char(0xc0 | (ch >> 6)); u8 += char(0x80 | (ch & 0x3f)); }
                    else { u8 += char(0xe0 | (ch >> 12)); u8 += char(0x80 | ((ch >> 6) & 0x3f)); u8 += char(0x80 | (ch & 0x3f)); }
                }
                std::string low = u8;
                for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                bool hit = false;
                for (const std::string& n : c.t->watch_texts) {
                    std::string ln = n;
                    for (char& ch : ln) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                    if (!ln.empty() && low.find(ln) != std::string::npos) hit = true;
                }
                if (ms > c.newest) c.newest = ms;
                if (!hit) return;
                auto it = c.t->watch_last.find(u8);
                if (it == c.t->watch_last.end() || ms - it->second > 2000) {
                    Log("test: text shows: \"%s\"", u8.c_str());
                }
                c.t->watch_last[u8] = ms;
            },
            &ctx);
        t.watch_since = ctx.newest;
    }
    if (!t.log_objs.empty() && s.world_up && !s.loading && Seconds(t.log_obj_at, now) >= 0.5) {
        t.log_obj_at = now;
        for (const std::int32_t e : t.log_objs) {
            float p[3] = {};
            if (!worldact::ObjectPoint(e, 0.0f, 0.0f, p)) continue;
            auto it = t.log_obj_pos.find(e);
            const bool moved = it == t.log_obj_pos.end() ||
                               std::fabs(it->second[0] - p[0]) + std::fabs(it->second[1] - p[1]) +
                                       std::fabs(it->second[2] - p[2]) > 0.05f;
            if (moved) {
                Log("test: object %d at (%.2f, %.2f, %.2f)%s", e, p[0], p[1], p[2],
                    it == t.log_obj_pos.end() ? " (first seen)" : "");
                t.log_obj_pos[e] = {p[0], p[1], p[2]};
            }
        }
    }
    if (!t.log_flags.empty() && s.world_up && !s.loading) {
        std::string line;
        bool changed = false;
        for (const std::uint32_t id : t.log_flags) {
            bool v = false;
            const int now_v = ReadEventFlag(id, &v) ? (v ? 1 : 0) : -1;
            auto it = t.log_flag_val.find(id);
            if (it == t.log_flag_val.end() || it->second != now_v) {
                changed = true;
                t.log_flag_val[id] = now_v;
            }
            char buf[32];
            std::snprintf(buf, sizeof buf, " %u=%d", id, now_v);
            line += buf;
        }
        if (changed) Log("test: flags%s", line.c_str());
    }
    if (!t.log_objanims.empty() && s.world_up && !s.loading) {
        for (const std::int32_t e : t.log_objanims) {
            worldact::ObjAnimState a;
            char buf[160];
            if (worldact::ReadObjAnim(e, &a)) {
                std::snprintf(buf, sizeof buf, "anim %d playing %d state %d flags %#x (handle %#x)", a.anim,
                              a.playing ? 1 : 0, a.state, a.flags, a.handle);
            } else {
                std::snprintf(buf, sizeof buf, "not loaded");
            }
            std::string& last = t.log_objanim_last[e];
            if (last != buf) {
                last = buf;
                // liftfix: where the local chr stands at each change (a lift car's ride and end against
                // its rider: in the car, out of it, falling) and its +0x1E1 flags.
                float p[3] = {};
                std::uint64_t a = 0, f = 0;
                if (s.player && SafeGet(s.player + 0x58, &a) && a && SafeGet(a + 8, &a) && a && SafeGet(a + 0x3b0, &a) &&
                    a && SafeGet(a + 0x68, &a) && a) {
                    SafeRead(a + 0x1e0, p, 12);
                }
                if (s.player) SafeRead(s.player + 0x1e1, &f, 5);
                Log("test: object %d %s; chr at (%.2f, %.2f, %.2f) f %010llx", e, buf, p[0], p[1], p[2],
                    static_cast<unsigned long long>(f));
            }
        }
    }
    if (t.echoes >= 0 && Seconds(st.world_at, now) >= 2.0 && !s.loading) {
        const bool ok = WritePlayerEchoes(t.echoes);
        Log("test: Blood Echoes -> %d %s", t.echoes, ok ? "written" : "NOT written (no player record)");
        t.echoes = -1;
    }
    if (t.armor && Seconds(st.world_at, now) >= 3.0 && s.world_up && !s.loading) {
        int ok = 0, all = 0;
        for (int k = 0; k < 4; ++k) {
            const std::uint32_t piece = t.armor + 1000u * static_cast<std::uint32_t>(k);
            if (!GameParamRow("EquipParamProtector", "EQUIP_PARAM_PROTECTOR_ST", piece)) continue;
            ++all;
            ok += EquipSlotItem(8 + k, kItemProtector, piece) ? 1 : 0;  // head .. legs (slots 8..11)
        }
        Log("test: armor set %u put on: %d of %d pieces", t.armor, ok, all);
        t.armor = 0;
        if (ok > 0) b2::NoteOwnAppearanceChanged();
    }
    if (!t.goods.empty() && !t.goods_done && Seconds(st.world_at, now) >= 8.0 && s.world_up && !s.loading) {
        t.goods_done = true;
        for (const auto& [id, n] : t.goods) {
            const int before = GoodsCount(std::uint32_t(id));
            // goods=ID:N tops up to N; goods=ID:0 (or any N below the count) takes the rest away
            // (a key item a fixture's player must not hold yet, dlast D4).
            const bool ok = before >= 0 && before != n ? GiveGoods(std::uint32_t(id), n - before) : before == n;
            Log("test: goods %d x%d -> x%d (%s)", id, before, GoodsCount(std::uint32_t(id)), ok ? "ok" : "failed");
        }
    }
    // With place= the stand waits for the placed map (the Ritual Altar is in m21_01, not the Dream
    // block m21_00 a save starts in: a stand there kept the player where it was).
    const bool stand_map_ok =
        !t.place || (t.place_done && (s.map_id & 0xffff0000u) == (t.place_map & 0xffff0000u));
    if (t.stand && !t.stand_done && stand_map_ok && s.world_up && !s.loading && s.player &&
        Seconds(st.world_at, now) >= 10.0 && Seconds(t.st_at, now) >= 0.0) {
        if (t.st_phase == 0 && t.st_obj) {
            std::string what;
            const bool ok = worldact::StandAtObject(t.st_obj, t.st_obj_dist, t.st_obj_angle, &what);
            Log("test: stand at object %d (%.1f m, %.0f deg): %s", t.st_obj, double(t.st_obj_dist),
                double(t.st_obj_angle), ok ? what.c_str() : "not loaded");
            if (ok) {
                worldact::ObjectPoint(t.st_obj, t.st_obj_dist, t.st_obj_angle, t.st_pos);  // phase 1 checks it
                t.st_phase = 1;
                t.st_at = now + std::chrono::milliseconds(1500);
            } else if (++t.st_retries < 10) {
                t.st_at = now + std::chrono::milliseconds(2000);
            } else {
                t.stand_done = true;
            }
        } else if (t.st_phase == 0) {
            alignas(16) float pos[4] = {t.st_pos[0], t.st_pos[1], t.st_pos[2], 1.0f};
            alignas(16) float rot[4] = {0, t.st_yaw, 0, 0};
            std::uint64_t vtbl = 0, fn = 0;
            if (SafeGet(s.player, &vtbl) && SafeGet(vtbl + 0x5f8, &fn) && fn) {
                using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
                reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
                float at[3] = {};
                SafeRead(s.player + 0xc0, at, sizeof at);
                Log("test: stand at (%.2f, %.2f, %.2f) yaw %.3f, in place (now at %.2f, %.2f, %.2f)", pos[0], pos[1],
                    pos[2], rot[1], at[0], at[1], at[2]);
            } else {
                Log("test: stand: no PlayerIns warp");
            }
            t.st_phase = 1;
            t.st_at = now + std::chrono::milliseconds(1500);
        } else if (t.st_phase == 1) {
            float at[3] = {}, r[4] = {};
            SafeRead(s.player + 0xc0, at, sizeof at);
            SafeRead(s.player + 0xd0, r, sizeof r);
            const float dx = at[0] - t.st_pos[0], dz = at[2] - t.st_pos[2];
            if (dx * dx + dz * dz > 2.25f && t.st_retries < 10) {
                // Not there (a place= reload that never came, a warp the game dropped): warp again.
                ++t.st_retries;
                Log("test: stand: still at (%.2f, %.2f, %.2f); warping again (%d)", at[0], at[1], at[2], t.st_retries);
                t.st_phase = 0;
                t.st_at = now + std::chrono::milliseconds(2000);
                return;
            }
            if (t.st_button) {
                runtime_pad_director_press(t.st_button);
            }
            Log("test: stand: at (%.2f, %.2f, %.2f) yaw %.3f; pressed 0x%x", at[0], at[1], at[2], r[1], t.st_button);
            t.st_phase = 2;
            t.st_at = now + std::chrono::milliseconds(250);
        } else if (t.st_phase == 2) {
            runtime_pad_director_press(0);
            t.st_phase = 3;
            t.st_at = now + std::chrono::milliseconds(600);
        } else if (t.st_phase == 3) {
            // The menu walk, event driven (MenuScript: '+' waits for the next screen's texts).
            std::uint64_t seq = 0;
            std::int64_t last_ms = 0;
            BbGameMenu::TextActivity(&seq, &last_ms);
            const std::int64_t now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            std::string line;
            const double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
            const std::uint32_t held = t.st_seq.Step(
                secs, seq, seq ? double(now_ms - last_ms) / 1000.0 : 1e9, &line, [](const std::string& text) {
                    const std::int64_t ms = BbGameMenu::TextSeenMs(text.c_str());
                    return ms > 0 ? double(ms) / 1000.0 : -1.0;
                });
            // Only on a change: a wait step must not hold the pad at 0 every frame (live g1g: the
            // altar autopilot's Cross on "Search using Chalice Glyph" was released at once).
            static std::uint32_t seq_held = 0;
            if (held || seq_held) {
                runtime_pad_director_press(held);
            }
            seq_held = held;
            // The raw pad state the game read while a press is held (once per press).
            static std::uint32_t logged_held = 0;
            if (held && held != logged_held) {
                const std::uint32_t read = runtime_pad_last_buttons();
                if (read & held) {
                    Log("test: seq: the game read pad buttons 0x%x (held 0x%x)", read, held);
                    logged_held = held;
                }
            } else if (!held) {
                logged_held = 0;
            }
            if (!line.empty()) {
                Log("test: seq %s (texts seq %llu)", line.c_str(), static_cast<unsigned long long>(seq));
            }
            // Every second: the texts on screen now (first 60 ASCII chars each), to learn the real ones.
            static std::int64_t texts_logged = 0;
            if (now_ms - texts_logged >= 1000) {
                std::string all;
                BbGameMenu::RecentTexts(now_ms - 600, [](const char16_t* text, std::int64_t, void* user) {
                    std::string& out = *static_cast<std::string*>(user);
                    std::string a;
                    for (const char16_t* c = text; *c && a.size() < 60; ++c) a += *c < 128 && *c >= 32 ? char(*c) : '.';
                    if (out.find(a) == std::string::npos && out.size() < 1500) out += " | " + a;
                }, &all);
                if (all != t.st_last_texts) {
                    t.st_last_texts = all;
                    Log("test: texts now:%s", all.c_str());
                }
                texts_logged = now_ms;
            }
            if (t.st_seq.finished()) {
                runtime_pad_director_press(0);
                t.stand_done = true;
                Log("test: seq %s", t.st_seq.stuck() ? "STUCK" : "done");
            }
        }
    }
    if (t.approach && !t.approach_done && s.world_up && !s.loading && Seconds(st.world_at, now) >= 12.0 &&
        (!t.place || t.place_done) && Seconds(t.ap_at, now) >= 0.0) {
        std::uint32_t area = 0;
        float pos[3] = {0, 0, 0};
        if (ReadPlayerPos(&area, pos)) {
            const double dx = t.ap_x - pos[0], dz = t.ap_z - pos[2];
            const double dist = std::sqrt(dx * dx + dz * dz);
            const double want = std::atan2(dx, dz);
            auto wrap = [](double a) { while (a > M_PI) a -= 2 * M_PI; while (a < -M_PI) a += 2 * M_PI; return a; };
            const bool on_route = !t.route.empty();
            if (on_route && s.player) {
                // The stage record the K3 hook reacts to on a host: log every block change of the walk.
                std::uint32_t blk = 0;
                if (SafeRead(s.player + 0x3f8, &blk, sizeof blk) && blk != t.route_block) {
                    Log("test: route: stage record %#x -> %#x at (%.1f, %.1f, %.1f), waypoint %zu/%zu, map 0x%08x",
                        t.route_block, blk, double(pos[0]), double(pos[1]), double(pos[2]), t.route_i + 1, t.route.size(),
                        area);
                    t.route_block = blk;
                }
            }
            if (t.ap_phase == 1) { // a step ends: release, let it settle
                WritePadFile("");
                t.ap_phase = 2;
                t.ap_at = now + std::chrono::milliseconds(on_route ? 150 : 300);
            } else if (t.ap_phase == 0 || t.ap_phase == 2) {
                if (t.ap_phase == 2) {
                    const double mx = pos[0] - t.ap_from[0], mz = pos[2] - t.ap_from[1];
                    const bool moved = std::sqrt(mx * mx + mz * mz) > 0.05;
                    t.ap_still = moved ? 0 : t.ap_still + 1;
                    if (moved && t.ap_slide == 0) {
                        // Camera-relative stick: correct the stick angle by how far the walk missed.
                        t.ap_angle = wrap(t.ap_angle + wrap(want - std::atan2(mx, mz)));
                    }
                }
                if (on_route && dist <= t.ap_r && t.route_i + 1 < t.route.size()) {
                    // A waypoint reached: on to the next one, no stop.
                    ++t.route_i;
                    t.ap_x = t.route[t.route_i][0];
                    t.ap_z = t.route[t.route_i][2];
                    t.ap_r = kRouteReach;
                    t.ap_steps = 0;
                    t.ap_still = 0;
                    t.ap_slide = 0;
                    t.ap_slides_here = 0;
                    t.ap_phase = 0;
                    t.ap_at = now;
                    if (t.route_i % 5 == 0) {
                        Log("test: route: waypoint %zu/%zu, at (%.1f, %.1f, %.1f), %.0f s, %d side step(s), %d hop(s)",
                            t.route_i, t.route.size(), double(pos[0]), double(pos[1]), double(pos[2]),
                            Seconds(t.route_started, now), t.route_slides, t.route_hops);
                    }
                } else {
                const std::array<float, 3>& wp = on_route ? t.route[t.route_i] : std::array<float, 3>{};
                if (on_route && t.ap_still >= 3 && t.ap_slides_here >= kRouteSlidesBeforeHop && !std::isnan(wp[1]) &&
                    dist <= kRouteHopMax && t.ap_slide == 0) {
                    // Still blocked after the side steps (an enemy's body, a cart, a railing the
                    // navmesh walks around): one short chr warp onto this waypoint (a navmesh face
                    // of the route), counted and logged; then the stick walk goes on.
                    const float to[3] = {wp[0], wp[1] + 0.3f, wp[2]};
                    const bool ok = PutChr(s, to, static_cast<float>(want));
                    ++t.route_hops;
                    Log("test: route: HOP %d (%.1f m) to waypoint %zu/%zu (%.1f, %.1f, %.1f)%s", t.route_hops, dist,
                        t.route_i + 1, t.route.size(), double(to[0]), double(to[1]), double(to[2]), ok ? "" : " FAILED");
                    t.ap_still = 0;
                    t.ap_slides_here = 0;
                    t.ap_at = now + std::chrono::milliseconds(400);
                    t.ap_phase = 0;
                } else {
                if (on_route && t.ap_still >= 3 && t.ap_slide == 0 && t.ap_slides_here < 6 && dist > t.ap_r) {
                    // Against a wall: side steps (alternating sides) to slide along it, then on.
                    t.ap_slide = (t.ap_slides_here % 2 == 0) ? 3 : -3;
                    ++t.ap_slides_here;
                    ++t.route_slides;
                    t.ap_still = 0;
                }
                Log("test: approach: (%.2f, %.2f) -> (%.2f, %.2f): %.2f m, stick %.0f deg (step %d)", pos[0], pos[2],
                    t.ap_x, t.ap_z, dist, t.ap_angle * 180 / M_PI, t.ap_steps);
                const bool blocked = on_route ? (t.ap_still >= 3 && t.ap_slides_here >= 6) : t.ap_still >= 20;
                if (dist <= t.ap_r || t.ap_steps >= t.ap_max || blocked) {
                    t.ap_phase = 3;
                    t.ap_at = now + std::chrono::milliseconds(1200);
                    Log("test: approach: %s at %.2f m", dist <= t.ap_r ? "arrived" : blocked ? "BLOCKED (no way past)" : "GAVE UP", dist);
                    if (on_route) {
                        std::uint32_t f = 0;
                        if (t.route_god && s.player && SafeRead(s.player + 0x1e1, &f, 4)) {
                            f &= ~(1u << 14);
                            SafeWrite(s.player + 0x1e1, &f, 4);
                        }
                        t.route_god = false;
                        Log("test: route: %s at waypoint %zu/%zu (%.1f, %.1f, %.1f) after %.0f s, %d side step(s), %d hop(s)",
                            dist <= t.ap_r ? "DONE" : "STOPPED", t.route_i + 1, t.route.size(), double(pos[0]),
                            double(pos[1]), double(pos[2]), Seconds(t.route_started, now), t.route_slides, t.route_hops);
                    }
                } else {
                    ++t.ap_steps;
                    t.ap_from[0] = pos[0];
                    t.ap_from[1] = pos[2];
                    double angle = t.ap_angle;
                    if (t.ap_slide != 0) {
                        angle += (t.ap_slide > 0 ? 1 : -1) * M_PI / 2;
                        t.ap_slide += t.ap_slide > 0 ? -1 : 1;
                    }
                    const int lx = 128 + static_cast<int>(std::lround(127 * std::sin(angle)));
                    const int ly = 128 - static_cast<int>(std::lround(127 * std::cos(angle)));
                    char tok[48];
                    std::snprintf(tok, sizeof tok, "lx=%d ly=%d", std::clamp(lx, 0, 255), std::clamp(ly, 0, 255));
                    WritePadFile(tok);
                    t.ap_phase = 1;
                    const double secs = on_route ? std::clamp(dist / 4.0, 0.12, 0.7) : std::clamp(dist / 3.0, 0.12, 0.45);
                    t.ap_at = now + std::chrono::milliseconds(static_cast<int>(secs * 1000));
                }
                }
                }
            } else if (t.ap_phase == 3 && !t.ap_button && t.st_seq.size() && !t.stand && !t.ap_faced &&
                       [&] {
                           if (t.ap_prompt.empty()) {
                               return t.ap_taps == 0;
                           }
                           const std::int64_t seen = BbGameMenu::TextSeenMs(t.ap_prompt.c_str());
                           if (t.ap_taps > 0 && seen > t.ap_tap_ms) {
                               Log("test: approach: '%s' shows after %d tap(s)", t.ap_prompt.c_str(), t.ap_taps);
                               t.ap_prompt_seen = true;
                               return false;
                           }
                           return t.ap_taps < 8;
                       }()) {
                // A menu walk follows (an action prompt needs the player to face it; live g1f: the
                // walk stopped facing away, and a PlayerIns warp does not turn the player in the
                // Dream): a short stick tap toward the target turns it there (~0.1 m); with
                // prompt=TEXT more taps, 45 deg apart, until that prompt shows (live g1k: the first
                // tap did not face the altar).
                const int tap = t.ap_taps++;
                t.ap_tap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                const double mx = pos[0] - t.ap_from[0], mz = pos[2] - t.ap_from[1];
                if (std::sqrt(mx * mx + mz * mz) > 0.05) {
                    t.ap_angle = wrap(t.ap_angle + wrap(want - std::atan2(mx, mz)));
                }
                // The stick angle is camera-relative: ap_angle walks toward `want`; the face point
                // is `want` turned by the difference of the two world directions.
                double stick = t.ap_angle + tap * (M_PI / 4);
                if (t.ap_face) {
                    const double fwant = std::atan2(double(t.ap_fx) - pos[0], double(t.ap_fz) - pos[2]);
                    stick = wrap(t.ap_angle + wrap(fwant - want) + tap * (M_PI / 4));
                }
                const int lx = 128 + static_cast<int>(std::lround(127 * std::sin(stick)));
                const int ly = 128 - static_cast<int>(std::lround(127 * std::cos(stick)));
                char tok[48];
                std::snprintf(tok, sizeof tok, "lx=%d ly=%d", std::clamp(lx, 0, 255), std::clamp(ly, 0, 255));
                WritePadFile(tok);
                t.ap_phase = 6;
                t.ap_at = now + std::chrono::milliseconds(120);
                Log("test: approach: a short step to face %s (stick %.0f deg)", t.ap_face ? "the face= point" : "the target",
                    stick * 180 / M_PI);
            } else if (t.ap_phase == 6) {
                WritePadFile("");
                t.ap_phase = 3;
                t.ap_at = now + std::chrono::milliseconds(600);
            } else if (t.ap_phase == 3) { // arrived: the button
                if (t.ap_button) {
                    runtime_pad_director_press(t.ap_button);
                    Log("test: approach: pressed 0x%x (map 0x%08x)", t.ap_button, area);
                    t.ap_phase = 4;
                    t.ap_at = now + std::chrono::milliseconds(250);
                } else {
                    t.approach_done = true;
                }
            } else if (t.ap_phase == 4) {
                runtime_pad_director_press(0);
                t.approach_done = true;
            }
        }
    }
    // approach=... with seq=... and no stand=: the menu walk starts once the walk arrived (a spot a
    // warp cannot put the player on, e.g. the Dream's ritual gravestones).
    if (t.approach_done && !t.stand && t.st_seq.size() && !t.st_seq_on) {
        t.st_seq_on = true;
        t.stand = true;
        t.st_phase = 3;
        // Face the target where the walk stopped (an action prompt needs the player to face it;
        // live g1c: arrived at 1.25 m facing past the altar, no prompt). The PlayerIns warp at the
        // own spot only turns the player.
        float at[3] = {};
        std::uint64_t vtbl = 0, fn = 0;
        if (t.ap_prompt_seen) {
            // parfin3 g1a / g1b: the turn warp below moved the facing off the prompt the taps had
            // found (the prompt went 1-2 s later and the menu never opened). Keep it.
            Log("test: approach done: '%s' shows; facing kept; the menu walk (seq) starts", t.ap_prompt.c_str());
        } else if (s.player && SafeRead(s.player + 0xc0, at, sizeof at) && SafeGet(s.player, &vtbl) &&
            SafeGet(vtbl + 0x5f8, &fn) && fn) {
            std::uint32_t area = 0;
            float pos[3] = {};
            if (ReadPlayerPos(&area, pos)) {
                at[0] = pos[0], at[1] = pos[1], at[2] = pos[2];
            }
            alignas(16) float p4[4] = {at[0], at[1], at[2], 1.0f};
            alignas(16) float rot[4] = {0, static_cast<float>(std::atan2(-(t.ap_x - at[0]), -(t.ap_z - at[2]))), 0, 0};
            using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
            reinterpret_cast<WarpFn>(fn)(s.player, p4, rot, 0);
            Log("test: approach done: turned to face (%.2f, %.2f), yaw %.2f; the menu walk (seq) starts", t.ap_x,
                t.ap_z, rot[1]);
        } else {
            Log("test: approach done: the menu walk (seq) starts");
        }
    }
    if (t.place && !t.place_done && Seconds(st.world_at, now) >= t.place_at && s.world_up && !s.loading) {
        t.place_done = true;
        const bool ok = t.warp_point ? TravelWarpForTest(t.place_map, t.warp_point)
                                     : TravelPlaceForTest(t.place_map, t.place_pos, t.place_yaw);
        Log("test: place: %s", ok ? "requested" : "refused (unknown map)");
    }
    // Host of a party: award_lot / set_flag wait until a cooperator has been in the world for the
    // test delay (the live-sync checks need a summoned guest); without a party, 12 s / delay.
    bool party_host;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        party_host = st.role == PartyRole::Host;
    }
    if (party_host && s.cooperators > 0 && !t.coop_seen) {
        t.coop_seen = true;
        t.coop_at = now;
        Log("test: a cooperator is in the world; party test actions in %.0f s", t.delay);
    }
    const bool party_ready = !party_host || (t.coop_seen && Seconds(t.coop_at, now) >= t.delay);
    if (!t.set_flags.empty() && party_ready && Seconds(st.world_at, now) >= t.delay && s.world_up && !s.loading) {
        for (const std::uint32_t id : t.set_flags) {
            bool before = false, after = false;
            ReadEventFlag(id, &before);
            const bool ok = WriteEventFlag(id, true);
            ReadEventFlag(id, &after);
            Log("test: set_flag %u: %d -> %d (%s)", id, int(before), int(after), ok ? "written" : "NOT written");
        }
        t.set_flags.clear();
    }
    if (t.award_lot > 0 && party_ready && Seconds(st.world_at, now) >= 12.0 && s.world_up && !s.loading) {
        const int before = GoodsCount(kGoodsBeckoning);
        const bool ok = AwardItemLot(std::uint32_t(t.award_lot));
        Log("test: award_lot %d: %s; goods 200 x%d -> x%d", t.award_lot, ok ? "awarded" : "NOT awarded", before,
            GoodsCount(kGoodsBeckoning));
        t.award_lot = -1;
    }
    // PR1 probe: a character without the bells (removes goods 200 / 205 through GiveItemDirect
    // with a negative count, 10 s after the world is up). Test saves only.
    if (t.drop_bells && !t.drop_done && Seconds(st.world_at, now) >= 10.0 && s.world_up && !s.loading) {
        t.drop_done = true;
        const int b = GoodsCount(kGoodsBeckoning), r = GoodsCount(kGoodsSmallResonant);
        const bool ok_b = b > 0 ? GiveGoods(kGoodsBeckoning, -b) : true;
        const bool ok_r = r > 0 ? GiveGoods(kGoodsSmallResonant, -r) : true;
        Log("test: drop_bells: goods 200 x%d -> x%d (%s), 205 x%d -> x%d (%s)", b, GoodsCount(kGoodsBeckoning),
            ok_b ? "ok" : "failed", r, GoodsCount(kGoodsSmallResonant), ok_r ? "ok" : "failed");
    }
    if (t.ring && !t.ring_done && Seconds(st.world_at, now) >= t.delay && s.world_up && !s.loading) {
        t.ring_done = true;
        t.rung_at = now;
        Log("test: before %s: %s", t.ring, Describe(s).c_str());
        Log("test: before %s: start %s", t.ring, DescribeStart(ReadStartFlags(), true).c_str());
        std::int64_t r = 0;
        const bool ok = LuaEventRaiseNow(t.ring, &r);
        Log("test: %s raised: %s, dispatcher returned %lld (1: a handler ran)", t.ring, ok ? "yes" : "no (no event manager)",
            static_cast<long long>(r));
        // A name no handler table has, for comparison (the dispatcher answers 0).
        std::int64_t none = -1;
        LuaEventRaiseNow("OnEvent_BbportNoSuchEvent", &none);
        Log("test: control event OnEvent_BbportNoSuchEvent: dispatcher returned %lld", static_cast<long long>(none));
        const GameSnapshot after = ReadGameState(true);
        Log("test: right after %s: %s", t.ring, Describe(after).c_str());
    }
}

} // namespace

PartyDirector& PartyDirector::Get() {
    static PartyDirector d;
    return d;
}

void PartyDirector::Configure(PartyRole role, int max_players) {
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    st.role = role;
    st.max_players = max_players < 2 ? 2 : max_players > party::kMaxPartyPlayers ? party::kMaxPartyPlayers : max_players;
}

void PartyDirector::ConfigureFromEnv() {
    State& st = S();
    PartyRole role = PartyRole::None;
    if (const char* p = bb_party_value()) {
        std::string lower(p);
        for (char& c : lower) {
            c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        role = lower == "host" ? PartyRole::Host : PartyRole::Guest;
        if (const char* h = std::getenv("BB_PARTY_HOST"); h && h[0]) {
            role = h[0] == '1' ? PartyRole::Host : PartyRole::Guest;
        }
    }
    int max_players = 3;
    if (const char* m = std::getenv("BB_PARTY_MAX"); m && m[0]) {
        max_players = std::atoi(m);
    }
    Configure(role, max_players);
    progress::SetRole(role == PartyRole::Host    ? progress::Role::Host  // C2 progress sync
                      : role == PartyRole::Guest ? progress::Role::Guest
                                                 : progress::Role::None);
    std::lock_guard<std::mutex> lk(st.mu);
    if (const char* e = std::getenv("BB_PARTY_RING_EVERY"); e && e[0] && std::atof(e) >= 1.0) {
        st.ring_every = std::atof(e);
    }
    if (!bb_party_switch_on("BB_PARTY_FAST_JOIN")) {
        st.fast_join = false;
        st.ring_delay = 5.0;
        st.continue_settle = 4.0;
        st.continue_every = 2.5;
    }
    // Tests: hold Continue this long on the title (a host that comes up late).
    if (const char* d = std::getenv("BB_PARTY_CONTINUE_SETTLE"); d && d[0] && std::atof(d) >= 0.0) {
        st.continue_settle = std::atof(d);
    }
    if (const char* d = std::getenv("BB_PARTY_RING_DELAY"); d && d[0] && std::atof(d) >= 0.0) {
        st.ring_delay = std::atof(d);
    }
    if (const char* a = std::getenv("BB_PARTY_AUTO"); a && a[0] == '0') {
        st.auto_ring = false;
    }
    if (const char* c = std::getenv("BB_PARTY_AUTOCONTINUE"); c && c[0]) {
        st.auto_continue = c[0] == '0' ? 0 : 1;
    }
    st.auto_continue = ShouldAutoContinue(st.auto_continue,
        bb_party_value_on(std::getenv("BB_PARTY_RESTARTED")));
    // A crash-restarted game (run.bat's BB_PARTY_RESTARTED=1) takes BB_PARTY_DIRECTOR_TEST_RESTART
    // as its test chain when that is set ("none": no steps), so a scripted run's chain is not run a
    // second time from its start after a --crash-plan kill (tools/mp/smoke_party.py).
    if (const char* rs = std::getenv("BB_PARTY_RESTARTED"); rs && rs[0] == '1') {
        if (const char* rt = std::getenv("BB_PARTY_DIRECTOR_TEST_RESTART"); rt) {
            const std::string chain = std::strcmp(rt, "none") == 0 ? std::string() : std::string(rt);
            Log("test: crash restart: the director test chain is BB_PARTY_DIRECTOR_TEST_RESTART (%s)",
                chain.empty() ? "none" : chain.c_str());
            _putenv_s("BB_PARTY_DIRECTOR_TEST", chain.c_str());
        }
    }
    if (const char* t = std::getenv("BB_PARTY_DIRECTOR_TEST"); t && t[0]) {
        st.test.on = true;
        std::string list = t;
        std::size_t at = 0;
        while (at <= list.size()) {
            std::size_t end = list.find(',', at);
            if (end == std::string::npos) {
                end = list.size();
            }
            std::string item = list.substr(at, end - at);
            // "~" at the end of a travel step: its delay counts from the end of a cutscene that plays
            // after the previous step (a boss intro), not from the previous step itself.
            const bool after_scene = item.size() > 1 && item.back() == '~';
            if (after_scene) {
                item.pop_back();
            }
            // "?FLAG=V" (V 0 / 1, default 1) at the end of a travel step: its flag condition.
            std::uint32_t wait_flag = 0;
            bool wait_value = true;
            bool wait_near = false;
            float near_pos[3] = {0, 0, 0}, near_r = 0;
            std::int32_t wait_obj = 0, wait_obj_anim = -1;
            if (item.rfind("host_", 0) == 0 || item.rfind("guest_", 0) == 0) {
                if (const std::size_t q = item.find('?'); q != std::string::npos) {
                    unsigned long f = 0;
                    int v = 1;
                    if (item.compare(q + 1, 4, "obj=") == 0) {
                        if (std::sscanf(item.c_str() + q + 5, "%d:%d", &wait_obj, &wait_obj_anim) != 2 || wait_obj <= 0) {
                            wait_obj = 0;
                        }
                    } else if (item.compare(q + 1, 5, "near=") == 0) {
                        wait_near = std::sscanf(item.c_str() + q + 6, "%f:%f:%f:%f", &near_pos[0], &near_pos[1],
                                                &near_pos[2], &near_r) == 4 && near_r > 0;
                    } else if (std::sscanf(item.c_str() + q + 1, "%lu=%d", &f, &v) >= 1 && f > 0 && f <= 0xffffffffull) {
                        wait_flag = static_cast<std::uint32_t>(f);
                        wait_value = v != 0;
                    }
                    item.erase(q);
                }
            }
            at = end + 1;
            if (item == "log_state") {
                st.test.log_state = true;
            } else if (item == "start") {
                st.test.start = true;
            } else if (item.rfind("award_lot=", 0) == 0) {
                st.test.award_lot = std::atoi(item.c_str() + 10);
            } else if (item.rfind("goods=", 0) == 0) {
                int id = 0, n = 0;
                if (std::sscanf(item.c_str() + 6, "%d:%d", &id, &n) == 2 && id > 0 && n >= 0 && n <= 99) {
                    st.test.goods.emplace_back(id, n);
                }
            } else if (item.rfind("warp=", 0) == 0) {
                unsigned map = 0, point = 0;
                if (std::sscanf(item.c_str() + 5, "%x:%u", &map, &point) == 2) {
                    st.test.place = true;
                    st.test.place_map = map;
                    st.test.warp_point = point;
                }
            } else if (item.rfind("place=", 0) == 0) {
                unsigned map = 0;
                float x = 0, y = 0, z = 0, yaw = 0;
                if (std::sscanf(item.c_str() + 6, "%x:%f:%f:%f:%f", &map, &x, &y, &z, &yaw) >= 4) {
                    st.test.place = true;
                    st.test.place_map = map;
                    st.test.place_pos[0] = x;
                    st.test.place_pos[1] = y;
                    st.test.place_pos[2] = z;
                    st.test.place_yaw = yaw;
                }
            } else if (item.rfind("set_flag=", 0) == 0) {
                for (const char* p = item.c_str() + 9; *p;) {
                    char* end = nullptr;
                    const unsigned long id = std::strtoul(p, &end, 10);
                    if (end == p) {
                        break;
                    }
                    st.test.set_flags.push_back(static_cast<std::uint32_t>(id));
                    p = *end ? end + 1 : end;
                }
            } else if (item == "drop_bells") {
                st.test.drop_bells = true;
            } else if (item == "grant_bells") {
                st.test.grant_bells = true;
            } else if (item == "ring_host") {
                st.test.ring = kHostBell;
            } else if (item == "ring_guest") {
                st.test.ring = kGuestBell;
            } else if (item.rfind("stand=obj:", 0) == 0) {
                int entity = 0;
                float dist = 1.2f, angle = 0;
                char button[16] = {0};
                const int n = std::sscanf(item.c_str() + 10, "%d:%f:%f:%15s", &entity, &dist, &angle, button);
                if (n >= 1 && entity > 0) {
                    st.test.stand = true;
                    st.test.st_obj = entity;
                    st.test.st_obj_dist = n >= 2 ? dist : 1.2f;
                    st.test.st_obj_angle = n >= 3 ? angle : 0.0f;
                    const std::string b = button;
                    st.test.st_button = b == "cross" ? 0x4000u : b == "triangle" ? 0x1000u : b == "square" ? 0x8000u
                                      : b == "circle" ? 0x2000u : 0u;
                }
            } else if (item.rfind("stand=", 0) == 0) {
                float x = 0, y = 0, z = 0, yaw = 0;
                char button[16] = {0};
                const int n = std::sscanf(item.c_str() + 6, "%f:%f:%f:%f:%15s", &x, &y, &z, &yaw, button);
                if (n >= 4) {
                    st.test.stand = true;
                    st.test.st_pos[0] = x;
                    st.test.st_pos[1] = y;
                    st.test.st_pos[2] = z;
                    st.test.st_yaw = yaw;
                    const std::string b = button;
                    st.test.st_button = b == "cross" ? 0x4000u : b == "triangle" ? 0x1000u : b == "square" ? 0x8000u
                                      : b == "circle" ? 0x2000u : 0u;
                }
            } else if (item.rfind("seq=", 0) == 0) {
                std::string bad;
                if (!st.test.st_seq.Parse(item.substr(4), &bad)) {
                    Log("BB_PARTY_DIRECTOR_TEST: seq token '%s' unknown; seq ignored", bad.c_str());
                    st.test.st_seq = MenuScript{};
                }
            } else if (item.rfind("prompt=", 0) == 0) {
                st.test.ap_prompt = item.substr(7);
            } else if (item.rfind("face=", 0) == 0) {
                float x = 0, z = 0;
                if (std::sscanf(item.c_str() + 5, "%f:%f", &x, &z) == 2) {
                    st.test.ap_face = true;
                    st.test.ap_fx = x;
                    st.test.ap_fz = z;
                }
            } else if (item.rfind("approach=", 0) == 0) {
                float x = 0, z = 0, r = 1.0f;
                char button[16] = {0};
                const int n = std::sscanf(item.c_str() + 9, "%f:%f:%f:%15s", &x, &z, &r, button);
                if (n >= 2) {
                    st.test.approach = true;
                    st.test.ap_x = x;
                    st.test.ap_z = z;
                    st.test.ap_r = n >= 3 ? r : 1.0f;
                    const std::string b = button;
                    st.test.ap_button = b == "cross" ? 0x4000u : b == "triangle" ? 0x1000u : b == "square" ? 0x8000u
                                      : b == "circle" ? 0x2000u : 0u;
                }
            } else if (item.rfind("a11_move=", 0) == 0) {
                float x = 0, y = 0, z = 0, at = 2;
                if (std::sscanf(item.c_str() + 9, "%f:%f:%f@%f", &x, &y, &z, &at) >= 3) {
                    st.test.a11_move = true;
                    st.test.a11_pos[0] = x;
                    st.test.a11_pos[1] = y;
                    st.test.a11_pos[2] = z;
                    st.test.a11_move_at = at;
                }
            } else if (item == "a11_mortal") {
                st.test.a11_mortal = true;
            } else if (item.rfind("a11_lamp=", 0) == 0) {
                st.test.a11_lamp = static_cast<std::uint32_t>(std::strtoul(item.c_str() + 9, nullptr, 10));
            } else if (item.rfind("a11_kill=", 0) == 0) {
                unsigned id = 0;
                float after = 20;
                if (std::sscanf(item.c_str() + 9, "%u@%f", &id, &after) >= 1) {
                    st.test.a11_kill = true;
                    st.test.a11_entity = id;
                    st.test.a11_kill_after = after;
                }
            } else if (item == "god") {
                st.test.god = true;
            } else if (item == "log_pos") {
                st.test.log_pos = true;
            } else if (item.rfind("log_pos=", 0) == 0) {
                st.test.log_pos = true;
                st.test.log_pos_every = std::min(10.0, std::max(0.2, std::atof(item.c_str() + 8)));
            } else if (item.rfind("watch_text=", 0) == 0) {
                std::string rest = item.substr(11);
                for (std::size_t a = 0; a <= rest.size();) {
                    const std::size_t b = rest.find('/', a);
                    const std::string one = rest.substr(a, b == std::string::npos ? std::string::npos : b - a);
                    if (!one.empty()) st.test.watch_texts.push_back(one);
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
            } else if (item.rfind("log_obj=", 0) == 0) {
                const int e = std::atoi(item.c_str() + 8);
                if (e > 0) st.test.log_objs.push_back(e);
            } else if (item.rfind("log_objanim=", 0) == 0) {
                const int e = std::atoi(item.c_str() + 12);
                if (e > 0) st.test.log_objanims.push_back(e);
            } else if (item.rfind("log_flag=", 0) == 0) {
                for (const char* q = item.c_str() + 9; *q;) {
                    char* end = nullptr;
                    const unsigned long id = std::strtoul(q, &end, 10);
                    if (end == q) break;
                    if (id) st.test.log_flags.push_back(static_cast<std::uint32_t>(id));
                    q = *end == '/' ? end + 1 : end;
                }
            } else if (item.rfind("echoes=", 0) == 0) {
                st.test.echoes = std::atoi(item.c_str() + 7);
            } else if (item.rfind("armor=", 0) == 0) {
                st.test.armor = static_cast<std::uint32_t>(std::strtoul(item.c_str() + 6, nullptr, 10));
            } else if (item.rfind("bell_guest@", 0) == 0) {
                st.test.bell = kGuestBell;
                st.test.bell_area = std::atoi(item.c_str() + 11);
            } else if (item.rfind("bell_host@", 0) == 0) {
                st.test.bell = kHostBell;
                st.test.bell_area = std::atoi(item.c_str() + 10);
            } else if (item == "bell_host") {
                st.test.bell = kHostBell;
            } else if (item == "bell_guest") {
                st.test.bell = kGuestBell;
            } else if (item.rfind("level=", 0) == 0) {
                st.test.level = std::atoi(item.c_str() + 6);
            } else if (item.rfind("insight=", 0) == 0) {
                st.test.insight = std::atoi(item.c_str() + 8);
            } else if (item.rfind("host_warp=", 0) == 0 || item.rfind("host_die", 0) == 0 ||
                       item.rfind("guest_die", 0) == 0 || item.rfind("host_kill=", 0) == 0 ||
                       item.rfind("host_move=", 0) == 0 || item.rfind("guest_move=", 0) == 0 ||
                       item.rfind("host_press=", 0) == 0 || item.rfind("guest_press=", 0) == 0 ||
                       item.rfind("host_lastlamp=", 0) == 0 || item.rfind("guest_lastlamp=", 0) == 0 || item.rfind("host_lamp=", 0) == 0 ||
                       item.rfind("guest_lamp=", 0) == 0 || item.rfind("host_alone=", 0) == 0 || item.rfind("guest_kill=", 0) == 0 ||
                       item.rfind("host_outage=", 0) == 0 || item.rfind("guest_outage=", 0) == 0 ||
                       item.rfind("host_poise=", 0) == 0 || item.rfind("host_spfx=", 0) == 0 ||
                       item.rfind("host_gfx=", 0) == 0 ||
                       item.rfind("host_rule=", 0) == 0 ||
                       item.rfind("guest_home", 0) == 0 || item.rfind("host_leave", 0) == 0 || item.rfind("host_kick=", 0) == 0 ||
                       item.rfind("guest_leave", 0) == 0 || item.rfind("host_rejoin", 0) == 0 ||
                       item.rfind("guest_rejoin", 0) == 0 || item.rfind("host_gather", 0) == 0) {
                TravelStep step;
                step.after_scene = after_scene;
                step.kind = item.rfind("guest_outage", 0) == 0  ? TravelStep::GuestOutage
                            : item.rfind("host_outage", 0) == 0   ? TravelStep::HostOutage
                            : item.rfind("host_poise", 0) == 0    ? TravelStep::HostPoise
                            : item.rfind("host_gfx", 0) == 0      ? TravelStep::HostGfx
                            : item.rfind("host_rule=", 0) == 0    ? TravelStep::HostPoise
                            : item.rfind("host_spfx", 0) == 0     ? TravelStep::HostSpfx
                            : item.rfind("guest_move", 0) == 0    ? TravelStep::GuestMove
                            : item.rfind("guest_press", 0) == 0  ? TravelStep::GuestPress
                            : item.rfind("host_press", 0) == 0   ? TravelStep::HostPress
                            : item.rfind("host_lastlamp", 0) == 0 ? TravelStep::HostLastLamp
                            : item.rfind("guest_lastlamp", 0) == 0 ? TravelStep::GuestLastLamp
                            : item.rfind("host_lamp=", 0) == 0   ? TravelStep::HostLamp
                            : item.rfind("guest_lamp=", 0) == 0  ? TravelStep::GuestLamp
                            : item.rfind("host_alone=", 0) == 0  ? TravelStep::HostAlone
                            : item.rfind("guest_kill=", 0) == 0  ? TravelStep::GuestKill
                            : item.rfind("guest_home", 0) == 0   ? TravelStep::GuestHome
                            : item.rfind("host_kick=", 0) == 0   ? TravelStep::HostKick
                            : item.rfind("host_leave", 0) == 0   ? TravelStep::HostLeave
                            : item.rfind("guest_leave", 0) == 0  ? TravelStep::GuestLeave
                            : item.rfind("host_rejoin", 0) == 0  ? TravelStep::HostRejoin
                            : item.rfind("guest_rejoin", 0) == 0 ? TravelStep::GuestRejoin
                            : item.rfind("host_gather", 0) == 0  ? TravelStep::HostGather
                            : item[0] == 'g'                     ? TravelStep::GuestDie
                            : item.rfind("host_die", 0) == 0     ? TravelStep::HostDie
                            : item.rfind("host_kill", 0) == 0    ? TravelStep::HostKill
                            : item.rfind("host_move", 0) == 0    ? TravelStep::HostMove
                                                                 : TravelStep::HostWarp;
                if (step.kind == TravelStep::HostWarp || step.kind == TravelStep::HostKill) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + 10, nullptr, 10));
                }
                if ((step.kind == TravelStep::HostKill || step.kind == TravelStep::GuestKill) && item.find(":hp") != std::string::npos) {
                    step.args = "hp";  // host_kill=ID:hp@S: HP 0 only (the game's own death path)
                    // host_kill=ID:hpNN@S: HP set to NN % of its max (a fight's damage; the chr lives)
                    const int pct = std::atoi(item.c_str() + item.find(":hp") + 3);
                    if (pct > 0) step.args += std::to_string(pct);
                }
                if (step.kind == TravelStep::HostLamp) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + 10, nullptr, 10));
                }
                if ((step.kind == TravelStep::GuestRejoin || step.kind == TravelStep::HostRejoin) &&
                    item.find(":now") != std::string::npos) {
                    step.args = "now";  // guest_rejoin:now@S: press it even while still in the session
                }
                if (step.kind == TravelStep::HostLastLamp || step.kind == TravelStep::GuestLastLamp) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + item.find('=') + 1, nullptr, 10));
                }
                if (step.kind == TravelStep::HostLamp || step.kind == TravelStep::GuestLamp ||
                    step.kind == TravelStep::HostAlone || step.kind == TravelStep::GuestKill) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + item.find('=') + 1, nullptr, 10));
                }
                if ((step.kind == TravelStep::HostLamp || step.kind == TravelStep::GuestLamp) &&
                    item.find(":story") != std::string::npos) {
                    step.args = "story";
                }
                if ((step.kind == TravelStep::HostDie || step.kind == TravelStep::GuestDie) &&
                    item.find(":ladder") != std::string::npos) {
                    step.args = "ladder";  // host_die:ladder@S%MAP: only while on a ladder (fallcam 3)
                }
                if (step.kind == TravelStep::GuestDie && item.find(":kidnap") != std::string::npos) {
                    step.args = "kidnap";  // guest_die:kidnap@S%MAP: a Snatcher's grab-death (kidnapall)
                }
                if ((step.kind == TravelStep::HostDie || step.kind == TravelStep::GuestDie) && item.find('%') != std::string::npos) {
                    // host_die@S%MAP: S s after the last load / previous step, in that map (no re-join)
                    step.map = item.substr(item.find('%') + 1);
                }
                if (step.kind == TravelStep::HostKick) {
                    step.args = item.substr(item.find('=') + 1, item.find_first_of("@%") - item.find('=') - 1);
                }
                if (step.kind == TravelStep::HostOutage || step.kind == TravelStep::GuestOutage) {
                    step.buttons = static_cast<std::uint32_t>(std::strtoul(item.c_str() + item.find('=') + 1, nullptr, 10));
                }
                if (step.kind == TravelStep::HostPoise || step.kind == TravelStep::HostSpfx || step.kind == TravelStep::HostGfx) {
                    step.args = item.substr(item.find('=') + 1, item.find_first_of("@%") - item.find('=') - 1);
                }
                if (step.kind == TravelStep::HostPress || step.kind == TravelStep::GuestPress) {
                    step.buttons = static_cast<std::uint32_t>(std::strtoul(item.c_str() + item.find('=') + 1, nullptr, 0));
                    // "!": only once the own chr is alive and the world up (not into a death screen).
                    step.alive = item.find('!') != std::string::npos;
                    // BUTTONS:lock: R3 only while the lock-on is off (a second try after an R3 that
                    // only turned the camera; never a tap that releases the lock).
                    if (item.find(":lock") != std::string::npos) step.args = "lock";
                    if (const std::size_t pc = item.find('%'); pc != std::string::npos) {
                        step.map = item.substr(pc + 1, item.find('!', pc) == std::string::npos ? std::string::npos
                                                                                              : item.find('!', pc) - pc - 1);
                    }
                }
                if (step.map.empty() && item.find('%') != std::string::npos) {
                    // Any step with %MAP runs on the steady-world clock (S s after the last load or
                    // the previous step, in that map): chains of warps / kills without re-joins.
                    step.map = item.substr(item.find('%') + 1);
                }
                if (item.find('&') != std::string::npos && !step.map.empty()) {
                    // "...%MAP&": also out of the party (guest_leave, then a step in the own world),
                    // S s after the own world is up again or the previous step, in that map.
                    step.out_ok = true;
                    step.map.erase(step.map.find('&') == std::string::npos ? step.map.size() : step.map.find('&'));
                }
                if (step.kind == TravelStep::HostMove || step.kind == TravelStep::GuestMove) {
                    // host_move=X:Y:Z@S[%MAP] (':' separates: ',' separates the list items)
                    const char* p = item.c_str() + item.find('=') + 1;
                    for (int k = 0; k < 3; ++k) {
                        char* end = nullptr;
                        step.pos[k] = std::strtof(p, &end);
                        p = (end && *end == ':') ? end + 1 : end;
                    }
                    if (p && p > item.c_str() && p[-1] == ':' &&
                        (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.' || *p == '+')) {
                        step.yaw = std::strtof(p, nullptr);  // X:Y:Z:YAW
                        step.has_yaw = true;
                    }
                    // X:Y:Z[:YAW]:native: the game's own in-world player warp (0x154B110) instead
                    // of the bare chr warp (the bug 7 A/B).
                    step.warp = item.find(":native") != std::string::npos ? 1 : 0;
                    if (const std::size_t pn = item.find(":pin="); pn != std::string::npos) {
                        step.pin = std::min(10.0, std::max(0.0, std::atof(item.c_str() + pn + 5)));
                    }
                    step.ground = item.find(":ground") != std::string::npos;
                    if (const std::size_t pc = item.find('%'); pc != std::string::npos) {
                        step.map = item.substr(pc + 1);
                    }
                }
                if (const std::size_t at_sign = item.find('@'); at_sign != std::string::npos) {
                    step.delay = std::atof(item.c_str() + at_sign + 1);
                }
                if (const std::size_t hash = item.find('#'); hash != std::string::npos) {
                    step.min_join = std::atoi(item.c_str() + hash + 1);
                }
                if (const std::size_t caret = item.find('^'); caret != std::string::npos) {
                    step.min_coop = std::atoi(item.c_str() + caret + 1);  // "^N": N cooperators in
                    if (const std::size_t star = item.find('*', caret); star != std::string::npos)
                        step.coop_rise = std::atoi(item.c_str() + star + 1);  // "^N*K": the K-th time
                }
                // "%MAP#N" / "%MAP!": the map name ends at the next option (a '#2' in it never matched).
                if (const std::size_t cut = step.map.find_first_of("#!@^"); cut != std::string::npos) {
                    step.map.erase(cut);
                }
                step.wait_flag = wait_flag;
                step.wait_near = wait_near;
                std::memcpy(step.near_pos, near_pos, sizeof near_pos);
                step.near_r = near_r;
                step.wait_obj = wait_obj;
                step.wait_obj_anim = wait_obj_anim;
                step.wait_value = wait_value;
                st.test.steps.push_back(step);
            } else if (item.rfind("host_mark", 0) == 0 || item.rfind("guest_mark", 0) == 0 ||
                       item.rfind("host_stage=", 0) == 0 || item.rfind("host_walk=", 0) == 0 ||
                       item.rfind("host_headstone=", 0) == 0 || item.rfind("host_flag=", 0) == 0 ||
                       item.rfind("guest_flag=", 0) == 0 ||
                       item.rfind("host_obj=", 0) == 0 || item.rfind("guest_obj=", 0) == 0 ||
                       item.rfind("host_chr=", 0) == 0 || item.rfind("guest_chr=", 0) == 0 ||
                       item.rfind("host_skipremo", 0) == 0 || item.rfind("guest_skipremo", 0) == 0 ||
                       item.rfind("guest_stick=", 0) == 0 || item.rfind("host_use=", 0) == 0 ||
                       item.rfind("host_stick=", 0) == 0 || item.rfind("host_route=", 0) == 0 ||
                       item.rfind("guest_route=", 0) == 0 || item.rfind("guest_fixture", 0) == 0 ||
                       item.rfind("guest_use=", 0) == 0) {
                TravelStep step;
                step.kind = item.rfind("host_obj=", 0) == 0     ? TravelStep::HostObj
                            : item.rfind("guest_obj=", 0) == 0  ? TravelStep::GuestObj
                            : item.rfind("host_chr=", 0) == 0   ? TravelStep::HostChr
                            : item.rfind("guest_chr=", 0) == 0  ? TravelStep::GuestChr
                            : item.rfind("guest_mark", 0) == 0    ? TravelStep::GuestMark
                            : item.rfind("host_mark", 0) == 0   ? TravelStep::HostMark
                            : item.rfind("host_stage=", 0) == 0 ? TravelStep::HostStage
                            : item.rfind("host_walk=", 0) == 0  ? TravelStep::HostWalk
                            : item.rfind("host_flag=", 0) == 0  ? TravelStep::HostFlag
                            : item.rfind("guest_flag=", 0) == 0 ? TravelStep::GuestFlag
                            : item.rfind("host_skipremo", 0) == 0  ? TravelStep::HostSkipRemo
                            : item.rfind("guest_skipremo", 0) == 0 ? TravelStep::GuestSkipRemo
                            : item.rfind("guest_stick=", 0) == 0   ? TravelStep::GuestStick
                            : item.rfind("host_stick=", 0) == 0    ? TravelStep::HostStick
                            : item.rfind("host_route=", 0) == 0    ? TravelStep::HostRoute
                            : item.rfind("guest_route=", 0) == 0   ? TravelStep::GuestRoute
                            : item.rfind("host_use=", 0) == 0      ? TravelStep::HostUse
                            : item.rfind("guest_use=", 0) == 0     ? TravelStep::GuestUse
                            : item.rfind("guest_fixture", 0) == 0  ? TravelStep::GuestFixture
                                                                : TravelStep::HostHeadstone;
                if (const std::size_t pc = item.find('%'); pc != std::string::npos) {
                    step.map = item.substr(pc + 1); // only while the current map name contains it
                }
                if (const std::size_t eq = item.find('='); eq != std::string::npos) {
                    const std::size_t stop = item.find_first_of("@#%", eq);
                    step.args = item.substr(eq + 1, stop == std::string::npos ? std::string::npos : stop - eq - 1);
                }
                if (const std::size_t at_sign = item.find('@'); at_sign != std::string::npos) {
                    step.delay = std::atof(item.c_str() + at_sign + 1);
                }
                if (const std::size_t hash = item.find('#'); hash != std::string::npos) {
                    step.min_join = std::atoi(item.c_str() + hash + 1);
                }
                if (const std::size_t caret = item.find('^'); caret != std::string::npos) {
                    step.min_coop = std::atoi(item.c_str() + caret + 1);  // "^N": N cooperators in
                    if (const std::size_t star = item.find('*', caret); star != std::string::npos)
                        step.coop_rise = std::atoi(item.c_str() + star + 1);  // "^N*K": the K-th time
                }
                // "%MAP#N" / "%MAP!": the map name ends at the next option (a '#2' in it never matched).
                if (const std::size_t cut = step.map.find_first_of("#!@^"); cut != std::string::npos) {
                    step.map.erase(cut);
                }
                step.wait_flag = wait_flag;
                step.wait_near = wait_near;
                std::memcpy(step.near_pos, near_pos, sizeof near_pos);
                step.near_r = near_r;
                step.wait_obj = wait_obj;
                step.wait_obj_anim = wait_obj_anim;
                step.wait_value = wait_value;
                st.test.steps.push_back(step);
            } else if (!item.empty()) {
                Log("BB_PARTY_DIRECTOR_TEST: '%s' is not log_state, start, grant_bells, drop_bells, award_lot=N, set_flag=N[:N], insight=N, "
                    "ring_host, ring_guest, bell_host, bell_guest, host_warp=ID@S, host_kill=ENTITY@S, host_move=X:Y:Z[:YAW]@S[%MAP], guest_move=..., host_press=BUTTONS@S[%MAP][!], guest_press=BUTTONS@S[%MAP][! = alive], host_die@S, guest_die@S, "
                    "host_mark[=lua|=EFFECT]@S, guest_mark@S, host_stage=A:B:R:I:P|bonfire:ID@S, host_walk=X:Y:Z:YAW[/...]@S or host_headstone=TALK:DOWNS@S; ignored",
                    item.c_str());
            }
        }
        if (const char* d = std::getenv("BB_PARTY_DIRECTOR_TEST_DELAY"); d && d[0]) {
            st.test.delay = std::atof(d);
        }
    }
    bool known = true;
    st.start_mode = ParseStartMode(std::getenv("BB_PARTY_START"), &known);
    if (!known) {
        Log("BB_PARTY_START='%s' is not prologue_solo, skip_prologue or immediate; using prologue_solo", std::getenv("BB_PARTY_START"));
    }
    if (const char* g = std::getenv("BB_PARTY_GRANT_BELLS"); g && g[0] == '0') {
        st.grant_bells = false;
    }
    st.watchdog = HostWatchdog(HostWatchdogConfigFromEnv());
    Log("director: role %s, max %d players, %s every %.0f s (first %.1f s in the world), fast join %s, start %s, "
        "bell grants %s%s%s",
        role == PartyRole::Host ? "host" : role == PartyRole::Guest ? "guest" : "none", st.max_players,
        st.auto_ring ? "bell" : "no automatic bell", st.ring_every, st.ring_delay, st.fast_join ? "on" : "off",
        StartModeName(st.start_mode),
        st.grant_bells ? "on" : "off", st.test.on ? "; test: " : "", st.test.on ? std::getenv("BB_PARTY_DIRECTOR_TEST") : "");
}

void PartyDirector::SetLink(party::PartyLink* link) {
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    st.link = link;
}

PartyRole PartyDirector::Role() const {
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.role;
}

bool GatherGuest(const GameSnapshot& s, party::PartyLink* link, Clock::time_point now);

/// "Enter the party's dungeon" in one press (docs/party/chalice.md 7): after the altar's glyph box
/// took the party dungeon's glyph, the remaining prompts are answered on the game's own pad path:
/// OK on "Search using Chalice Glyph", the search result, YES on "Join the selected Chalice
/// Dungeon?", the altar again and its first awaken point. Text-gated and strict: a prompt that
/// does not come in 10 s stops the walk (no blind presses). The awaken menu's first Cross is often
/// swallowed (live g1h: the menu stayed open), so it is pressed up to three times; the load that
/// starts ends the walk. Off by default until verified live (0.5.2): BB_PARTY_ALTAR_AUTO=1 turns it on.
void AltarAutopilotTick(const GameSnapshot& s) {
    static const bool off = [] {
        const char* e = std::getenv("BB_PARTY_ALTAR_AUTO");
        return !(e && e[0] == '1');
    }();
    static MenuScript script;
    static bool on = false;
    if (off) {
        return;
    }
    if (chalice::TakeAltarAutopilot()) {
        script = MenuScript{};
        std::string bad;
        on = script.Parse("!cross@Search using Chalice Glyph/!cross@Search Results/!left@Join the selected Chalice Dungeon/"
                          "cross/wait/cross/!cross@Select location to awaken in/wait/cross/wait/cross/wait/cross",
                          &bad);
        Log("altar: entering the party's dungeon (%s)", on ? "prompts answered for the player" : bad.c_str());
    }
    if (!on) {
        return;
    }
    if (s.loading || !s.world_up) {
        // The awaken load started: done.
        runtime_pad_director_press(0);
        on = false;
        Log("altar: the party's dungeon is loading");
        return;
    }
    std::uint64_t seq = 0;
    std::int64_t last_ms = 0;
    BbGameMenu::TextActivity(&seq, &last_ms);
    const auto now = Clock::now();
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
    std::string line;
    const std::uint32_t held = script.Step(secs, seq, seq ? double(now_ms - last_ms) / 1000.0 : 1e9, &line,
                                           [](const std::string& text) {
                                               const std::int64_t ms = BbGameMenu::TextSeenMs(text.c_str());
                                               return ms > 0 ? double(ms) / 1000.0 : -1.0;
                                           });
    runtime_pad_director_press(held);
    if (!line.empty()) {
        Log("altar: %s", line.c_str());
    }
    if (script.finished()) {
        runtime_pad_director_press(0);
        on = false;
        Log("altar: %s", script.stuck() ? "a prompt did not come; the player takes over" : "prompts answered");
    }
}

void PartyDirector::Tick() {
    State& st = S();
    const auto now = Clock::now();
    const std::uint64_t tid = ThreadId();
    if (!st.started) {
        st.started = true;
        st.start = now;
        st.last_log = now;
        st.tick_thread = tid;
        Log("tick: first call on thread %llu (SprjFlipper::Update entry)", static_cast<unsigned long long>(tid));
    } else if (tid != st.tick_thread) {
        if (st.foreign_ticks++ < 4) {
            Log("tick: called on thread %llu, not %llu", static_cast<unsigned long long>(tid),
                static_cast<unsigned long long>(st.tick_thread));
        }
        return;
    }
    ++st.ticks;
    BbBootWatchState::Tick(); // BB_BOOT_WATCHDOG: the main loop moves
    HeapWatchTick();          // BB_HEAP_WATCH_LOG: live bytes per call site in a game heap
    // The cheap reads every frame; the game's cooperator count twice a second.
    const bool full = Seconds(st.last_full, now) >= 0.5;
    if (full) {
        st.last_full = now;
    }
    perf::Scope zone(perf::Zone::DirReadState);
    GameSnapshot s = ReadGameState(full);
    zone.End();
    if (!full && st.have_last) {
        s.cooperators = st.last.cooperators;
    }
    LogTransitions(st, s);
    // A6: the host ended the party (or is gone) while this guest stands in its world: home at once
    // through the game's own send-home (OnLeave_Limit) instead of the game's room-gone path 10 s
    // later with "Session lost" (0xcc/5001 from OnRoomDisappeared 0x138A590, live).
    if (g_send_home_request.load(std::memory_order_relaxed) && s.world_up && !s.loading) {
        g_send_home_request = false;
        std::uint64_t wts = 0;
        std::uint8_t own = 1;
        SafeGet(Guest(0x5556678), &wts);
        if (wts) SafeGet(wts + 0x1592, &own);
        std::string why;
        {
            std::lock_guard<std::mutex> lk(g_send_home_mu);
            why = g_send_home_why;
        }
        if (own == 0 && OnLeaveLimitOk()) {
            RunOnLeaveLimit();
            Log("host lost (%s): home now (OnLeave_Limit)", why.c_str());
        } else {
            Log("host lost (%s): already in the own world", why.c_str());
        }
    }
    if (s.world_up && !s.loading && !st.world_seen) {
        st.world_seen = true;
        st.world_at = now;
    } else if (!s.world_up) {
        st.world_seen = false;
    }
    st.last = s;
    st.have_last = true;
    if (full) party::runtime_host_cooperators(s.cooperators);  // F5: an outage's dropped cooperator
    zone.Next(perf::Zone::DirNpcTest);
    NpcTestTick(s); // BB_PARTY_TEST_NPC fixture (party_npc_test.h)
    b2::Tick(s);    // B2 seamless prototype (party_b2.h; BB_PARTY_B2=1)
    zone.End();

    PartyRole role;
    party::PartyLink* link;
    StartMode start_mode;
    bool grant_bells;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        role = st.role;
        link = st.link;
        start_mode = st.start_mode;
        grant_bells = st.grant_bells;
    }
    // Test aids: scripted new game, prologue fast-forward (test_newgame.h).
    NewGameTestTick(s, TitleReadyForInput(role, link));
    // Test aid (BB_PARTY_TEST_START_LAMP=ID): one lamp warp 3 s after this player's first steady
    // own world, before any join (a late-joiner run with the host in another map than the
    // guest's save: the director's host_lamp step needs a join first).
    if (const char* sl = std::getenv("BB_PARTY_TEST_START_LAMP"); sl && sl[0]) {
        static bool done = false;
        static Clock::time_point steady_at{};
        static bool steady = false;
        const bool now_steady = s.world_up && !s.loading && s.map_id != 0xffffffffu;
        if (now_steady != steady) {
            steady = now_steady;
            steady_at = now;
        }
        if (!done && steady && Seconds(steady_at, now) >= 3.0) {
            done = true;
            const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(sl, nullptr, 10));
            Log("test: start lamp %u (from %s)", id, MapName(s.map_id).c_str());
            if (role == PartyRole::Host) {
                // Decided (no "Travel with the party?" question): a guest connected to the link but
                // not yet in the world made the player's-own-use path ask, and the unanswered
                // question held the warp for the whole run (pard S1, 2026-10-15).
                RunLampWarpDecided(id, false);
            } else {
                split::NoteLocalUse(id);
                reinterpret_cast<void(BB_COOP_SYSV*)(std::uint32_t)>(Guest(kLampWarp))(id);
            }
        }
    }
    // Late joiner catch-up (party_catchup.h): a guest behind the host is brought up to it and
    // gathered at the host's lamp before any summon.
    {
        std::uint32_t host_map = 0;
        if (role == PartyRole::Guest && link) {
            for (const party::RosterEntry& e : link->roster()) {
                if (e.slot == party::kHostSlot) {
                    host_map = e.map_id;
                }
            }
        }
        CatchupTick(s, role == PartyRole::Guest && link && link->state() == party::LinkState::Connected,
                    Seconds(st.start, now), host_map, SkipPrologueOn(start_mode));
    }
    // C1: the campaign start (readiness, bells).
    if (full || !st.start_logged) {
        perf::Scope z(perf::Zone::DirStart);
        UpdateStart(st, s, SkipPrologueOn(start_mode) ? StartMode::SkipPrologue : start_mode);
        if (((grant_bells && role != PartyRole::None) || st.test.grant_bells) && st.world_seen &&
            Seconds(st.world_at, now) >= std::min(5.0, st.ring_delay) && !PrologueSkipBusy() && !CatchupBusy()) {
            GrantBells(st, s);
        }
    }
    zone.Next(perf::Zone::Progress);
    progress::DirectorTick(s, link); // C2: flag capture / apply, host -> guest sync (party_progress.h)
    zone.Next(perf::Zone::DirTitle);
    // Title: the FROM client's state when it changes (sign-in debugging).
    if (role != PartyRole::None && full && !s.world_up) {
        std::string fc = DescribeFromClient();
        if (fc != st.last_from_client) {
            Log("title: %s", fc.c_str());
            st.last_from_client = std::move(fc);
        }
    }
    // Title: Continue on our own when asked (BB_PARTY_AUTOCONTINUE=1) or after a supervised crash
    // restart (run.bat's BB_PARTY_RESTARTED=1). A recovered party marker alone does not press
    // buttons on a game the player started by hand.
    if (role != PartyRole::None) {
        int ac;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            ac = st.auto_continue;
        }
        if (ac == 1 && !NewGameTestRequested()) {
            AutoContinue(st, s, role, link, now);
        }
    }
    zone.Next(perf::Zone::DirSend);
    // The roster entry.
    if (link) {
        const party::MemberState ms = LocalState(s, st.start_prologue);
        const std::uint32_t map = s.world_up ? s.map_id : 0;
        if (!st.sent_any || ms != st.sent_state || map != st.sent_map) {
            st.sent_any = true;
            st.sent_state = ms;
            st.sent_map = map;
            link->set_local_state(ms, map);
        }
    }
    // The party HUD: our vitals out, the companions' board for the overlay (party_hud.h).
    if (role != PartyRole::None) PartyHudTick(s, link, LocalState(s, st.start_prologue));
    // Pings (I2, party_ping.h): the button -> the spot looked at -> the party; theirs -> the board.
    ping::Tick(s, role != PartyRole::None ? link : nullptr, s.world_up && !s.loading && s.player);
    // #19: who in the party is still in its prologue (the holds below and in the sync modules;
    // the Party page's "Waiting: X is in the prologue"; the host's sign board).
    if (link && role != PartyRole::None) {
        const std::vector<party::RosterEntry> roster = link->roster();
        std::string self;
        for (const party::RosterEntry& e : roster) {
            if (e.slot == link->local_slot()) self = e.name;
        }
        g_host_prologue.store(role == PartyRole::Guest && HostInPrologue(roster));
        std::string names = MembersInPrologue(roster, self);
        bool changed;
        {
            std::lock_guard<std::mutex> lk(g_prologue_names_mu);
            changed = names != g_prologue_names;
            if (changed) g_prologue_names = names;
        }
        if (changed) {
            Log("prologue: %s", names.empty() ? "nobody else in the party is in its prologue"
                                              : (names + " still in the prologue: no summons, gathers or party syncs "
                                                         "touch a prologue world until everybody is ready").c_str());
            party::bridge::OnOthersPrologue(names);
        }
        if (role == PartyRole::Host) {
            std::set<std::string> ids;
            for (const party::RosterEntry& e : roster) {
                if (e.connected && e.state == party::MemberState::Prologue) ids.insert(e.name);
            }
            bbnet::party::FromApi::instance().set_prologue_members(ids);
        }
    } else {
        g_host_prologue.store(false);
    }
    // C4 rot, D7 enemy scaling, E2 extras: the host's rules for the party (party_balance.h).
    if (role != PartyRole::None) balance::Tick(s, link, role == PartyRole::Host);
    if (role != PartyRole::None) npcfight::Tick(s, role == PartyRole::Host);  // D11: the assassins on a guest
    if (role != PartyRole::None) enemyai::Tick(s, role == PartyRole::Guest);  // a lone guest's enemies' AI
    // I1 friendly fire / PvP (party_pvp.h): the party rule on this game's team tables.
    if (role != PartyRole::None) pvp::Tick(s, role == PartyRole::Host);
    // B1: the host's warps go to every guest (reliable: replayed to a member who reconnects).
    if (link && role == PartyRole::Guest) {
        StoryIntent up;
        while (PopGuestStoryUp(&up)) {  // D10: the guest's own cutscene, for the host to share
            if (st.start_prologue || g_host_prologue.load()) {
                Log("story: own cutscene %u not sent to the host: %s is in its prologue (#19)", up.id,
                    st.start_prologue ? "this guest" : "the host");
                continue;
            }
            link->send_event(0, kStoryUpEventName, StoryToJsonText(up));
            Log("story: own cutscene %u sent to the host (D10)", up.id);
        }
    }
    // F5 fallback: the host's game ended the session by itself (b2::TakeHostIrregularLeave): it is
    // over for every member, but the host's game takes ~30 s to leave the room (live), and meanwhile
    // a guest would stand in a dead session (0.6.1 regression run). The guests hear it at once.
    if (link && role == PartyRole::Host && b2::TakeHostIrregularLeave()) {
        link->post_event(party::kBroadcast, kHostSessionEndEvent, "{\"why\":\"irregular_leave\"}");
        Log("F5: our game ended the session irregularly (OnIrregularLeaveSession): the members go home now");
    }
    if (link && role == PartyRole::Host) {
        TravelIntent t;
        if (s.session_role == RoleHost) {
            st.last_hosting = now;
        }
        const bool hosting_recently =
            st.last_hosting != Clock::time_point{} && now - st.last_hosting < std::chrono::seconds(60);
        const bool audience = HostTravelHasAudience(hosting_recently, link->roster());
        while (PopHostTravel(&t)) {
            if (!audience) {
                // Nobody in this world: the members in their prologue or own worlds stay put.
                Log("travel #%llu (%s) not sent: nobody is in this world (members in their prologue or own worlds)",
                    static_cast<unsigned long long>(t.seq), TravelKindName(t.kind));
                continue;
            }
            // Into (or respawning in) a Chalice Dungeon: which one (guests follow only into it).
            if (TravelIntoChalice(t) || static_cast<std::uint32_t>(t.last_lamp) / 100000 == 29) {
                t.chalice_channel = chalice::Read().channel;
            }
            link->post_event(party::kBroadcast, kTravelEventName, TravelToJsonText(t));
            party::bridge::OnTravel(TravelKindLabel(t.kind));
            Log("travel #%llu (%s) sent to the party", static_cast<unsigned long long>(t.seq), TravelKindName(t.kind));
            PhantomNoteHostTravel(t); // a rest (Dream, death, Mark): guests refill too
        }
        // C4: the host's cutscenes, endings and time of day (party_story.h).
        StoryIntent si;
        while (PopHostStory(&si)) {
            if (st.start_prologue) {
                // #19: the host's own prologue scenes are its own (a guest replayed the host's first
                // Dream cutscene in its world, playtest 1 G2:21127).
                Log("story #%llu (%s %u) not sent: the host is in its prologue", static_cast<unsigned long long>(si.seq),
                    StoryKindName(si.kind), si.id);
                continue;
            }
            link->post_event(party::kBroadcast, kStoryEventName, StoryToJsonText(si));
            Log("story #%llu (%s %u) sent to the party", static_cast<unsigned long long>(si.seq),
                StoryKindName(si.kind), si.id);
        }
        // D10: story notices only the host's event shows (party_story StoryNoticeMessage).
        StoryNotice sn;
        while (PopHostStoryNotice(&sn)) {
            link->post_event(party::kBroadcast, kStoryNoticeEventName, StoryNoticeToJsonText(sn));
            Log("story notice %u (event %u) sent to the party", sn.message, sn.event_id);
        }
        // C3: the host's item lots to every guest; the full list to a member that (re)joined.
        std::vector<ItemGrant> items;
        ItemGrant g;
        while (PopHostItems(&g)) {
            items.push_back(g);
        }
        if (!items.empty()) {
            link->post_event(party::kBroadcast, kItemsEventName, ItemsToJsonText(items));
            Log("items: %zu lots sent to the party", items.size());
        }
        // The host's chalices (its own and those the guests sent): every guest gets the ones it
        // lacks (party_chalice.h).
        std::vector<std::uint32_t> chalices;
        if (chalice::PopHostGoods(&chalices)) {
            link->post_event(party::kBroadcast, chalice::kGoodsEventName, chalice::GoodsToJsonText(chalices));
        }
        // The party's gestures (the host's own and those it learned from the guests; party_gestures.h).
        std::vector<std::uint32_t> gestures;
        if (gestures::PopOwn(&gestures)) {
            link->post_event(party::kBroadcast, gestures::kEventName, gestures::ToJsonText(gestures));
        }
        // The host's own online id: the party's dungeon at its altar is another member's ritual.
        static Clock::time_point host_name_at{};
        if (Seconds(host_name_at, Clock::now()) >= 2.0) {
            host_name_at = Clock::now();
            for (const party::RosterEntry& e : link->roster()) {
                if (e.slot == party::kHostSlot) chalice::SetHostName(e.name);
            }
        }
        int slot = -1;
        while (TakeHostFullItems(&slot, &items)) {
            link->post_event(slot, kItemsFullEventName, ItemsToJsonText(items));
            Log("items: full list (%zu lots) sent to slot %d", items.size(), slot);
        }
    }
    // A guest's chalices go to the host (any member can perform the ritual; party_chalice.h).
    if (link && role == PartyRole::Guest) {
        std::vector<std::uint32_t> chalices;
        if (chalice::PopHostGoods(&chalices)) {
            link->post_event(party::kHostSlot, chalice::kGoodsEventName, chalice::GoodsToJsonText(chalices));
        }
        std::vector<std::uint32_t> gestures;  // its gestures go to the host (party_gestures.h)
        if (gestures::PopOwn(&gestures)) {
            link->post_event(party::kHostSlot, gestures::kEventName, gestures::ToJsonText(gestures));
        }
        std::string form;  // its own ritual dungeon's FormData for the host's board
        if (chalice::PopGuestForm(&form)) {
            link->post_event(party::kHostSlot, chalice::kFormEventName, form);
        }
        std::string gift;  // D5: an NPC's gift to this guest in the host's world (party_items.h)
        while (PopGuestGift(&gift)) {
            link->post_event(party::kHostSlot, kGiftUpEventName, gift);
        }
    }
    // B2 (party_b2.h): guest "arrived" / host notes.
    if (link) {
        b2::SetLocalSlot(link->local_slot());
        {
            // Split travel: where every member is (party_b2 SplitTick; docs/party/split_travel.md).
            party::MemberVitals vit[party::kMaxVitals];
            const int vn = link->vitals(vit, party::kMaxVitals);
            const std::vector<party::RosterEntry> roster = link->roster();
            std::vector<b2::Where> where;
            for (int i = 0; i < vn; ++i) {
                b2::Where w;
                w.slot = vit[i].slot;
                w.map = vit[i].map_id;
                w.has_pos = (vit[i].flags & party::kVitalsHasPos) != 0;
                std::memcpy(w.pos, vit[i].pos, sizeof w.pos);
                w.age_ms = vit[i].age_ms;
                w.connected = false;
                for (const party::RosterEntry& e : roster) {
                    if (e.slot == w.slot) {
                        w.connected = e.connected;
                    }
                }
                where.push_back(w);
            }
            b2::SetWhere(where);
            {
                std::lock_guard<std::mutex> lk(g_where_mu);
                g_where = where;
                g_where_local = link->local_slot();
            }
        }
        int b2slot = party::kBroadcast;
        std::string b2body;
        while (b2::PopOutgoing(&b2slot, &b2body)) {
            link->post_event(b2slot, b2::kEventName, b2body);
        }
    }
    zone.Next(perf::Zone::StatusBridge);
    // The overlay's Party tab: status board and its commands (party_status_bridge.h).
    if (role != PartyRole::None &&
        party::bridge::Tick({role == PartyRole::Host, link, s.world_up, s.loading, s.session_role, s.cooperators,
                             link ? link->max_players() : 3, InWorld(s) && !st.start_ready,
                             StartStepName(st.start_step), role == PartyRole::Guest && StoryMirrorPlaying()})
            .ring_now) {
        st.rung_ever = false; // Rejoin while connected: the next bell goes up at once
    }
    // A host summon that never completes: end the session and ring again (party_watchdog.h).
    if (role == PartyRole::Host && full) {
        HostWatchdogTick(st, s, link, now);
    }
    // Phantoms as full players: the host's lamp / rest / boss Insight out, the guest's refill and
    // Insight in (party_phantom.h).
    zone.Next(perf::Zone::Phantom);
    PhantomTick(link);
    // Guests use the Doll, shops and workshop in the host's Dream (party_guestsvc.h).
    GuestServicesTick(s, link);
    // A guest's levers / doors / NPC talk change the host's world (party_worldact.h).
    worldact::Tick(s, link);
    npctalk::Tick(s, link);  // shared NPC dialogue (party_npctalk.h)
    split::Tick(s, link);  // split travel: the lamp choice dialog (party_split.h)
    if (role != PartyRole::None) {
        gather::Tick(s, link, role == PartyRole::Host);  // I5 Gather the party (party_gather.h)
        vote::Tick(s, link, role == PartyRole::Host);    // 0.7.3 lamp travel voting (party_vote.h)
    }
    if (role == PartyRole::Host) {
        AltarAutopilotTick(s);  // "Enter the party's dungeon" in one press
    }
    zone.Next(perf::Zone::DirRing);
    if (st.test.on) {
        RunTravelSteps(st, s, role, now);
        RunTest(st, s, now);
        if ((st.test.log_state || st.test.start) && Seconds(st.last_log, now) >= 5.0) {
            const double secs = Seconds(st.last_log, now);
            if (st.test.start) {
                Log("start: %s", DescribeStart(st.cs, InWorld(s)).c_str());
            }
            Log("state: %s; %llu ticks in %.1f s on thread %llu; game Lua dispatches %llu on it, %llu elsewhere",
                Describe(s).c_str(), static_cast<unsigned long long>(st.ticks - st.ticks_at_log), secs,
                static_cast<unsigned long long>(st.tick_thread),
                static_cast<unsigned long long>(g_dispatch_main.load()),
                static_cast<unsigned long long>(g_dispatch_other.load()));
            st.last_log = now;
            st.ticks_at_log = st.ticks;
        }
    }
    // Fast join, host: a member's new sign is on our board - let the game's get-list poll run on
    // its next frame (the FROM client's countdown [0x5540288]+0x100, set to the 1 s interval after
    // each poll; the builder 0x1e946e0 requests once it is <= 0) instead of up to 1 s later.
    if (role == PartyRole::Host && st.fast_join) {
        const std::uint64_t signs = bbnet::sign_board_changes();
        if (signs != st.signs_seen) {
            st.signs_seen = signs;
            std::uint64_t cm = 0;
            float countdown = 0;
            if (SafeGet(Guest(0x5540288), &cm) && cm && SafeGet(cm + 0x100, &countdown) && countdown > 0) {
                const float zero = 0;
                SafeWrite(cm + 0x100, &zero, sizeof zero);
                Log("sign on the board: the game polls it now (was in %.2f s)", countdown);
            }
        }
    }
    if (role == PartyRole::Host) {
        const std::uint64_t summons = bbnet::summon_requests();
        if (summons != st.summons_seen) {
            st.summons_seen = summons;
            st.summon_at = now;
        }
    }
    if (role == PartyRole::Host && st.fast_join) {
        for (std::string& name : bbnet::party::PartyHostService::instance().take_restarted()) {
            Log("%s restarted its game: its old peer is cleared once our game is out of that session", name.c_str());
            st.restarted.push_back(std::move(name));
        }
        // Not while our game is still in the session with the dead peer (it ends that one itself).
        if (!st.restarted.empty() && s.session_role == RoleIdle) {
            for (const std::string& name : st.restarted) {
                Log("%s: %d stale peer(s) of our game cleared", name.c_str(), ClearStalePeers(name));
                // Playtest 1: its sign / direct summon hold (a room served to the old run, or the
                // lost link's 26 s) refused the new run's summon ("a crashed member's connection is
                // still held"); our game is out of that session and its peer is gone: end the hold.
                if (bbnet::party::FromApi::instance().hold_left_ms() > 0) {
                    bbnet::party::FromApi::instance().hold_signs(name, 1);
                    Log("%s: its sign hold ended (the new run can be summoned now)", name.c_str());
                }
            }
            st.restarted.clear();
        }
    }
    if (!st.poll_logged && Seconds(st.last_ring, now) >= 2.5) {
        st.poll_logged = true;
        Log("bell +2.5 s: %s", DescribeSignPolling().c_str());
    }
    UdpReachTick(st, role, link, now);
    // Automatic summoning. The host also rings while hosting: the 2nd / 3rd cooperator of a
    // 3- / 4-player party (HostMayRing checks the count).
    if (!st.auto_ring || role == PartyRole::None || !link || !s.world_up || s.loading || !st.world_seen ||
        Seconds(st.world_at, now) < st.ring_delay || !SessionMayRing(role == PartyRole::Host, s.session_role) ||
        PrologueSkipBusy() || CatchupBusy()) {
        return;
    }
    // A11 (ERSC_PARITY.md, user decision): a player joining while the host fights a boss or
    // watches a cutscene waits outside - the host does not ring until it ends (the guest's sign
    // waits at the host's lamp in its own world, GatherGuest).
    if (role == PartyRole::Host) {
        const bool boss = BossBarShown(), remo = CutscenePlaying();
        if (boss || remo) {
            // Both sides see why (party status line) and the guests get it again every 10 s
            // (a guest connecting mid-fight); direct summon refuses meanwhile (party_dsummon).
            const int kind = boss ? 1 : 2;
            if (kind != st.a11_kind || Seconds(st.a11_told, now) >= 10.0) {
                st.a11_kind = kind;
                st.a11_told = now;
                coop::b2::HostBusyChanged(boss, remo);
            }
            if (!st.a11_waiting) {
                st.a11_waiting = true;
                // A bell rung before the fight keeps the game searching (live: summoned 29 s into
                // the fight): it goes now.
                const bool dropped = RemoveSpEffect(kHostBellEffect);
                Log("A11: no bell while the host %s: joiners wait outside%s", boss ? "fights a boss" : "watches a cutscene",
                    dropped ? " (the ringing bell stopped)" : "");
            }
            return;
        }
        if (st.a11_waiting) {
            st.a11_waiting = false;
            st.a11_kind = 0;
            coop::b2::HostBusyChanged(false, false);
            st.rung_ever = false;  // ring at once
            Log("A11: the boss fight / cutscene is over: ringing for waiting joiners again");
        }
    }
    if (role == PartyRole::Guest && st.rung_ever && Seconds(st.last_ring, now) < st.ring_every) {
        // The host entered its Chalice Dungeon after this guest rang: the waiting sign gets the
        // chalice wait (9900) too, else the host's summon loads the guest without the quick join
        // (live: the guest rang ~10 s before the host's roster showed area 29).
        const std::uint64_t player = LocalPlayer();
        std::uint32_t host_map = 0;
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot == party::kHostSlot) host_map = e.map_id;
        }
        if (((host_map >> 24) & 0xff) == 29 && host_map != 0xffffffffu && HasSpEffect(player, kGuestBellEffect) &&
            !HasSpEffect(player, kChaliceWaitEffect) && Seconds(st.chalice_wait_at, now) >= 5.0) {
            st.chalice_wait_at = now;
            const bool ok = ApplySpEffect(kChaliceWaitEffect);
            Log("bell: the host entered a chalice dungeon (%s) while this guest waits: SpEffect %d %s",
                MapName(host_map).c_str(), kChaliceWaitEffect, ok ? "applied" : "NOT applied");
        }
    }
    if (role == PartyRole::Guest && b2::DirectSummonTakeGather()) {
        // Playtest 1 #9: the host's room is in another map (or the host refused for it): this
        // guest's bell sign goes down (the sign path would summon it from here just the same) and
        // the gather below takes it to the host's lamp with a loading screen; it rings there.
        const bool dropped = RemoveSpEffect(kGuestBellEffect);
        st.rung_ever = false;
        Log("bell: the host is in another map: the bell goes down%s; gathering to the host first",
            dropped ? " (its sign withdrawn)" : "");
    }
    if (st.rung_ever && Seconds(st.last_ring, now) < st.ring_every) {
        return;
    }
    // C1 (party_start.h): no bell before this player finished its start (clinic / first death
    // pitfalls); the host rings only for members that are ready (roster Home / Joining).
    if (role == PartyRole::Guest) {
        if (StoryBusy()) {
            return; // C4: a cutscene / ending replay first, then the rejoin
        }
        const bool party_ready = !AnyMemberInPrologue(link->roster());
        if (st.start_ready && !party_ready && !OthersInPrologue().empty()) {
            // #19: no bell and no gather while the host (or another member) is in its prologue:
            // the gather's lamp warp took a guest out of its own first Dream into the clinic.
            static Clock::time_point said{};
            if (Seconds(said, now) >= 30.0) {
                said = now;
                Log("bell: waiting: %s in the prologue (summons start once everybody is ready)",
                    OthersInPrologue().c_str());
            }
        }
        if (st.self_unreachable && GuestMayRing(st.start_ready, link->state() == party::LinkState::Connected, party_ready)) {
            // No usable UDP endpoint (UdpReachTick): a summon could only time out.
            if (Seconds(st.reach_said, now) >= 30.0) {
                st.reach_said = now;
                Log("bell: not ringing: UDP to the host is blocked (no STUN answer yet; retrying)");
            }
            return;
        }
        if (GuestMayRing(st.start_ready, link->state() == party::LinkState::Connected, party_ready)) {
            // Not before the host's map is known (it is still loading): the gather below needs it,
            // and a guest that rang meanwhile was summoned at its own world's spot (the Hunter's
            // Dream) into a host standing in Central Yharnam (hostdeath spec1 / spec2).
            std::uint32_t host_map_now = 0;
            for (const party::RosterEntry& e : link->roster()) {
                if (e.slot == party::kHostSlot) host_map_now = e.map_id;
            }
            if (host_map_now == 0 || host_map_now == 0xffffffffu) {
                static Clock::time_point said{};
                if (Seconds(said, now) >= 10.0) {
                    said = now;
                    Log("bell: waiting for the host's map (the host is still loading) before ringing");
                }
                return;
            }
            // Playtest 1 #9: nor before this guest's OWN map is known (it reads 0xffffffff for a few
            // ticks after the world is up): the gather below then saw no map, the bell rang at once
            // and the direct summon joined the host's room from the Hunter's Dream without loading
            // the host's map.
            if (s.map_id == 0 || s.map_id == 0xffffffffu) {
                static Clock::time_point said{};
                if (Seconds(said, now) >= 10.0) {
                    said = now;
                    Log("bell: waiting for this guest's own map before ringing (the gather needs it)");
                }
                return;
            }
            if (GatherGuest(s, link, now)) {
                return; // travelling to the host's lamp in its own world first; ring there
            }
            Ring(st, kGuestBell, "guest, ready and idle in its own world", now);
        }
    } else {
        int max_players;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            max_players = st.max_players;
        }
        // Not while a summon our game asked for is under way (a bell then restarts its search
        // and the summon fails: the guest waits for the summon data until it gives up).
        if (st.summons_seen && Seconds(st.summon_at, now) < 20.0) {
            return;
        }
        // Members nobody can summon (UdpReachTick) do not count: no bell for them alone.
        const std::vector<party::RosterEntry> roster = SummonableRoster(st, link->roster());
        const bool may = link->state() == party::LinkState::Hosting &&
                         HostMayRing(st.start_ready, s.cooperators, max_players, roster);
        // A waiting member still in another map than ours (it gathers to our lamp first, with a
        // loading screen) is not waiting for this bell yet: the grace starts once it is here.
        bool member_elsewhere = false;
        for (const party::RosterEntry& e : roster) {
            if (e.slot != party::kHostSlot && e.connected &&
                (e.state == party::MemberState::Home || e.state == party::MemberState::Joining) &&
                dsummon::MapKnown(e.map_id) && dsummon::MapKnown(s.map_id) && !dsummon::SameMap(e.map_id, s.map_id)) {
                member_elsewhere = true;
            }
        }
        if (!may || member_elsewhere) {
            st.member_waiting = false;
            st.member_waiting_at = now;  // waited 0 s (HostBellHeld: within the grace)
        } else if (!st.member_waiting) {
            st.member_waiting = true;
            st.member_waiting_at = now;
        }
        if (may && b2::DirectSummonHoldsHostBell(Seconds(st.member_waiting_at, now))) {
            // Playtest 1 #2: the guest's own ring asks for this host's room (direct summon); the
            // host's bell (its effect showed on the host) is only the fallback after a failure.
            if (!st.bell_hold_logged) {
                st.bell_hold_logged = true;
                Log("bell: a party member waits; no Beckoning Bell while the direct summon handles it "
                    "(it rings as the fallback)");
            }
        } else if (may) {
            st.bell_hold_logged = false;
            Ring(st, kHostBell, "host ready, a ready party member waits", now);
        } else if (link->state() == party::LinkState::Hosting && st.start_ready && b2::TitleJoinPending() &&
                   s.cooperators + 1 < max_players && !AnyMemberInPrologue(link->roster()) &&
                   roster.size() == link->roster().size()) {
            // A guest at its title asks for this room (title join): the room comes with the bell.
            Ring(st, kHostBell, "host ready, a guest joins from its title", now);
        }
    }
}

std::string PartyLogStamp() {
    static const bool on = [] {
        const char* t = std::getenv("BB_PARTY_DIRECTOR_TEST");
        const char* l = std::getenv("BB_PARTY_LOG_TIME");
        return (t && t[0]) || (l && l[0] == '1');
    }();
    if (!on) {
        return {};
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char b[32];
    std::snprintf(b, sizeof b, "[%02d:%02d:%02d.%03d] ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return b;
}

/// Gather (BB_PARTY_GATHER, default on): before a guest rings, its own world goes to the host's
/// last lamp when the host's map (roster slot 0) is another area/block than the guest's own, so
/// the summon brings it into the host's map and "home" always follows the host (a fresh session,
/// or after any return home). The lamp id is the host's last lamp (party_phantom / travel
/// events: GuestDeathRedirectLamp). One gather per 90 s; logs why it does or does not run.
bool GatherGuest(const GameSnapshot& s, party::PartyLink* link, Clock::time_point now) {
    static const bool on = !(std::getenv("BB_PARTY_GATHER") && std::getenv("BB_PARTY_GATHER")[0] == '0');
    static Clock::time_point last{};
    static bool ever = false;
    if (!on || !link || !s.world_up || s.loading || s.session_role != RoleIdle) {
        return false;
    }
    std::uint32_t host_map = 0;
    for (const party::RosterEntry& e : link->roster()) {
        if (e.slot == party::kHostSlot) {
            host_map = e.map_id;
        }
    }
    // The host's last lamp unless it lies in another map than the one the host stands in (a fresh
    // host's default record): then a lamp of the host's map.
    std::uint32_t lamp = GatherLampFor(GuestDeathRedirectLamp(), host_map);
    {
        // The host's last lamp not known yet (a fresh session) or not in its map: the lamp of the
        // host's map nearest to where the host stands (its VITALS position), not simply the map's
        // first one (m24_01's first is the clinic's 1st Floor Sickroom).
        const std::uint32_t known = GuestDeathRedirectLamp();
        const bool usable = known != kTravelNone && known != 0 && ids::EntityMap(known) >> 16 == host_map >> 16;
        party::MemberVitals vit[party::kMaxVitals];
        const int vn = link->vitals(vit, party::kMaxVitals);
        for (int i = 0; i < vn && !usable; ++i) {
            if (vit[i].slot != party::kHostSlot || !(vit[i].flags & party::kVitalsHasPos)) {
                continue;
            }
            float best = 1e18f;
            for (const std::uint32_t id : ids::kPartyReturnPoints) {
                float sp[4];
                if (ids::EntityMap(id) >> 16 != host_map >> 16 || !ids::ReturnSpot(id, sp)) {
                    continue;
                }
                const float dx = sp[0] - vit[i].pos[0], dy = sp[1] - vit[i].pos[1], dz = sp[2] - vit[i].pos[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < best) {
                    best = d;
                    lamp = id;
                }
            }
        }
    }
    if (!host_map || host_map == 0xffffffffu || s.map_id == 0xffffffffu || lamp == kTravelNone || lamp == 0) {
        return false;
    }
    if ((host_map >> 16) == (s.map_id >> 16)) {
        // Same area / block: the summon lands in the host's map; step aside first (B2 spacing).
        return b2::GuestPreRing(s, host_map);
    }
    // The host in a Chalice Dungeon (or its last lamp in one): no gather. A warp into area 29 would
    // build this game's own slot's dungeon (or none, seen live); the summon itself brings the guest
    // in (quick join by ChannelId, docs/party/chalice.md 4), so it rings where it stands.
    if (((host_map >> 24) & 0xff) == 29 || lamp / 100000 == 29) {
        static Clock::time_point said{};
        if (Seconds(said, now) >= 60.0) {
            said = now;
            Log("gather: the host is in a chalice dungeon (%s, lamp %u); ringing here, the summon quick-joins it",
                MapName(host_map).c_str(), lamp);
        }
        return false;
    }
    // A5 (BB_PARTY_GATHER_DSUM, default on since 0.6.2; =0 the old gather): ring from the own map
    // and let the direct summon join the host's room from here. Its game does not load the host's
    // map (live a5 2026-10-13: still m21_00, "apart 1"); party_b2's CarryArrivalTick then runs the
    // B2 re-join into the host's map: one loading screen for the join instead of the gather's lamp
    // load plus the summon's. A failed direct summon (DirectSummonCarries false) gathers as before.
    static const bool carry = !(std::getenv("BB_PARTY_GATHER_DSUM") && std::getenv("BB_PARTY_GATHER_DSUM")[0] == '0');
    if (carry && b2::DirectSummonCarries() && !(ever && Seconds(last, now) < 90.0)) {
        static Clock::time_point said{};
        if (Seconds(said, now) >= 30.0) {
            said = now;
            Log("gather: own world %s, host in %s; no gather: the direct summon loads this guest into the host's map",
                MapName(s.map_id).c_str(), MapName(host_map).c_str());
        }
        return false;
    }
    // par_c6 (live): the guest gathered to the restarted host's Dream lamp while the host moved on
    // to Central Yharnam; arrived at the gather's map, it then waited out the rest of the 90 s
    // (83 s apart). Once the gather has loaded its target map (and the world is steady again), the
    // host having moved is a new gather after 10 s.
    static std::uint32_t target_map = 0xffffffffu;
    const bool arrived = target_map != 0xffffffffu && (s.map_id >> 16) == (target_map >> 16);
    if (ever && Seconds(last, now) < (arrived ? 10.0 : 90.0)) {
        // Still in another map than the host's while the gather's warp has not loaded yet: no
        // ring here (it would summon this guest at its own world's spot into the host's map).
        return true;
    }
    if (ever && arrived) {
        Log("gather: arrived at the last gather's map %s, the host has moved on to %s: gathering again",
            MapName(target_map).c_str(), MapName(host_map).c_str());
    }
    ever = true;
    last = now;
    target_map = ids::EntityMap(lamp);
    TravelIntent t;
    t.kind = TravelKind::Lamp;
    t.lamp_id = lamp;
    Log("gather: own world %s, host in %s; to the host's lamp %u before ringing", MapName(s.map_id).c_str(),
        MapName(host_map).c_str(), lamp);
    // No ring this tick either way: the warp may run through split travel's lamp interceptor and
    // report false while the load goes ahead (a bell rung now placed the sign before the gather's
    // load, hostdeath join2b); the next ring comes after the load (one gather per 90 s).
    if (!ReplayTravelNow(t)) {
        Log("gather: the replay reported no warp; ringing after the next steady world");
    }
    return true;
}

bool LocalStartReady() {
    return g_local_start_ready.load();
}

bool LocalStartInPrologue() {
    const StartStep step = static_cast<StartStep>(g_local_start_step.load());
    return step == StartStep::Opening || step == StartStep::Clinic || step == StartStep::FirstDream;
}

bool LocalPrologueHold() {
    return g_local_prologue.load();
}

bool SavedPrologue() {
    return LoadSavedPrologue();
}

bool PartyPrologueHold(const char** why) {
    if (g_local_prologue.load()) {
        if (why) *why = "this player is in its prologue";
        return true;
    }
    if (g_host_prologue.load()) {
        if (why) *why = "the host is in its prologue";
        return true;
    }
    if (why) *why = "";
    return false;
}

std::string OthersInPrologue() {
    std::lock_guard<std::mutex> lk(g_prologue_names_mu);
    return g_prologue_names;
}

bool MemberRoster(int slot, int* state, std::uint32_t* map) {
    *state = -1;
    *map = 0;
    party::PartyLink* link;
    {
        State& st = S();
        std::lock_guard<std::mutex> lk(st.mu);
        link = st.link;
    }
    if (!link) return false;
    for (const party::RosterEntry& e : link->roster()) {
        if (e.slot == slot) {
            *state = int(e.state);
            *map = e.map_id;
            return true;
        }
    }
    return false;
}

bool PartyRequested() {
    const char* t = std::getenv("BB_PARTY_DIRECTOR_TEST");
    const char* i = std::getenv("BB_PARTY_ITEMS_TEST"); // C3 single-instance check
    return bb_party_enabled() || (t && t[0]) || (i && i[0]) || NpcTestRequested() || StoryTestRequested() ||
           b2::Requested() || NewGameTestRequested();
}

void CoopTick() {
    using perf::Zone;
    perf::FrameBoundary(); // BB_PARTY_PERF=1: closes the last frame's per-zone sums
    perf::Scope total(Zone::Tick);
    perf::Scope z(Zone::LuaEvents);
    LuaEventsTick();
    z.Next(Zone::Director);
    PartyDirector::Get().Tick();
    chalice::Tick(PartyDirector::Get().Role() == PartyRole::Host); // the local dungeon, the party's chalices
    gestures::Tick(PartyDirector::Get().Role() == PartyRole::Host); // the party's gestures (party_gestures.h)
    z.Next(Zone::Travel);
    TravelTick(); // B1 guest replay (party_travel.h)
    lamptravel::Tick(); // direct lamp travel's warp menu choice (lamp_travel.h)
    z.Next(Zone::Story);
    StoryTick(); // C4 cutscenes / endings (party_story.h)
    z.Next(Zone::Items);
    ItemsTick();    // C3 guest item replay (party_items.h)
    GameSaveTick(); // save requests after summoned writes (game_state.h)
    z.Next(Zone::Seamless);
    SeamlessRulesTick(); // A6 param rules, EMEVD filter stats (seamless_rules.h)
    z.Next(Zone::Fourp);
    fourp::FourpTick(); // 4-player rules: the party's max players (party_fourp.h)
    plog::GameTick();   // party logging: snapshots, area events (party_log_game.h; cheap at Normal)
}

void PartyInit(unsigned char* image, std::uint64_t size) {
    perf::Init(); // BB_PARTY_PERF=1 (party_perf.h), before any hook goes in
    plog::GameInit(); // BB_PARTY_LOG: line tags, user-mark relay (party_log_game.h)
    if (!HooksInit(image, size)) {
        Log("no image; party director off");
        return;
    }
    LuaEventsInit();
    progress::Init();         // C2 progress sync (party_progress.h)
    progress::InstallHooks(); // byte-verified flag hooks (BB_PARTY_PROGRESS=0: none)
    chalice::InstallProbes(); // summon message log (party_chalice.h)
    OnlineDiagInit();    // FROM client drop-offline log (party_online.h)
    SeamlessRulesInit(); // A6: party patch report, EMEVD filter (seamless_rules.h)
    fourp::FourpInit();  // 4-player parties: H1-H4, E6, P5 (party_fourp.h)
    PartyDirector::Get().ConfigureFromEnv();
    if (const char* t = std::getenv("BB_PARTY_TRACE_BELL"); t && t[0] == '1') {
        InstallBellTrace(std::make_integer_sequence<int, sizeof(g_trace_points) / sizeof(g_trace_points[0])>{});
    }
    if (const char* t = std::getenv("BB_PARTY_TRACE_SSINFO"); t && t[0] == '1') {
        HookCallSite(kSsParseCall, kSsParse, reinterpret_cast<const void*>(&SsParseHook), "ss.info parse log");
    }
    InstallTravelPatches(); // B1: Dream gate + travel hooks (byte-verified)
    b2::Init();             // B2 seamless prototype (BB_PARTY_B2=1; byte-verified)
    split::Init();          // split travel's "Travel with the party?" (needs B2)
    InstallStoryHooks();    // C4: bank-2002 capture, guest mirror / replay (byte-verified)
    InstallItemsPatches();  // C3: award hook, parity patches (byte-verified)
    PhantomInit();          // guest respawn at the host's lamp, refill, boss Insight
    npctalk::Init();        // shared NPC dialogue: talk line / menu observers (party_npctalk.h)
    worldact::Init();       // guest world actions, instanced treasure pickups (byte-verified)
    GuestServicesInit();    // guest Doll / shops / workshop in the host's Dream (byte-verified)
    balance::Init();        // C4 rot hook, D7 scaling (party_balance.h)
    enemyai::Init();        // C2 / split travel: a lone guest's enemies run on its own AI (party_enemyai.h)
    npcfight::Init();       // D11: NPC hunter fights with the whole party (party_npcfight.h)
    g_tick_installed = HookPrologue(kFlipperUpdate,
                                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                     0x83, 0xec, 0x38},
                                    &FlipperEntry, "main-thread tick (SprjFlipper::Update)");
    if (!g_tick_installed) {
        Log("no main-thread tick: the director and Lua events are off");
        return;
    }
    if (S().test.on && S().test.log_state) {
        // The game's own Lua events: their thread (the tick's?) and what follows a bell.
        HookPrologue(kDispatchByName, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &DispatchEntry, "Lua event log (LuaEvent_DispatchByName)");
    }
}

} // namespace coop
