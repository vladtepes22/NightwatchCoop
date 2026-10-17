// SPDX-License-Identifier: GPL-3.0-or-later
// Party story, phase C4 "everyone sees each cutscene and reaches the same ending"
// (docs/party/cutscenes_endings.md sections 4.1-4.4).
//
//   host:  the bank-2002 EMEVD handler 0x17C53B0 (this, SprjEmkEventIns* event) gets an entry hook
//          that reads the instruction (event+0xB0: {bank, id, size, pad, arg offset}) and its
//          arguments (event+0xB8, else the EVD base, as the handler does) and queues a
//          StoryIntent{cutscene} for the plays of the host's own world. The ending events
//          (m21 12100180 / 12100000 / 12100002) become StoryIntent{ending} right there, before the
//          host's ending step tears the room down; the tick also polls flags 21/22/23 (fallback)
//          and the time-of-day value 9800..9802 (StoryIntent{time_of_day}). The director polls
//          PopHostStory() and sends EVENT "story" (StoryToJsonText); nothing here sends anything.
//   guest: RequestGuestStory() (any thread) hands the intent to GuestStory; StoryTick() (main
//          thread, the coop tick) runs what GuestStory::Step answers:
//          - live mirror while still a phantom in the host world: 0x131A9F0(0, id for the own sex,
//            mode 2, -1, -1, player) and wait for the remo's playing bit (*(*(0x5540058)+8)+0x168
//            bit 0) to rise and fall. Never a warp / time-of-day variant on a phantom.
//          - replay queue in its own world (WorldTransitionState+0x1592 == 1): play the remo
//            (mode 0) unless it was mirrored, then the time of day exactly like 0x1CECEB0
//            (SetEventFlagValue(EventFlagMan, 9800, 3, {0,4,6,7}[tod]); byte [*(0x553B148)+0x81]=1)
//            and the story flags of that scene.
//          - the same queue while summoned (seamless play may have no own-world phase at all):
//            once the guest is a steady phantom (role 6) and calm (no HP lost for 15 s), every
//            item except an ending runs there: a missed scene plays through the phantom path
//            (mode 2, as the live mirror), the time of day and the story flags go into the own
//            save's pool (C2's mode-1 pool write; 9800..9802 bits, no world refresh byte - the
//            world shown is the host's), never into the host-world overlay. An ending runs last:
//            the guest's lamp warp to its own Hunter's Dream takes it out of the host's world
//            (as a B1 travel follow does) and the ending program below runs there.
//          - endings: A = warp to the own Hunter's Dream (0x13CDF30(2102961)) and set 72100130
//            (m21 12100180 does the rest natively); B / C = the event body emulated in the own
//            Dream (12101800[/12101850], 9180, remo, NG cycle +1, 6604, 6601|6602, 6603, last
//            22|23) -> the native ending step -> staff roll -> NG+.
//          While it is busy the director does not ring the guest bell (StoryBusy()).
//
// Env: BB_PARTY_STORY=0 (no capture, no replay), BB_PARTY_STORY_MIRROR=0 (no live mirror),
//      BB_PARTY_STORY_SUMMONED=0 (replay queue only in the own world, as before seamless play),
//      BB_PARTY_STORY_PLAYER=<n> (the 0x131A9F0 player argument; default 10000 = the local
//      player, resolved by 0x13C97A0; -1 = no appearance snapshot),
//      BB_PARTY_STORY_ENDING_SUMMONED=0 (a summoned guest's ending waits for the own world),
//      BB_PARTY_STORY_TEST=cutscene:<remo id>[,mirror] (single instance test: queue a replay of
//      that remo once the world has been up 10 s; "mirror" uses the phantom path / mode 2),
//      BB_PARTY_STORY_TEST=ending:<1|2|3>[,summoned] (queue that ending as if the host had sent it,
//      BB_PARTY_STORY_TEST_DELAY s after the world is up; "summoned": only once a phantom).
//
// The pure part (kinds, JSON, the host queue, GuestStory's mirror / replay programs) has no game
// dependency and is unit-tested (tests/test_party_story.cpp, built with BB_PARTY_STORY_NO_GAME).
#pragma once

#include "net/json.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace coop {

enum class StoryKind : std::uint8_t {
    Unknown = 0,
    Cutscene,  ///< a 2002 play: id = remo id
    Ending,    ///< id = ending type 1 (A, flag 21), 2 (B, 22), 3 (C, 23)
    TimeOfDay, ///< id = tod 1 evening, 2 night, 3 Blood Moon
};
const char* StoryKindName(StoryKind k);
StoryKind StoryKindFromName(const std::string& name);

