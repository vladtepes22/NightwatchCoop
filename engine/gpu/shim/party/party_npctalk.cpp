#include "lamp_travel.h"
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared NPC dialogue (see party_npctalk.h; RE in docs/party/world_actions.md "Shared NPC dialogue").
#include "party_npctalk.h"

#include "net/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace coop::npctalk {

const char* KindName(Kind k) {
    switch (k) {
    case Kind::Line: return "line";
    case Kind::End: return "end";
    case Kind::Choice: return "choice";
    case Kind::Gift: return "gift";
    }
    return "?";
}

namespace {

bool KindFromName(const std::string& s, Kind* out) {
    for (Kind k : {Kind::Line, Kind::End, Kind::Choice, Kind::Gift}) {
        if (s == KindName(k)) {
            *out = k;
            return true;
        }
    }
    return false;
}

const json::Value* Find(const json::Value& v, const char* key) {
    return v.type == json::Value::Type::Object ? v.find(key) : nullptr;
}

bool GetU32(const json::Value& v, const char* key, std::uint32_t* out) {
    const json::Value* x = Find(v, key);
    if (!x) return true; // optional
    if (x->type != json::Value::Type::Number || !(x->number >= 0) || x->number > 4294967295.0 ||
        x->number != std::floor(x->number)) {
        return false;
    }
    *out = static_cast<std::uint32_t>(x->number);
    return true;
}

bool GetI32(const json::Value& v, const char* key, std::int32_t* out) {
    const json::Value* x = Find(v, key);
    if (!x) return true;
    if (x->type != json::Value::Type::Number || x->number < -2147483648.0 || x->number > 2147483647.0 ||
        x->number != std::floor(x->number)) {
        return false;
    }
    *out = static_cast<std::int32_t>(x->number);
    return true;
}

} // namespace

std::string ToJsonText(const TalkEvent& e) {
    json::Value v = json::Value::make_object();
    v.set("k", KindName(e.kind));
    v.set("npc", e.npc);
    v.set("talk", e.talk);
    if (e.msg) v.set("msg", e.msg);
    if (e.voice) v.set("voice", e.voice);
    v.set("map", e.map);
    if (e.has_pos) {
        json::Value p = json::Value::make_array();
        for (float f : e.pos) p.push(json::Value(double(std::round(f * 100.0f) / 100.0f)));
        v.set("pos", p);
    }
    if (e.kind == Kind::Gift) {
        v.set("lot", e.lot);
        if (e.item_id >= 0) {
            v.set("cat", e.item_cat);
            v.set("item", e.item_id);
        }
    }
    if (!e.who.empty()) v.set("who", e.who);
    if (e.slot >= 0) v.set("slot", e.slot);
    return json::dump(v, 0);
}

bool FromJsonText(const std::string& text, TalkEvent* out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) return fail("npc_talk: not JSON");
    if (v.type != json::Value::Type::Object) return fail("npc_talk: not an object");
    TalkEvent e;
    const json::Value* k = Find(v, "k");
    if (!k || k->type != json::Value::Type::String || !KindFromName(k->string, &e.kind)) return fail("npc_talk: unknown kind");
    std::int32_t slot = -1;
    if (!GetU32(v, "npc", &e.npc) || !GetU32(v, "talk", &e.talk) || !GetU32(v, "msg", &e.msg) ||
        !GetU32(v, "voice", &e.voice) || !GetU32(v, "map", &e.map) || !GetI32(v, "lot", &e.lot) ||
        !GetU32(v, "cat", &e.item_cat) || !GetI32(v, "item", &e.item_id) || !GetI32(v, "slot", &slot)) {
        return fail("npc_talk: bad number");
    }
    e.slot = slot;
    if (e.npc == 0 && e.kind != Kind::Gift) return fail("npc_talk: no NPC");
    if (e.voice > 999999999u) return fail("npc_talk: voice id out of range");
    if (e.item_cat > 15) return fail("npc_talk: item category out of range");
    if (const json::Value* p = Find(v, "pos")) {
        if (p->type != json::Value::Type::Array || p->array.size() != 3) return fail("npc_talk: bad position");
        for (int i = 0; i < 3; ++i) {
            const json::Value& c = p->array[std::size_t(i)];
            if (c.type != json::Value::Type::Number || !std::isfinite(c.number) || std::fabs(c.number) > 1e6) {
                return fail("npc_talk: bad position");
            }
            e.pos[i] = float(c.number);
        }
        e.has_pos = true;
    }
    if (const json::Value* w = Find(v, "who")) {
        if (w->type != json::Value::Type::String || w->string.size() > 16) return fail("npc_talk: bad name");
        for (char c : w->string) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return fail("npc_talk: bad name");
        }
        e.who = w->string;
    }
    *out = e;
    return true;
}

