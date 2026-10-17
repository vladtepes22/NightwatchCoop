// SPDX-License-Identifier: GPL-3.0-or-later
// PartyDirector (plan phase A5): automatic summoning, driven once a frame on the game's main
// thread (coop tick: the SprjFlipper::Update entry, 0x2034770, which the frame function
// 0x2018d20 calls before the task system runs - in menus and in the world alike).
//
//   guest: world up, no loading screen, session idle, party link connected
//            -> raise OnEvent_SendSoulSign_NormalCoop (Small Resonant Bell) every BB_PARTY_RING_EVERY s
//   host:  world up, no loading screen, session idle, cooperators < max - 1 and a party member
//          waits (roster: connected, Home or Joining)
//            -> raise OnEvent_Call_SOS (Beckoning Bell) every BB_PARTY_RING_EVERY s
// The local roster state (Title / Loading / Prologue / Home / InHostWorld) and map go to PartyLink.
// Campaign start (C1, party_start.h): nothing is rung before the local player is "ready"
// (BB_PARTY_START=prologue_solo: prologue done; immediate: opening cutscene done); until then the
// roster state is Prologue, so the host does not ring for that member either. Once ready, in its
// own world, the player gets the bells it lacks (BB_PARTY_GRANT_BELLS, default on).
//
// Env: BB_PARTY=host|join|<code> (role: host when "host" or BB_PARTY_HOST=1, else guest; unset, empty,
//        0, off, false, no = off: gpu/party_env.h),
//      BB_PARTY_HOST_WATCHDOG / _STALL_S / _GONE_S / _HARD_S: the host session watchdog (party_watchdog.h),
//      BB_PARTY_MAX (2..4, default 3), BB_PARTY_RING_EVERY (s, default 30),
//      BB_PARTY_AUTO=0 (no automatic bells: state tracking and logs only),
//      BB_PARTY_START=prologue_solo (default) | immediate, BB_PARTY_GRANT_BELLS=0 (no bell grants),
//      BB_PARTY_DIRECTOR_TEST=item[,item..] - offline test mode:
//        log_state    the state every 5 s and the game's own Lua event dispatches
//        start        the campaign start flags and bell counts every 5 s (and on each change)
//        award_lot=N  awards item lot N once, 12 s after the world is up (grant call probe)
//        drop_bells   removes goods 200 / 205 10 s after the world is up (PR1 probe, test saves)
//        grant_bells  grants the missing bells once ready (also without a party / with
//                     BB_PARTY_GRANT_BELLS=0) and logs the counts before and after
//        insight=N    writes the player's Insight once the world is up
//        echoes=N     writes the player's Blood Echoes once the world is up
//        goods=ID:N   tops goods ID up to N once, 8 s after the world is up (repeatable item)
//        log_pos      the player's area / position (FrpgNetMan +0xa7c / +0xaac) every 3 s
//        place=MAP:X:Y:Z:YAW  one forced placement (hex packed map), 15 s after the world is up
//        warp=MAP:POINT       one stage warp to warp point / entity POINT of MAP instead
//        ring_host / ring_guest  raises that bell once, BB_PARTY_DIRECTOR_TEST_DELAY s (default 10)
//                     after the world is up, and logs what follows (and the start flags/bells)
//        host_warp=ID@S  host: S s after its next guest joined, the lamp warp 0x13CDF30(ID) on
//                     the main thread (the path a lamp / the warp menu takes: B1 sees it)
//        host_die@S / guest_die@S  that role: S s after its next (re)join, ForceDead on the
//                     local player (Lua ForceDead 0x13320C0(0, 10000))
//        host_kill=ENTITY@S  host: ForceDead 0x13320C0(0, ENTITY) (a boss: the game's own defeat
//                     path) S s after the world is steady again with a cooperator since the
//                     previous step (no re-join needed: a B2 host keeps its cooperators)
//        host_move=X:Y:Z@S[%MAP] / guest_move=...  that role's chr warped to (X, Y, Z) (chr vfunc
//                     +0x5F8) S s after the later of its last load and its previous step, only
//                     in a map whose name contains MAP (e.g. %m24_01); X:Y:Z:YAW also sets the facing
//        host_press=BUTTONS@S / guest_press=...  that role taps pad buttons (0x4000 Cross) through
//                     runtime_pad_director_press (in-process, no OS input), same timing
//                     A "#N" suffix (host_die@15#2) holds a step until this player's Nth join.
//                     The travel steps run in list order, each after a join that followed
//                     the previous step (a chain: warp, rejoin, die, rejoin, ...); with them the
//                     party logs get wall-clock stamps.
#pragma once