constexpr std::uint32_t kStoryNone = 0xffffffffu;

struct StoryIntent {
    std::uint64_t seq = 0;
    StoryKind kind = StoryKind::Unknown;
    std::uint32_t id = kStoryNone;      ///< remo id / ending type / tod
    std::uint32_t mode = 0;             ///< remo play mode as the host ran it (0, 2, 8)
    std::uint32_t instr = 0;            ///< 2002[instr]
    std::uint32_t event_id = 0;         ///< EMEVD event (SprjEmkEventIns+0x28)
    std::uint32_t map = 0;              ///< event map (+0x68)
    int tod = -1;                       ///< time of day applied at the remo's end (instr 6/7), else -1
    std::uint32_t warp_point = kStoryNone; ///< instr 2/4/6 (diagnostics; a guest never warps)
    std::uint32_t warp_map = kStoryNone;   ///< area << 24 | block << 16
    std::vector<std::uint32_t> flags;   ///< story flags to set ON after the scene (own world)
    bool force = false;                 ///< test: replay even when not ReplayWorthy
};
std::string DescribeStory(const StoryIntent& s);

// ---- PartyLink EVENT "story" payload ----
constexpr const char* kStoryEventName = "story";
json::Value StoryToJson(const StoryIntent& s);
bool StoryFromJson(const json::Value& v, StoryIntent* out, std::string* error = nullptr);
std::string StoryToJsonText(const StoryIntent& s);
bool StoryFromJsonText(const std::string& text, StoryIntent* out, std::string* error = nullptr);

// ---- Data (docs/party/cutscenes_endings.md 3.x / 4.6) ----
/// The remo id for a player: the `+1000` sibling (sAA_BB_1NNN) when the scene has one and the
/// player's EMEVD sex is 1 (1003[12] jumps there when chr data +0xCA == 0), else the base id.
/// Accepts either id of a pair.
std::uint32_t RemoForSex(std::uint32_t remo_id, bool sex_variant);
/// Ending type 1/2/3 of an ending event (12100180 / 12100000 / 12100002), else 0.
int EndingTypeOfEvent(std::uint32_t event_id);
/// Ending type 1/2/3 of an ending remo (either sex), else 0.
int EndingTypeOfRemo(std::uint32_t remo_id);
/// Story flags a guest sets after replaying `remo_id` in its own world (empty: none).
std::vector<std::uint32_t> StoryFlagsForRemo(std::uint32_t remo_id);
/// The scene changes the guest's world or is a story scene it must see even if it missed the
/// live mirror (boss intros etc. are mirror-only).
bool ReplayWorthy(const StoryIntent& s);
/// A remo id the game has: remo/sAA_BB_NNNN.remobnd.dcx or an EMEVD 2002 play (party_ids.h,
/// generated from the game data by tools/party/ids_tool.py). Anything else would make the
/// guest's game look for a file that is not there.
bool StoryRemoKnown(std::uint32_t remo_id);
/// A story intent from the host (bbport security pass): false (with `why`) for a cutscene id
/// the game does not have; otherwise its flags are cut to the ones the guest's own table gives
/// for that remo (the host only ever sends those) - a host cannot set arbitrary event flags.
bool SanitizePeerStory(StoryIntent* s, std::string* why = nullptr);
/// G3: a guest's own 2002[6] play in the host's world (a scene + a time of day + a warp inside the
/// map it plays in: the Cathedral Ward door 12400750, day -> evening) that the host plays as its own
/// (the scene, the warp to the same point, the time of day) and sends to the party: a known remo, a
/// tod 1..3 later than the host's (`host_tod_value`: its 9800..9802 3-bit value) and a warp in the
/// host's map block (`host_map`). False (with `why`): the host plays only the scene (D10).
bool GuestPhaseChangeOk(const StoryIntent& s, int host_tod_value, std::uint32_t host_map, std::string* why = nullptr);
/// D5 / D10 (pard4): `event_id` is a host-only action-button event a guest may now press (Amelia's
/// skull 12401803 with its scene `remo_id` 24000030): its 2002[6] has no warp (point -1), which
/// GuestPhaseChangeOk accepts for these, and the host sets the event's own flag after playing it.
bool GuestPressedHostOnlyEvent(std::uint32_t event_id, std::uint32_t remo_id);
/// 9800..9802 as a 3-bit value (9800 = bit 2) for a tod (0..3): {0, 4, 6, 7}; -1 out of range.
int TodFlagValue(int tod);
/// The tod a 3-bit 9800..9802 value stands for (the highest table entry it reaches), 0..3.
int TodFromFlagValue(int value);