const char* NpcNameForTalk(std::uint32_t talk) {
    // Talk lines are numbered per speaker in blocks of 10000 (the talk FMG of 1.09; the block's
    // lines name the speaker, docs/party/world_actions.md "Shared NPC dialogue").
    switch (talk / 10000) {
    case 40: return "the Doll";
    case 50: return "Gehrman";
    case 70: return "Annalise";
    case 130: return "Iosefka";
    case 140: return "Iosefka"; // the clinic's other "Iosefka" (behind the same door)
    case 180: return "Arianna";
    case 190: return "Gilbert";
    case 200: return "the little girl";
    case 230: return "Djura";
    case 240: return "Alfred";
    case 250: return "Eileen";
    case 1030: return "Valtr";
    case 1040: return "Simon";
    case 1060: return "Adeline";
    default: return nullptr;
    }
}

std::string CaptionFor(const std::string& who, std::uint32_t talk) {
    const char* npc = NpcNameForTalk(talk);
    return (who.empty() ? std::string("A hunter") : who) + " talking to " + (npc ? npc : "someone");
}

int RangeFromEnv(const char* env, int setting) {
    int v = setting;
    if (env && env[0]) {
        char* end = nullptr;
        const long x = std::strtol(env, &end, 10);
        if (end && !*end && x >= 0) v = int(std::min<long>(x, 500));
    }
    return std::clamp(v, 0, 500);
}

bool EnabledFromEnv(const char* env, bool setting) {
    if (env && env[0] == '0' && !env[1]) return false;
    if (env && env[0] == '1' && !env[1]) return true;
    return setting;
}

const int kRangeTable[11] = {5, 10, 15, 20, 25, 30, 40, 50, 75, 100, 0};

int RangePosition(int metres) {
    if (metres <= 0) return 10;
    int best = 0;
    for (int i = 1; i < 10; ++i) {
        if (std::abs(kRangeTable[i] - metres) < std::abs(kRangeTable[best] - metres)) best = i;
    }
    return best;
}

int RangeAtPosition(int position) {
    return kRangeTable[std::clamp(position, 0, 10)];
}

std::string DescribeRange(int metres) {
    if (metres <= 0) return "anywhere on the same map";
    return "within " + std::to_string(metres) + " m of the NPC";
}

double LineSeconds(std::uint32_t chars) {
    return std::clamp(2.0 + 0.07 * double(chars), 2.5, 10.0);
}

double RemoteLineLimit(std::uint32_t chars) {
    return std::max(LineSeconds(chars), 32.0);
}

const char* ReachName(Reach r) {
    switch (r) {
    case Reach::Share: return "share";
    case Reach::FarNotice: return "far (notice only)";
    case Reach::Ignore: return "ignore";
    }
    return "?";
}

Reach Decide(bool enabled, bool same_world, bool same_map, float dist_m, int range_m) {
    if (!enabled || !same_world || !same_map) return Reach::Ignore;
    if (range_m <= 0 || dist_m < 0.0f) return Reach::Share;
    return dist_m <= float(range_m) ? Reach::Share : Reach::FarNotice;
}

void RecentLines::Note(std::uint32_t npc, std::uint32_t talk, double now) {
    while (!q_.empty() && now - q_.front().at > kWindow) q_.pop_front();
    if (q_.size() >= 64) q_.pop_front();
    q_.push_back({npc, talk, now});
}

bool RecentLines::Recent(std::uint32_t npc, std::uint32_t talk, double now) const {
    for (const Entry& e : q_) {
        if (e.npc == npc && e.talk == talk && now - e.at <= kWindow && now >= e.at - 1.0) return true;
    }
    return false;
}

bool ChoiceTracker::OnCommand(std::uint32_t cmd, std::uint32_t npc, std::int32_t a1, std::int32_t a2,
                              std::int32_t result, std::uint32_t* msg, std::uint32_t* npc_out) {
    bool chosen = false;
    const bool was_shown = shown_;
    if (shown_ && cmd != kAdd && npc == npc_ && (result != shown_result_ || cmd == kClear)) {
        // The menu's row was decided: the result changed since the menu showed (live: the ESD runs
        // a wait command every frame while the menu is up, the result stays until the choice), or
        // the next menu is being built (the same row chosen again: the result did not change).
        shown_ = false;
        armed_ = false;
        const auto it = rows_.find(result);
        if (it != rows_.end()) {
            *msg = it->second;
            *npc_out = npc_;
            chosen = true;
        }
    }
    if (cmd == kClear) {
        if (npc == npc_ || rows_.empty()) {  // another NPC's script clears only its own list
            rows_.clear();
            shown_ = armed_ = false;
        }
    } else if (cmd == kAdd) {
        if (npc != npc_) rows_.clear();
        npc_ = npc;
        if (a1 >= 0 && a2 > 0 && rows_.size() < 32) rows_[a1] = std::uint32_t(a2);
        shown_ = false;
        armed_ = true;
    } else if (!was_shown && armed_ && npc == npc_) {
        // The first other command of that NPC after its rows: the one that shows the menu.
        shown_ = true;
        shown_result_ = result;
    }
    return chosen;
}

bool EventRate::Take(int slot, double now) {
    if (slot < 0 || slot >= 8) return false;
    if (now - start_[slot] >= kWindow) {
        start_[slot] = now;
        used_[slot] = 0;
    }
    if (used_[slot] >= kPerWindow) return false;
    ++used_[slot];
    return true;
}

} // namespace coop::npctalk

