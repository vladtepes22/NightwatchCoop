// SPDX-License-Identifier: GPL-3.0-or-later
// Party text chat (0.7.8, CL-5b356c): a short line any member types (keyboard) or picks from a preset list (pad) reaches every
// member, shown in a small fading log with the sender's name in that member's party colour
// (party_hud.h MemberColour).
//
//   input    keyboard: the chat key (default T; bbport.ini party_chat_key / BB_PARTY_CHAT_KEY, an SDL key name) opens the
//            overlay's text box; Enter sends, Esc cancels. Pad: the touchpad held + L1 (Back + L1 on pads without a
//            touch surface) opens the same box with the preset list (D-pad picks, Cross sends, Circle cancels).
//   wire     EVENT "party_chat" {"text":"..."}: a guest's to the host, the host's own broadcast; the host relays a guest's
//            to every other member with the slot and the name from its roster (never trusted from a guest). A member
//            without chat (an older build, or the setting off) ignores the event: it is no part of the version hash.
//   limits   text sanitised (UTF-8 checked; control, bidi, zero-width and combining characters dropped; spaces collapsed)
//            and cut to kMaxChars; the sender waits kSendGapMs between lines; the host relays a member's lines at most
//            kRelayBurst per kRelayWindowS; a receiver applies the same limit.
//   filter   with party_hide_bad_names on (default) the offensive-name filter masks words of a RECEIVED line with
//            asterisks (MaskWords: each word, then 2 and 3 word runs, then the whole line).
//   privacy  the text is never written to a log (party_log_game.cpp keeps chat bodies out of the event trace; the
//            game side logs the sender slot and the length only).
//
// Settings: bbport.ini party_chat (default OFF), party_chat_receive (default on: off hides others' lines),
// party_chat_key. System > Party chat; the launcher's Party tab. Env: BB_PARTY_CHAT=0/1 over the setting.
// Test aid: BB_PAD_FILE "chat <text>" / "chatpad <n>" (src/runtime_pad.c).
//
// The pure part (this file and party_chat.cpp) is unit-tested (tests/test_party_chat.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace party::chat {

constexpr const char* kEventName = "party_chat";
constexpr int kMaxChars = 120;              // characters of one line
constexpr std::size_t kMaxBytes = 4 * 120;  // and its UTF-8 bytes
constexpr std::int64_t kSendGapMs = 1500;   // between two lines of this player
constexpr int kRelayBurst = 4;              // host: a member's lines relayed per window
constexpr double kRelayWindowS = 10.0;
constexpr int kMaxSlots = 8;
constexpr int kNameBytes = 17;
constexpr int kLogLines = 6;                // lines kept / shown
constexpr std::int64_t kLifeMs = 14000;     // a line shows this long
constexpr std::int64_t kFadeMs = 2500;      // ... fading out over its last kFadeMs
constexpr int kPresetCount = 6;

/// The pad's preset lines ("Help!", "Follow me", ...), index 0..kPresetCount-1.
const char* Preset(int i);
/// The presets as the overlay's pick list ("label" lines).
std::vector<std::string> PresetList();

/// A line made fit to send / show: invalid UTF-8 -> "" (refused); controls, bidi and zero-width characters,
/// combining marks, private use and the like dropped; every space run one space; trimmed; cut at `max_chars`
/// characters. "" when nothing is left.
std::string Sanitise(const std::string& utf8, int max_chars = kMaxChars);
/// Number of characters (code points) of valid UTF-8.
std::size_t CharCount(const std::string& utf8);

/// EVENT party_chat. slot / who: set by the host from its roster. t: the host clock (ms) when sent.
struct ChatEvent {
    std::string text;
    int slot = -1;
    std::string who;
    std::int64_t t = 0;
};
std::string ToJson(const ChatEvent& e);
/// Parses and validates: text sanitised again (empty after it: refused), slot -1..kMaxSlots-1, name 1-16 of
/// [A-Za-z0-9_-], at most 1024 bytes.
bool FromJson(const std::string& text, ChatEvent* out, std::string* error = nullptr);

/// A token bucket per slot: `burst` lines, refilled over `window_s`.
class Rate {
public:
    Rate(int burst, double window_s);
    bool Take(int slot, double now_s);
private:
    int burst_;
    double window_s_;
    double tokens_[kMaxSlots];
    double last_[kMaxSlots] = {};
};

/// This player's own send gap: false while less than kSendGapMs since the last accepted line.
class SendGap {
public:
    bool Take(std::int64_t now_ms);
private:
    std::int64_t last_ = -1000000;
};

/// `text` with every offensive word (and 2-3 word run, and as a last resort the whole line) replaced by asterisks of
/// the same length (name_filter.h tiers on tokens). Unchanged when nothing matches.
std::string MaskWords(const std::string& text);

/// A line as drawn.
struct Line {
    int slot = -1;
    bool mine = false;
    char name[kNameBytes] = {};
    std::string text;    // as received (sanitised)
    std::string masked;  // MaskWords(text)
    std::int64_t until_ms = 0;  // steady clock (party_hud NowMs)
    std::uint32_t seq = 0;
};
/// The log (any thread): Push appends (the oldest beyond kLogLines goes); Read copies the live lines, oldest first;
/// expired ones are dropped.
void Push(const Line& l);
void ClearAll();
std::vector<Line> Read(std::int64_t now_ms);
/// Opacity of a line with `left_ms` to live: 1, fading to 0 over the last kFadeMs.
float Alpha(std::int64_t left_ms);
/// How long a line sent at host time `t` still shows at host time `now` (0: too old).
std::int64_t LifeLeft(std::int64_t t, std::int64_t now);

}  // namespace party::chat

namespace party {
class PartyLink;
}
namespace coop {
struct GameSnapshot;
namespace chat {
/// Chat on (the setting, BB_PARTY_CHAT over it) and a party running.
bool Enabled();
/// Others' lines shown (party_chat_receive).
bool Receiving();
/// The chat key (an SDL key code), from BB_PARTY_CHAT_KEY / party_chat_key; default T.
std::uint32_t ChatKey();
/// Input (any thread): open the chat box (`with_presets`: the pad's list). False: chat is not available.
bool RequestOpen(bool with_presets, const char* source);
/// Test aid: send `text` now (BB_PAD_FILE chat line).
void RequestSend(const std::string& text);
/// Link thread: EVENT party_chat from `slot` (host: a guest's; guest: the host's / relayed).
void OnEvent(int slot, const std::string& body);
/// Main thread, once a frame (the director): the box -> send; incoming events -> the log.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool in_world);
}  // namespace chat
}  // namespace coop