#include <cstdint>
#include <string>

namespace party {
class PartyLink;
}

namespace coop {

enum class PartyRole { None, Host, Guest };

class PartyDirector {
public:
    static PartyDirector& Get();
    /// From the environment (BB_PARTY, BB_PARTY_MAX, BB_PARTY_RING_EVERY, BB_PARTY_DIRECTOR_TEST).
    void ConfigureFromEnv();
    void Configure(PartyRole role, int max_players);
    /// The party's link (A3/A4 wiring); nullptr detaches. The link must outlive its attachment.
    void SetLink(party::PartyLink* link);
    PartyRole Role() const;
    /// Main thread, once a frame.
    void Tick();

private:
    PartyDirector() = default;
};

/// Main thread: the game's own leave (0x13CDDA0: BeginLeaveSession + drop the presentation
/// members, as the host watchdog uses it), byte-checked; false when the code is not 1.09's.
bool LeaveGameSession();
/// Main thread: the Small Resonant Bell's SpEffect 9005 on the local player (the bell as the item
/// rings it): is it on, drop it (the game's RemoveSpEffect), apply it again (a new ring).
bool GuestBellEffectOn();
bool DropGuestBellEffect();
bool ApplyGuestBellEffect();
/// Any thread: the host ended the party / is gone: a guest still in the host's world goes home at
/// once (OnLeave_Limit on the next main-thread tick).
void RequestGuestSendHome(const std::string& why);
// F5: host -> guests: the host's game ended the co-op session by itself (OnIrregularLeaveSession);
// a guest still in the host's world goes home now (one loading screen) instead of standing in a
// dead session until the host's game leaves the room.
constexpr const char* kHostSessionEndEvent = "host_session_end";

/// Any thread: this player finished its campaign start (C1, party_start.h StartReady) - the host
/// refuses direct summons until then (playtest 1 #1).
bool LocalStartReady();
/// The director's start step is a prologue step (opening, clinic, first Dream): not ready for a
/// known reason. False while the step is unknown (no world, a load just ended, flags unreadable)
/// or ready.
bool LocalStartInPrologue();
/// Any thread (playtest 1 #19): the party must not touch this player's world now - it is in its
/// prologue itself (opening, clinic, first Dream visit until the headstone; kept over loading
/// screens), or (a guest) the host is. Gathers, catch-up, flag / item / cutscene syncs and rot
/// wait; `why` gets a short reason. The rule: no party action into a prologue world, and summons
/// start only once everybody is ready.
bool PartyPrologueHold(const char** why = nullptr);
/// This player alone is in its prologue (the part of PartyPrologueHold that is about itself).
bool LocalPrologueHold();
/// The other connected members in their prologue ("Hunter1", "Hunter1 and Hunter2"; "" none).
std::string OthersInPrologue();
/// Any thread (#19): the save being continued (party::flow::CurrentSaveSlot(); unknown: the one
/// played last) was still in its prologue when last seen in the world
/// (BB_GPU_USER_DIR/party_prologue.txt, per save slot): a guest's title join is skipped for it.
bool SavedPrologue();
/// Any thread: the roster entry of link slot `slot` (state: party::MemberState as int; map: the
/// roster's map id). False when the slot is not on the roster (state -1, map 0).
bool MemberRoster(int slot, int* state, std::uint32_t* map);

/// BB_PARTY or BB_PARTY_DIRECTOR_TEST is set (non-empty).
bool PartyRequested();
/// "[hh:mm:ss.mmm] " (local wall clock) when BB_PARTY_LOG_TIME=1 or BB_PARTY_DIRECTOR_TEST is
/// set, else "": lets two instances' logs be lined up (travel timings).
std::string PartyLogStamp();
/// bbgpu_patch_image: binds the image, checks the Lua dispatcher and installs the main-thread
/// tick (only call it when PartyRequested()).
void PartyInit(unsigned char* image, std::uint64_t size);
/// The main-thread tick: Lua events, then the director.
void CoopTick();

} // namespace coop