#ifndef BB_PARTY_NPCTALK_NO_GAME

#include "bbport_game_menu.h"
#include "bbport_settings.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "party_director.h"
#include "party_guestsvc.h"
#include "party_hud.h"
#include "party_link.h"
#include "party_status.h"
#include "seamless_rules.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <vector>

namespace coop::npctalk {
namespace {

using party::PartyLink;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party: npc_talk: %s\n", line);
    std::fflush(stdout);
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- offsets (ours) ----
constexpr u64 kTalkLine = 0x2049170;      // (state, voiceId)
constexpr u64 kTalkCommand = 0x203FFD0;   // (env, call)
constexpr u64 kSoundPlay = 0x27D3A80;     // (handle out, name, pos16, -1, 0, 1, 0)
constexpr u64 kSoundPoll = 0x27E2C50;     // (handle)
constexpr u64 kSoundStart = 0x27E2380;    // (handle)
constexpr u64 kSoundStop = 0x27E23F0;     // (handle)
constexpr u64 kSoundOff = 0x553D6F0;      // u8: no sound
constexpr u64 kMessageUi = 0x5562878;     // MessageUI*
constexpr u64 kWorldTalkMan = 0x5540508;  // SprjWorldTalkMan*: +0x3FF8 a talk plays
constexpr u64 kWorldChrMan = 0x553e878;

// ---- state shared with the hooks (main thread) ----
struct LocalLine {
    std::uint32_t npc, talk, voice;
    u64 state;
};
std::mutex g_mu;
std::vector<LocalLine> g_lines;              // from the line observer
std::vector<TalkEvent> g_out;                // choices / gifts to send
std::deque<std::pair<int, std::string>> g_in; // link thread -> tick
std::atomic<bool> g_on{false};               // hooks record (feature on)
std::atomic<CommandTrace> g_trace{nullptr};
std::atomic<bool> g_cmd_trace{false};
ChoiceTracker g_choices;                      // main thread (command observer)
std::atomic<std::uint32_t> g_conv_npc{0};    // the local player's current conversation
std::atomic<std::uint32_t> g_conv_talk{0};
std::atomic<double> g_conv_at{0};

std::mutex g_cap_mu;
Caption g_cap;

// The talker's choice / gift ("Hunter chose: ..."): kept under the header for kNoteMs even when the
// NPC's answer line follows at once (live: the answer came 0.1 s after the choice and replaced it).
constexpr std::int64_t kNoteMs = 6000;
std::string g_note;
std::int64_t g_note_until_ms = 0;

void SetCaption(const std::string& header, const std::string& line, double secs) {
    std::lock_guard<std::mutex> lk(g_cap_mu);
    const std::int64_t now = NowMs();
    g_cap.header = header;
    g_cap.line = line.empty() && now < g_note_until_ms ? g_note : line;
    g_cap.until_ms = std::max(now + std::int64_t(secs * 1000.0), line.empty() ? g_note_until_ms : 0);
    ++g_cap.seq;
}

/// A choice / gift line: shown now and kept for kNoteMs under the next lines' header.
void SetNote(const std::string& header, const std::string& line) {
    {
        std::lock_guard<std::mutex> lk(g_cap_mu);
        g_note = line;
        g_note_until_ms = NowMs() + kNoteMs;
    }
    SetCaption(header, line, double(kNoteMs) / 1000.0);
}

std::string Utf8(const char16_t* s) {
    std::string out;
    if (!s) return out;
    for (; *s && out.size() < 600; ++s) {
        std::uint32_t c = *s;
        if (c >= 0xD800 && c <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
            ++s;
        }
        if (c == '\n' || c == '\r') c = ' ';
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string TalkText(std::uint32_t msg) {
    return msg ? Utf8(BbGameMenu::GameText(1, msg)) : std::string();
}

/// A talk menu row's text (event text FMG 30: "Channel Blood Echoes", "Leave" ...), else a talk line.
std::string MenuRowText(std::uint32_t msg) {
    if (!msg) return {};
    const std::string t = Utf8(BbGameMenu::GameText(30, msg));
    return t.empty() ? TalkText(msg) : t;
}

bool ChrPos(u64 chr, float out[3]) {
    u64 a = 0, b = 0, c = 0, d = 0;
    return chr && SafeGet(chr + 0x58, &a) && a && SafeGet(a + 8, &b) && b && SafeGet(b + 0x3b0, &c) && c &&
           SafeGet(c + 0x68, &d) && d && SafeRead(d + 0x1e0, out, 12) && std::isfinite(out[0]) &&
           std::isfinite(out[1]) && std::isfinite(out[2]);
}

bool MyPos(float out[3]) {
    u64 man = 0, player = 0;
    return SafeGet(Guest(kWorldChrMan), &man) && man && SafeGet(man + 0x60, &player) && player &&
           ChrPos(player, out);
}

bool NpcPos(std::uint32_t npc, float out[3]) {
    return npc && ChrPos(GuestSvcFindChr(npc), out);
}

float Dist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

u64 MessageUi() {
    u64 ui = 0;
    return SafeGet(Guest(kMessageUi), &ui) ? ui : 0;
}

/// A game dialog is up (MessageUI +0x78: the dialog kind, 0 none; e.g. "Travel with the party?"):
/// no shared subtitle or caption over it (live: both were drawn across the dialog's text).
std::atomic<bool> g_dialog_up{false};
bool DialogUp(u64 ui) {
    std::int32_t kind = 0;
    return ui && SafeGet(ui + 0x78, &kind) && kind != 0;
}

bool LocalTalkPlaying() {
    u64 wtm = 0;
    std::uint8_t on = 0;
    return SafeGet(Guest(kWorldTalkMan), &wtm) && wtm && SafeGet(wtm + 0x3ff8, &on) && on != 0;
}

// ---- hooks (main thread) ----
BB_COOP_SYSV void LineEntry(u64 state, u64 voice, u64, u64, u64, u64) {
    if (!g_on.load(std::memory_order_relaxed) || !state) return;
    std::int32_t npc = 0, base = 0, index = 0;
    if (!SafeGet(state + 4, &npc) || !SafeGet(state + 8, &base) || !SafeGet(state + 0xc, &index) || base <= 0 ||
        index < 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_lines.size() < 64) {
        g_lines.push_back({std::uint32_t(npc), std::uint32_t(base + index), std::uint32_t(voice), state});
    }
}

/// A talk command argument as an int ({value, ?, type}: type 1 int, 2 float).
std::int32_t Arg(u64 call, u64 vtbl, int i) {
    u64 fn = 0;
    if (!SafeGet(vtbl + 0x28, &fn) || !fn) return -1;
    const u64 p = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, int)>(fn)(call, i);
    std::int32_t type = 0, iv = 0;
    float fv = 0;
    if (!p || !SafeGet(p + 8, &type)) return -1;
    // Type 1 holds a float, type 2 an int (0x203FFD0 case 0x44 copies a type 1 value into a float
    // and converts a type 2 one; live: AddTalkListData's message ids are type 2).
    if (type == 2 && SafeGet(p, &iv)) return iv;
    if (type == 1 && SafeGet(p, &fv) && std::isfinite(fv) && std::fabs(fv) < 2e9f) return std::int32_t(fv);
    return -1;
}

BB_COOP_SYSV void CommandEntry(u64 env, u64 call, u64 a2, u64 a3, u64 a4, u64 a5) {
    if (const CommandTrace t = g_trace.load(std::memory_order_relaxed)) t(env, call);
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    if (!g_on.load(std::memory_order_relaxed) || !call || !env) return;
    u64 vtbl = 0, idfn = 0, ctrl = 0;
    if (!SafeGet(call, &vtbl) || !SafeGet(vtbl + 0x18, &idfn) || !idfn) return;
    const std::uint32_t cmd = reinterpret_cast<std::uint32_t(BB_COOP_SYSV*)(u64)>(idfn)(call);
    std::int32_t npc = 0;
    if (!SafeGet(env + 0x10, &ctrl) || !ctrl || !SafeGet(ctrl + 0xc, &npc)) npc = 0;
    std::int32_t a1 = -1, a2v = -1;
    if (cmd == ChoiceTracker::kAdd) {
        a1 = Arg(call, vtbl, 1);
        a2v = Arg(call, vtbl, 2);
    }
    std::int32_t result = -1;
    const u64 ui = MessageUi();
    if (ui) SafeGet(ui + 0xec0, &result);
    if (g_cmd_trace.load(std::memory_order_relaxed) && npc > 0) {
        std::int32_t req = -1, dlg = -1;
        if (ui) {
            SafeGet(ui + 0x58, &req);
            SafeGet(ui + 0x78, &dlg);
        }
        Log("trace: talk command %u npc %d (args %d %d, rows %zu, result %d, MessageUI +0x58 %d +0x78 %d)", cmd, npc,
            a1, a2v, g_choices.Rows(), result, req, dlg);
    }
    std::uint32_t msg = 0, who = 0;
    if (g_choices.OnCommand(cmd, std::uint32_t(npc), a1, a2v, result, &msg, &who)) {
        TalkEvent e;
        e.kind = Kind::Choice;
        e.npc = who;
        e.msg = msg;
        e.talk = g_conv_talk.load();
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_out.size() < 32) g_out.push_back(e);
    }
}

// ---- voice (main thread) ----
using SoundPlayFn = void(BB_COOP_SYSV*)(void*, const char*, const float*, int, int, int, int);
using SoundHandleFn = u64(BB_COOP_SYSV*)(void*);
bool g_sound_ok = false;
alignas(16) std::uint32_t g_voice[2] = {0, 0};
bool g_voice_live = false;

bool CheckSound() {
    static int ok = -1;
    if (ok < 0) {
        ok = MatchesOriginal(kSoundPlay, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53}) &&
             MatchesOriginal(kSoundStop, {0x55, 0x48, 0x89, 0xe5}) && MatchesOriginal(kSoundStart, {0x55, 0x48, 0x89, 0xe5}) &&
             MatchesOriginal(kSoundPoll, {0x55, 0x48, 0x89, 0xe5});
        if (!ok) Log("voice off: the sound functions differ from 1.09");
    }
    return ok == 1;
}

void StopVoice() {
    if (g_voice_live && CheckSound()) reinterpret_cast<SoundHandleFn>(Guest(kSoundStop))(g_voice);
    g_voice_live = false;
}

/// The NPC's voice line `voice` at `pos`, as the talk system plays it (0x2049170).
const char* PlayVoice(std::uint32_t voice, const float pos[3]) {
    if (!voice) return "no voice id";
    if (!CheckSound()) return "sound functions differ";
    std::uint8_t off = 0;
    if (SafeGet(Guest(kSoundOff), &off) && off) return "the game's sound is off";
    StopVoice();
    char name[0x104] = {};
    std::snprintf(name, sizeof name, "v%09u", voice);
    alignas(16) float p[4] = {pos[0], pos[1], pos[2], 0.0f};
    alignas(16) std::uint32_t h[2] = {0, 0};
    reinterpret_cast<SoundPlayFn>(Guest(kSoundPlay))(h, name, p, -1, 0, 1, 0);
    reinterpret_cast<SoundHandleFn>(Guest(kSoundPoll))(h);
    g_voice[0] = h[0];
    g_voice[1] = h[1];
    reinterpret_cast<SoundHandleFn>(Guest(kSoundStart))(g_voice);
    g_voice_live = true;
    return nullptr;
}

// ---- listener state (main thread) ----
struct Remote {
    bool active = false;
    std::uint32_t msg = 0, npc = 0, talk = 0;
    double until = 0;
    bool wrote = false;     // we set MessageUI +0x280
    bool native = true;     // the game's subtitle (else the caption line)
    std::string header;
};
Remote g_remote;
RecentLines g_local_recent;
EventRate g_rate;
std::map<std::string, double> g_far_noticed; // "who:npc" -> when
struct Conv {
    u64 state = 0;
    std::uint32_t npc = 0, talk = 0, msg = 0;
    double last = 0;
    bool ended = true;
} g_conv;

bool Enabled() {
    return EnabledFromEnv(std::getenv("BB_PARTY_NPC_TALK"), BbSettings::Get().party_npc_talk.load());
}
int Range() {
    return RangeFromEnv(std::getenv("BB_PARTY_NPC_TALK_RANGE"), BbSettings::Get().party_npc_talk_range.load());
}
bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

/// Is the player of `slot` in the host's world (the host always is)?
bool SlotInHostWorld(PartyLink* link, int slot) {
    if (slot == party::kHostSlot) return true;
    if (!link) return false;
    for (const party::RosterEntry& r : link->roster()) {
        if (r.slot == slot) return r.state == party::MemberState::InHostWorld;
    }
    return false;
}

std::string SlotName(PartyLink* link, int slot) {
    if (link) {
        for (const party::RosterEntry& r : link->roster()) {
            if (r.slot == slot) return r.name;
        }
    }
    return slot == party::kHostSlot ? "Host" : "Hunter";
}

/// This game shows the host's world (host, or a guest summoned into it).
bool InHostWorld(PartyRole role) {
    return role == PartyRole::Host || (role == PartyRole::Guest && LocalSessionRole() == RoleClient);
}

void Send(PartyLink* link, PartyRole role, TalkEvent e) {
    if (!link) return;
    if (role == PartyRole::Host) {
        e.slot = party::kHostSlot;
        e.who = SlotName(link, party::kHostSlot);
        link->post_event(party::kBroadcast, kEventName, ToJsonText(e));
    } else if (role == PartyRole::Guest) {
        link->post_event(party::kHostSlot, kEventName, ToJsonText(e));
    }
}

void ShowLine(const TalkEvent& e, float dist, double now) {
    const std::string header = CaptionFor(e.who, e.msg ? e.msg : e.talk);
    const bool native = !EnvOff("BB_PARTY_NPC_TALK_SUBTITLE");
    StopVoice();
    g_remote.active = true;
    g_remote.msg = e.msg;
    g_remote.npc = e.npc;
    g_remote.talk = e.talk;
    // Until the talker's next line or end (dlast D8: not the speaking-time estimate, which ended
    // lines the talker still showed up to 1.1 s early); the limit only covers a lost end.
    g_remote.until = now + RemoteLineLimit(std::uint32_t(TalkText(e.msg).size()));
    g_remote.native = native;
    g_remote.header = header;
    const std::string text = TalkText(e.msg);
    SetCaption(header, native ? std::string() : text, 26.0);
    const char* voice_why = "off (BB_PARTY_NPC_TALK_VOICE=0)";
    if (!EnvOff("BB_PARTY_NPC_TALK_VOICE")) {
        float at[3];
        if (!NpcPos(e.npc, at)) {
            if (e.has_pos) std::memcpy(at, e.pos, sizeof at);
            else MyPos(at);
        }
        const char* why = PlayVoice(e.voice, at);
        voice_why = why ? why : "played";
        if (!why) {
            Log("voice v%09u at (%.1f, %.1f, %.1f) through 0x27D3A80 (handle %08x)", e.voice, at[0], at[1], at[2],
                g_voice[0]);
        }
    }
    Log("shared line from %s: talk %u msg %u npc %u, %.1f m from the NPC: subtitle %s, voice %s: \"%s\"",
        e.who.c_str(), e.talk, e.msg, e.npc, dist, native ? "game" : "caption", voice_why, text.c_str());
}

void EndRemote(const char* why) {
    if (!g_remote.active) return;
    g_remote.active = false;
    if (g_remote.wrote) {
        const u64 ui = MessageUi();
        const std::uint32_t zero = 0;
        if (ui && !LocalTalkPlaying()) SafeWrite(ui + 0x280, &zero, 4);
        g_remote.wrote = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_cap_mu);
        const std::int64_t now = NowMs();
        if (now < g_note_until_ms) {
            g_cap.line = g_note; // the choice stays its own time
            g_cap.until_ms = std::min<std::int64_t>(g_cap.until_ms, std::max(now + 2500, g_note_until_ms));
        } else {
            g_cap.line.clear();
            g_cap.until_ms = std::min<std::int64_t>(g_cap.until_ms, now + 2500);
        }
    }
    Log("shared conversation line over (%s)", why);
}

