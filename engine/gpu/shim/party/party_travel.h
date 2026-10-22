// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel, phase B1 "follow and rejoin" (docs/party/travel.md sections 1-3): when the host
// warps (lamp, Hunter's Dream, headstone, scripted transition, Lua stage warp, death, Hunter's
// Mark), every guest warps to the same place in its own world; the director's bells then bring
// the party together again there.
//
//   host:  the travel funnel SessionWorldTransition 0x13CDE30 (all 19 stage changes go through
//          it) is replaced by a wrapper that reads the caller (return address, table 1.6) and the
//          WorldTransitionState (slot 0x5556678) fields the caller already wrote, and queues a
//          TravelIntent. The lamp warp 0x13CDF30 entry records its key (edi). Hunter's Mark is
//          caught at OnReviveMagic 0x1389D20 (it leaves the session before it reaches the funnel
//          through a tail jump). The director polls PopHostTravel() and sends the intent over
//          PartyLink (EVENT "travel", TravelToJson); nothing here sends anything.
//   guest: RequestGuestTravel() (any thread) keeps the newest intent by seq; TravelTick() (main
//          thread, the coop tick) replays it once the world is up and no load or stage request
//          is pending: 0x13CDF30(id) for lamps / deaths / Hunter's Mark, 0x132E010 / 0x132E050
//          for scripted and Lua stage warps, the forced-placement setters for an exact transform.
//          While a travel is pending or under way, the stock "send home" warps (OnLeave_Limit
//          0x138A51C, HostDead_1 0x1381A93, BlockClear2_1 0x13856F4, BlockClear2_3's tail jump
//          0x13859DF) go to that destination instead of the guest's pre-summon spot.
//          A guest's own death (PartyGhostDeath_2's call 0x1382663) and a guest's Hunter's Mark
//          (OnReviveMagic_1's tail jump 0x1389F20) go to the pending travel destination, else to
//          the host's last lamp (SetGuestDeathRedirect; party_phantom.h keeps it current), so the
//          guest respawns where the host will be and the director's bells bring it back.
//   both:  InstallTravelPatches(): the hooks above plus the Hunter's Dream gate (area table dword
//          0x47304B0 2100 -> -1, Small Resonant Bell availability 0x157F6D1 / 0x157F6D3), each
//          byte-verified; a mismatch is logged and that site skipped.
//
// Env: BB_PARTY_TRAVEL=0 (no travel hooks, no replay), BB_PARTY_DREAM_GATE=0 (no Dream patches).
//
// The pure part (kinds, JSON, queues, the replay gate) has no game dependency and is unit-tested
// (tests/test_party_travel.cpp, built with BB_PARTY_TRAVEL_NO_GAME).
#pragma once

#include "net/json.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace coop {

enum class TravelKind : std::uint8_t {
    Unknown = 0,
    Lamp,          ///< 0x13CDF30 (EMEVD 2003[49], warp menu): lamp, Hunter's Dream, headstone
    ScriptedWarp,  ///< EMEVD 2003[14] Warp Player: packed map + warp point
    LuaStageWarp,  ///< WarpNextStage 0x132E010: packed map + warp point
    LuaBonfireWarp,///< WarpNextStage_Bonfire 0x132E050 / Lua_Warp_1 0x138C570: bonfire id (+0x10)
    LuaStageKick,  ///< WarpNextStageKick 0x1332B80: whatever +0x0C / +0x10 hold
    HostDeath,     ///< SoloPlayDeath_2: respawn at the last lamp (+0x1528)
    HuntersMark,   ///< OnReviveMagic: back to the last lamp (+0x1528)
    Transform,     ///< exact map + position + rotation (forced placement); no native id
    // Guest-side funnel callers (never broadcast; the send-home retarget sites):
    GuestSentHome, ///< OnLeave_Limit / HostDead_1
    GuestDied,     ///< PartyGhostDeath_2
    BossCleared,   ///< BlockClear2_1 / BlockClear2_3
    MissionOrPk,   ///< Failed_BossAreaMission_LeaveMap / PlayerKill_4030_1
    TitleOrDebug,  ///< 0x132E0C0 (map 0x01000000), 0x1317550 debug presets
};
const char* TravelKindName(TravelKind k);
/// Plain text for the player (the travel toast / last event), never the internal name:
/// "Respawning with the party" for a host death, "Travelling with the party" by default.
const char* TravelKindLabel(TravelKind k);
/// The name back; Unknown for anything else.
TravelKind TravelKindFromName(const std::string& name);
/// The kind of a direct `call 0x13CDE30` from its call address (return address - 5), table 1.6.
/// Tail jumps (0x13859DF, 0x1389F20, 0x1383A5C, 0x13175EF) cannot be told by their return
/// address; they map here too for completeness, but the hooks identify them otherwise.
TravelKind TravelKindFromCallSite(std::uint64_t call_site);
/// The host broadcasts this kind (the guest-side and title/debug kinds stay local).
bool TravelKindBroadcast(TravelKind k);

