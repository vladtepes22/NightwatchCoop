// SPDX-License-Identifier: GPL-3.0-or-later
// Shared NPC dialogue (D8): when a party member talks to an NPC, the members near that NPC get
// the conversation too - the game's own subtitle line, the NPC's voice from the NPC, a small
// "[Hunter talking to Gilbert]" caption, the talker's menu choices and the NPC's gifts. Passive:
// nothing opens a menu or takes input on the listeners.
//
// RE (eboot 1.09, our offsets; docs/party/world_actions.md "Shared NPC dialogue"):
//   talk line   0x2049170(state, voiceId): every talk line the game plays (the talk command
//               TalkToPlayer 0x2045870 -> 0x20478A0, and the chain's next row in the talk update
//               0x2047CB0). state+4 NPC entity, +8 base TalkParam id, +0xC row index (already the
//               new row): TalkParam row = base + index ({msgId, voiceId, ...}); after the call
//               state+0x34 = msgId, +0x38 = playing. The voice: "v%09d" (voiceId) through
//               0x27D3A80(handle out, name, pos, -1, 0, 1, 0) at the NPC's position, then
//               0x27E2C50 / 0x27E2380 on the handle; 0x27E23F0 stops it. 0x553D6F0 != 0: no sound.
//   subtitle    the talk update writes, every frame a line plays, MessageUI (*0x5562878) +0x80C =
//               msgId, +0x810 = 0, +0x58 = 4 (talk), +0x280 = 1: the game's subtitle of that line.
//   choices     the talk command switch 0x203FFD0(env, call): command 0x13 AddTalkListData(index,
//               msgId, ..) into MessageUI +0xEB0, 0x14 ClearTalkListData; the chosen row index is
//               MessageUI +0xEC0 (test 0x17 GetTalkListEntryResult).
//   gifts       the item lot award 0x17DDC50 (party_items' hook reports the lot here).
//
// Talker: the line / choice / gift goes to the party as EVENT "npc_talk" (a guest to the host, the
// host to every other member). Listener (main-thread tick): same world (both in the host's world)
// and same map, then within the range of the NPC (Party rules "NPC dialogue range", default 25 m,
// 0 = the whole map): the game's subtitle for the line, the voice at the NPC, the caption; further
// away only a quiet "Hunter is talking to Gilbert" party notice. A line the listener's own game
// played for the same NPC in the last 20 s is not doubled. Every shared line is logged with its
// talk id and the distance decision ("Party: npc_talk: ...").
//
// Env: BB_PARTY_NPC_TALK=0/1 (over the Party rules setting), BB_PARTY_NPC_TALK_RANGE=<m> (0 = map),
//      BB_PARTY_NPC_TALK_VOICE=0 (no voice), BB_PARTY_NPC_TALK_SUBTITLE=0 (caption overlay lines
//      instead of the game's subtitle), BB_PARTY_NPC_TALK_TRACE=1 (every talk command id).
//
// The pure part (names, JSON, range, dedupe, the choice tracker) has no game dependency and is
// unit-tested (tests/test_party_npctalk.cpp, built with BB_PARTY_NPCTALK_NO_GAME).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace party {
class PartyLink;
}