/// Every frame a remote line shows: the game's subtitle request, as the talk update writes it.
void DriveSubtitle(double now) {
    if (!g_remote.active) return;
    if (now > g_remote.until) {
        EndRemote("timeout");
        return;
    }
    if (!g_remote.native || !g_remote.msg) return;
    if (LocalTalkPlaying()) return; // the local game's own talk wins
    const u64 ui = MessageUi();
    // D8 (pard4): also under the listener's own full-screen menu (the Doll's level-up screen, the
    // inventory: party_hud GameMenu, the menu flow +0xa88), as under a dialog.
    const bool menu = party::hud::GameMenu();
    if (DialogUp(ui) || menu) {
        if (g_remote.wrote) {
            const std::uint32_t zero = 0;
            SafeWrite(ui + 0x280, &zero, 4);
            g_remote.wrote = false;
            Log("game subtitle held: %s is up", menu ? "the listener's full-screen menu" : "a game dialog");
        }
        return;
    }
    std::int32_t req = 0;
    if (!ui || !SafeGet(ui + 0x58, &req) || (req != 0 && req != 4)) return; // a menu / dialog is up
    const std::int32_t msg = std::int32_t(g_remote.msg), four = 4, one = 1;
    const std::uint8_t zero = 0;
    SafeWrite(ui + 0x80c, &msg, 4);
    SafeWrite(ui + 0x810, &zero, 1);
    SafeWrite(ui + 0x58, &four, 4);
    SafeWrite(ui + 0x280, &one, 4);
    if (!g_remote.wrote) Log("game subtitle: MessageUI +0x80C = %u (talk request 4)", g_remote.msg);
    g_remote.wrote = true;
}