constexpr std::uint32_t kTravelNone = 0xffffffffu;

struct TravelIntent {
    std::uint64_t seq = 0;          ///< host's counter; guests keep only the newest
    TravelKind kind = TravelKind::Unknown;
    std::uint32_t call_site = 0;    ///< our offset of the funnel call (diagnostics)
    std::uint32_t lamp_id = kTravelNone;   ///< the id to replay through 0x13CDF30
    std::uint32_t packed_map = kTravelNone;///< +0x0C: area << 24 | block << 16 | region << 8 | index
    std::uint32_t warp_point = kTravelNone;///< +0x10: warp point / bonfire id
    std::uint32_t respawn_mode = 0;        ///< +0x1538: 1 world, 2 Hunter's Dream
    std::uint64_t respawn_record = ~0ull;  ///< +0x153C {id, aux}
    std::uint64_t last_lamp = ~0ull;       ///< +0x1528 {id, aux}
    bool has_pos = false;                  ///< pos_map / pos / rot below are valid
    std::uint32_t pos_map = kTravelNone;
    float pos[4] = {0, 0, 0, 0};
    float rot[4] = {0, 0, 0, 0};
    /// The host's Chalice Dungeon (party_chalice.h ChannelId) when the destination is inside one
    /// (TravelIntoChalice); 0 otherwise.
    std::uint64_t chalice_channel = 0;
    /// The traveller goes alone (docs/party/split_travel.md): the other members stay where they
    /// are and do not load. False (the default, also when a peer omits it): the party travels
    /// together (B2 follow).
    bool alone = false;

    std::uint32_t Area() const { return (packed_map >> 24) & 0xff; }
    std::uint32_t Block() const { return (packed_map >> 16) & 0xff; }
    std::uint32_t Region() const { return (packed_map >> 8) & 0xff; }
    std::uint32_t Index() const { return packed_map & 0xff; }
};
/// One line for logs.
std::string DescribeTravel(const TravelIntent& t);

// ---- PartyLink EVENT "travel" payload ----
constexpr const char* kTravelEventName = "travel";
json::Value TravelToJson(const TravelIntent& t);
/// False (and *error, when given) on a missing / malformed member; *out is untouched then.
bool TravelFromJson(const json::Value& v, TravelIntent* out, std::string* error = nullptr);
std::string TravelToJsonText(const TravelIntent& t);
bool TravelFromJsonText(const std::string& text, TravelIntent* out, std::string* error = nullptr);

// ---- Peer validation (bbport security pass) ----
/// Peer ids are checked against the game's own tables (party_ids.h, generated from the game
/// data by tools/party/ids_tool.py), not by shape.
/// An existing map: area << 24 | block << 16 | region << 8 | index (a packed map, a pos map).
bool TravelMapKnown(std::uint32_t packed_map);
/// A ReturnPointParam ("WarpParam") row 0x13CDF30 can warp to: lamp, headstone, last lamp.
bool TravelLampKnown(std::uint32_t id);
/// A bonfire / warp point id 0x132E050 can take: an entity id AABnnnn of map mAA_0B.
bool TravelBonfireKnown(std::uint32_t id);
/// A host's travel intent before a guest replays it: false (with `why`) for a kind the host
/// never broadcasts; a lamp / record id that is no ReturnPointParam row, a packed map that is no
/// game map, a bonfire id that is no entity of its map and a stage warp point that is not in the
/// destination map become kTravelNone; a transform with an unknown map or non-finite /
/// far-out coordinates is dropped (has_pos false). The guest's game is then only asked to warp
/// somewhere its own data names.
bool SanitizePeerTravel(TravelIntent* t, std::string* why = nullptr);