namespace coop {
struct GameSnapshot;

namespace npctalk {

constexpr const char* kEventName = "npc_talk";

enum class Kind : std::uint8_t { Line = 1, End, Choice, Gift };
const char* KindName(Kind k);

/// One shared moment of a conversation.
struct TalkEvent {
    Kind kind = Kind::Line;
    std::uint32_t npc = 0;      ///< the NPC's entity id
    std::uint32_t talk = 0;     ///< TalkParam row (line) / the conversation's last line (others)
    std::uint32_t msg = 0;      ///< talk message (FMG 1): the line, or the chosen menu row's text
    std::uint32_t voice = 0;    ///< voice id of the line (0: none)
    std::uint32_t map = 0;      ///< the talker's map id
    float pos[3] = {};          ///< the NPC's position in the talker's game
    bool has_pos = false;
    std::int32_t lot = -1;      ///< gift: the item lot
    std::uint32_t item_cat = 0; ///< gift: the lot's first item (FMG 10 goods, 11 weapon, 12 armor)
    std::int32_t item_id = -1;
    std::string who;            ///< the talker's party name (set by the host from its roster)
    int slot = -1;              ///< the talker's party slot (set by the host)
};
std::string ToJsonText(const TalkEvent& e);
/// Parses and validates (known kind, sane ids, finite position, name <= 16 chars).
bool FromJsonText(const std::string& text, TalkEvent* out, std::string* error = nullptr);

/// The speaker of a talk line by its TalkParam / talk message block (id / 10000), e.g. 1900000 ->
/// "Gilbert"; null when the block is not a known NPC.
const char* NpcNameForTalk(std::uint32_t talk);
/// "Hunter talking to Gilbert" / "Hunter talking to someone".
std::string CaptionFor(const std::string& who, std::uint32_t talk);

/// The range a listener uses: env BB_PARTY_NPC_TALK_RANGE (metres, digits only) over `setting`;
/// clamped 0..500 (0 = the whole map).
int RangeFromEnv(const char* env, int setting);
/// On / off: env BB_PARTY_NPC_TALK ("0" / "1") over `setting`.
bool EnabledFromEnv(const char* env, bool setting);
/// The Party rules slider (0..10): 5, 10, 15, 20, 25, 30, 40, 50, 75, 100 m, the whole map.
extern const int kRangeTable[11];
int RangePosition(int metres);
int RangeAtPosition(int position);
/// "within 25 m of the NPC" / "anywhere on the same map".
std::string DescribeRange(int metres);

/// How long a listener shows a shared line when the talker's next line / end does not come first
/// (speech ~0.07 s a character): 2.5 .. 10 s.
double LineSeconds(std::uint32_t chars);
/// How long a listener keeps a shared line at most when neither the talker's next line nor its end
/// arrives: the talker's own conversation ends (and sends End) after 30 s without a line, so the
/// listener waits for that, plus 2 s; never less than the speaking-time estimate. The estimate alone
/// ended lines the talker held longer (a voice past ~0.07 s a character) up to 1.1 s early (live
/// npctalk2 / pard4 D8).
double RemoteLineLimit(std::uint32_t chars);

enum class Reach : std::uint8_t {
    Share,      ///< subtitle + voice + caption
    FarNotice,  ///< a quiet notice only
    Ignore,     ///< another map / another world / off
};
const char* ReachName(Reach r);
/// `dist_m` < 0: unknown (shared: same map is all we know). `range_m` 0: the whole map.
Reach Decide(bool enabled, bool same_world, bool same_map, float dist_m, int range_m);

/// Lines the listener's own game played (npc, talk) and when: a remote copy within `window` s is
/// not played again.
class RecentLines {
public:
    static constexpr double kWindow = 20.0;
    void Note(std::uint32_t npc, std::uint32_t talk, double now);
    bool Recent(std::uint32_t npc, std::uint32_t talk, double now) const;

private:
    struct Entry {
        std::uint32_t npc, talk;
        double at;
    };
    std::deque<Entry> q_;
};

/// The talk menu's choice from the talk commands (one tracker per game, main thread): Clear,
/// Adds, then the command that shows the menu; the first command after it reads the chosen row.
class ChoiceTracker {
public:
    static constexpr std::uint32_t kAdd = 0x13, kClear = 0x14;
    /// A talk command `cmd` of NPC `npc` (args a1, a2 as ints); `result` = MessageUI +0xEC0 now.
    /// Returns true with *msg / *npc_out once a shown menu's row was decided (the result changed
    /// since the menu showed, or the NPC's next menu is being cleared) and that row is in the list.
    bool OnCommand(std::uint32_t cmd, std::uint32_t npc, std::int32_t a1, std::int32_t a2, std::int32_t result,
                   std::uint32_t* msg, std::uint32_t* npc_out);
    std::size_t Rows() const { return rows_.size(); }

private:
    std::map<std::int32_t, std::uint32_t> rows_; // index -> msg
    std::uint32_t npc_ = 0;
    bool shown_ = false;
    bool armed_ = false; ///< rows were added since the last menu
    std::int32_t shown_result_ = -1; ///< MessageUI +0xEC0 when the menu showed
};

/// Events per talker per 10 s at most (a chatty NPC chains ~1 line / 2 s).
class EventRate {
public:
    static constexpr int kPerWindow = 24;
    static constexpr double kWindow = 10.0;
    bool Take(int slot, double now);

private:
    double start_[8] = {-1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18};
    int used_[8] = {};
};

// ---- game side ----
/// Hooks (observers: they only read). Before the game runs (PartyInit).
void Init();
/// Main thread, once a frame (the director's tick).
void Tick(const GameSnapshot& s, party::PartyLink* link);
/// Link thread: EVENT npc_talk from `slot` (host: a guest's; guest: the host's / relayed).
void OnEvent(int slot, const std::string& body);
/// Main thread: the game awarded item lot `lot` to the local player (party_items' hook).
void NoteAward(std::int32_t lot);
/// guestsvc's test trace of the talk command switch (that hook is ours; it calls this first).
using CommandTrace = void (*)(std::uint64_t env, std::uint64_t call);
void SetCommandTrace(CommandTrace fn);
void ObserveCommand(std::uint64_t env, std::uint64_t call);

/// The overlay (any thread): the caption under the subtitles - a header ("Hunter talking to
/// Gilbert"), an optional line (the talker's choice / gift, or the subtitle text when the game's
/// own subtitle could not be used) - and its fade-out time.
struct Caption {
    std::string header;
    std::string line;
    std::int64_t until_ms = 0; ///< steady-clock ms
    std::uint64_t seq = 0;
};
Caption CurrentCaption();

} // namespace npctalk
} // namespace coop