// ---- Host queue (the 2002 hook / tick pushes, the director pops) ----
class HostStoryQueue {
public:
    static constexpr std::size_t kMax = 32;
    bool Push(const StoryIntent& s);
    bool Pop(StoryIntent* out);
    std::size_t Size() const { return q_.size(); }

private:
    std::deque<StoryIntent> q_;
    std::uint64_t last_seq_ = 0;
};

// ---- Guest ----
/// What GuestStory needs from the game (injectable for tests).
struct StoryState {
    bool world_up = false;
    bool loading = false;
    bool transition_requested = false; ///< WorldTransitionState +0x08
    int session_role = -1;             ///< SprjSessionManager +0x124 (6 client)
    bool own_world = false;            ///< WorldTransitionState +0x1592 == 1
    bool remo_playing = false;         ///< SprjRemo player +0x168 bit 0
    bool sex_variant = false;          ///< play the +1000 remo files
    std::uint32_t map_id = 0xffffffffu;
    int tod_value = 0;                 ///< 9800..9802 3-bit value of the own save (summoned: the pool)
    bool calm = true;                  ///< no HP lost lately (a summoned replay waits for it)
};

struct StoryAction {
    enum Kind : std::uint8_t {
        None,
        PlayRemo,      ///< 0x131A9F0(0, id, mode, -1, -1, player)
        SetFlags,      ///< SetEventFlag(man, f, on) for each
        ApplyTod,      ///< 0x1CECEB0's body with `tod`
        NgCycle,       ///< GameDataMan+0x68 = min(+1, 7)  (2003[21])
        TravelDream,   ///< 0x13CDF30(2102961)
        ForceEnding,   ///< GameDataMan+0x6C = ending; GSM+0x1550 = 1 (RequestEnding body)
        AskEnding,     ///< the guest is asked "play this ending now?" (GuestStory::AnswerEnding)
    };
    Kind kind = None;
    std::uint32_t id = 0;   ///< remo id
    std::uint32_t mode = 0;
    std::vector<std::uint32_t> flags;
    bool on = true;
    int tod = -1;
    int ending = 0;
    bool mirror = false;    ///< part of a live mirror
    std::string why;        ///< log text
};
const char* StoryActionName(StoryAction::Kind k);

class GuestStory {
public:
    static constexpr int kStableTicks = 30;
    static constexpr double kMirrorTimeout = 60.0;   ///< s a mirror may wait for its gate
    static constexpr double kRemoStartTimeout = 5.0; ///< s for the playing bit to rise
    /// A live mirror shorter than this was cut (the guest's own load or travel stopped it: live parg,
    /// the Blood Moon mirror ran 1.4 s on a guest that was still loading the host's map): not
    /// counted as seen, so the queued replay plays the scene again once the guest is steady.
    static constexpr double kMirrorMinSeen = 3.0;
    static constexpr double kRemoTimeout = 600.0;    ///< s for it to fall
    static constexpr double kTravelTimeout = 180.0;  ///< s for the Dream warp to arrive
    static constexpr double kEndingWait = 8.0;       ///< s after 21/22/23 before ForceEnding
    static constexpr std::uint32_t kDreamLamp = 2102961;
    static constexpr std::size_t kMaxQueue = 32;

    bool mirror_enabled = true;
    bool summoned_replay = true;  ///< run the replay queue (except endings) while summoned
    /// A summoned guest with a queued ending leaves the host's world for its own Hunter's Dream
    /// (lamp warp) and runs the ending there (BB_PARTY_STORY_ENDING_SUMMONED=0: wait for home).
    bool summoned_ending = true;