// ---- Chalice Dungeons (docs/party/chalice.md) ----
/// The destination is inside a Chalice Dungeon (area 29): the map, the exact-position map, the
/// lamp (ReturnPointParam 29xxxxx) or the bonfire / warp point entity.
bool TravelIntoChalice(const TravelIntent& t);
/// A guest replays a warp into a Chalice Dungeon only into the dungeon its own game has in its
/// current chalice slot (`local_channel`): the layout is generated locally from that slot, so a
/// warp into another one (or none) would load a different dungeon. The guest joins the host's
/// dungeon at the altar first (the channel board's glyph / random join).
bool ChaliceReplayAllowed(const TravelIntent& t, std::uint64_t local_channel);

// ---- How a guest replays an intent ----
enum class ReplayMethod : std::uint8_t {
    None,         ///< nothing usable: leave the game's own warp alone
    LampWarp,     ///< 0x13CDF30(id)
    StageWarp,    ///< 0x132E010(0, area, block, region, index, warp point)
    BonfireWarp,  ///< 0x132E050(0, id)
    Transform,    ///< 0x156CF10/20/40/60, 0x1332BC0(0, -1), 0x13CDE30()
};
const char* ReplayMethodName(ReplayMethod m);
struct ReplayPlan {
    ReplayMethod method = ReplayMethod::None;
    std::uint32_t id = kTravelNone; ///< LampWarp / BonfireWarp
};
/// The native path for `t` (the exact transform as the fallback when the kind's own id is missing).
ReplayPlan ChooseReplay(const TravelIntent& t);

// ---- Host queue (the funnel hook pushes, the director pops) ----
class HostTravelQueue {
public:
    static constexpr std::size_t kMax = 16;
    /// Drops `t` when its seq is not newer than the last one pushed; drops the oldest when full.
    bool Push(const TravelIntent& t);
    bool Pop(TravelIntent* out);
    std::size_t Size() const { return q_.size(); }

private:
    std::deque<TravelIntent> q_;
    std::uint64_t last_seq_ = 0;
};

// ---- Guest state: the newest intent, the replay gate, the "travel under way" window ----
// ---- Guest death / guest Hunter's Mark redirect (docs/party/phantom_limits.md 3.2) ----
enum class GuestRedirect : std::uint8_t {
    None,              ///< leave the game's own warp (home, the pre-summon spot)
    TravelDestination, ///< a host travel is pending or under way: go there
    HostLamp,          ///< 0x13CDF30(host's last lamp id)
};
const char* GuestRedirectName(GuestRedirect r);
/// What a guest-side funnel call of `kind` (GuestDied, HuntersMark) does; the send-home and other
/// kinds are not redirected here. `have_travel_destination`: GuestTravel::Destination() is set.
GuestRedirect ChooseGuestRedirect(TravelKind kind, bool have_travel_destination, std::uint32_t host_lamp_id);
/// The host's last lamp id (low dword of its WorldTransitionState +0x1528) from an intent;
/// kTravelNone when it has none.
std::uint32_t HostLampFromTravel(const TravelIntent& t);
/// Where a guest gathers before its summon: the host's last lamp when that lamp is in the map
/// (area / block) the host stands in, else a lamp of the host's map (the first ReturnPointParam
/// row there), else the last lamp as it is. `host_map` packed (AA BB CC DD; 0 / all ones unknown).
/// A fresh character's last-lamp record is a default in Central Yharnam while it stands in the
/// Hunter's Dream (campaign_soak.md open 1).
std::uint32_t GatherLampFor(std::uint32_t host_lamp, std::uint32_t host_map);

/// What the gate needs from the game (injectable for tests).
struct ReplayState {
    bool world_up = false;
    bool loading = false;
    bool transition_requested = false; ///< WorldTransitionState +0x08
    int session_role = -1;             ///< SprjSessionManager +0x124
};
/// World up, no loading screen, no stage request pending, not joining (4) or leaving (7).
bool ReplayAllowedNow(const ReplayState& s);