std::string ItemName(std::uint32_t cat, std::int32_t id) {
    if (id < 0) return {};
    std::uint32_t fmg = 0;
    switch (cat) {
    case 0: fmg = 11; break;
    case 1: fmg = 12; break;
    case 2: fmg = 13; break;
    case 4: fmg = 10; break;
    default: return {};
    }
    return Utf8(BbGameMenu::GameText(fmg, std::uint32_t(id)));
}

void Handle(const TalkEvent& e, const GameSnapshot& s, PartyRole role, PartyLink* link, double now) {
    const bool enabled = Enabled();
    const bool same_world = InHostWorld(role) && SlotInHostWorld(link, e.slot);
    const bool same_map = e.map == s.map_id;
    float me[3], npc[3];
    float dist = -1.0f;
    if (MyPos(me)) {
        if (NpcPos(e.npc, npc)) dist = Dist(me, npc);
        else if (e.has_pos) dist = Dist(me, e.pos);
    }
    const int range = Range();
    if (e.kind == Kind::End) { // the talker's line is over: whatever we show of that NPC ends too
        if (g_remote.active && g_remote.npc == e.npc) EndRemote("the talker's line ended");
        return;
    }
    const Reach r = Decide(enabled, same_world, same_map, dist, range);
    const char* npc_name = NpcNameForTalk(e.kind == Kind::Line && e.msg ? e.msg : e.talk);
    Log("%s from %s (slot %d): talk %u msg %u npc %u (%s), map %s/%s, %s, %.1f m, range %d m -> %s", KindName(e.kind),
        e.who.c_str(), e.slot, e.talk, e.msg, e.npc, npc_name ? npc_name : "?", MapName(e.map).c_str(),
        MapName(s.map_id).c_str(), same_world ? "same world" : "another world", dist, range, ReachName(r));
    if (r == Reach::Ignore) return;
    if (r == Reach::FarNotice) {
        if (e.kind != Kind::Line) return;
        const std::string key = e.who + ":" + std::to_string(e.npc);
        auto it = g_far_noticed.find(key);
        if (it == g_far_noticed.end() || now - it->second > 60.0) {
            g_far_noticed[key] = now;
            std::string n = e.who + " is talking to " + (npc_name ? npc_name : "someone");
            party::status::SetLastEvent(n);
            Log("far notice: %s", n.c_str());
        }
        return;
    }
    switch (e.kind) {
    case Kind::Line:
        if (g_local_recent.Recent(e.npc, e.talk, now)) {
            Log("line t%u: this game played it for npc %u itself (last 20 s): not doubled", e.talk, e.npc);
            return;
        }
        ShowLine(e, dist, now);
        break;
    case Kind::End:
        break;
    case Kind::Choice: {
        const std::string text = MenuRowText(e.msg);
        const std::string line = e.who + " chose: " + (text.empty() ? "#" + std::to_string(e.msg) : text);
        SetNote(CaptionFor(e.who, e.talk ? e.talk : e.msg), line);
        Log("choice shown: %s", line.c_str());
        break;
    }
    case Kind::Gift: {
        const std::string item = ItemName(e.item_cat, e.item_id);
        const std::string line = e.who + " received " + (item.empty() ? "a gift" : item) +
                                 (npc_name ? std::string(" from ") + npc_name : std::string());
        SetNote(CaptionFor(e.who, e.talk), line);
        Log("gift shown: %s (lot %d, item %u:%d)", line.c_str(), e.lot, e.item_cat, e.item_id);
        break;
    }
    }
}