    /// False when `s.seq` is not newer than the newest seen. `in_host_world`: the guest is a
    /// phantom right now (a cutscene is mirrored live).
    bool Offer(const StoryIntent& s, bool in_host_world, double now);
    /// Once a frame. The action to run now, if any.
    std::optional<StoryAction> Step(const StoryState& st, double now);
    /// Something is queued, mirrored or running (the director holds the guest bell).
    bool Busy() const;
    /// The replay part only (the mirror is not a reason to hold the bell).
    bool ReplayBusy() const { return !queue_.empty() || (running_ && !running_mirror_); }
    /// The item running now is a summoned replay (mode-2 play, own-pool writes).
    bool RunningSummoned() const { return running_ && running_summoned_; }
    bool Seen(std::uint32_t remo_id) const;
    /// The guest watched this scene in its own world (its own event played it): a later catch-up
    /// replay of the host's copy skips the scene and keeps only its flags / time of day.
    void NoteSeen(std::uint32_t remo_id);
    std::size_t QueueSize() const { return queue_.size(); }
    bool MirrorPending() const { return mirror_.has_value(); }
    bool Running() const { return running_; }
    std::uint64_t LastSeq() const { return last_seq_; }
    /// User decision (2026-10-15): the host's ending asks each guest first. YES: `ending` (see
    /// GuestEndingType) replays in the own Dream (staff roll, NG+ in the own save) as before;
    /// NO ("not now"): the guest only goes home to its own Dream, its save keeps its cycle and
    /// state, and it can reach the ending later. No answer in kEndingAskTimeout s counts as NO.
    void AnswerEnding(bool yes, int ending);
    /// The running item waits for the guest's answer.
    bool EndingAskPending() const { return !ops_.empty() && ops_.front().kind == Op::AskEnding; }
    /// BB_PARTY_ENDING_ASK=0: the old way, no question (the ending replays at once).
    void SetAskEnding(bool on) { ask_ending_ = on; }

    /// Sidecar persistence: queue, seen remo ids, last seq.
    json::Value ToJson() const;
    bool FromJson(const json::Value& v, std::string* error = nullptr);
    /// The queue / seen set changed since the last call (then false until the next change).
    bool TakeDirty();
    /// The remo of a live mirror that stopped before kMirrorMinSeen (0 = none), once.
    std::uint32_t TakeMirrorCut() {
        const std::uint32_t r = mirror_cut_;
        mirror_cut_ = 0;
        return r;
    }

private:
    struct Op {
        enum Kind : std::uint8_t {
            Play, WaitRemoStart, WaitRemoEnd, Flags, Tod, NgCycle, TravelDream, WaitArrive,
            EndingCheck, AskEnding,
        };
        Op(Kind k, std::uint32_t i = 0, std::uint32_t m = 0) : kind(k), id(i), mode(m) {}
        Kind kind;
        std::uint32_t id = 0, mode = 0;
        std::vector<std::uint32_t> flags;
        bool on = true;
        int tod = -1, ending = 0;
        bool asked = false;
    };
    void PushEndingBody(int ending);
    void Build(const StoryIntent& s, bool mirror, bool summoned = false);
    std::optional<StoryAction> RunOps(const StoryState& st, double now);
    std::optional<StoryAction> Run(const StoryState& st, double now);
    void Finish(bool ok);

    std::optional<StoryIntent> mirror_;
    double mirror_since_ = 0;
    std::deque<StoryIntent> queue_;
    std::set<std::uint32_t> seen_;
    std::uint32_t mirror_cut_ = 0; ///< a live mirror stopped early (kMirrorMinSeen); TakeMirrorCut
    std::uint64_t last_seq_ = 0;
    int stable_mirror_ = 0, stable_own_ = 0;
    bool dirty_ = false;

    bool running_ = false, running_mirror_ = false, running_summoned_ = false;
    StoryIntent cur_{};
    std::deque<Op> ops_;
    double op_since_ = 0;
    bool op_started_ = false;
    bool load_seen_ = false;
    bool ask_ending_ = true;
};

constexpr double kEndingAskTimeout = 120.0;
/// The ending a guest who answered YES gets: A (1, Yharnam Sunrise) when the host submitted;
/// after a refusal (host B or C) the guest's own cords decide as the game's would for its save:
/// three Umbilical Cords used (its own flag 9900) -> C (3, Childhood's Beginning), else B (2).
int GuestEndingType(int host_ending, bool own_three_cords);

/// D10: a client's own copy of a host-world event runs that event's 2002 remo right after the
/// live mirror played the same scene (campaign soak run 5f: Mergo 26000010, Gehrman 21000040), so
/// the guest saw it twice. Skip that 2002[1..7] when the event's scene was mirrored. On by default
/// (BB_PARTY_STORY_DEDUPE=0 turns it off). Skipping only the remo keeps the event chain running.
inline bool StoryDedupeSkip(bool enabled, int bank, int id, int event_id, bool client, bool mirrored) {
    return enabled && client && bank == 2002 && id >= 1 && id <= 7 && event_id > 0 && mirrored;
}
/// BB_PARTY_STORY_DEDUPE: unset / anything but "0" = on.
inline bool StoryDedupeEnabled(const char* env) { return !(env && env[0] == '0' && env[1] == 0); }