// ---- Who follows a host travel (docs/party/ERSC_PARITY.md B3) ----
/// Only members in the host's session follow its travel: a summoned guest (session role 6), one
/// on its way in (joining, 4), or one that left the host's world moments ago (sent home by the
/// travel itself, a B2 fallback: `client_ago` s since it last was a client, < 0 never). A member
/// still in its prologue (party start not READY) or playing alone in its own world without being
/// summoned stays where it is: no load, no warp (live: a guest in the Dream was warped into the
/// clinic by the host's prologue lamp; a host's prologue death sent an unsummoned guest home).
enum class TravelFollow : std::uint8_t {
    Follow,
    IgnorePrologue, ///< party start not READY: in its own prologue
    IgnoreOwnWorld, ///< ready, but alone in its own world (not summoned)
};
const char* TravelFollowName(TravelFollow f);
struct FollowState {
    bool start_ready = true;  ///< party_start READY (the director's UpdateStart)
    int session_role = -1;    ///< SprjSessionManager +0x124
    double client_ago = -1;   ///< s since the session role last was client (6); < 0 never
};
constexpr double kFollowClientWindow = 60.0; ///< s after leaving the host's world that still follow
TravelFollow GuestFollowsHostTravel(const FollowState& s);

// ---- The host's empty-room check after its travel (HostCloseEmptyRoom) ----
/// Playtest 1 (H1:4389, H3:2654): "no arrival within 180 s" came at once when a guest restarted,
/// because the travel mark stayed set while B2 kept the room (the check is skipped then) and the
/// 180 s had long passed. While B2 holds the room its re-join owns that travel: the mark is
/// dropped once the host is back in its world and the re-join is over; the 180 s count only runs
/// while nobody holds the room.
enum class EmptyRoomStep : std::uint8_t {
    None,     ///< nothing to do now
    Drop,     ///< forget the travel (B2's re-join took the room through it)
    Timeout,  ///< 180 s without an arrival: leave the room as it is
    Check,    ///< in the new world: close the room if it is empty
};
struct EmptyRoomView {
    double since_travel = -1;   ///< s since the host's last broadcast travel; < 0 none
    bool load_seen = false;     ///< its loading screen came
    bool in_world = false;      ///< world up, no loading screen
    bool b2_holds = false;      ///< B2 keeps the room (a member's task exists)
    bool b2_rejoin = false;     ///< B2's re-join after the load still runs
};
constexpr double kEmptyRoomTimeout = 180.0;
EmptyRoomStep PlanEmptyRoom(const EmptyRoomView& v);

class GuestTravel {
public:
    static constexpr int kStableTicks = 30;          ///< frames the gate must hold before a replay
    static constexpr double kPendingTimeout = 180.0; ///< s: a pending intent nobody could replay
    static constexpr double kActiveTimeout = 120.0;  ///< s: replayed, the load never finished

    enum class Phase { Idle, Pending, Underway };
    /// False when `t.seq` is not newer than the newest seen (duplicate / out of order).
    bool Offer(const TravelIntent& t, double now);
    /// Once a frame. Returns the intent to replay now (then Underway), else nothing.
    std::optional<TravelIntent> Step(const ReplayState& s, double now);
    /// The destination the send-home retarget may use (Pending or Underway).
    std::optional<TravelIntent> Destination() const;
    /// The send-home retarget replayed it itself: no second replay from Step.
    void MarkReplayed(double now);
    /// Forget the current travel (a guest it does not concern).
    void Drop() { phase_ = Phase::Idle; }
    Phase GetPhase() const { return phase_; }
    std::uint64_t LastSeq() const { return last_seq_; }

private:
    Phase phase_ = Phase::Idle;
    TravelIntent cur_{};
    std::uint64_t last_seq_ = 0;
    double since_ = 0;
    int stable_ = 0;
    bool load_seen_ = false;
};

