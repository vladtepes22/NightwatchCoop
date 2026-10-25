// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel, phase B2 "true seamless" PROTOTYPE (docs/party/b2_seamless.md): the session and
// the remote players survive a host map load. Off unless BB_PARTY_B2=1.
//
// What it does (all sites byte-verified before anything is written; our offsets):
//   K1  0x19471B1 call BeginLeaveSession (stage payload dtor 0x19470F0): no leave while a session
//       is up (role 3/6). Skipped when the XML "Keep session on map reload" retarget is applied.
//   K2  0x1947207 call 0x1E54830 (CSMultiPlayMan reset, same dtor): the human
//       CSMultiPlayerInsTasks are kept (the NPC tasks are still destroyed, as the reset does).
//   K3  0x193C657 call BeginLeaveSession / K4 0x193C689 call 0x15BED10 (in-game stage update,
//       stage-record UID changed): no leave, no presentation reset while a session is up.
//   K5  0x13809B7 call BeginLeaveSession (Lua OnMatchingCheck: a member's chr is missing):
//       suppressed while a B2 re-join is in flight.
//   K5b 0x1380A65 call BeginLeaveSession (Lua OnMatchingError: the host's check missed this
//       guest's chr under its player number): ignored for 10 s after the guest changed its number.
//   K6  0x178524F call 0x1317780 (Lua OnBeJoinStart_* after the host's flag snapshot): suppressed
//       while a guest B2 re-join is in flight (the vanilla SummonReloadStart would overwrite the
//       guest's own-world return snapshot 0x16 90/B0/D0 and start its own warp).
//   K7  0x13CDEAE `jne` -> `jmp` in SessionWorldTransition 0x13CDE30, only around the guest's own
//       B2 warp (main thread, written and restored around the call): the guest keeps the room.
//   Driver (main thread, the coop tick):
//     host  after each own load with a session up: per member of the network flow's member list
//           (flow 0x553D6D0 +0x68) re-sends the join snapshot: packet 0x19, 0x10 flags
//           (0x1786050), 0x11 world snapshot (0x1786360), bullets (0x1A04190), the WorldChrMan
//           net-sync queues (0x18F0DB0, 0x18F0E80), NPC summons (0x15BD0E0), packet 0x42.
//           On a guest's "arrived" event it rewinds every remote CSMultiPlayerInsTask whose
//           PlayerIns is gone to STEP_Create (+0x54 = 4) with the create bit (+0xED |= 1).
//     guest on the host's travel event: flow +0x84 = 1 (awaiting the snapshot); when the game
//           applied the host's flags (+0x84 = 2) it sets +0x84 = 3 (host-world load) and replays
//           the host's destination (party_travel ReplayTravelNow) under K7; after the load
//           (+0x84 = 4, set by 0x178D9C0) it rewinds its remote tasks and sends "arrived".
//   Probe (always with BB_PARTY_B2=1): one "B2 load report" line per load: local PlayerIns
//           pointer before / after, CSMultiPlayMan human / NPC tasks (step, mode, flags), flow
//           state, session role - what single-instance runs can check.
//
// Env: default ON in party mode (BB_PARTY); BB_PARTY_B2=0 off (B1 only), =1 on without BB_PARTY;
//      any failed / timed-out guest step falls back to B1 (FallbackToB1); BB_PARTY_B2_FORCE=1 treats "no session" as a session for K2 (to
//      exercise the keep path offline with NPC tasks, single-instance test only).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop {
struct TravelIntent;
struct GameSnapshot;
} // namespace coop