/// D10 belt and braces (playtest 1 bug 14): the host plays a guest's story_up cutscene for the
/// whole party unless the host played that scene in the last kRecent s (its own event, or an
/// earlier guest's) or the host is in a boss fight (the guest's copy of a first-fight intro
/// arriving after the host's own intro already started the fight). Not thread-safe (callers lock).
class HostStoryUpGuard {
public:
    static constexpr double kRecent = 600.0;
    void NotePlayed(std::uint32_t remo_id, double now);
    /// nullptr: play it; else why not.
    const char* Refuse(std::uint32_t remo_id, bool boss_fight, double now) const;

private:
    std::map<std::uint32_t, double> played_;  // base remo id -> last play
};

// ---- Story notices (D10, pard3) ----
/// A game dialog (EMEVD 2007[1]) that tells the story and that only the host's event shows (its
/// client copy ends on 1003[6]): Yahar'gul's "Ritual secret broken. Seek the nightmare newborn."
/// (m28_00 12800140, message 10012014, after the Blood Moon warp). The host relays it, every guest
/// shows it as a one-button (OK) game message once it is calm. The other 2007[1] messages are a
/// player's own interaction (a locked door, "Only a special emblem opens the main gate."): each
/// player's game shows its own, never relayed.
bool StoryNoticeMessage(std::uint32_t message_id);
constexpr const char* kStoryNoticeEventName = "story_notice";
struct StoryNotice {
    std::uint64_t seq = 0;
    std::uint32_t message = 0;
    std::uint32_t event_id = 0;
};
std::string StoryNoticeToJsonText(const StoryNotice& n);
bool StoryNoticeFromJsonText(const std::string& text, StoryNotice* out, std::string* error = nullptr);
/// Guest: a queued notice is shown now? World up, no load, no cutscene, no other game dialog or
/// menu, not hit in the last few seconds. Dropped after kStoryNoticeMaxWait.
bool StoryNoticeShowNow(bool world_up, bool loading, bool cutscene, bool dialog_up, bool calm);
constexpr double kStoryNoticeMaxWait = 300.0;

#ifndef BB_PARTY_STORY_NO_GAME
/// Host (the EMEVD dispatch hook): this game's event shows a story notice: relay it.
void NoteHostStoryNotice(std::uint32_t message_id, std::uint32_t event_id);
/// Host: the next notice to broadcast (any thread).
bool PopHostStoryNotice(StoryNotice* out);
/// Guest: the host's notice (the link thread); shown by StoryTick.
void RequestGuestStoryNotice(const StoryNotice& n);
/// bbgpu_patch_image, once, after coop::HooksInit (party mode only): the bank-2002 capture hook.
void InstallStoryHooks();
/// Host: the next story intent to broadcast (any thread).
bool PopHostStory(StoryIntent* out);
/// Guest: the host's intent (any thread; the PartyLink callback).
void RequestGuestStory(const StoryIntent& s);
/// Guest: the live mirror played the cutscene of EMEVD event `event_id` in the last 10 minutes
/// (the client's own copy of that event's 2002 is then a duplicate).
bool StoryMirroredEvent(std::uint32_t event_id);
/// Main thread, once a frame (the coop tick): host polls, guest mirror / replay.
void StoryTick();
/// D10 (cutscenes play for the whole party): a story cutscene a GUEST's own events play in the
/// host's world. Guest: PopGuestStoryUp() gives each one for EVENT "story_up" to the host; the
/// guest then drops the host's broadcast of that same remo (it has watched it). Host:
/// HostStoryFromGuest() (link thread) queues it; StoryTick plays it on the host (mode 0) and sends
/// it to every guest like a captured play of its own.
constexpr const char* kStoryUpEventName = "story_up";
bool PopGuestStoryUp(StoryIntent* out);
bool HostStoryFromGuest(int slot, const std::string& body, std::string* why);
/// Host, any thread: a member joined (its catch-up gets the host's time of day, e.g. the Blood Moon).
void StoryMemberJoined();
/// The guest is replaying a story item (hold the bell / rejoin).
bool StoryBusy();
/// BB_PARTY_STORY_TEST is set (single instance test; the party layer must start for it).
bool StoryTestRequested();
/// steady_clock seconds of the last ending this game ran (21/22/23, 72100130, the ending step); 0 none.
double StoryLastEndingAt();
#endif

} // namespace coop
