// SPDX-License-Identifier: GPL-3.0-or-later
// Party campaign start (C1, docs/party/campaign_start.md §4): when is a player "ready" for the
// party's summons, and which bells it still lacks. Pure logic, no game access (the director feeds
// it the flags read by game_state.h ReadStartFlags), so it is unit-tested on its own
// (tests/test_party_start.cpp, target party-start-test).
//
// BB_PARTY_START=prologue_solo (default): everyone plays the prologue alone, in parallel; a player
//   is ready at 12410000 (opening cutscene done) && 9401 (first Dream cutscene) && 12101020 &&
//   12101021 (both starting weapons taken) && 9180 OFF (no scripted cutscene) && out of the FIRST
//   Hunter's Dream visit (playtest 1 #19: the weapons are taken in the Dream, but the vanilla
//   prologue goes on there - Notebook, then the headstone to the clinic's sickroom lamp; a
//   "ready" guest was gathered by a lamp warp into the clinic instead). Until then the member's
//   roster state is Prologue (not Home), so nobody rings for it, it rings nothing itself, and no
//   party sync (flags, items, cutscenes, gathers, catch-up, rot) touches its world. Summons start
//   only once EVERY connected member is ready (AnyMemberInPrologue), whoever finishes first.
// BB_PARTY_START=immediate (experimental, §4.5): ready as soon as 12410000 is ON and 9180 OFF
//   (summons in the clinic, with the `Bells anywhere` patches; guests are unarmed).
#pragma once

#include "party_link.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop {

/// Explicit Continue or a supervised crash restart may advance the title. A recovered
/// party identity alone never authorizes input; configured 0 keeps manual control.
bool ShouldAutoContinue(int configured, bool supervised_restart);

enum class StartMode { PrologueSolo, Immediate };

/// "prologue_solo" / "immediate" (case-insensitive; also "solo", "prologue", "clinic"); empty or
/// unknown -> PrologueSolo (*known = false for an unknown non-empty value).
StartMode ParseStartMode(const char* text, bool* known = nullptr);
const char* StartModeName(StartMode m);

// Flags (GetEventFlagValue 0x13CFD80 on EventFlagMan *0x553B100).
constexpr std::uint32_t kFlagOpeningDone = 12410000;  // cutscene after character creation
constexpr std::uint32_t kFlagCutscenePending = 9180;  // a scripted cutscene pending / playing
constexpr std::uint32_t kFlagFirstDream = 9401;       // first Hunter's Dream cutscene done
constexpr std::uint32_t kFlagFirstDeathToBase = 9402; // first death in m24_01 seen
constexpr std::uint32_t kFlagWeaponRight = 12101020;  // right-hand starting weapon taken
constexpr std::uint32_t kFlagWeaponLeft = 12101021;   // left-hand starting weapon taken
constexpr std::uint32_t kFlagDollAwake = 12100105;
constexpr std::uint32_t kFlagBeckoningLot = 6622;     // lot 10010 (Beckoning Bell) received
constexpr std::uint32_t kFlagResonantShop = 6610;     // Small Resonant Bell bought
constexpr std::uint32_t kGoodsBeckoning = 200;
constexpr std::uint32_t kGoodsSmallResonant = 205;
constexpr std::uint32_t kLotBeckoning = 10010;

/// What the director read this tick. `flags_ok` false: EventFlagMan missing (title) or the
/// flag function's code differs; then every flag is unknown.
struct StartFlags {
    bool flags_ok = false;
    bool opening_done = false, cutscene = false, first_dream = false, first_death = false;
    bool weapon_right = false, weapon_left = false, doll_awake = false;
    bool beckoning_lot = false, resonant_shop = false;
    int beckoning_count = -1, resonant_count = -1; ///< goods in the inventory; -1 unknown
    /// Where the player stands (packed map id; 0 / 0xffffffff unknown) and whether this process saw
    /// it out of the Hunter's Dream with both weapons taken (the director's latch): the first Dream
    /// visit ends with the headstone (#19). Not flags of the save, filled in by the caller.
    std::uint32_t map_id = 0xffffffffu;
    bool left_first_dream = false;
};