bool g_installed = false;

} // namespace

void SetCommandTrace(CommandTrace fn) {
    g_trace.store(fn);
}

void ObserveCommand(u64 env,u64 call) { CommandEntry(env,call,0,0,0,0); }

void Init() {
    if (g_installed) return;
    g_installed = true;
    g_cmd_trace = std::getenv("BB_PARTY_NPC_TALK_TRACE") && std::getenv("BB_PARTY_NPC_TALK_TRACE")[0] == '1';
    // push rbp; mov rbp, rsp; push r15, r14, r12, rbx; sub rsp, 0x170 (18 bytes; then mov r14d, esi).
    const bool line = HookPrologue(kTalkLine, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
                                               0x81, 0xec, 0x70, 0x01, 0x00, 0x00},
                                   &LineEntry, "NPC talk line (observer)");
    // push rbp; mov rbp, rsp; push r15..r12, rbx; sub rsp, 0x3c8 (20 bytes; then mov rbx, rsi).
    const bool cmd = lamptravel::ObservesTalkCommands() || HookPrologue(kTalkCommand, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                 0x53, 0x48, 0x81, 0xec, 0xc8, 0x03, 0x00, 0x00},
                                  &CommandEntry, "talk command switch (observer)");
    Log("shared NPC dialogue: talk line observer %s, talk menu observer %s; %s, %s", line ? "on" : "NOT installed",
        cmd ? "on" : "NOT installed", Enabled() ? "on" : "off", DescribeRange(Range()).c_str());
}