#ifndef BB_PARTY_TRAVEL_NO_GAME
/// bbgpu_patch_image, once, after coop::HooksInit (party mode only): the Dream gate patches and
/// the travel hooks, each byte-verified (logged and skipped on a mismatch).
void InstallTravelPatches();
/// Host: the next travel to broadcast (any thread).
bool PopHostTravel(TravelIntent* out);
/// Guest: the host's travel (any thread; the PartyLink callback).
void RequestGuestTravel(const TravelIntent& t);
/// Guest: notes the host's chalice dungeon and says whether this game stays out of the host's
/// travel into a Chalice Dungeon (one it does not have, or one entered from outside: the summon
/// brings it in). Both travel paths ask it (RequestGuestTravel and b2::OnHostTravel).
bool GuestSkipsChaliceTravel(const TravelIntent& t);
/// Guest: the host's last travel went into a Chalice Dungeon this game does not have in its
/// current slot (only a summon brings it in, from its own world).
bool HostInOtherChalice();
/// Main thread, once a frame (the coop tick): the guest replay.
void TravelTick();
/// Main thread: replays `t` now with the native warp ChooseReplay picks (B2 drives this itself,
/// party_b2.h). False when the intent has no usable destination or travel is off.
bool ReplayTravelNow(const TravelIntent& t);
/// Guest: a veto on the stock send-home warps (B2 keeps a guest in the session through a host's
/// death). `fn(site)` true = the send-home at `site` is dropped (nothing runs). Any thread.
using SendHomeVetoFn = bool (*)(std::uint64_t site);
void SetSendHomeVeto(SendHomeVetoFn fn);
/// True while the veto runs for the player's own Leave (the party's AllowSendHome before
/// OnLeave_Limit), false for the game's own send-home (room gone, irregular leave). Any thread.
bool SendHomeVetoForOwnLeave();
/// The next send-home (a guest's Leave: OnLeave_Limit called by the party itself) goes home as
/// in vanilla: no party travel retarget, no B2 veto.
void AllowNextSendHome();
/// Guest: a hold on its own death's funnel call (PartyGhostDeath_2 0x1382663). `fn()` true = held:
/// nothing runs now; the holder later either drops it (the host's travel brings the guest along)
/// or runs GuestDeathRedirectNow(). Game thread.
using GuestDeathHoldFn = bool (*)();
void SetGuestDeathHold(GuestDeathHoldFn fn);
/// Host: an interceptor of its own death's funnel call (SoloPlayDeath_2 0x1381EDE). `fn()` true =
/// taken: no capture, no broadcast, no stage request (B2 revives the host in place, party_b2.h);
/// the holder may later run HostDeathFunnelNow(). Game thread.
using HostDeathInterceptFn = bool (*)();
void SetHostDeathIntercept(HostDeathInterceptFn fn);
/// A taken host death carried out the vanilla way now (main thread): the travel captured and
/// broadcast (the party follows) and the stock funnel (the respawn load).
void HostDeathFunnelNow();
/// A held guest death, carried out now (main thread): the redirect to the host's last lamp, or
/// the stock funnel (home) when there is none.
void GuestDeathRedirectNow();
/// The travel hooks are installed and on (BB_PARTY_TRAVEL is not 0 and the funnel hook is in).
bool TravelEnabled();
/// The director's party start readiness (party_start.h StartReady), for GuestFollowsHostTravel.
void SetLocalStartReady(bool ready);
/// Host, main thread: a load of its own world at an exact transform (map, position, rotation:
/// where it stands), broadcast as a Transform travel so the guests in this world follow with one
/// load (a guest's rest resets the host's world, playtest 1 bug 3). False: not the host in its own
/// world, a stage request already armed, or travel off.
bool HostTravelTransformNow(std::uint32_t map, const float pos[4], const float rot[4], const char* why);
/// Test fixtures (BB_PARTY_DIRECTOR_TEST place=...): warps the local player to an exact transform
/// through the forced-placement sequence (main thread, world up). False when the map is unknown.
bool TravelPlaceForTest(std::uint32_t map, const float pos[3], float yaw);
/// Test fixtures (warp=MAP:POINT): a stage warp 0x132E010 to warp point / entity `point` of `map`.
bool TravelWarpForTest(std::uint32_t map, std::uint32_t point);
/// Guest: the host's last lamp id for the death / Hunter's Mark redirect (kTravelNone: no redirect,
/// the guest goes home as in vanilla). Any thread. A travel intent with a last-lamp record also
/// updates it.
void SetGuestDeathRedirect(std::uint32_t host_lamp_id);
std::uint32_t GuestDeathRedirectLamp();
/// Guest: runs (on the game thread that died / used the Mark) after a redirect was carried out.
using GuestRedirectFn = void (*)(TravelKind kind, GuestRedirect how, std::uint32_t lamp_id);
void SetGuestRedirectCallback(GuestRedirectFn fn);
/// BB_PARTY_GUEST_RESPAWN=0: guest deaths and Hunter's Marks go home as in vanilla.
void SetGuestDeathRedirectEnabled(bool on);
/// Runs on the game thread for every lamp / headstone warp 0x13CDF30(id) the game itself starts
/// (not our replays); true = consumed: the warp does not happen (party_guestsvc: a guest's lamp
/// use becomes a request to the host).
using LampWarpInterceptor = bool (*)(std::uint32_t id);
void SetLampWarpInterceptor(LampWarpInterceptor fn);
/// The game's call chain of the lamp warp being intercepted now (return addresses as guest offsets,
/// innermost first, 0 = unknown): who started it (split travel's diagnostics, bug 20).
void LampWarpCallers(std::uint64_t out[6]);
/// Runs on the game thread for every lamp / headstone warp 0x13CDF30(id) the game itself starts
/// (not our replays), before the interceptor; observes only (party_balance: a lamp rest cleanses
/// the death debuff). One observer; nullptr removes it.
using LampUseObserver = void (*)(std::uint32_t id);
void SetLampUseObserver(LampUseObserver fn);
/// The native lamp warp 0x13CDF30(id) (main thread; the host's travel hooks see it as usual).
void RunLampWarp(std::uint32_t id);
/// The same for a warp that is no lamp use of the player (a title-joined guest going home at the
/// session's end, the fall recovery): the LampUseObserver does not see it (no rest, playtest 1 #21).
/// `why` (a string literal) is logged.
void RunLampWarpNotRest(std::uint32_t id, const char* why);
/// The same, decided already (split travel: the player chose alone / with the party): the lamp
/// interceptor is not asked again. On the host `alone` marks the captured travel
/// (TravelIntent::alone: the guests stay).
void RunLampWarpDecided(std::uint32_t id, bool alone);
/// Host: the party's travel to `id` (decided, never alone) that is no lamp use (no rest): the
/// Snatcher's kidnap of a guest takes the party to the Gaol (party_kidnap_rules.h). `why` a literal.
void RunLampWarpPartyNotRest(std::uint32_t id, const char* why);
/// Host: the next travel the funnel captures is alone (split travel; cleared by the capture).
void SetNextHostTravelAlone(bool alone);
/// Host: every captured host death (TravelKind::HostDeath) is an alone travel (the guests play
/// on; the hostdeath rule "everyone respawns at their own last lamp"). Any thread.
void SetHostDeathAlone(bool alone);
/// Host: seconds since the funnel captured an alone travel (1e9: never). B2 uses it to tell an
/// alone load (no snapshot re-send, nobody to wait for).
double LastAloneCaptureAgo();
/// The Hunter's Mark item reaching the funnel (site 0x18FFA1D, any role; not our replays):
/// `fn(last lamp id)` true = consumed (no stage request; split travel asks, then runs a decided
/// lamp warp to that lamp). Game thread.
using MarkInterceptor = bool (*)(std::uint32_t last_lamp);
/// Every lamp warp 0x13CDF30(id) that is not our replay, after the interceptor: `fn(id, alone)`
/// true = done without a load (the lamp is in the loaded map: an in-world move). Game thread.
using SameMapWarpFn = bool (*)(std::uint32_t id, bool alone);
void SetSameMapWarp(SameMapWarpFn fn);
/// A scripted stage warp (EMEVD 2003[14] / Lua WarpNextStage) reaching the funnel with GSM +0x0C the
/// packed map and +0x10 the warp point: `fn(map, point)` true = done without a load (an in-world
/// move to the point inside the loaded map). Game thread.
using SameMapStageFn = bool (*)(std::uint32_t map, std::uint32_t point);
void SetSameMapStage(SameMapStageFn fn);
void SetMarkInterceptor(MarkInterceptor fn);
/// The lamp warp 0x13CDF30(id) as a travel replay (main thread): the travel capture, the guest
/// lamp interceptor and the send-home retarget stand aside, the forced placement is cleared. On
/// a summoned guest it leaves the host's world and loads the own one (C4's ending replay).
void RunLampWarpReplay(std::uint32_t id);
#endif

} // namespace coop