/// The Hunter's Dream (m21_00; the Abandoned Old Workshop m21_01 is not it). Unknown -> false.
bool InHuntersDream(std::uint32_t map_id);
/// Out of the first Dream visit: the latch, the Doll awake (Insight: well past the prologue) or a
/// known map that is not the Dream. An unknown map (the first ticks after a load) is not.
bool LeftFirstDream(const StartFlags& f);

/// Where a player is in the start sequence (§4.4). Ordered: a later step implies the earlier ones.
enum class StartStep {
    NoWorld = 0,   ///< title / character creation / no player record
    Unknown,       ///< in the world, flags unreadable
    Opening,       ///< 12410000 OFF: the opening cutscene (or before it)
    Clinic,        ///< opening done, 9401 OFF: clinic / Central Yharnam until the first death
    FirstDream,    ///< 9401 ON, starting weapons not both taken, or still in that first Dream visit
    Ready,
};
const char* StartStepName(StartStep s);

/// The step from the flags. `in_world`: world up and not loading.
StartStep ClassifyStart(const StartFlags& f, bool in_world);

/// Ready for the party's summons under `mode`: in the world, flags readable, no cutscene pending,
/// and (prologue_solo) step Ready or (immediate) the opening done.
bool StartReady(StartMode mode, const StartFlags& f, bool in_world);

/// One line for logs ("step clinic: 12410000 1, 9180 0, 9401 0, ...").
std::string DescribeStart(const StartFlags& f, bool in_world);

/// The bells a ready player in its own world should be given (BB_PARTY_GRANT_BELLS, §4.3).
struct BellGrant {
    bool beckoning_lot = false;   ///< award lot 10010 (Beckoning Bell, sets 6622)
    bool beckoning_goods = false; ///< 6622 already ON but no bell: goods 200 directly
    bool resonant = false;        ///< goods 205 + flag 6610
    bool any() const { return beckoning_lot || beckoning_goods || resonant; }
};
/// Nothing when the counts are unknown (< 0) or the flags unreadable.
BellGrant PlanBellGrant(const StartFlags& f);

/// The roster state of a player idle in its own world: Home once ready, else Prologue.
party::MemberState OwnWorldState(bool ready);
/// A party member (not the host) waits for the host's bell: connected and Home or Joining
/// (a member still in its Prologue does not count).
bool MemberWaits(const std::vector<party::RosterEntry>& roster);
/// Lobby decision for the automatic bells (the session / loading / interval gates are the
/// director's). Host: ready itself, room for a cooperator, a member waits and no connected member
/// is still in its prologue (#19: summons start once everybody is ready). Guest: ready itself and
/// connected to the host.
bool HostMayRing(bool host_ready, int cooperators, int max_players, const std::vector<party::RosterEntry>& roster);
/// Guest: also nobody in the party (the host included) still in its prologue (`party_ready`,
/// !AnyMemberInPrologue): its gather and bell would touch a prologue world.
bool GuestMayRing(bool guest_ready, bool connected, bool party_ready = true);
/// A connected member (any slot, the host too) whose roster state is Prologue.
bool AnyMemberInPrologue(const std::vector<party::RosterEntry>& roster);
/// The connected members in their prologue except `except_name` ("Hunter1", "Hunter1 and Hunter2"); "".
std::string MembersInPrologue(const std::vector<party::RosterEntry>& roster, const std::string& except_name = {});
/// The host's roster entry says Prologue (a guest's view).
bool HostInPrologue(const std::vector<party::RosterEntry>& roster);
/// The session roles (SprjSessionManager +0x124, game_state.h) the automatic bell may ring in:
/// a guest only idle in its own world (0); the host idle (0) or already hosting (3) - with one
/// cooperator in, the next member of a 3- or 4-player party is summoned by ringing again while
/// hosting (the game's own Beckoning Bell works there). Never while trying to host / join,
/// failing, as a client or leaving.
bool SessionMayRing(bool host, int session_role);
/// Host: a travel of its own concerns the party (is broadcast) only while someone is in its
/// session: it hosts (session role 3) or hosted moments ago (`hosting_recently`: the travel's
/// own leave may already have dropped the role), or a connected member is in its world or
/// joining it. Members in their prologue or alone in their own worlds are not moved (the
/// receivers check again: GuestFollowsHostTravel, party_travel.h).
bool HostTravelHasAudience(bool hosting_recently, const std::vector<party::RosterEntry>& roster);