void NoteAward(std::int32_t lot) {
    const double now = Now();
    if (!g_on.load() || lot <= 0 || now - g_conv_at.load() > 30.0 || !g_conv_npc.load()) return;
    TalkEvent e;
    e.kind = Kind::Gift;
    e.npc = g_conv_npc.load();
    e.talk = g_conv_talk.load();
    e.lot = lot;
    if (const u64 row = GameParamRow("ItemLotParam", "ITEMLOT_PARAM_ST", std::uint32_t(lot))) {
        std::int32_t id = -1;
        std::uint32_t cat = 0;
        if (SafeGet(row, &id) && SafeGet(row + 0x20, &cat) && id > 0 && cat <= 15) {
            e.item_id = id;
            e.item_cat = cat;
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_out.size() < 32) g_out.push_back(e);
}

void OnEvent(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_in.size() < 128) g_in.emplace_back(slot, body);
}

void Tick(const GameSnapshot& s, PartyLink* link) {
    const bool enabled = Enabled();
    g_on.store(enabled && g_installed);
    const double now = Now();
    const PartyRole role = PartyDirector::Get().Role();
    std::vector<LocalLine> lines;
    std::vector<TalkEvent> out;
    std::deque<std::pair<int, std::string>> in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        lines.swap(g_lines);
        out.swap(g_out);
        in.swap(g_in);
    }
    const bool world = s.world_up && !s.loading;
    const bool share = enabled && link && world && InHostWorld(role) &&
                       (role == PartyRole::Host || role == PartyRole::Guest);
    // A conversation's end also goes out after the talker's world went down (a load, Leave): the
    // listeners keep a line until the talker's end now (dlast D8), so it must not be lost.
    const bool send_end = enabled && link && (role == PartyRole::Host || role == PartyRole::Guest);
    // ---- talker: the local game's lines ----
    for (const LocalLine& l : lines) {
        std::uint32_t msg = l.talk;
        if (const u64 row = GameParamRow("TalkParam", "TALK_PARAM_ST", l.talk)) {
            std::int32_t m = 0;
            if (SafeGet(row, &m) && m > 0) msg = std::uint32_t(m);
        }
        g_local_recent.Note(l.npc, l.talk, now);
        g_conv = {l.state, l.npc, l.talk, msg, now, false};
        g_conv_npc = l.npc;
        g_conv_talk = msg; // the speaker's block (names) and the choice / gift caption
        g_conv_at = now;
        if (g_remote.active && g_remote.npc == l.npc) EndRemote("the local game talks to that NPC itself");
        TalkEvent e;
        e.kind = Kind::Line;
        e.npc = l.npc;
        e.talk = l.talk;
        e.msg = msg;
        e.voice = l.voice;
        e.map = s.map_id;
        e.has_pos = NpcPos(l.npc, e.pos);
        const char* name = NpcNameForTalk(msg);
        Log("local line: talk %u msg %u voice %u npc %u (%s)%s: \"%s\"", l.talk, msg, l.voice, l.npc, name ? name : "?",
            share ? " -> party" : " (not shared: not in a party world)", TalkText(msg).c_str());
        if (share) Send(link, role, e);
    }
    if (!g_conv.ended) {
        // The conversation is over when the talk state stops (0x2048E60: +0x31 = 0, +0x34 = -1).
        // Not +0x38 (a timed-line flag, 0 for lines that wait for the voice) nor MessageUI +0x280:
        // live, both dropped 0.3 s into lines that still showed.
        std::uint8_t started = 1, playing = 1;
        std::int32_t shown = 0, ui_talk = 1;
        const u64 ui = MessageUi();
        const bool read = g_conv.state && SafeGet(g_conv.state + 0x31, &started) &&
                          SafeGet(g_conv.state + 0x38, &playing) && SafeGet(g_conv.state + 0x34, &shown);
        if (ui) SafeGet(ui + 0x280, &ui_talk);
        const bool over = read && (!started || shown == -1);
        static double logged = 0;
        if (g_cmd_trace.load() && now - logged >= 1.0) {
            logged = now;
            Log("trace: talk state +0x31 %u +0x38 %u +0x34 %d, MessageUI +0x280 %d, local talk %d", started, playing,
                shown, ui_talk, LocalTalkPlaying() ? 1 : 0);
        }
        if ((over && now - g_conv.last > 0.3) || now - g_conv.last > 30.0) {
            g_conv.ended = true;
            TalkEvent e;
            e.kind = Kind::End;
            e.npc = g_conv.npc;
            e.talk = g_conv.talk;
            e.msg = g_conv.msg;
            e.map = s.map_id;
            Log("local line t%u over (%s)%s", g_conv.talk, over ? "the talk stopped" : "30 s without a line",
                send_end ? " -> party" : "");
            if (send_end) Send(link, role, e);
        }
    }
    for (TalkEvent& e : out) {
        e.map = s.map_id;
        if (e.kind == Kind::Choice) {
            Log("local choice: npc %u msg %u \"%s\"%s", e.npc, e.msg, MenuRowText(e.msg).c_str(), share ? " -> party" : "");
        } else {
            Log("local gift: lot %d (item %u:%d \"%s\") during the talk with npc %u%s", e.lot, e.item_cat, e.item_id,
                ItemName(e.item_cat, e.item_id).c_str(), e.npc, share ? " -> party" : "");
        }
        if (share) Send(link, role, e);
    }
    // ---- listener ----
    for (const auto& [slot, body] : in) {
        TalkEvent e;
        std::string err;
        if (!FromJsonText(body, &e, &err)) {
            Log("event from slot %d refused: %s", slot, err.c_str());
            continue;
        }
        if (role == PartyRole::Host) {
            if (slot == party::kHostSlot) continue;
            if (!g_rate.Take(slot, now)) {
                Log("event from slot %d dropped: more than %d in %.0f s", slot, EventRate::kPerWindow, EventRate::kWindow);
                continue;
            }
            e.slot = slot; // the host names the talker (never trusted from a guest)
            e.who = SlotName(link, slot);
            if (link) {
                const std::string relay = ToJsonText(e);
                for (const party::RosterEntry& r : link->roster()) {
                    if (r.slot != slot && r.slot != party::kHostSlot && r.connected) {
                        link->post_event(r.slot, kEventName, relay);
                    }
                }
            }
        } else if (role == PartyRole::Guest) {
            if (slot != party::kHostSlot) continue;
            if (e.slot < 0) e.slot = party::kHostSlot;
            if (link && e.slot == link->local_slot()) continue; // our own line back
        } else {
            continue;
        }
        if (world) Handle(e, s, role, link, now);
    }
    if (!world && g_remote.active) EndRemote("loading");
    g_dialog_up.store(world && DialogUp(MessageUi()));
    DriveSubtitle(now);
}

Caption CurrentCaption() {
    if (g_dialog_up.load()) return {}; // hidden under a game dialog
    if (party::hud::GameMenu()) {      // D8 (pard4): and under the listener's own full-screen menu
        static std::atomic<std::int64_t> logged_until{0};
        std::lock_guard<std::mutex> lk(g_cap_mu);
        if (g_cap.until_ms > NowMs() && logged_until.exchange(g_cap.until_ms) != g_cap.until_ms) {
            Log("caption held: the listener's full-screen menu is up (level-up screen, inventory ...)");
        }
        return {};
    }
    std::lock_guard<std::mutex> lk(g_cap_mu);
    return g_cap;
}

} // namespace coop::npctalk

#endif // BB_PARTY_NPCTALK_NO_GAME