namespace coop::b2 {

constexpr const char* kEventName = "b2";

bool Requested();
/// F5: true once after the host's game ended the co-op session by itself (BeginLeaveSession from the
/// OnIrregularLeaveSession sites: its send queue past 1000 messages 0x178980d, the receive stall
/// 0x1789cb4).
bool TakeHostIrregularLeave();
/// PartyInit, after InstallTravelPatches (bbgpu_patch_image, game not running yet).
void Init();
bool Enabled();
/// The local player stands in another player's world (WorldTransitionState +0x1592 == 0: summoned,
/// or a guest's alone loads in the host's world). Its event flags are then that world's, not its save's.
bool InOtherWorld();
/// Host: a member task (mode 1) exists - a guest is still in the session (main thread).
bool HostHasMemberTasks();
/// Guest, own world, before ringing (main thread): steps aside from the host's spot so the join
/// placement is beside the host. True while it is still moving (do not ring yet).
bool GuestPreRing(const GameSnapshot& s, std::uint32_t host_map);
/// The local PartyLink slot (the director, every tick): guests stand beside the host by slot.
void SetLocalSlot(int slot);
/// Main thread, once a frame (the director tick).
void Tick(const GameSnapshot& s);
/// H5: the local player's appearance changed (the Dream mirror's editor closed; main thread): its
/// record goes to the members again and they rebuild its chr (no load).
void NoteOwnAppearanceChanged();
/// Guest: the host's travel event. True when B2 takes it (B1's replay must not run it).
bool OnHostTravel(const TravelIntent& t);
/// A PartyLink EVENT "b2" from `from_slot` (any thread).
void OnLinkEvent(int from_slot, const std::string& body);
/// Direct summon (party_dsummon.h): the guest rang its bell; the online id of a link slot.
void DirectSummonRing();
void DirectSummonName(int slot, const std::string& name);
/// Host, A11: its boss fight / cutscene hold started, ended or is re-announced: the party status
/// line on both sides ("Waiting: the host is in a boss fight") and a dsum_busy event to the guests.
void HostBusyChanged(bool boss_bar_up, bool cutscene_playing);
/// Test aid (guest_die:kidnap): what a guest's copy of the Snatcher's grab event does at the grab
/// (party_kidnap_rules.h SkipTrigger: the host is asked for the party's kidnap); the death follows.
void TestGuestKidnapGrab();
/// An event to send (the director's tick drains it): slot -1 = to the host / broadcast.
bool PopOutgoing(int* slot, std::string* body);

// ---- Title join (BB_PARTY_TITLE_JOIN; docs/party/join_timing.md) ----
/// The guest joins the host's session from the title and Continue loads the host's world.
bool TitleJoinRequested();
/// Guest at the title, before AutoContinue presses Continue (main thread): true while the title
/// join runs (hold Continue); false to press (joined and ready, failed, or off).
bool TitleJoinHold(const GameSnapshot& s);
/// Host: a guest at its title waits for this host's room (the director rings for it).
bool TitleJoinPending();
/// Guest (A5, BB_PARTY_GATHER_DSUM experiment): the direct summon is on and its last attempt did
/// not fail (dsummon::GatherFirst).
bool DirectSummonCarries();
/// Guest (playtest 1 #9): the last direct summon failed because this guest is in another map than
/// the host's room: true once (the director drops the bell and gathers with a loading screen).
bool DirectSummonTakeGather();
/// Host (playtest 1 #2): the Beckoning Bell stays down now (dsummon::HostBellHeld): a member has
/// waited `waiting_s`; a room is on its way, a guest just joined, or its own ring comes first.
bool DirectSummonHoldsHostBell(double waiting_s);
/// Host: its AutoContinue is on the way into the world (a waiting title-join guest keeps waiting).
void SetTitleContinuing(bool on);

// ---- Split travel (docs/party/split_travel.md) ----
/// Where a member is (the party link's vitals): the director passes every member each tick.
struct Where {
    int slot = -1;
    bool connected = true;
    std::uint32_t map = 0;  ///< 0xAABBCCDD, 0 unknown
    bool has_pos = false;
    float pos[3] = {0, 0, 0};
    std::uint32_t age_ms = 0;
};
/// Main thread (the director, every tick, before Tick).
void SetWhere(const std::vector<Where>& members);
/// Main thread: member `slot`'s last vitals (as SetWhere got them). False: not known.
bool MemberWhere(int slot, Where* out);
/// Why a member travels alone (logs; Death keeps the player's own respawn records).
/// Join (A5): a cross-map direct summon joined the host's room from another map; the one load
/// into the host's map (its lamp nearest the host; the own last lamp is not changed).
/// Gather (I5): the host gathered the party and this guest said YES from another map.
enum class AloneWhy { Lamp, Headstone, HuntersMark, Death, Join, Gather };
const char* AloneWhyName(AloneWhy w);
/// Guest, main thread: loads t's destination ALONE and stays in the session; the other members
/// do not load. Destination in the host's area: a B2 re-join (the host re-sends its snapshot; one
/// load for this guest only) at the destination itself; elsewhere: an alone load in host-world
/// mode (no snapshot; flags from the mirror). False: not possible now (no session, a re-join
/// running, no destination) - the caller keeps the old behaviour.
/// `rest_lamp` (a rest at a lamp, pt1polish): into the host's area the rest rides on the re-join
/// (the host's rest load first, then this guest's one load); *rest_carried says it did - otherwise
/// the caller sends its rest request the usual way.
bool TravelAlone(const TravelIntent& t, AloneWhy why, std::uint32_t rest_lamp = 0, bool* rest_carried = nullptr);
/// Guest, main thread, dead: respawns at its OWN last lamp (GSM +0x1528) as an alone travel
/// (TravelAlone with AloneWhy::Death): in the host's area a re-join, elsewhere an alone load.
bool RespawnAloneAtOwnLamp();
/// Guest, main thread, alive (D3, pard4): an alone travel to the HOST's last lamp (its own when the
/// host's is not known; AloneWhy::Lamp, no rest): a vanilla loading screen out of a boss fight that
/// cannot run here (the host is in another area) to where the party is. `why` goes to the log.
bool ReloadAtHostLamp(const char* why);
/// An intent's destination map (packed; the lamp / last-lamp record's map for lamp kinds), 0 unknown.
std::uint32_t DestMapOf(const TravelIntent& t);
/// Main thread: lamp / return point `id` lies in the block the local player stands in (and its
/// MSB spot is known): the player is put there with the chr warp, no load. `beside`: next to
/// the spot by party slot. False: not in this block (a real map change: load as before).
/// For hostdeath: a respawn at a lamp of the current map is this plus the revive.
bool WarpToLampNoLoad(std::uint32_t id, bool beside);
/// The local player already stands at lamp `id`'s spot (same block, within 2 m): a warp there is a no-op.
bool AtLampSpot(std::uint32_t id);
/// Host, main thread: the party's move to lamp `id` inside the loaded map: the host warps
/// in-world and EVENT b2 {"op":"warp","lamp":id} moves the guests of this map beside it (others,
/// apart, travel there with one load of their own). False: not in this block.
bool HostPartyWarpNoLoad(std::uint32_t id);
/// I5 gather (guest, main thread): the host's chr in this world, placed where the host reports
/// itself (the vitals), in the block this guest stands in: true and its distance. False: not here.
bool HostChrHere(float* distance_m);
/// I5 gather (guest, main thread): an in-world warp beside the host's chr (BesideHost: the B2
/// spacing by party slot, the host's facing), no load. False: the host's chr is not here (HostChrHere)
/// or this guest is dead. `spot` (with `spot_yaw`): beside that spot instead of the host's chr (the
/// ground the host asked from, gather::SpotFor). `test_lift_m`: test aid, that much higher. `dest`:
/// where the guest was put; `from` (x, y, z, yaw): where it stood before.
bool GatherBesideHost(const char* why, const float* spot = nullptr, float spot_yaw = 0.0f, float test_lift_m = 0.0f,
                      float* dest = nullptr, float* from = nullptr, int attempt = 0, float* host_y = nullptr);
/// The game's last safe ground spot of the own chr (PlayerIns +0x400 -> +0x30); false: not readable.
bool OwnSafeGround(float v[3]);
/// The own chr's position; false: not readable.
bool OwnWorldPos(float p[3]);
/// I5 gather (guest, main thread): the host's chr position in this world (HostChrHere). False: not here.
bool HostChrPos(float pos[3]);
/// Main thread: the local chr's facing (its rotation's yaw), 0 unknown.
float OwnYaw();
/// Main thread: the local chr's ladder action state (-1 none, >= 0 on a ladder, below -1 unread).
int OwnLadderState();
/// I5 gather (guest, main thread): back to `pos` (where it stood before the gather), in-world, no load.
bool GatherWarpBack(const float pos[3], float yaw, const char* why);
/// I5 gather (guest, main thread): the party's one-load travel into the host's map (TravelAlone,
/// AloneWhy::Gather, to the lamp of the host's map nearest the host: a re-join with the host's
/// snapshot). The caller puts the guest beside the host once it has arrived. False: not possible now.
bool GatherTravelToHost();
/// C5: this player is dead in a boss-fight limbo and follows a living member (spectating).
bool Spectating();
/// Lamp / return point `id` lies in the block the local player stands in (playtest 1 bug 10: a
/// guest's warp there is its own loading screen, never a party move).
bool LampInLoadedMap(std::uint32_t id);
/// Guest, main thread: a lamp of the loaded map as this guest's own load (a B2 re-join into the
/// host's area: one loading screen for this guest only, the host does not move). False: not possible.
/// `rest` (D9): the lamp use is a rest - the host's rest load rides on the re-join (the host's world
/// resets for everyone there first); *rest_carried says it did.
bool GuestSoloLampLoad(std::uint32_t id, bool mark, bool rest = false, bool* rest_carried = nullptr);
/// Host, any thread: a guest (party slot `slot`) rested at a lamp and travels alone: the host's
/// world resets with a rest load where the host stands (a vanilla loading screen; the guests in
/// this world follow it). Queued; run by the B2 tick when the host is ready (party_death_rules
/// PlanRestLoad).
void HostRestRequest(int slot, std::uint32_t lamp);
/// Party slot `slot`'s map as its vitals report it (0 unknown) and whether it is in another area
/// than this player (split travel). For world actions (D3): a host validates an apart guest's
/// flags against the guest's own map (docs/party/split_travel.md 6).
std::uint32_t MemberMap(int slot);
bool MemberApart(int slot);
/// Test aid: the nearest living enemy of the local block within `max_m` m (entity id; 0 none).
std::uint32_t NearestEnemyForTest(float max_m);
/// Test aid: kills enemy `id` of the local world (HP 0; a client's Lua ForceDead does nothing).
bool KillEnemyForTest(std::uint32_t id);
/// Host, main thread: the next own travel is alone (TravelIntent::alone): the guests do not
/// follow and the host does not re-send its snapshot after its load. Expires after 10 s.
void HostTravelAloneNext();
/// Members in another area than this player's (host: guests elsewhere; guest: the host and other
/// guests elsewhere). The host watchdog counts them as present.
int MembersApart();
/// Host: a B2 re-join after its own load is running (the session watchdog stands aside).
bool HostRejoinRunning();
/// Any role: a B2 re-join (a host load's re-sync, a guest's follow / alone travel back) is running.
bool RejoinRunning();
/// Guest: a travel re-join is between the host's travel and its own arrival (the game shows its
/// loading card from the start, ~10 s before its loading flag rises): the party overlay hides.
bool GuestTravelPending();
/// The local player's HP (-1: no chr).
int LocalHp();
/// Host: when (Now() clock, s) the last member_down notice came; 0 never (test aid).
double LastMemberDownAt();
/// The clock of LastMemberDownAt.
double NowSeconds();
/// Host: when it last sent its room to a title joiner (NowSeconds clock; -1e9: never). Test aid
/// BB_PARTY_STORY_TEST=serve:<remo> (A11's admitted-just-before-a-cutscene race).
double TitleRoomServedAt();
/// Host, main thread (batch 6): hold its lamp travel while a member it just served a room joins.
/// `who` gets a short reason when held.
bool HostTravelHeldForJoin(std::string* who);
/// A lamp / headstone id as a travel intent (ReturnPointParam row; main thread). False: no row.
bool LampTravelIntent(std::uint32_t id, TravelIntent* t);
/// Host in a session with at least one member task (someone to travel with or without).
bool HostHasMembers();
/// Guest: its own choice takes it away from the host (alone at a lamp / headstone, its own Hunter's
/// Mark: party_split). While the host is in a Chalice Dungeon it then stays out until Party > Rejoin
/// (deathrules::PlanChaliceApart). A death clears it (a death is never a choice).
void NoteChosenApart(const char* why);
/// Guest: Party > Rejoin pressed. True: it was apart in the session from a host in a Chalice Dungeon
/// and now leaves the session (its own world); the bell's summon quick-joins it in.
bool ChaliceRejoinPressed();
/// C6 (RE): the game's NexusRevolution transports seen created (0xCDB250 via 0xCDAC45) and, per
/// transport, where its copy of the transport config lives (the word 0x4d0, then 3000, 5000, 6000,
/// 25000 idle, 20000, 200). Newest first; entries can be stale (check before use).
struct NrTransport {
    std::uint64_t owner = 0, transport = 0, factory = 0;
    std::uint32_t* config = nullptr;
};
std::vector<NrTransport> NrTransports();
/// C6 crash rejoin (host): `name`'s link ended with a connection reset (its game crashed). 1.5 s
/// later, on the game's own thread, the game's transport connections to sources silent since
/// then are released through the transport's own release (vtable +0x28), and the sign hold for
/// `name` ends 1 s after that (party_nr_drop.h). BB_PARTY_NR_DROP=0 turns it off.
void NrDropSilentPeers(const std::string& name, int delay_ms = 1500);
/// F5 (host): `name` is back after a network outage (its link went silent and resumed; slot kept)
/// while its sign hold runs. 1.5 s later (one scan, on the game's own thread) a game connection
/// to a source silent since then is released as above: that member's game left our session during
/// the outage (an alive one sends again as soon as the network is back). Either way the hold ends
/// then (none found: nothing is held), so the first direct summon is not refused.
void NrDropAfterOutage(const std::string& name);
/// A drop (crash or outage) is waiting for its scan: the direct summon answer waits (<= 3 s).
bool NrDropPending();
/// RE: offsets in [base, base+span) holding the transport config's timing values.
std::string NrFindTimeouts(std::uint64_t base, std::size_t span);

/// The game's own in-world warp of the local player (0x154B110, as a cutscene's end warp or a
/// stage spot warp moves it: block, navmesh, chr reset, camera / draw-group warp record). Main thread.
bool NativeWarpLocal(std::uint64_t player, const float* pos16, const float* rot16, const char* why);
/// The local player's in-world warp: NativeWarpLocal unless BB_PARTY_WARP_REFRESH=0, else the chr
/// warp vfunc +0x5F8. Main thread.
bool LocalChrWarp(std::uint64_t player, const float* pos16, const float* rot16, const char* why);
/// Guest: the boss-clear action-button hold (+0x52) is lifted for this session (K10 kept the
/// party; issue #4 item 1). The item box must not put the hold back then.
bool K10ActionHoldLifted();

} // namespace coop::b2