// ---- Late joiner catch-up (party_catchup.cpp is the game glue) ----
// A guest whose save is behind the host's (a fresh character, or one still in the prologue) is
// brought up to the host before any summon: once its own world is steady (an opening cutscene is
// skipped, or waited for), the campaign-start flags the host necessarily has (12410000, 9402,
// 9401, 12417810, 72101000/011, 12101020/21, 12100105, 9404; 9180 off - test_newgame.h's list)
// go into its own save, the starting weapons 7000000 / 14000000 are given when it has none, and
// it lamp-warps to the host's last lamp (the gather). The warp's load opens C2's post-load
// window, where the host's flag snapshot (lamps, shortcuts, bosses, ...) lands in the own save;
// C3's full item list follows at the summon. Only a party guest connected to a host whose own
// snapshot says it is past the prologue; never the host, never a solo game.
struct CatchupInput {
    bool guest = false;       ///< party guest, link connected
    bool host_known = false;  ///< the host's C2 flag snapshot arrived
    bool host_ready = false;  ///< the host's snapshot: 12410000 and 9401 ON
    bool in_world = false;    ///< world up, not loading
    bool own_idle = false;    ///< own world, session role idle (0), flag store mode 0
    bool remo_playing = false;
    StartFlags flags;         ///< the guest's own
    double t = 0;             ///< seconds, monotonic
};
struct CatchupAction {
    enum Kind : std::uint8_t { None, SkipOpening, ApplyStart, Warp, Finished };
    Kind kind = None;
    std::string why;
};
class Catchup {
public:
    enum class Phase : std::uint8_t { Idle, Opening, Apply, Warp, WaitWarp, Done };
    static constexpr double kSteady = 3.0;        ///< s in the own world before deciding
    static constexpr double kSkipEvery = 1.0;     ///< s between two skip requests (the remo turns skippable after its fade-in)
    static constexpr int kSkipTries = 60;
    static constexpr double kWarpTimeout = 40.0;  ///< s for the gather warp's load
    /// Once a frame.
    CatchupAction Step(const CatchupInput& in);
    Phase phase() const { return phase_; }
    /// Applying / warping: the director holds its bells.
    bool Busy() const { return phase_ == Phase::Apply || phase_ == Phase::Warp || phase_ == Phase::WaitWarp; }
    bool Applied() const { return applied_; }

private:
    Phase phase_ = Phase::Idle;
    double world_at_ = -1, phase_at_ = 0, skip_at_ = -1e9;
    int skips_ = 0;
    bool load_seen_ = false, applied_ = false;
};
const char* CatchupPhaseName(Catchup::Phase p);

/// #19 / playtest 1 save audit S5: party_prologue.txt, whether each character (save slot) was in
/// its prologue when last seen in its own world: "last V" (the last character seen; 0 / 1) and one
/// "slot S V" line per slot. A guest at its title asks for the slot it continues; without an entry
/// for it (or with the slot unknown) the last character's value counts. A file from before the
/// slot key ("1" / "0") is read as that "last" value: the same answer as before, which holds a
/// title join rather than risk loading a prologue save into the host's world.
struct SavedPrologueFile {
    int last = 0;                               ///< 0 / 1
    std::vector<std::pair<int, int>> slots;     ///< (save slot, 0 / 1)
};
SavedPrologueFile ParseSavedPrologue(const std::string& text);
std::string FormatSavedPrologue(const SavedPrologueFile& f);
bool SavedPrologueFor(const SavedPrologueFile& f, int slot);
/// The character in `slot` (-1 unknown: only "last") seen in its own world: true when the file
/// changed.
bool NoteSavedPrologueFor(SavedPrologueFile& f, int slot, bool prologue);

} // namespace coop
