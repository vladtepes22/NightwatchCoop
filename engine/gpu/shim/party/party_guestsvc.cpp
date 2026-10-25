// SPDX-License-Identifier: GPL-3.0-or-later
// Guest services in the host's Hunter's Dream (see party_guestsvc.h; the RE is
// docs/party/guest_services.md).
//
// Offsets are ours (raw ELF VA of eboot.elf 1.09), each read from the disassembly:
//   0x203AD60 talk environment test-function switch (rdi result {value, type}, rsi = r15 talk
//     environment, rdx = r14 call {vfunc +0x10 id, +0x18 argument count});
//   0x203BA66 case 19 IsClientPlayer: argument count 1, then
//   0x203BA78 41 0f b6 47 25   movzx eax, byte [r15+0x25]
//   0x203BA7D e9 4d 34 00 00   jmp 0x203EECF   ([r12+8] = 2 int, [r12] = eax)
#include "party_guestsvc.h"
#include "party_bossfog_rules.h"
#include "party_npctalk.h"
#include "party_log_game.h"

#include "net/json.h"

#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace coop {

bool IsDreamMap(std::uint32_t map_id) {
    return map_id != 0xffffffffu && (map_id >> 24) == 21;
}

bool GuestServicesGate(const GuestServicesInput& in) {
    return in.enabled && in.world_up && !in.loading && in.session_role == kSessionRoleClient &&
           IsDreamMap(in.map_id);
}

std::string DescribeRecordChange(const PlayerRecord& a, const PlayerRecord& b) {
    std::string out;
    auto field = [&](const char* name, int x, int y) {
        if (x < 0 || y < 0 || x == y) {
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s%s %d -> %d", out.empty() ? "" : ", ", name, x, y);
        out += buf;
    };
    field("level", a.level, b.level);
    field("echoes", a.echoes, b.echoes);
    field("Insight", a.insight, b.insight);
    return out;
}

bool GuestServiceTalk(std::uint32_t talk) {
    switch (talk) {
    case 210304: // Doll: level up (MSB entity 2100700)
    case 210691: // Bath Messenger: shop, sell (2100211)
    case 210692: // workbench: upgrade, Blood Gems, repair, crush (2100201)
    case 210693: // storage (2100200)
    case 210694: // Bath Messenger shop (2100215)
    case 210695: // Bath Messenger shop (2100216)
    case 210696: // mirror: appearance (2100202)
    case 210697: // dress-up messenger (2100218)
    case 210698: // Insight messenger: Insight shop (2100219)
    case 210699: // Memory Altar: runes, covenants (2100203; prompt 70000008)
        return true;
    default:
        return false;
    }
}

bool GuestServiceTalkAllowed(std::uint32_t talk, const std::string& extra) {
    if (GuestServiceTalk(talk) || extra == "all") {
        return true;
    }
    std::size_t at = 0;
    while (at < extra.size()) {
        std::size_t end = extra.find(',', at);
        if (end == std::string::npos) {
            end = extra.size();
        }
        const std::string item = extra.substr(at, end - at);
        char* stop = nullptr;
        const unsigned long v = std::strtoul(item.c_str(), &stop, 10);
        if (!item.empty() && stop && !*stop && v == talk) {
            return true;
        }
        at = end + 1;
    }
    return false;
}

bool GehrmanLastEvent(bool f1028, bool f1029, bool f1030) {
    return f1028 || (f1029 && !f1030);
}

bool HeadstoneTalk(std::uint32_t talk) {
    return (talk >= 210000 && talk <= 210003) || talk == 210721;
}

bool LampTalk(std::uint32_t talk) {
    const std::uint32_t map = talk / 1000;  // area * 10 + block
    return map >= 210 && map <= 369 && map % 10 <= 2 && talk % 1000 <= 3;
}

bool ChaliceLampTalk(std::uint32_t talk) {
    return talk >= 290110 && talk <= 290119;
}

bool LampLitFlagAllowed(std::uint32_t id) {
    const std::uint32_t type = id / 10000000, area = id / 100000 % 100, zone = id / 1000 % 10, n = id % 1000;
    if (area < 21 || area > 36 || id / 10000 % 10 > 2) {
        return false;
    }
    if (type == 1 && zone == 7 && n >= 800 && n < 900 && (n % 20 == 10 || n % 20 == 11)) {
        return true;  // the lamp object's base+10 / base+11
    }
    return type == 7 && zone == 0 && n >= 200 && n < 300;  // 7AAB02xx lamp lit
}

std::string LampLitToJsonText(const std::vector<std::uint32_t>& ids, bool own) {
    json::Value v = json::Value::make_object();
    json::Value a = json::Value::make_array();
    for (std::uint32_t id : ids) {
        a.push(static_cast<unsigned>(id));
    }
    v.set("ids", std::move(a));
    if (own) {
        v.set("own", true);
    }
    return json::dump(v, 0);
}

bool LampLitFromJsonText(const std::string& text, std::vector<std::uint32_t>* ids, std::string* error, bool* own) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object || json::arr(v, "ids").size() > kMaxLampLitIds) {
            throw std::runtime_error("bad lamp_lit body");
        }
        std::vector<std::uint32_t> out;
        for (const json::Value& x : json::arr(v, "ids")) {
            const std::uint64_t id = json::as_u64(x, "id");
            if (id > 0xffffffffull || !LampLitFlagAllowed(std::uint32_t(id))) {
                throw std::runtime_error("not a lamp flag");
            }
            out.push_back(std::uint32_t(id));
        }
        *ids = std::move(out);
        if (own) {
            const json::Value* o = v.find("own");
            *own = o && o->type == json::Value::Type::Bool && o->boolean;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

std::string LampRequestToJsonText(const LampRequest& r) {
    json::Value v = json::Value::make_object();
    v.set("id", r.id);
    v.set("seq", json::hex(r.seq));
    if (r.rest) {
        v.set("rest", true);
    }
    return json::dump(v, 0);
}

bool LampRequestFromJsonText(const std::string& text, LampRequest* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        LampRequest r;
        r.id = json::u32(v, "id");
        r.seq = json::u64(v, "seq");
        if (const json::Value* rest = v.find("rest")) {
            r.rest = rest->type == json::Value::Type::Bool && rest->boolean;
        }
        *out = r;
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool SanitizePeerLampRequest(const LampRequest& r, std::string* why) {
    if (!TravelLampKnown(r.id)) {
        if (why) {
            *why = "id " + std::to_string(r.id) + " is no ReturnPointParam row";
        }
        return false;
    }
    return true;
}

LampRequestAction ChooseLampRequestAction(const LampRequest& r, bool lamp_in_loaded_map) {
    if (r.rest) {
        return LampRequestAction::RestLoad;
    }
    return lamp_in_loaded_map ? LampRequestAction::IgnoreSameMap : LampRequestAction::PartyTravel;
}

bool LampRequestGate::Accept(bool host_ready, double now, std::string* why) {
    if (!host_ready) {
        if (why) {
            *why = "the host is not in the world";
        }
        return false;
    }
    if (now - last_ < kMinInterval) {
        if (why) {
            *why = "another request was accepted less than 10 s ago";
        }
        return false;
    }
    last_ = now;
    return true;
}

std::vector<EmevdSkipRule> LampSkipRules() {
    std::vector<EmevdSkipRule> out;
    // 7200: a world lamp -> the Dream; 12107000: a Dream headstone -> its lamp (m21_00_00_00, one
    // instance per lamp: 1003[6] end if client, wait for the menu's flag, 2003[49] warp).
    ParseEmevdSkipRules("7200:1003:6@0,12107000:1003:6@0", &out);
    return out;
}

std::vector<EmevdSkipRule> ChaliceLampSkipRules() {
    std::vector<EmevdSkipRule> out;
    // m29 event 12907020 (one instance per dungeon lamp): 1003[6] end if client, the menu's flag
    // 72900100 + n, the kneel, 2003[49] to the Dream's altar spot (live-checked data: chalicelamp).
    ParseEmevdSkipRules("12907020:1003:6@0", &out);
    return out;
}

std::vector<EmevdSkipRule> DollPlacementRules() {
    std::vector<EmevdSkipRule> out;
    ParseEmevdSkipRules("12100110:2003:22@25,12100110:2003:17@26", &out);
    for (EmevdSkipRule& r : out) {
        r.client_only = true;
    }
    return out;
}

bool DollLevelUpCharTypeTest(int event_id, int bank, int id, int entity, int type) {
    return event_id >= 12100112 && event_id <= 12100114 && bank == 4 && id == 3 && entity == 10000 &&
           (type == 0 || type == 1);
}

bool PerPlayerMessengerEvent(std::uint32_t event_id) {
    // 0.7.3: the Dream's gift messengers too - right / left starting weapons 12101020 / 12101021,
    // Notebook 12101022, Beckoning Bell 12101024, Old Hunter Bell 12101026, Eye 12101028. Each
    // ends on each player's own pick / personal flag; another game's end of its copy reached this
    // game's copy (live r4: the guest's copy ended, 57 ms later the host's set 12101020 with none of
    // the host's own stock flags: its messengers sank before it picked).
    return event_id == 12105040 || event_id == 12101020 || event_id == 12101021 || event_id == 12101022 ||
           event_id == 12101024 || event_id == 12101026 || event_id == 12101028;
}

bool GuestLampsEnabledFromEnv(const char* v, bool fallback) {
    if (v && v[0] == '0' && !v[1]) {
        return false;
    }
    if (v && v[0] == '1' && !v[1]) {
        return true;
    }
    return fallback;
}

bool GuestServicesEnabledFromEnv(const char* v) {
    return !(v && v[0] == '0' && !v[1]);
}

} // namespace coop

#ifndef BB_PARTY_GUESTSVC_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "seamless_rules.h"
#include "party_director.h"
#include "party_link.h"
#include "party_progress.h"
#include "party_worldact.h"
#include "party_split.h"
#include "party_vote.h"
#include "party_b2.h"
#include "party_items.h"
#include "party_balance.h"
#include "party_guestvals_rules.h"
#include "lamp_travel_rules.h"
#include "party_ids.h"
#include "party_death_rules.h"
#include "bbport_game_menu.h"
#include "bbport_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <vector>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c
extern "C" void runtime_pad_director_sticks(int lx, int ly, int rx, int ry); // src/runtime_pad.c

namespace coop {

/// Guest lamps / headstones: env BB_PARTY_GUEST_LAMPS ("0" / "1") over the Party page setting.
// Test / diagnostic escape hatch only (no menu row: off would bring the dead lamps back, the host
// stuck in its dungeon): BB_PARTY_CHALICE_LAMPS=0 leaves the dungeon lamps' talk tests as the game has them.
bool ChaliceLampsOn() {
    static const bool on = !(std::getenv("BB_PARTY_CHALICE_LAMPS") && std::getenv("BB_PARTY_CHALICE_LAMPS")[0] == '0');
    return on;
}
bool GuestLampsOn() {
    return GuestLampsEnabledFromEnv(std::getenv("BB_PARTY_GUEST_LAMPS"), BbSettings::Get().party_guest_lamps.load());
}
bool GuestLampsAtStart() {
    static const bool on = GuestLampsOn();
    return on;
}
namespace {

constexpr u64 kIsClientLoad = 0x203BA78;   // movzx eax, byte [r15+0x25]
constexpr u64 kIsClientJmp = 0x203BA7D;    // jmp 0x203EECF
constexpr u64 kTestResultInt = 0x203EECF;  // [r12+8] = 2; [r12] = eax
// Case 52 IsMultiplayerInProgress: ... setg cl; or cl, al; movzx eax, cl; jmp 0x203EECF.
constexpr u64 kMultiOr = 0x203C564;        // 08 c1 0f b6 c1
constexpr u64 kMultiJmp = 0x203C569;       // e9 61 29 00 00 -> 0x203EECF
// Lamp object update 0x12F5C40: +0x49 = peer count > 0 || presentation count > 1.
constexpr u64 kLampMultiGate = 0x12F831C;  // test ecx,ecx; setg cl; or cl,al (then mov [r13+0x49], cl)

std::atomic<bool> g_lamps{false};
std::atomic<bool> g_world_talk{false};      // D8: guests talk to the host world's NPCs (BB_PARTY_GUEST_TALK)
std::atomic<std::uint32_t> g_world_talk_overrides{0};
// The host's headstones in a party (default on; BB_PARTY_HEADSTONES=0 off): IsMultiplayerInProgress
// answers 0 for the headstone scripts, so the host can travel from the Dream with guests present
// (the party follows by B2 / B1 travel). A guest's headstones stay behind IsClientPlayer unless
// BB_PARTY_GUEST_LAMPS=1.
std::atomic<bool> g_headstones{false};
std::atomic<int> g_session_role{-1};       // the tick's last read (the interceptor's thread)
std::atomic<std::uint32_t> g_multi_overrides{0};
std::atomic<std::uint32_t> g_lamp_talk_overrides{0};
std::atomic<std::uint32_t> g_lamp_talk_held{0};  // a guest's lamp kept closed during / right after an NPC talk
// Diagnostics: the talk scripts that asked IsMultiplayerInProgress (logged by the tick, once each).
std::atomic<std::uint32_t> g_multi_talks[16];
std::atomic<std::uint32_t> g_client_talks[16];  // ... and IsClientPlayer (any map)
void NoteTalk(std::atomic<std::uint32_t> (&slots)[16], std::uint32_t talk) {
    for (auto& slot : slots) {
        std::uint32_t cur = slot.load(std::memory_order_relaxed);
        if (cur == talk) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, talk)) {
            return;
        }
    }
}
void NoteMultiTalk(std::uint32_t talk) {
    for (auto& slot : g_multi_talks) {
        std::uint32_t cur = slot.load(std::memory_order_relaxed);
        if (cur == talk) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, talk)) {
            return;
        }
    }
}
std::mutex g_lamp_mu;
std::deque<LampRequest> g_lamp_out;        // guest: to send
std::deque<std::pair<int, LampRequest>> g_lamp_in; // host: from guests (slot, request)
std::uint64_t g_lamp_seq = 0;
LampRequestGate g_lamp_gate;
std::atomic<int> g_lamp_requests_made{0}; // guest: lamp warps turned into requests (tests)

std::atomic<bool> g_gate{false};
// Gehrman's ordinary talk for a guest (the tick: the gate is open and his last event is not up).
std::atomic<bool> g_gehrman_talk{false};
std::atomic<std::uint32_t> g_overrides{0};
// Talk environments seen asking while the gate was open (discovery log, tick side).
constexpr int kEnvSlots = 32;
std::atomic<std::uint64_t> g_envs[kEnvSlots];

struct State {
    bool init = false, enabled = false, installed = false;
    std::string extra; // BB_PARTY_GUEST_SERVICES_TALK
    bool open = false;
    PlayerRecord rec;
    std::uint32_t overrides_logged = 0;
    double last_check = 0;
    std::uint64_t logged_envs[kEnvSlots] = {};
    // BB_PARTY_GUESTSVC_TEST (live tests only): "echoes=N;warp=x,y,z,yaw[/x,y,z,yaw...]": the echoes
    // and the first warp 15 s after the gate opened (the summoned reload is over by then), each
    // further warp 8 s later.
    std::string test;
    std::vector<std::string> test_chain;  // "fixture|fixture|...": the next ones, run one after another
    bool test_done = false;
    int test_step = 0;
    int opens = 0;  // how often the gate opened (BB_PARTY_GUESTSVC_TEST "gate=N": start at the N-th)
    double test_next = 0;
    double open_at = 0;
    double changed_at = 0; // the last record change (0: none pending)
} g;

void NoteEnv(std::uint64_t env) {
    for (auto& slot : g_envs) {
        std::uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == env) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, env)) {
            return;
        }
    }
}

double Now();

// 0.7.6: seconds since the guest's own NPC conversation was last on: its talk playing, or the item-get
// box a conversation's gift opens (MessageUI +0x130; its OK press is the one that fell through to a lamp
// in reach). 0 while either is up. Main thread. BB_PARTY_LAMP_TALK_GRACE=0 (tests only) turns the hold off.
double ConversationIdleSeconds() {
    static double last_box = -1e9;
    const double now = Now();
    std::uint64_t ui = 0;
    std::uint32_t up = 0;
    if (SafeGet(Guest(0x5562878), &ui) && ui && SafeGet(ui + 0x130, &up) && up != 0) {
        last_box = now;
    }
    return std::min(npctalk::LocalTalkIdleSeconds(), now - last_box);
}

// The same filter, outside the Dream gate: a guest's world lamp (BB_PARTY_GUEST_LAMPS) answers 0 so
// its menu opens; its warp becomes a lamp request to the host (GuestLampIntercept).
std::uint64_t LampClientAnswer(std::uint64_t env, std::uint64_t value) {
    const std::uint32_t talk = *reinterpret_cast<const std::uint32_t*>(env + 0x10);
    const bool lamp = LampTalk(talk) || (ChaliceLampTalk(talk) && ChaliceLampsOn());
    if (!lamp || HeadstoneTalk(talk) || !g_lamps.load(std::memory_order_relaxed) ||
        g_session_role.load(std::memory_order_relaxed) != kSessionRoleClient) {
        return value;
    }
    // 0.7.6: not while the local game's NPC talk plays or just stopped (the talk's Cross press fell
    // through to a lamp in reach: a guest at a window resident sat down at the lamp and got its list).
    static const bool hold = !(std::getenv("BB_PARTY_LAMP_TALK_GRACE") && std::getenv("BB_PARTY_LAMP_TALK_GRACE")[0] == '0');
    if (hold && !lamptravel::GuestLampMayOpen(ConversationIdleSeconds())) {
        g_lamp_talk_held.fetch_add(1, std::memory_order_relaxed);
        return value;
    }
    g_lamp_talk_overrides.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// Main thread (talk scripts): the game's answer `value` of IsClientPlayer for talk environment `env`.
std::uint64_t BB_COOP_SYSV IsClientFilter(std::uint64_t env, std::uint64_t value) {
    if ((value & 0xff) != 0) {
        NoteTalk(g_client_talks, *reinterpret_cast<const std::uint32_t*>(env + 0x10));
    }
    if ((value & 0xff) != 0 && LampClientAnswer(env, value) == 0) {
        return 0;
    }
    // D8: a guest talks to the NPCs of the host's world (their quest flags then go to the host and
    // the guest's own save as world actions, party_worldact.h).
    if ((value & 0xff) != 0 && g_world_talk.load(std::memory_order_relaxed) &&
        g_session_role.load(std::memory_order_relaxed) == kSessionRoleClient &&
        worldact::WorldTalkAllowed(*reinterpret_cast<const std::uint32_t*>(env + 0x10))) {
        g_world_talk_overrides.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (!g_gate.load(std::memory_order_relaxed) || (value & 0xff) == 0) {
        return value;
    }
    NoteEnv(env);
    // The talk script id (t210304 -> 210304; the case-19 environment is valid here: the game
    // just read +0x25).
    const std::uint32_t talk = *reinterpret_cast<const std::uint32_t*>(env + 0x10);
    // Headstones (t210000-t210003) on a guest with the party lamps on: their warp list comes from
    // the lamp flags of the host-world overlay, which the host-world mirror (party_progress) now
    // fills (before it, choosing an area crashed the guest: the list indexed past its end). The
    // choice sets the lamp's flag; m21's event 12107000 (client end skipped, LampSkipRules) warps,
    // which becomes the guest's lamp request to the host (the party follows the host's warp).
    const bool headstone = g_lamps.load(std::memory_order_relaxed) && talk >= 210000 && talk <= 210003;
    const bool gehrman = talk == kGehrmanTalk && g_gehrman_talk.load(std::memory_order_relaxed);
    if (!headstone && !gehrman && !GuestServiceTalkAllowed(talk, g.extra)) {
        return value;
    }
    g_overrides.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// EMEVD dispatch thread: the Doll's level-up kneel event 12100112 for a client (see the header).
std::atomic<std::uint32_t> g_doll_rewrites{0};
void DollRewriter(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr) {
    if (event_id < 12100112 || event_id > 12100114 || bank != 4 || id != 3) {
        return;
    }
    std::uint32_t size = 0;
    const std::uint64_t args = EmevdInstructionArgs(event, instr, &size);
    std::int32_t entity = 0;
    std::int8_t type = -1;
    if (!args || size < 9 || !SafeGet(args + 4, &entity) || !SafeGet(args + 8, &type) ||
        !DollLevelUpCharTypeTest(event_id, bank, id, entity, type)) {
        return;
    }
    // The test compares the LOCAL chr's type (PlayerIns +0x78) at this moment. After a B2 load into
    // the Dream (a party travel, a re-join) a guest's body is an own-world one (type 0) for ~0.3 s
    // until GuestChrTypeTick makes it a white phantom (1) again; these events run their first test
    // right at the load, so "1" then ended them for the rest of the stay: no kneel, no gesture, no
    // LevelUp menu (live sj8: the Doll's lines, then nothing - a player report: "the Doll randomly
    // doesn't work for guests"). The test now asks for the type the chr has when it runs.
    std::int8_t want = LocalSessionRole() == kSessionRoleClient ? 1 : 0;
    {
        std::uint64_t wcm = 0, me = 0;
        std::int32_t now_type = -1;
        if (want == 1 && SafeGet(Guest(0x553e878), &wcm) && wcm && SafeGet(wcm + 0x60, &me) && me &&
            SafeGet(me + 0x78, &now_type) && (now_type == 0 || now_type == 1)) {
            want = static_cast<std::int8_t>(now_type);
        }
    }
    if (type != want && SafeWrite(args + 8, &want, 1)) {
        g_doll_rewrites.fetch_add(1, std::memory_order_relaxed);
    }
}

// Main thread (talk scripts): IsMultiplayerInProgress for talk environment `env` (both roles):
// the headstones work in a party.
std::uint64_t BB_COOP_SYSV MultiFilter(std::uint64_t env, std::uint64_t value) {
    if (!(g_lamps.load(std::memory_order_relaxed) || g_headstones.load(std::memory_order_relaxed)) ||
        (value & 0xff) == 0) {
        return value;
    }
    const std::uint32_t talk = *reinterpret_cast<const std::uint32_t*>(env + 0x10);
    NoteMultiTalk(talk);
    // BB_PARTY_GUEST_ALTARS=1 (probe): the Dream's altar scripts t210050-t210110 too.
    static const bool altars = [] {
        const char* a = std::getenv("BB_PARTY_GUEST_ALTARS");
        return a && a[0] == '1';
    }();
    // G1 (0.6.2): the HOST's chalice altars too (live g1d: with a guest summoned into the host's
    // Dream the altar t210100 parked in its empty machine, no prompt, so the host could not enter
    // the party's dungeon); BB_PARTY_HOST_ALTARS=0 keeps them blocked while a guest is present.
    static const bool host_altars = !(std::getenv("BB_PARTY_HOST_ALTARS") && std::getenv("BB_PARTY_HOST_ALTARS")[0] == '0');
    const bool altar = (altars || (host_altars && LocalSessionRole() != kSessionRoleClient)) && talk >= 210050 &&
                       talk <= 210110 && talk % 10 == 0;
    // A Chalice Dungeon's lamps (t290110-t290119), both roles: the host's way out of its dungeon
    // and a guest's lamp (its warp becomes the party's travel question / request as for a world lamp).
    const bool chalice_lamp = ChaliceLampTalk(talk) && ChaliceLampsOn();
    if (!HeadstoneTalk(talk) && !LampTalk(talk) && !altar && !chalice_lamp) {
        return value;
    }
    g_multi_overrides.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("[coop] Party guest services: %s\n", line);
    std::fflush(stdout);
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

PlayerRecord RecordOf(const GameSnapshot& s) {
    PlayerRecord r;
    r.level = s.level;
    r.echoes = s.echoes;
    r.insight = s.insight;
    return r;
}

// Discovery: which fields of a talk environment name its script (t210140 = 210140 ...).
void LogEnv(std::uint64_t env) {
    std::string found;
    auto scan = [&](std::uint64_t base, const char* path, std::size_t n) {
        for (std::size_t off = 0; off + 4 <= n; off += 4) {
            std::int32_t v = 0;
            if (!SafeGet(base + off, &v)) {
                return;
            }
            if (v >= 100000 && v <= 999999) {
                char buf[96];
                std::snprintf(buf, sizeof buf, " %s+0x%zx=%d", path, off, v);
                found += buf;
            }
        }
    };
    scan(env, "", 0x200);
    for (std::size_t off = 0; off < 0x100; off += 8) {
        std::uint64_t p = 0;
        if (!SafeGet(env + off, &p) || p < 0x10000 || (p & 7)) {
            continue;
        }
        char path[32];
        std::snprintf(path, sizeof path, "[+0x%zx]", off);
        scan(p, path, 0x100);
    }
    std::string hex;
    for (std::size_t off = 0; off < 0x80; off += 8) {
        std::uint64_t q = 0;
        SafeGet(env + off, &q);
        char buf[24];
        std::snprintf(buf, sizeof buf, " %016llx", static_cast<unsigned long long>(q));
        hex += buf;
    }
    std::uint32_t talk = 0;
    SafeGet(env + 0x10, &talk);
    Log("talk script t%u (environment %p) asks IsClientPlayer: %s; ids:%s; head:%s", talk, reinterpret_cast<void*>(env),
        GuestServiceTalkAllowed(talk, g.extra) || (talk == kGehrmanTalk && g_gehrman_talk.load()) ? "allowed" : "kept vanilla",
        found.empty() ? " none" : found.c_str(),
        hex.c_str());
}

// Game thread (0x13CDF30, not our replays): a guest in the host's world used a lamp / headstone.
// Split travel asks first (alone / with the party) and consumes the warp; its answer "with the
// party" comes back through GuestLampRequestNow.
bool GuestLampIntercept(std::uint32_t id) {
    plog::NoteLoadReason("lamp / headstone warp " + std::to_string(id));  // party logging (party_log_game.h)
    if (split::OnLampWarp(id)) {
        g_lamp_requests_made.fetch_add(1);  // the lamppress test stops here too
        return true;
    }
    if (!g_lamps.load() || PartyDirector::Get().Role() != PartyRole::Guest ||
        g_session_role.load() != kSessionRoleClient) {
        return false;
    }
    return GuestLampRequestNow(id);
}
} // namespace

bool GuestLampRequestNow(std::uint32_t id) {
    if (!g_lamps.load() || PartyDirector::Get().Role() != PartyRole::Guest) {
        return false;
    }
    LampRequest r;
    {
        std::lock_guard<std::mutex> lk(g_lamp_mu);
        r.id = id;
        r.seq = ++g_lamp_seq;
        if (g_lamp_out.size() < 8) {
            g_lamp_out.push_back(r);
        }
    }
    Log("lamp: the guest's warp to %u becomes a request to the host (the party travels together)", id);
    g_lamp_requests_made.fetch_add(1);
    return true;
}

bool GuestRestRequestNow(std::uint32_t id) {
    if (PartyDirector::Get().Role() != PartyRole::Guest) {
        return false;
    }
    LampRequest r;
    {
        std::lock_guard<std::mutex> lk(g_lamp_mu);
        r.id = id;
        r.seq = ++g_lamp_seq;
        r.rest = true;
        if (g_lamp_out.size() < 8) {
            g_lamp_out.push_back(r);
        }
    }
    Log("lamp: the guest rested at %u and travels alone: the host's world gets a rest load", id);
    return true;
}

void LogButtons();  // the registration probe (below)

namespace {

// Diagnostics (BB_PARTY_GUESTSVC_TEST "probe"): the character with `entity` the way the talk
// environment finds it (0x203AC3D..0x203AD09): WorldChrMan +0x90 count of chr sets, +0x98[i] a set
// with +0xF0 count and +0xF8 sorted {entity, handle} pairs; handle bits 14..19 = slot block,
// bits 0..13 = index; block < WorldChrMan+0x20: entry = [WorldChrMan+0x28] + block * 0x160 ->
// +0xC0 array (stride 0x38), else (block == [[WorldChrMan+8]+0x18]) WorldChrMan+0x38 array;
// [entry] = ChrIns*.
int g_find_pick = 0;        // a Chalice Dungeon talk (29xxxx): which of the Bath Messengers, nearest first
std::size_t g_find_count = 0;  // how many there were at the last look

std::uint64_t FindChr(std::uint32_t entity, std::string* diag = nullptr) {
    const std::uint64_t wcm = ReadGameState(false).world_chr_man;
    std::int32_t sets = 0;
    const bool got = wcm && SafeGet(wcm + 0x90, &sets);
    if (diag) {
        char b[96];
        std::snprintf(b, sizeof b, "wcm %p sets %d", reinterpret_cast<void*>(wcm), got ? sets : -1);
        *diag = b;
    }
    if (!got || sets <= 0 || sets > 4096) {
        return 0;
    }
    std::vector<std::pair<float, std::uint64_t>> found;
    for (std::int32_t i = 0; i < sets; ++i) {
        std::uint64_t set = 0, arr = 0;
        std::int32_t n = 0;
        if (!SafeGet(wcm + 0x98 + 8 * std::uint64_t(i), &set) || !set || !SafeGet(set + 0xf0, &n) ||
            !SafeGet(set + 0xf8, &arr) || n <= 0 || n > 100000) {
            continue;
        }
        if (diag && i < 8) {
            std::int32_t first = 0, last = 0;
            SafeGet(arr, &first);
            SafeGet(arr + 8 * std::uint64_t(n - 1), &last);
            char b[96];
            std::snprintf(b, sizeof b, "; set %d: %d ids %d..%d", i, n, first, last);
            *diag += b;
        }
        for (std::int32_t k = 0; k < n; ++k) {
            std::int32_t id = 0, handle = 0;
            if (!SafeGet(arr + 8 * std::uint64_t(k), &id) || id != std::int32_t(entity)) {
                continue;
            }
            SafeGet(arr + 8 * std::uint64_t(k) + 4, &handle);
            if (handle < 0) {
                return 0;
            }
            const std::uint32_t block = (std::uint32_t(handle) >> 14) & 0x3f, index = std::uint32_t(handle) & 0x3fff;
            std::int32_t blocks = 0;
            std::uint64_t entry = 0;
            SafeGet(wcm + 0x20, &blocks);
            if (std::int32_t(block) < blocks) {
                std::uint64_t base = 0, list = 0;
                SafeGet(wcm + 0x28, &base);
                SafeGet(base + block * 0x160 + 0xc0, &list);
                entry = list ? list + index * 0x38 : 0;
            } else {
                std::uint64_t list = 0;
                SafeGet(wcm + 0x38, &list);
                entry = list ? list + index * 0x38 : 0;
            }
            std::uint64_t chr = 0;
            if (entry) {
                SafeGet(entry, &chr);
            }
            if (entity / 10000 != 29 || !chr) {
                return chr;
            }
            // A Chalice Dungeon holds one Bath Messenger (t290300) per layer: the one nearest this player.
            float me[3] = {}, at[3] = {};
            float d2 = 1e30f;
            if (balance::ChrPosition(ReadGameState(false).player, me) && balance::ChrPosition(chr, at)) {
                d2 = (me[0] - at[0]) * (me[0] - at[0]) + (me[1] - at[1]) * (me[1] - at[1]) + (me[2] - at[2]) * (me[2] - at[2]);
            }
            found.emplace_back(d2, chr);
        }
    }
    g_find_count = found.size();
    if (found.empty()) {
        return 0;
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });  // a stable order while the player moves
    return found[std::min<std::size_t>(std::size_t(std::max(g_find_pick, 0)), found.size() - 1)].second;
}

// Live tests: the save's Doll progress (campaign_start.md 1.3): 12100105 awake, 9401 first Dream,
// Insight >= 1, 12101024 / 12101025 (Beckoning Bell messenger). Logged once per world-up.
bool CheckDollProgress(const GameSnapshot& s, const char* who) {
    bool awake = false, first = false, m24 = false, m25 = false;
    progress::ReadFlag(12100105, &awake);
    progress::ReadFlag(9401, &first);
    progress::ReadFlag(12101024, &m24);
    progress::ReadFlag(12101025, &m25);
    const bool ok = awake && first && s.insight >= 1;
    Log("test: %s progress: 12100105=%d 9401=%d Insight=%d 12101024=%d 12101025=%d (%s, map %s)%s", who, awake, first,
        s.insight, m24, m25, SessionRoleName(s.session_role), MapName(s.map_id).c_str(),
        ok ? "" : " -- PROGRESS MISSING: the Doll cannot be awake on this save");
    return ok;
}

void ProbeDoll() {
    std::string diag;
    const std::uint64_t chr = FindChr(210304, &diag); // the table is keyed by talk script id
    {
        const GameSnapshot me = ReadGameState(false);
        std::uint64_t phys = 0;
        float p[4] = {};
        if (me.player) {
            SafeRead(me.player + 0xc0, p, sizeof p);
        }
        (void)phys;
        Log("probe: lookup %s; local player %p at +0xc0 (%.2f, %.2f, %.2f)", diag.c_str(),
            reinterpret_cast<void*>(me.player), p[0], p[1], p[2]);
    }
    std::int32_t type = -1, st10 = -1;
    std::uint8_t b20 = 0, draw[8] = {};
    std::uint64_t sub = 0, phys = 0;
    float pos[4] = {};
    if (chr) {
        SafeGet(chr + 0x78, &type);
        SafeGet(chr + 0x18, &sub);
        if (sub) {
            SafeGet(sub + 0x10, &st10);
            SafeGet(sub + 0x20, &b20);
        }
        SafeRead(chr + 0x1e0, draw, sizeof draw);
        SafeRead(chr + 0xc0, pos, sizeof pos);
    }
    // Discovery: where does the Doll keep her position? Float triples near one of her MSB spots
    // (2102301 / 2102302 / 2102303 / 2102304), in the chr and one pointer deep.
    if (chr) {
        static const float spots[4][3] = {{-5.8f, -5.62f, -13.6f}, {-13.27f, -1.05f, 1.73f}, {-7.4f, -6.25f, -25.09f},
                                          {-5.8f, -4.83f, -11.6f}};
        std::string hits;
        auto scan = [&](std::uint64_t base, const char* path, std::size_t n) {
            for (std::size_t off = 0; off + 12 <= n && hits.size() < 600; off += 4) {
                float v[3];
                if (!SafeRead(base + off, v, sizeof v)) {
                    return;
                }
                for (const auto& sp : spots) {
                    if (std::fabs(v[0] - sp[0]) < 1.5f && std::fabs(v[1] - sp[1]) < 1.5f && std::fabs(v[2] - sp[2]) < 1.5f) {
                        char b[96];
                        std::snprintf(b, sizeof b, " %s+0x%zx=(%.2f,%.2f,%.2f)", path, off, v[0], v[1], v[2]);
                        hits += b;
                        break;
                    }
                }
            }
        };
        scan(chr, "", 0x800);
        for (std::size_t off = 0; off < 0x200; off += 8) {
            std::uint64_t q = 0;
            if (!SafeGet(chr + off, &q) || q < 0x10000 || (q & 7)) {
                continue;
            }
            char path[24];
            std::snprintf(path, sizeof path, "[+0x%zx]", off);
            scan(q, path, 0x400);
        }
        Log("probe: Doll position candidates:%s", hits.empty() ? " none" : hits.c_str());
        float f[32] = {};
        SafeRead(chr + 0xa0, f, sizeof f);
        std::string fl;
        for (int k = 0; k < 32; ++k) {
            char b[24];
            std::snprintf(b, sizeof b, " %.3f", f[k]);
            fl += b;
        }
        Log("probe: Doll chr +0xa0..+0x120 floats:%s", fl.c_str());
    }
    std::string flags;
    for (const std::uint32_t f : {9401u, 100u, 1000u, 1001u, 1002u, 1003u, 12100105u, 12100500u, 12100501u, 12100502u,
                                  12100510u, 12105100u}) {
        bool on = false;
        const bool ok = progress::ReadFlag(f, &on);
        char b[32];
        std::snprintf(b, sizeof b, " %u=%s", f, ok ? (on ? "1" : "0") : "?");
        flags += b;
    }
    {
        const GameSnapshot me = ReadGameState(false);
        std::uint8_t own = 0xff;
        std::uint64_t wts = 0;
        if (SafeGet(Guest(0x5556678), &wts) && wts) {
            SafeGet(wts + 0x1592, &own);
        }
        float p[4] = {}, pr[4] = {}, dr[4] = {};
        if (me.player) {
            SafeRead(me.player + 0xc0, p, sizeof p);
            SafeRead(me.player + 0xd0, pr, sizeof pr);
        }
        if (chr) {
            SafeRead(chr + 0xd0, dr, sizeof dr);
        }
        const float dx = pos[0] - p[0], dz = pos[2] - p[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        // Angle between the player's facing (-sin, -cos) and the direction to the Doll.
        const float fx = -std::sin(pr[1]), fz = -std::cos(pr[1]);
        const float cosang = dist > 1e-3f ? (fx * dx + fz * dz) / dist : 1.0f;
        Log("probe: role %s, own world byte %u, Insight %d; player (%.2f, %.2f, %.2f) yaw %.3f; Doll yaw %.3f; "
            "distance %.2f m, facing her within %.0f degrees",
            SessionRoleName(me.session_role), own, me.insight, p[0], p[1], p[2], pr[1], dr[1], dist,
            std::acos(std::fmax(-1.0f, std::fmin(1.0f, cosang))) * 57.2958f);
    }
    Log("probe: Doll chr %p type %d, +0x18->+0x10 %d +0x20 0x%02x, +0x1e0 %02x %02x %02x %02x %02x %02x %02x %02x, "
        "pos (%.2f, %.2f, %.2f); flags%s",
        reinterpret_cast<void*>(chr), type, st10, b20, draw[0], draw[1], draw[2], draw[3], draw[4], draw[5], draw[6],
        draw[7], pos[0], pos[1], pos[2], flags.c_str());
}

// H5 evidence: the covenant is the oath rune's SpEffect on the player (EMEVD 4[5] checks 6142 for
// the League, 6100 for the Vilebloods): 6100 Corruption, 6110 Radiance, 6120 Hunter, 6142
// Impurity. The SpEffect list: ChrIns +0x1C8, first node +8, id +0x40, next +0x58.
std::string OathEffects(u64 player) {
    static const std::pair<int, const char*> kOath[] = {
        {6100, "Vilebloods"}, {6110, "Executioners"}, {6120, "Hunter of Hunters"}, {6142, "League"}};
    std::string out;
    u64 list = 0, node = 0;
    if (player && SafeGet(player + 0x1c8, &list) && list && SafeGet(list + 8, &node)) {
        for (int guard = 0; node && guard < 512; ++guard) {
            std::int32_t id = 0;
            if (!SafeGet(node + 0x40, &id)) break;
            for (const auto& [e, name] : kOath)
                if (id == e) out += (out.empty() ? "" : ", ") + std::string(name) + " (" + std::to_string(e) + ")";
            if (!SafeGet(node + 0x58, &node)) break;
        }
    }
    return out.empty() ? "none" : out;
}

// Live test (BB_PARTY_GUESTSVC_TEST "levelup"): the Doll's level up on the guest, event-driven
// through the game's own pad path (runtime_pad_director_press, no OS input). Each stage waits for
// what the game shows: message lookups (BbGameMenu::LastLookupMs / TextActivity: a text on screen
// is looked up every frame) and the character record. Talk menu texts (talk ESD t210304):
// 15000100 "Channel the Blood Echoes", 15000007 "Talk", 15000005 "Leave".
// The level-up menu: talk command OpenSoul (command switch 0x203FFD0 case 31 at 0x2040B4E) queues a
// menu request (0x1C4A520) with the factory 0x1BB7F60, which builds the menu named "LevelUp"
// (0x49B46F6, through 0x1C1CDF0). A pre-hook on the factory tells the test (and the log) that the
// game really built the level-up menu on this game.
std::atomic<std::uint32_t> g_levelup_menus{0};
BB_COOP_SYSV void LevelUpMenuFactoryEntry(u64, u64, u64, u64, u64, u64) {
    g_levelup_menus.fetch_add(1, std::memory_order_relaxed);
}

// Menu request 0x1C4A520(out, factory std::function*, ...): the OpenSoul command (0x2040BB2) passes
// a functor whose call pointer ([[rsi+0x20]+8]) is the LevelUp factory 0x1BB7F60. Its first test is
// 0x176BFE0 (called at 0x1C4A549; false = no menu). Logged for the live tests.
std::atomic<std::uint32_t> g_menu_requests{0}, g_levelup_requests{0};
std::atomic<bool> g_menu_req_active{false};
BB_COOP_SYSV void MenuRequestEntry(u64, u64 functor, u64, u64, u64, u64) {
    g_menu_requests.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t impl = 0, fn = 0;
    if (functor && SafeGet(functor + 0x20, &impl) && impl && SafeGet(impl + 8, &fn) && fn == Guest(0x1BB7F60)) {
        g_levelup_requests.fetch_add(1, std::memory_order_relaxed);
    }
}

// Talk commands (switch 0x203FFD0: rsi = the call, vfunc +0x18 = command id 0..87): a count per id
// while the gate is open (the live tests: did OpenSoul, id 31, run on the guest?).
std::atomic<std::uint32_t> g_talk_cmds[96];
BB_COOP_SYSV void TalkCommandEntry(u64, u64 call, u64, u64, u64, u64) {
    if (!g_gate.load(std::memory_order_relaxed) || !call) {
        return;
    }
    std::uint64_t vtbl = 0, fn = 0;
    if (!SafeGet(call, &vtbl) || !SafeGet(vtbl + 0x18, &fn) || !fn) {
        return;
    }
    const std::uint32_t id = reinterpret_cast<std::uint32_t(BB_COOP_SYSV*)(u64)>(fn)(call);
    if (id < 96) {
        g_talk_cmds[id].fetch_add(1, std::memory_order_relaxed);
    }
}

struct LevelUpScript {
    enum Stage { Off, WaitPlaced, Greeting, MenuUp, WaitScreen, Raise, Confirm, Accept, WaitLevel, BackOut, Done, Keys };
    std::uint32_t menus0 = 0;
    bool shop = false;                 ///< "buy": a messenger's Purchase instead of the Doll's level up
    bool keybuy = false;               ///< keys=...,buy: the dialog's cursor found by trying Left / Right / Down
    bool open = false;                 ///< "open[=TEXT]": open a menu, then press "keys=" one by one
    std::string keys;                  ///< "cross,left,..." pressed 1.5 s apart once the menu is open
    std::size_t key_at = 0;
    std::uint32_t target = 0;          ///< talk id of the character / object to use
    int approach = 0;                  ///< WaitPlaced: placement attempts (distance x side)
    std::uint32_t menu_text = 15000100; ///< the talk menu row to choose
    std::uint32_t requests0 = 0;
    int insight0 = -1;
    Stage stage = Off;
    double at = 0, press_until = 0;
    int tries = 0, level0 = -1, echoes0 = -1;
    std::uint64_t seq0 = 0;
} g_lvl;
// The registration probe's last talk-type (13) button (diagnostics: a headstone's prompt never
// showed as an action-button text or a type-13 registration in the da runs).
std::atomic<double> g_talk_btn_at{0};
std::atomic<int> g_talk_btn_id{0};
std::atomic<std::uint64_t> g_btn_man{0};  // SprjActionButtonMan as the registration probe saw it

constexpr std::uint32_t kPadCross = 0x4000, kPadCircle = 0x2000, kPadRight = 0x20, kPadLeft = 0x80, kPadUp = 0x10,
                        kPadDown = 0x40;
constexpr std::uint32_t kTextChannel = 15000100, kTextLeave = 15000005, kTextPurchase = 15000140;

bool OnScreen(std::uint32_t id, std::int64_t within_ms = 400) {
    const std::int64_t ms = BbGameMenu::LastLookupMs(id);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    return ms && now - ms < within_ms;
}

// The action prompt: ActionButtonParam textId of the Dream's talk / examine regions (6003 / 6004 /
// 6019-6026 / 6030: 10010100, 10010200, 10010220, 70000009) - looked up every frame while shown.
// Any action-button text on screen: ActionButtonParam text ids are 1001xxxx (talk / examine /
// "Workshop"-style object prompts) or 7000xxxx; *id receives the first one seen.
bool PromptUp(std::uint32_t* id = nullptr) {
    std::uint32_t ids[256];
    const int n = BbGameMenu::RecentLookups(400, ids, 256);
    for (int i = 0; i < n; ++i) {
        if ((ids[i] >= 10010000 && ids[i] < 10020000) || (ids[i] >= 70000000 && ids[i] < 70010000)) {
            if (id) {
                *id = ids[i];
            }
            return true;
        }
    }
    return false;
}

// Stand the local player at `dist` m from `chr` on the side `side` (0 its front, 1 back, 2 / 3
// its sides; forward(yaw) = (-sin, -cos)), facing it (PlayerIns vfunc +0x5F8).
bool StandAt(const GameSnapshot& s, std::uint64_t chr, float dist, int side, float turn = 0.0f) {
    float d[4] = {}, r[4] = {};
    if (!chr || !s.player || !SafeRead(chr + 0xc0, d, sizeof d) || !SafeRead(chr + 0xd0, r, sizeof r)) {
        return false;
    }
    const float a = r[1] + turn + (side == 1 ? 3.14159265f : side == 2 ? 1.5707963f : side == 3 ? -1.5707963f : 0.0f);
    alignas(16) float pos[4] = {d[0] - dist * std::sin(a), d[1], d[2] - dist * std::cos(a), 1.0f};
    alignas(16) float rot[4] = {0, a + 3.14159265f, 0, 0};
    std::uint64_t vtbl = 0, fn = 0;
    if (!SafeGet(s.player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) {
        return false;
    }
    using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
    return true;
}

void Tap(std::uint32_t buttons, double now) {
    runtime_pad_director_press(buttons);
    g_lvl.press_until = now + 0.12;
}


void LevelUpTick(const GameSnapshot& s, double now) {
    LevelUpScript& L = g_lvl;
    if (L.stage == LevelUpScript::Off || L.stage == LevelUpScript::Done) {
        return;
    }
    if (L.press_until > 0) {
        if (now < L.press_until) {
            return;
        }
        runtime_pad_director_press(0);
        L.press_until = 0;
    }
    auto go = [&](LevelUpScript::Stage st, const char* what) {
        L.stage = st;
        L.at = now;
        L.tries = 0;
        Log("test: levelup: %s", what);
    };
    const double since = now - L.at;
    switch (L.stage) {
    case LevelUpScript::WaitPlaced: {
        // Step in along the target's front (1.2 m, then closer), 1 s at each spot: log the
        // action-button texts seen, press Cross, and let Greeting look for the talk menu.
        static const float dists[] = {1.2f, 1.0f, 0.85f, 0.7f, 0.55f, 0.4f};
        if (since < 1.0) {
            break;
        }
        if (L.approach > 0) {
            std::uint32_t prompt = 0;
            bool up = PromptUp(&prompt);
            if (g.test.find("needprompt") != std::string::npos && L.tries == 0) {
                // "needprompt" (headstone fixtures): the prompt's proof is a screenshot (the smoke's
                // watch on this line), 1.5 s before Cross; the registrations seen meanwhile are logged.
                if (since < 1.5) {
                    break;
                }
                L.tries = 1;
                LogButtons();
                Log("test: levelup: before Cross at %.2f m (prompt screenshot; text %s)", dists[L.approach - 1],
                    up ? std::to_string(prompt).c_str() : "not seen");
                L.at = now;
                break;
            }
            if (g.test.find("needprompt") != std::string::npos && since < 1.5) {
                break;
            }
            L.requests0 = g_menu_requests.load();
            Tap(kPadCross, now);
            Log("test: levelup: at %.2f m: action prompt %s%s; Cross", dists[L.approach - 1], up ? "text " : "not seen",
                up ? std::to_string(prompt).c_str() : "");
            go(LevelUpScript::Greeting, "advancing the lines until the menu is up");
            break;
        }
        const bool ok = StandAt(s, FindChr(L.target), dists[0], 0);
        float cpos[3] = {};
        const bool have_c = balance::ChrPosition(FindChr(L.target), cpos);
        Log("test: levelup: standing %.2f m in front of t%u%s (candidate %d of %zu, at %.1f %.1f %.1f)", dists[0], L.target,
            ok ? "" : " - warp failed", g_find_pick, g_find_count, have_c ? double(cpos[0]) : 0.0, have_c ? double(cpos[1]) : 0.0,
            have_c ? double(cpos[2]) : 0.0);
        L.approach = 1;
        // A Chalice Dungeon's Bath Messenger can stand far from where the world is loaded: the floor streams in
        // after the warp, press Cross only then (the player falls through it otherwise).
        L.at = L.target / 10000 == 29 ? now + 9.0 : now;
        break;
    }
    case LevelUpScript::Greeting:
        if (L.open && L.menu_text == 0 && g_menu_requests.load() != L.requests0) {
            L.level0 = s.level;
            L.echoes0 = s.echoes;
            L.insight0 = s.insight;
            L.key_at = 0;
            go(LevelUpScript::Keys, "no talk list: the menu was requested (0x1C4A520): menu open");
        } else if (L.menu_text != 0 && OnScreen(L.menu_text) && OnScreen(kTextLeave)) {
            go(LevelUpScript::MenuUp, L.open ? "talk menu up: the chosen row is on screen"
                                    : L.shop ? "talk menu up: 'Purchase' (15000140) is on screen"
                                             : "talk menu up: 'Channel the Blood Echoes' (15000100) is on screen");
        } else if (since >= 1.5 * (L.tries + 1)) {
            const int limit = L.target == 210304 ? 12 : 3;
            if (++L.tries > limit) {
                static const float dists[] = {1.2f, 1.0f, 0.85f, 0.7f, 0.55f, 0.4f};
                if (L.approach < 6) {
                    const float d = dists[L.approach++];
                    StandAt(s, FindChr(L.target), d, 0);
                    Log("test: levelup: no menu; one step closer: %.2f m", d);
                    L.stage = LevelUpScript::WaitPlaced;
                    L.at = now;
                    L.tries = 0;
                    break;
                }
                if (L.target / 10000 == 29 && std::size_t(g_find_pick) + 1 < g_find_count) {
                    ++g_find_pick;  // the next Bath Messenger of the dungeon
                    L.approach = 0;
                    L.stage = LevelUpScript::WaitPlaced;
                    L.at = now;
                    L.tries = 0;
                    Log("test: levelup: no talk menu here: the next Bath Messenger (candidate %d)", g_find_pick);
                    break;
                }
                go(LevelUpScript::Done, "FAILED: no talk menu at any distance");
                break;
            }
            Tap(kPadCross, now);
            Log("test: levelup: line %d: Cross", L.tries);
        }
        break;
    case LevelUpScript::MenuUp:
        if (since >= 0.8) {
            // The menu's cursor starts on its first row, "Channel the Blood Echoes" (AddTalkListData
            // index 1). Make sure: Up wraps would move it, so press nothing but Cross.
            L.menus0 = g_levelup_menus.load();
            L.requests0 = g_menu_requests.load();
            Tap(kPadCross, now);
            go(LevelUpScript::WaitScreen, L.open ? "Cross on the chosen row"
                                          : L.shop ? "Cross on 'Purchase'" : "Cross on 'Channel the Blood Echoes'; the Doll's lines follow");
        }
        break;
    case LevelUpScript::WaitScreen:
        // The guest kneels (event 12100112), the Doll speaks; then OpenSoul builds the menu.
        if (L.open && g_menu_requests.load() != L.requests0) {
            L.level0 = s.level;
            L.echoes0 = s.echoes;
            L.insight0 = s.insight;
            L.key_at = 0;
            go(LevelUpScript::Keys, "the menu was requested (0x1C4A520): menu open");
        } else if (L.shop && g_menu_requests.load() != L.requests0) {
            go(LevelUpScript::Raise, "the shop menu was requested (OpenRegularShop -> 0x1C4A520): shop open");
        } else if (g_levelup_menus.load() != L.menus0) {
            go(LevelUpScript::Raise, "the game built the LevelUp menu (factory 0x1BB7F60): level-up screen open");
        } else if (since >= 2.0 * (L.tries + 1)) {
            if (++L.tries > 10) {
                go(LevelUpScript::Done, "FAILED: no LevelUp menu after the Doll's lines (10 presses)");
                break;
            }
            Tap(kPadCross, now);
            Log("test: levelup: Doll line %d: Cross", L.tries);
        }
        break;
    case LevelUpScript::Raise:
        if (since >= 1.5) {
            L.level0 = s.level;
            L.echoes0 = s.echoes;
            L.insight0 = s.insight;
            if (L.shop) {
                Tap(kPadCross, now);
                go(LevelUpScript::Confirm, "Cross on the shop's first item");
                break;
            }
            Tap(L.tries == 0 ? kPadDown : kPadRight, now);
            if (L.tries++ == 0) {
                Log("test: levelup: Down (first attribute)");
                L.at = now - 0.5;
                break;
            }
            go(LevelUpScript::Confirm, "Right: +1 on the selected attribute");
        }
        break;
    case LevelUpScript::Confirm:
        if (since >= 1.0) {
            Tap(kPadCross, now);
            go(LevelUpScript::Accept, "Cross: confirm the level up");
        }
        break;
    case LevelUpScript::Accept:
        if (L.shop && L.echoes0 >= 0 && (s.echoes != L.echoes0 || s.insight != L.insight0)) {
            go(LevelUpScript::WaitLevel, "the purchase went through");
        } else if (s.level > L.level0 && L.level0 >= 0) {
            go(LevelUpScript::WaitLevel, "level changed already");
        } else if (L.keybuy && since >= 1.2) {
            // Cancel just closes the dialog and the next Cross opens it again on the same row.
            static const std::uint32_t seq[] = {kPadLeft, kPadCross, kPadCross, kPadRight, kPadCross, kPadCross, kPadDown, kPadCross};
            static const char* const names[] = {"Left", "Cross", "Cross", "Right", "Cross", "Cross", "Down", "Cross"};
            if (L.tries >= 8) {
                go(LevelUpScript::WaitLevel, "no purchase after the dialog presses");
                break;
            }
            Tap(seq[L.tries], now);
            Log("test: levelup: buy dialog: %s (Insight %d, echoes %d)", names[L.tries], s.insight, s.echoes);
            ++L.tries;
            L.at = now;
        } else if (since >= 1.2) {
            if (++L.tries > 2) {
                go(LevelUpScript::WaitLevel, "no level change after the confirm presses");
                break;
            }
            // The confirmation dialog ("Spend N Blood Echoes and level up?"): the cursor starts on
            // NO (live: Cross there cancelled the raise), so Left to YES, then Cross.
            Tap(L.tries % 2 ? kPadLeft : kPadCross, now);
            Log("test: levelup: dialog: %s (echoes %d)", L.tries % 2 ? "Left (to YES)" : "Cross", s.echoes);
            L.at = now; // space the presses (1.2 s)
        }
        break;
    case LevelUpScript::WaitLevel:
        if (L.shop && L.echoes0 >= 0 && (s.echoes != L.echoes0 || s.insight != L.insight0)) {
            Log("test: levelup: PURCHASE on the guest: echoes %d -> %d, Insight %d -> %d (Sinister Root Chalices held: Lower "
                "Pthumeru 6302 x%d, Hintertomb 6312 x%d, Isz 6532 x%d)",
                L.echoes0, s.echoes, L.insight0, s.insight, GoodsCount(6302), GoodsCount(6312), GoodsCount(6532));
            go(LevelUpScript::BackOut, "backing out (Circle)");
        } else if (L.shop && since >= 4.0) {
            Log("test: levelup: FAILED: no purchase (echoes %d, Insight %d)", s.echoes, s.insight);
            go(LevelUpScript::BackOut, "backing out (Circle)");
        } else if (L.shop) {
        } else if (s.level > L.level0 && L.level0 >= 0) {
            Log("test: levelup: LEVEL UP on the guest: level %d -> %d, echoes %d -> %d", L.level0, s.level, L.echoes0,
                s.echoes);
            go(LevelUpScript::BackOut, "backing out (Circle)");
        } else if (since >= 4.0) {
            Log("test: levelup: FAILED: level still %d (echoes %d)", s.level, s.echoes);
            go(LevelUpScript::BackOut, "backing out (Circle)");
        }
        break;
    case LevelUpScript::Keys:
        if (since >= 1.5) {
            if (L.key_at >= L.keys.size()) {
                Log("test: levelup: keys done: level %d -> %d, echoes %d -> %d, Insight %d -> %d, Blood Stone Shards now %d, Twin %d; "
                    "covenant (oath rune SpEffects on the player): %s",
                    L.level0, s.level, L.echoes0, s.echoes, L.insight0, s.insight, GoodsCount(3000), GoodsCount(3010),
                    OathEffects(s.player).c_str());
                go(LevelUpScript::BackOut, "backing out (Circle)");
                break;
            }
            std::size_t end = L.keys.find(',', L.key_at);
            if (end == std::string::npos) {
                end = L.keys.size();
            }
            const std::string k = L.keys.substr(L.key_at, end - L.key_at);
            L.key_at = end + 1;
            if (k == "buy") {
                // The shop's purchase dialog ("Buy X for N Insight?", cursor on CANCEL): Left / Cross
                // until the echoes or Insight change (Accept), then the purchase is logged.
                L.shop = true;
                L.keybuy = true;
                L.echoes0 = s.echoes;
                L.insight0 = s.insight;
                Tap(kPadCross, now);
                go(LevelUpScript::Accept, "key buy: Cross on the selected row, then the dialog");
                break;
            }
            const std::uint32_t b = k == "cross" ? kPadCross : k == "circle" ? kPadCircle : k == "left" ? kPadLeft
                                    : k == "right" ? kPadRight : k == "up" ? kPadUp : k == "down" ? kPadDown : 0;
            if (b) {
                Tap(b, now);
            }
            Log("test: levelup: key %s (level %d, echoes %d, Insight %d, Blood Stone Shards %d, Twin %d, Blood Vials %d)",
                k.c_str(), s.level, s.echoes, s.insight, GoodsCount(3000), GoodsCount(3010), GoodsCount(1000));
            L.at = now;
        }
        break;
    case LevelUpScript::BackOut:
        if (since >= 1.0 * (L.tries + 1)) {
            // A game message box (MessageUI +0x78 != 0, e.g. the vanilla "storage is full" notice
            // after a purchase) takes Cross (OK); Circle backs out of the menus.
            std::uint64_t ui = 0;
            std::int32_t box = 0;
            const bool boxed = SafeGet(Guest(0x5562878), &ui) && ui && SafeGet(ui + 0x78, &box) && box != 0;
            if (++L.tries > (boxed ? 8 : 4)) {
                go(LevelUpScript::Done, boxed ? "done (a message box still up)" : "done (menus closed)");
                break;
            }
            Tap(boxed ? kPadCross : kPadCircle, now);
            if (boxed) {
                Log("test: levelup: a message box is up (kind %d): Cross (OK)", box);
            }
        }
        break;
    default:
        break;
    }
}

// Test fixture: stand the guest somewhere (PlayerIns vfunc +0x5F8 warp 0x18CD600: rsi float4
// position, rdx float4 rotation (0, yaw, 0, 0), ecx 0; the convention of 0x13AEADD) and/or set its
// echoes, so a scripted run can open a menu and spend them. Never on by default.
// Step `step` (0: the echoes and warp 0; k: warp k). False when there is no such step.
bool RunTestFixture(const GameSnapshot& s, int step) {
    const std::size_t e = g.test.find("echoes=");
    if (step == 0 && e != std::string::npos && s.player_rec) {
        const std::int32_t v = std::atoi(g.test.c_str() + e + 7);
        if (SafeWrite(s.player_rec + 0x94, &v, 4)) {
            Log("test: guest echoes set to %d", v);
        }
    }
    const std::size_t sh = g.test.find("shards=");
    if (step == 0 && sh != std::string::npos) {
        // Blood Stone Shards (goods 3000) and Twin Blood Stone Shards (3010: +3 -> +4 and up) for the
        // workbench check.
        const int n = std::atoi(g.test.c_str() + sh + 7);
        const int b0 = GoodsCount(3000), b1 = GoodsCount(3010);
        const bool ok = GiveGoods(3000, n) && GiveGoods(3010, n);
        Log("test: Blood Stone Shards %d -> %d, Twin Blood Stone Shards %d -> %d%s", b0, GoodsCount(3000), b1,
            GoodsCount(3010), ok ? "" : " (give failed)");
    }
    // "goods=ID:N" (e.g. the Rune Workshop Tool 4104 for the Memory Altar) and "rune=ID" (a Caryll
    // rune, EquipParamAccessory 100..150) for the altar check.
    if (const std::size_t gd = g.test.find("goods="); step == 0 && gd != std::string::npos) {
        unsigned id = 0;
        int n = 1;
        std::sscanf(g.test.c_str() + gd + 6, "%u:%d", &id, &n);
        const int b = GoodsCount(id);
        const bool ok = id && GiveGoods(id, n);
        Log("test: goods %u: %d -> %d%s", id, b, GoodsCount(id), ok ? "" : " (give failed)");
    }
    if (const std::size_t rn = g.test.find("rune="); step == 0 && rn != std::string::npos) {
        const unsigned id = unsigned(std::strtoul(g.test.c_str() + rn + 5, nullptr, 10));
        const int b = ItemCount(kItemAccessory, id);
        const bool ok = id && GiveItem(kItemAccessory, id, 1);
        Log("test: rune (accessory) %u: %d -> %d%s", id, b, ItemCount(kItemAccessory, id), ok ? "" : " (give failed)");
    }
    if (const std::size_t lt = g.test.find("lot="); step == 0 && lt != std::string::npos) {
        // "lot=N": the game's own item-lot award on the guest (e.g. 3401700: a Caryll Rune, a
        // generated gem, 90230, with its 1000 lot points: always), for the Memory Altar check.
        const unsigned id = unsigned(std::strtoul(g.test.c_str() + lt + 4, nullptr, 10));
        // Guest-only: the award is not reported up as an NPC gift (the host does not get it too).
        Log("test: item lot %u %s", id,
            id && AwardLotGuestOnly(id) ? "awarded (the game's award path, guest only)" : "NOT awarded");
    }
    const std::size_t w = g.test.find("warp=");
    if (w == std::string::npos) {
        return step == 0;
    }
    const char* at = g.test.c_str() + w + 5;
    for (int k = 0; k < step; ++k) {
        at = std::strchr(at, '/');
        if (!at) {
            return false;
        }
        ++at;
    }
    alignas(16) float pos[4] = {0, 0, 0, 1};
    alignas(16) float rot[4] = {0, 0, 0, 0};
    std::uint32_t talk_target = 0;
    if (std::strncmp(at, "doll", 4) == 0) {
        talk_target = 210304;
    } else if (std::strncmp(at, "talk:", 5) == 0) {
        talk_target = std::uint32_t(std::strtoul(at + 5, nullptr, 10));
    }
    if (talk_target && talk_target != 210304 && FindChr(talk_target)) {
        const std::uint64_t chr = FindChr(talk_target);
        float d[4] = {}, r[4] = {};
        SafeRead(chr + 0xc0, d, sizeof d);
        SafeRead(chr + 0xd0, r, sizeof r);
        const float yaw = r[1];
        pos[0] = d[0] - 1.2f * std::sin(yaw);
        pos[1] = d[1];
        pos[2] = d[2] - 1.2f * std::cos(yaw);
        rot[1] = yaw + 3.14159265f;
        Log("test: t%u's character stands at (%.2f, %.2f, %.2f) yaw %.3f; guest goes 1.2 m in front, yaw %.3f",
            talk_target, d[0], d[1], d[2], yaw, rot[1]);
    } else if (std::strncmp(at, "doll", 4) == 0 && FindChr(210304)) {
        // In front of the Doll as this game placed her: ChrIns +0xC0 position, +0xD0 rotation
        // (x, yaw, z, 0; 0.524 = her MSB 30 degrees). Live evidence: a character with yaw t faces
        // (-sin t, 0, -cos t) (a guest put at +(sin, cos) * 1.2 ended up behind her). So: 1.2 m along
        // her forward, facing back at her (yaw + pi).
        const std::uint64_t doll = FindChr(210304);
        float d[4] = {}, r[4] = {};
        SafeRead(doll + 0xc0, d, sizeof d);
        SafeRead(doll + 0xd0, r, sizeof r);
        // +0xC0 / +0xD0 keep her MSB spot by the lamp; event 12100110 short-warps her to one of three
        // stand spots each Dream load (2102301 lamp / 2102302 workshop / 2102303 lower garden), so the
        // live spot is the physics one (+0x58 -> +8 -> +0x3B0 -> +0x68: +0x1E0 position, +0x1D0
        // rotation). Release smoke full1 sj: she stood elsewhere, the guest talked to the empty lamp spot.
        {
            std::uint64_t ph = 0;
            float pp[4] = {}, pr[4] = {};
            if (SafeGet(doll + 0x58, &ph) && ph && SafeGet(ph + 8, &ph) && ph && SafeGet(ph + 0x3b0, &ph) && ph &&
                SafeGet(ph + 0x68, &ph) && ph && SafeRead(ph + 0x1e0, pp, sizeof pp) && SafeRead(ph + 0x1d0, pr, sizeof pr) &&
                (pp[0] != 0.0f || pp[2] != 0.0f)) {
                if (std::fabs(pp[0] - d[0]) + std::fabs(pp[2] - d[2]) > 0.5f) {
                    Log("test: the Doll was moved to her stand spot (%.2f, %.2f, %.2f) yaw %.3f (her MSB spot (%.2f, %.2f, %.2f))",
                        pp[0], pp[1], pp[2], pr[1], d[0], d[1], d[2]);
                }
                std::memcpy(d, pp, sizeof d);
                r[1] = pr[1];
            }
        }
        const float yaw = r[1];
        pos[0] = d[0] - 1.2f * std::sin(yaw);
        pos[1] = d[1];
        pos[2] = d[2] - 1.2f * std::cos(yaw);
        rot[1] = yaw + 3.14159265f;
        Log("test: the Doll stands at (%.2f, %.2f, %.2f) yaw %.3f on this game; guest goes 1.2 m in front, yaw %.3f",
            d[0], d[1], d[2], yaw, rot[1]);
    } else if (std::strncmp(at, "doll", 4) == 0) {
        // In front of the Doll's stand spot on this game (12100500-12100502, event 12100110):
        // MSB regions 2102301 / 2102302 / 2102303; 1.2 m off, facing her.
        static const float spots[3][3] = {{-5.8f, -5.62f, -13.6f}, {-13.27f, -1.05f, 1.73f}, {-7.4f, -6.25f, -25.09f}};
        int spot = 0;
        for (int k = 0; k < 3; ++k) {
            bool on = false;
            if (progress::ReadFlag(12100500 + k, &on) && on) {
                spot = k;
            }
        }
        pos[0] = spots[spot][0] + 0.6f;
        pos[1] = spots[spot][1];
        pos[2] = spots[spot][2] + 1.04f;
        rot[1] = std::atan2(-0.6f, -1.04f);
        Log("test: the Doll's spot on this game is #%d (12100500 + %d)", spot + 1, spot);
    } else if (talk_target && talk_target != 210304) {
        return false;
    } else if (std::sscanf(at, "%f,%f,%f,%f", &pos[0], &pos[1], &pos[2], &rot[1]) != 4) {
        return false;
    }
    if (!s.player) {
        return false;
    }
    std::uint64_t vtbl = 0, fn = 0;
    if (SafeGet(s.player, &vtbl) && SafeGet(vtbl + 0x5f8, &fn) && fn) {
        using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
        reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
        float now_at[3] = {};
        SafeRead(s.player + 0xc0, now_at, sizeof now_at);
        Log("test: warp #%d: the guest to (%.2f, %.2f, %.2f) yaw %.2f (now at %.2f, %.2f, %.2f)", step, pos[0], pos[1],
            pos[2], rot[1], now_at[0], now_at[1], now_at[2]);
    }
    return true;
}

} // namespace

BB_COOP_SYSV void LampObjEntry(u64 obj, u64, u64, u64, u64, u64); // test probe (below)
BB_COOP_SYSV void TalkManEntry(u64 man, u64, u64, u64, u64, u64);  // test probe (below)
BB_COOP_SYSV void ButtonEntry(u64, u64 type, u64 id, u64, u64, u64);  // test probe (below)

// Packet 0x31 (a network-synced EMEVD event's state) is drained by 0x12F0F90 (the EMEVD event
// manager's update) at 0x12F1744: 0x1789200(net man, peer, 0x31, buf, 0x100) until it returns 0;
// buf {+0 u16 kind (0: event state), +2 u16, +4 u32 size (< 0xF1), +8 i32 event id, +0xC i16 slot,
// +0x10 state} goes to every event instance with that id and slot (vfunc +0x28). The lower bath's
// messengers' animation event is dropped here (PerPlayerMessengerEvent).
constexpr u64 kEventSyncRead = 0x12f1744, kPacketRead = 0x1789200;
std::atomic<std::uint32_t> g_messenger_drops{0};
/// BB_PARTY_LAMP_EVENT_SYNC=1 (testing): a peer's lamp / headstone warp event state is applied again.
bool LampSyncFilterOn() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_LAMP_EVENT_SYNC");
        return !(v && v[0] == '1');
    }();
    return on;
}
/// BB_PARTY_BOSS_PHASE_SYNC=1 (testing): a guest's boss phase event state is applied on the host again.
bool BossPhaseSyncFilterOn() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_BOSS_PHASE_SYNC");
        return !(v && v[0] == '1');
    }();
    return on;
}
bool EventSyncLogOn() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_EVENT_SYNC_LOG");
        return v && v[0] == '1';
    }();
    return on;
}
bool MessengerSyncFilterOn() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_MESSENGER_SYNC");
        return !(v && v[0] == '1' && !v[1]);
    }();
    return on;
}
BB_COOP_SYSV u64 EventSyncRead(u64 man, u64 peer, u64 type, u64 buf, u64 size) {
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64);
    for (;;) {
        const u64 r = reinterpret_cast<Fn>(Guest(kPacketRead))(man, peer, type, buf, size);
        std::uint16_t kind = 1;
        std::int32_t event = 0;
        if (EventSyncLogOn() && std::int32_t(r) > 0 && buf && SafeGet(buf, &kind) && SafeGet(buf + 8, &event)) {
            // BB_PARTY_EVENT_SYNC_LOG=1: which network-synced EMEVD events a peer's state arrives for
            // (at most 600 lines).
            static int logged = 0;
            std::int16_t slot = 0;
            std::int32_t state = 0;
            SafeGet(buf + 0xc, &slot);
            SafeGet(buf + 0x10, &state);
            if (logged < 600) {
                ++logged;
                Log("event sync: a peer's packet 0x31 kind %u: event %d slot %d state %d", unsigned(kind), event, int(slot), state);
            }
            kind = 1;
            event = 0;
        }
        if (std::int32_t(r) <= 0 || !buf || !SafeGet(buf, &kind) || kind != 0 || !SafeGet(buf + 8, &event) ||
            !((MessengerSyncFilterOn() && PerPlayerMessengerEvent(std::uint32_t(event))) ||
              (LampSyncFilterOn() && deathrules::LampWarpEvent(std::uint32_t(event))) ||
              (BossPhaseSyncFilterOn() && bossfog::HostOnlyBossEvent(std::uint32_t(event)) &&
               PartyDirector::Get().Role() == PartyRole::Host))) {
            return r;
        }
        if (bossfog::HostOnlyBossEvent(std::uint32_t(event))) {
            // Main thread. A guest's copy of a boss's phase event finished on its own chr: not the host's call.
            static std::atomic<int> boss_drops{0};
            if (boss_drops.fetch_add(1) < 8) {
                Log("Boss phase: event %d's state from a guest not applied (the host's own boss chr decides when "
                    "the phase scene plays)", event);
            }
            continue;
        }
        // Main thread (the EMEVD update); the next packet, if any, is read in the same call.
        if (g_messenger_drops.fetch_add(1) < 8) {
            Log("%s: event %d's state from another player not applied (%s)",
                deathrules::LampWarpEvent(std::uint32_t(event)) ? "Lamp warp" : "Messengers", event,
                deathrules::LampWarpEvent(std::uint32_t(event))
                    ? "a lamp / headstone warp event runs from its own player's choice only, or a peer's pick would "
                      "use up this game's copy"
                    : "per player: each game runs its messengers from its own player's talk, pick and flags");
        }
    }
}

void InstallMessengerSyncFilter() {
    const char* v = std::getenv("BB_PARTY_MESSENGER_SYNC");
    if (v && v[0] == '1' && !v[1] && !LampSyncFilterOn()) {  // both old behaviours asked for: no hook
        Log("Bath Messengers: the animation events stay network-synced (BB_PARTY_MESSENGER_SYNC=1: the old "
            "behaviour: they rise and sink every 2 s while another player uses their shop)");
        return;
    }
    const bool ok = HookCallSite(kEventSyncRead, kPacketRead, reinterpret_cast<const void*>(&EventSyncRead),
                                 "Bath Messengers: per-player animation event (EMEVD sync packet 0x31)");
    Log("Bath Messengers: the lower bath's animation event 12105040 per player %s", ok ? "on" : "NOT installed");
}

void GuestServicesInit() {
    if (g.init) {
        return;
    }
    g.init = true;
    if (const char* t = std::getenv("BB_PARTY_GUESTSVC_TEST")) {
        g.test = t;
    }
    const char* party = std::getenv("BB_PARTY");
    g.enabled = party && party[0] && GuestServicesEnabledFromEnv(std::getenv("BB_PARTY_GUEST_SERVICES"));
    if (!g.enabled) {
        if (party && party[0]) {
            Log("off (BB_PARTY_GUEST_SERVICES=0): guests cannot use the Doll, shops or workshop in the host's Dream");
        }
        return;
    }
    if (const char* t = std::getenv("BB_PARTY_GUESTSVC_TEST")) {
        // "a|b|c": fixtures run one after another in one session (each starts once the previous
        // one's warps and menu script are done).
        std::string all = t;
        std::size_t at = 0;
        g.test_chain.clear();
        while (true) {
            const std::size_t bar = all.find('|', at);
            g.test_chain.push_back(all.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos) {
                break;
            }
            at = bar + 1;
        }
        g.test = g.test_chain.front();
        g.test_chain.erase(g.test_chain.begin());
    }
    if (const char* e = std::getenv("BB_PARTY_GUEST_SERVICES_TALK")) {
        g.extra = e;
    }
    g_lamps = GuestLampsAtStart();
    {
        const char* wt = std::getenv("BB_PARTY_GUEST_TALK");
        g_world_talk = !(wt && wt[0] == '0');
        Log("guests %s talk to NPCs in the host's world (BB_PARTY_GUEST_TALK)", g_world_talk.load() ? "can" : "cannot");
    }
    const char* hs = std::getenv("BB_PARTY_HEADSTONES");
    const bool headstones = g_lamps.load() || !(hs && hs[0] == '0' && !hs[1]);
    bool multi_ok = false;
    if (headstones) {
        multi_ok = Matches(kMultiJmp, {0xe9, 0x61, 0x29, 0x00, 0x00}) &&
                   HookValueFilter(kMultiOr, {0x08, 0xc1, 0x0f, 0xb6, 0xc1}, {0x4c, 0x89, 0xff, 0x89, 0xc6}, &MultiFilter,
                                   kTestResultInt, "talk IsMultiplayerInProgress (party headstones)");
        g_headstones = multi_ok;
        // World lamps too (host first): the lamp object's multiplayer byte +0x49 and the lamp talk
        // scripts' IsMultiplayerInProgress (MultiFilter, LampTalk) - the host's lamp use is then
        // an ordinary lamp warp the travel capture turns into a party move.
        const bool gate_ok = PatchBytes(kLampMultiGate, {0x85, 0xc9, 0x0f, 0x9f, 0xc1, 0x08, 0xc1},
                                        {0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90},
                                        "lamp multiplayer gate +0x49 (party lamps)");
        Log("headstones and lamps %s: the host travels from a headstone or a lamp with guests present (the party "
            "follows)%s", multi_ok ? "on" : "NOT installed", gate_ok ? "" : "; lamp gate +0x49 NOT patched");
    } else {
        Log("headstones off (BB_PARTY_HEADSTONES=0): vanilla, no headstone travel while in a party");
    }
    if (g_lamps.load()) {
        const bool ok = multi_ok;
        SetLampWarpInterceptor(&GuestLampIntercept);
        Log("lamps %s: lamps and headstones work in a party; a guest's lamp use moves the whole party (host-led)",
            ok ? "on" : "partly installed");
    } else {
        Log("lamps off (opt-in until tested live: BB_PARTY_GUEST_LAMPS=1)");
    }
    if (!Matches(kIsClientJmp, {0xe9, 0x4d, 0x34, 0x00, 0x00})) {
        Log("guest +0x%llx is not the expected jmp; not installed", static_cast<unsigned long long>(kIsClientJmp));
        g.enabled = false;
        return;
    }
    SetEmevdRewriter2(&DollRewriter);
    if (g.test.find("buttonprobe") != std::string::npos || g.test.find("needprompt") != std::string::npos) {
        HookPrologue(0x1CD91F0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
                                 0xec, 0x58},
                     &ButtonEntry, "action-button registration (test probe)");
    }
    if (g.test.find("talkprobe") != std::string::npos) {
        HookPrologue(0x13ADCC0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
                                 0xec, 0x68},
                     &TalkManEntry, "talk action-button update (test probe)");
    }
    if (g.test.find("lampprobe") != std::string::npos) {
        // push rbp; mov rbp, rsp; push r15..r12, rbx; sub rsp, 0x2848 (20 bytes; then mov [rbp-0x2820], rdx).
        HookPrologue(0x12F5C40, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                 0xec, 0x48, 0x28, 0x00, 0x00},
                     &LampObjEntry, "lamp object update (test probe)");
    }
    if (!g.test.empty()) {
        // Trace hooks for the live tests only (BB_PARTY_GUESTSVC_TEST): the LevelUp menu factory,
        // the talk command ids, the menu requests.
        // push rbp; mov rbp, rsp; push r15, r14, rbx; sub rsp, 0x48 (13 bytes, then mov rbx, rdi).
        HookPrologue(0x1BB7F60, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x48},
                     &LevelUpMenuFactoryEntry, "level-up menu factory (log)");
        // push rbp; mov rbp, rsp; push r15..r12, rbx; sub rsp, 0x3c8 (20 bytes; then mov rbx, rsi).
        // The talk command switch 0x203FFD0 is hooked by shared NPC dialogue (party_npctalk), which
        // calls this trace first.
        npctalk::SetCommandTrace([](u64 env, u64 call) { TalkCommandEntry(env, call, 0, 0, 0, 0); });
        // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; sub rsp, 0x48 (17 bytes; then mov rbx, rdx).
        HookPrologue(0x1C4A520, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
                                 0xec, 0x48},
                     &MenuRequestEntry, "menu request (log)");
    }
    // (0x176BFE0, the request's first test, is not hooked: replacing it stopped the guest from
    // joining in a live run.)
    EnsureEmevdFilter();
    InstallMessengerSyncFilter();
    // rdi = r15 (the talk environment), rsi = eax (the game's answer).
    g.installed = HookValueFilter(kIsClientLoad, {0x41, 0x0f, 0xb6, 0x47, 0x25}, {0x4c, 0x89, 0xff, 0x89, 0xc6},
                                  &IsClientFilter, kTestResultInt, "talk IsClientPlayer (guest services)");
    Log("%s: a guest in the host's Hunter's Dream can use the Doll, the messengers' shops and the workshop",
        g.installed ? "on" : "not installed");
}

std::vector<EmevdSkipRule> GuestServicesSkipRules() {
    const char* party = std::getenv("BB_PARTY");
    if (!party || !party[0] || !GuestServicesEnabledFromEnv(std::getenv("BB_PARTY_GUEST_SERVICES"))) {
        return {};
    }
    std::vector<EmevdSkipRule> out = DollPlacementRules();
    if (GuestLampsAtStart()) {
        for (const EmevdSkipRule& r : LampSkipRules()) {
            out.push_back(r);
        }
        if (ChaliceLampsOn()) {
            for (const EmevdSkipRule& r : ChaliceLampSkipRules()) {
                out.push_back(r);
            }
        }
    }
    return out;
}

std::mutex g_lit_mu;
// Host: the lamp (return point) a guest's lamp_lit event lit here last, and when (the guard).
std::atomic<std::uint32_t> g_guest_lit_rp{0};
std::atomic<double> g_guest_lit_at{-1};
std::deque<std::pair<int, std::vector<std::uint32_t>>> g_lit_in;  // host: from guests
std::vector<std::uint32_t> g_lit_out;                             // guest: to send

bool GuestServicesOnLampLit(int slot, const std::string& body, std::string* why) {
    if (!g_lamps.load() || PartyDirector::Get().Role() != PartyRole::Host) {
        if (why) {
            *why = "guest lamps are off on the host";
        }
        return false;
    }
    std::vector<std::uint32_t> ids;
    bool own = false;
    if (!LampLitFromJsonText(body, &ids, why, &own)) {
        return false;
    }
    const lampshare::Rule rule{BbSettings::Get().party_share_lamps.load(),
                               BbSettings::Get().party_guests_bring_lamps.load()};
    if (!lampshare::HostTakes(own, rule)) {
        if (why) {
            *why = rule.share ? "the guest's own lamps: guests bring their lamps is off on the host (the host's "
                                "world keeps its own lamps)"
                              : "the guest's own lamps: shared lamps are off on the host";
        }
        return false;
    }
    std::lock_guard<std::mutex> lk(g_lit_mu);
    if (g_lit_in.size() < 16) {
        g_lit_in.emplace_back(own ? -1 - slot : slot, std::move(ids));
    }
    return true;
}

/// Statsaudit finding 5 (user decision 2026-10-15: lamps are shared by the whole party), narrowed
/// for 0.7.2 (user decision 2026-10-21: "host state saves lamps, anybody can light them";
/// lampshare::Decide). Guest, in the host's world, every 20 s: the host's world's lit lamps go into
/// the OWN save (C2 carries the lamp-lit flag, this the headstone flags). The lamps lit in the own
/// save that the host's world lacks stay out of it, unless the host's "Guests bring their lamps"
/// rule is on: then they go to the host (EVENT lamp_lit with "own"), once per join (again after
/// 20 s, at most 3 times, while the host's mirror still lacks them), and the host lights them in
/// its world. A lamp the guest lights in the host's world is GuestLampLitTick's (always sent).
/// The host's "Shared lamps" rule off: nothing is sent, and C2 leaves lamps out of the own save.
std::vector<std::uint32_t> g_lit_own_out;  // under g_lit_mu

void GuestOwnLampsTick(const GameSnapshot& s) {
    static double joined_at = -1, last_sent = -1;
    static int tries = 0;
    static int share_was = -1;
    balance::Rules hr;
    const bool known = balance::HostRules(&hr);
    const int share = known ? int(hr.share_lamps) : -1;
    const lampshare::Rule rule{share == 1, known && hr.guests_bring_lamps};
    if (share >= 0 && share != share_was) {
        // C2: lamps into the own save only while the host shares them.
        progress::Policy p = progress::CurrentPolicy();
        p.mode[static_cast<int>(progress::Category::lamp_unlocked)] =
            lampshare::IntoOwnSave(rule) ? progress::SyncMode::SetOnly : progress::SyncMode::Off;
        progress::SetPolicy(p);
        Log("lamps: the host's rule: shared lamps %s (the host's lamps %s the own save)", share ? "on" : "off",
            share ? "go into" : "stay out of");
        share_was = share;
    }
    if (s.session_role != kSessionRoleClient || !s.world_up || s.loading) {
        if (s.session_role != kSessionRoleClient) {
            joined_at = -1;
            tries = 0;
        }
        return;
    }
    if (joined_at < 0) {
        joined_at = Now();
        last_sent = -1;
    }
    if (share != 1 || progress::FlagLoadMode() != 1 || Now() - joined_at < 8.0 ||
        (last_sent >= 0 && Now() - last_sent < 20.0)) {
        return;
    }
    last_sent = Now();
    std::vector<std::uint32_t> ids;
    std::string list, into_own;
    int written = 0;
    const auto mine = [](std::uint32_t id) {
        bool v = false;
        return progress::ReadFlagOwnSave(id, &v) && v;
    };
    const auto theirs = [](std::uint32_t id) {
        bool v = false;
        return progress::HostWorldFlag(id, &v) && v;
    };
    std::string held, kept;
    const bool bring = lampshare::SendOwnLamps(rule);
    for (const lamptravel::Lamp& l : lamptravel::kLamps) {
        std::vector<std::uint32_t> add = lampshare::Missing(l.aab, l.n, mine, theirs);
        const std::uint32_t boss = lampshare::BlockBossFlag(l.aab);
        if (!add.empty() && !bring) {
            kept += " " + std::to_string(lamptravel::ReturnPoint(l));
            add.clear();  // the host's world keeps its own lamps (0.7.2)
        }
        if (!add.empty() && lampshare::HeldForBoss(l.n, mine(boss), theirs(boss))) {
            held += " " + std::to_string(lamptravel::ReturnPoint(l));
            add.clear();  // sent at a later join, once the host's world has beaten that boss too
        }
        if (!add.empty()) {
            list += " " + std::to_string(lamptravel::ReturnPoint(l));
        }
        for (std::uint32_t id : add) {
            if (LampLitFlagAllowed(id)) {
                ids.push_back(id);
            }
        }
        // The other way: the host's lamps into the own save (MissingFromLit). C2 carries the lamp-lit flag 7AAB02xx,
        // but the lamp object's base+10 / base+11 (the headstone list's flag) are runtime-category
        // flags it never writes: without them the own save's headstones would not list the lamp.
        const std::vector<std::uint32_t> take = lampshare::MissingFromLit(l.aab, l.n, theirs, mine);
        bool any = false;
        for (std::uint32_t id : take) {
            if (progress::WriteFlagOwnSave(id, true)) {
                ++written;
                any = true;
            }
        }
        if (any) {
            into_own += " " + std::to_string(lamptravel::ReturnPoint(l));
        }
    }
    // The lamp as the host's world shows it to this guest (0.7.6 report): a lamp whose lamp-lit flag the
    // visible tree has (the mirror brings 7AAB02xx) but not its headstone flag base+10 reads as "not
    // kindled" to its object and as lit to its kindle event, so lighting it does nothing. Set the two
    // list flags in the visible tree too.
    std::string shown;
    for (const lamptravel::Lamp& l : lamptravel::kLamps) {
        const std::vector<std::uint32_t> f = lampshare::LitFlags(l.aab, l.n);
        bool lit7 = false, lit10 = true;
        if (progress::ReadFlag(f[2], &lit7) && lit7 && progress::ReadFlag(f[0], &lit10) && !lit10 &&
            progress::WriteFlagVisible(f[0], true)) {
            progress::WriteFlagVisible(f[1], true);
            shown += " " + std::to_string(lamptravel::ReturnPoint(l));
        }
    }
    if (!shown.empty()) {
        Log("lamps: the host's lit lamps shown lit here too (lamp-lit flag set, headstone flags were not): lamps%s",
            shown.c_str());
    }
    if (written > 0) {
        Log("lamps: %d flag(s) of the host's lit lamps written into the own save (shared lamps; lamps%s)", written,
            into_own.c_str());
        RequestGameSave("shared lamps from the host's world");
    }
    if (!held.empty() && tries < 3) {
        Log("lamps: held back (their area's boss is alive in the host's world, beaten in the own save): lamps%s",
            held.c_str());
    }
    if (tries >= 3) {
        return;
    }
    ++tries;
    if (!bring) {
        tries = 3;  // once per join
        if (kept.empty()) {
            Log("lamps: every lamp of the own save is lit in the host's world too");
        } else {
            Log("lamps: the own save's lamps the host's world lacks stay out of it (the host's rule: guests bring "
                "their lamps off; the host's world keeps its own): lamps%s",
                kept.c_str());
        }
        return;
    }
    if (ids.empty()) {
        tries = 3;  // nothing the host lacks
        Log("lamps: every lamp of the own save is lit in the host's world too");
        return;
    }
    Log("lamps: %zu flag(s) of lamps lit in the own save and not in the host's world (lamps%s): sent to the host "
        "(shared lamps, try %d)",
        ids.size(), list.c_str(), tries);
    std::lock_guard<std::mutex> lk(g_lit_mu);
    g_lit_own_out.insert(g_lit_own_out.end(), ids.begin(), ids.end());
}

/// Guest, in the host's world (load mode 1), once a second: a lamp the guest lit there sets its
/// flags in the host-world overlay only (the game writes the visible tree; the own pool keeps
/// its own lamps). The lamp flags its overlay has and the host's mirror image lacks go to the host
/// as EVENT lamp_lit; the host sets them in its world and the mirror brings them back.
/// 0.7.2: only a real lamp use counts (lampshare::CountsAsGuestLit): the guest at that lamp, no load
/// within the last 20 s. A lamp its game shows lit right after a load (a lamp warp's arrival lights
/// its lamp) or far from the guest is never sent for this map.
// chalicelamp: where lamp `rp` stands in the loaded map, for the "stands at the lamp" distance
// tests: the party's spot table, or in a Chalice Dungeon (its layout generated, no table spot) the
// lamp object 2901950 + n itself (lamp 2902950 + n; chalice.md section 8).
bool LampSpotNow(std::uint32_t rp, float sp[4]) {
    if (rp / 100000 == 29) {
        float at[3];
        if (!worldact::ObjectPoint(static_cast<std::int32_t>(rp - 1000u), 0.0f, 0.0f, at)) {
            return false;
        }
        sp[0] = at[0], sp[1] = at[1], sp[2] = at[2], sp[3] = 0.0f;
        return true;
    }
    return ids::ReturnSpot(rp, sp);
}

std::uint32_t LampOfLitFlag(std::uint32_t id) {
    if (id / 10000000 == 7) {
        return (id - 70000000u) / 10000u * 10000u + 2950u + (id % 10000u - 200u);
    }
    const std::uint32_t k = id % 10000u;
    return k >= 7810u ? (id - 10000000u) / 10000u * 10000u + 2950u + (k - 7810u) / 20u : 0u;
}

void GuestLampLitTick(const GameSnapshot& s) {
    static double last = 0, unsteady_at = 0;
    static std::uint32_t map = 0;
    static std::vector<std::uint32_t> sent;
    if (!s.world_up || s.loading) {
        unsteady_at = Now();
    }
    if (s.session_role != kSessionRoleClient || !s.world_up || s.loading || progress::FlagLoadMode() != 1 ||
        Now() - last < 1.0) {
        return;
    }
    last = Now();
    if (s.map_id != map) {
        map = s.map_id;
        sent.clear();
    }
    std::vector<std::uint32_t> ids;
    float me[3];
    const bool have_pos = s.player && balance::ChrPosition(s.player, me);
    const double since_load = Now() - unsteady_at;
    auto consider = [&](std::uint32_t id) {
        bool mine = false, host = false;
        if (LampLitFlagAllowed(id) && progress::ReadFlag(id, &mine) && mine && progress::HostWorldFlag(id, &host) &&
            !host && std::find(sent.begin(), sent.end(), id) == sent.end() && ids.size() < kMaxLampLitIds) {
            const std::uint32_t rp = LampOfLitFlag(id);
            float sp[4];
            float dist = -1.0f;
            if (have_pos && rp && LampSpotNow(rp, sp)) {
                dist = std::sqrt((me[0] - sp[0]) * (me[0] - sp[0]) + (me[1] - sp[1]) * (me[1] - sp[1]) +
                                 (me[2] - sp[2]) * (me[2] - sp[2]));
            }
            if (!lampshare::CountsAsGuestLit(since_load, dist)) {
                sent.push_back(id);  // never for this map: not a lamp this guest lit
                Log("lamp: flag %u (lamp %u) shows lit here but not in the host's world: not sent (%.0f s after a "
                    "load, %.1f m from the lamp: not this guest's lamp use)",
                    id, rp, since_load, double(dist));
                return;
            }
            ids.push_back(id);
        }
    };
    // The lamps of this map: FrpgNetMan (0x553B120) +0xC70 entries, +0x14 = object flag base.
    std::uint64_t net = 0, c70 = 0, head = 0, n = 0;
    if (SafeGet(Guest(0x553b120), &net) && net && SafeGet(net + 0xc70, &c70) && c70 && SafeGet(c70 + 0x30, &head) &&
        head && SafeGet(head, &n)) {
        for (int guard = 0; n && n != head && guard < 512; ++guard) {
            std::uint64_t e = 0;
            std::int32_t base = -1;
            if (SafeGet(n + 0x10, &e) && e && SafeGet(e + 0x14, &base) && base >= 0) {
                consider(std::uint32_t(base) + 10);
                consider(std::uint32_t(base) + 11);
            }
            if (!SafeGet(n, &n)) {
                break;
            }
        }
    }
    const std::uint32_t area = s.map_id >> 24, block = (s.map_id >> 16) & 0xff;
    for (std::uint32_t k = 200; k < 300; ++k) {
        consider(70000000 + area * 100000 + block * 10000 + k);
    }
    if (ids.empty()) {
        return;
    }
    sent.insert(sent.end(), ids.begin(), ids.end());
    std::string list;
    for (std::uint32_t id : ids) {
        list += " " + std::to_string(id);
    }
    Log("lamp: the guest lit a lamp of the host's world (overlay:%s); asking the host to light it", list.c_str());
    std::lock_guard<std::mutex> lk(g_lit_mu);
    g_lit_out.insert(g_lit_out.end(), ids.begin(), ids.end());
}

bool GuestServicesOnLampRequest(int slot, const std::string& body, std::string* why) {
    if (!g_lamps.load()) {
        if (why) {
            *why = "lamps are off on the host (BB_PARTY_GUEST_LAMPS)";
        }
        return false;
    }
    LampRequest r;
    if (!LampRequestFromJsonText(body, &r, why) || !SanitizePeerLampRequest(r, why)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_lamp_mu);
    if (g_lamp_in.size() >= 8) {
        if (why) {
            *why = "too many pending requests";
        }
        return false;
    }
    g_lamp_in.emplace_back(slot, r);
    return true;
}

// Live test (BB_PARTY_GUESTSVC_TEST "lamppress"): a guest in the host's world outside the Dream
// presses Cross whenever an action prompt is up (it arrives at the host's lamp after a host warp),
// at most 8 times, 1.5 s apart, until its lamp warp became a request.
// Test-only lamp object probe ("lampprobe"): the lamp action-object update 0x12F5C40(obj) records
// its objects; the tick logs each one's entity (+0x20), type (+0x28), current row (+0x2C) and the
// four "unavailable" bytes +0x48..+0x4B (travel.md 1.1: 0x48 lamp state, 0x49 multiplayer,
// 0x4A no lamp record, 0x4B someone else at it).
std::atomic<std::uint64_t> g_lamp_objs[16];
BB_COOP_SYSV void LampObjEntry(u64 obj, u64, u64, u64, u64, u64) {
    for (auto& slot : g_lamp_objs) {
        std::uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == obj) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, obj)) {
            return;
        }
    }
}
void LogLampObjs() {
    for (auto& slot : g_lamp_objs) {
        const std::uint64_t o = slot.load(std::memory_order_relaxed);
        if (!o) {
            continue;
        }
        std::int32_t ent = 0, row = 0;
        std::uint8_t type = 0, b[4] = {};
        SafeGet(o + 0x20, &ent);
        SafeGet(o + 0x28, &type);
        SafeGet(o + 0x2c, &row);
        SafeRead(o + 0x48, b, 4);
        // Its FrpgNetMan (0x553B120) +0xC70 lamp entry (the type's source, 0x146E360): list at
        // +0x30 (node +0 next, +0x10 entry), entry +8 entity, +0xC / +0xE s16 counters, +0x10 lit,
        // +0x11 registered, +0x14 flag base, +0x18.
        std::string entry = "no +0xC70 entry";
        std::uint64_t net = 0, c70 = 0, head = 0;
        if (SafeGet(Guest(0x553b120), &net) && net && SafeGet(net + 0xc70, &c70) && c70 &&
            SafeGet(c70 + 0x30, &head) && head) {
            std::uint64_t n = 0;
            SafeGet(head, &n);
            for (int guard = 0; n && n != head && guard < 512; ++guard) {
                std::uint64_t e = 0;
                std::int32_t eent = 0;
                if (SafeGet(n + 0x10, &e) && e && SafeGet(e + 8, &eent) && eent == ent) {
                    std::int16_t c = 0, d = 0;
                    std::uint8_t lit = 0, reg = 0;
                    std::int32_t f14 = 0, f18 = 0;
                    SafeGet(e + 0xc, &c);
                    SafeGet(e + 0xe, &d);
                    SafeGet(e + 0x10, &lit);
                    SafeGet(e + 0x11, &reg);
                    SafeGet(e + 0x14, &f14);
                    SafeGet(e + 0x18, &f18);
                    char t[160];
                    std::snprintf(t, sizeof t, "+0xC70 entry %p: +0xC %d +0xE %d lit %u reg %u +0x14 %d +0x18 %d",
                                  reinterpret_cast<void*>(e), c, d, lit, reg, f14, f18);
                    entry = t;
                    break;
                }
                if (!SafeGet(n, &n)) {
                    break;
                }
            }
        }
        Log("test: lamp object %p entity %d type %u row %d: +0x48 %u +0x49 %u +0x4A %u +0x4B %u; %s",
            reinterpret_cast<void*>(o), ent, type, row, b[0], b[1], b[2], b[3], entry.c_str());
    }
}

// Test probe ("talkprobe"): the talk action-button update 0x13ADCC0(talkman, ...) - the manager that
// registers a talk's action button (type 0xF) for the nearest talk entry in range (0x13A3710)
// unless the state mask 0x176B7E0() has bits outside 0xC8. Entries: +0x90 array (0x140 each),
// +0x98 count; entry +0x58 map object id, +0x5C state (3/4 skipped), +0x90 talk object (vfunc
// +0x98: its talk id).
std::atomic<std::uint64_t> g_talkman{0};
BB_COOP_SYSV void TalkManEntry(u64 man, u64, u64, u64, u64, u64) {
    g_talkman.store(man, std::memory_order_relaxed);
}
// "buttonprobe": every action-button registration 0x1CD91F0(man, type, button id, ...) (the game
// calls it each frame for each button in range), distinct (type, id) with counts.
std::mutex g_btn_mu;
std::map<std::uint64_t, std::uint32_t> g_btn_seen;
BB_COOP_SYSV void ButtonEntry(u64 man, u64 type, u64 id, u64 p5, u64 p6, u64 p7) {
    g_btn_man.store(man, std::memory_order_relaxed);
    if ((type & 0xffu) == 13) {
        g_talk_btn_at.store(Now(), std::memory_order_relaxed);
        g_talk_btn_id.store(int(id & 0xffffffffu), std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(g_btn_mu);
    if (g_btn_seen.size() < 256) {
        // key: type (8 bits) | p5 | p6 | p7 flags | id
        ++g_btn_seen[(type & 0xffu) << 40 | (p5 & 1) << 34 | (p6 & 1) << 33 | (p7 & 1) << 32 | (id & 0xffffffffu)];
    }
}
void LogButtons() {
    std::map<std::uint64_t, std::uint32_t> seen;
    {
        std::lock_guard<std::mutex> lk(g_btn_mu);
        seen.swap(g_btn_seen);
    }
    std::string list;
    for (const auto& [k, n] : seen) {
        char b[48];
        std::snprintf(b, sizeof b, " %u:%d(p5 %u p6 %u p7 %u) x%u", unsigned(k >> 40), std::int32_t(k & 0xffffffffu),
                      unsigned(k >> 34 & 1), unsigned(k >> 33 & 1), unsigned(k >> 32 & 1), n);
        list += b;
    }
    const std::uint64_t man = g_btn_man.load();
    std::uint8_t m52 = 0, m55 = 0;
    SafeGet(man + 0x52, &m52);
    SafeGet(man + 0x55, &m55);
    std::uint64_t g = 0;
    std::int32_t c0 = -1, c118 = -1;
    if (SafeGet(Guest(0x5562878), &g) && g) {
        SafeGet(g + 0xc0, &c0);
        SafeGet(g + 0x118, &c118);
    }
    using StFn = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::int32_t);
    const std::uint64_t st = reinterpret_cast<StFn>(Guest(0x13cc180))(10000, -1) & 0xff;
    list += "; man +0x52 " + std::to_string(m52) + " +0x55 " + std::to_string(m55) + ", 0x13CC180 " +
            std::to_string(st) + ", [0x5562878] +0xC0 " + std::to_string(c0) + " +0x118 " + std::to_string(c118);
    Log("test: buttonprobe: registrations since the last line:%s", list.empty() ? " none" : list.substr(0, 400).c_str());
}

void LogTalkMan() {
    const std::uint64_t man = g_talkman.load(std::memory_order_relaxed);
    if (!man) {
        Log("test: talkprobe: talk manager not seen");
        return;
    }
    std::uint64_t arr = 0;
    std::int32_t n = 0, cur = 0, btn = 0;
    std::uint8_t on = 0, want = 0;
    SafeGet(man + 0x90, &arr);
    SafeGet(man + 0x98, &n);
    SafeGet(man + 0x9c, &on);
    SafeGet(man + 0x9d, &want);
    SafeGet(man + 0xa0, &cur);
    SafeGet(man + 0xa4, &btn);
    using MaskFn = std::uint32_t(BB_COOP_SYSV*)();
    const std::uint32_t mask = reinterpret_cast<MaskFn>(Guest(0x176b7e0))();
    std::string list;
    for (int i = 0; i < n && i < 64 && arr; ++i) {
        const std::uint64_t e = arr + std::uint64_t(i) * 0x140;
        std::uint32_t obj = 0;
        std::int32_t st = 0;
        std::uint8_t f = 0;
        std::uint64_t talk = 0, vt = 0, fn = 0;
        SafeGet(e + 0x58, &obj);
        SafeGet(e + 0x5c, &st);
        SafeGet(e + 0x108, &f);
        std::int32_t tid = -1;
        if (SafeGet(e + 0x90, &talk) && talk && SafeGet(talk, &vt) && vt && SafeGet(vt + 0x98, &fn) && fn) {
            using IdFn = std::int32_t(BB_COOP_SYSV*)(std::uint64_t);
            tid = reinterpret_cast<IdFn>(fn)(talk);
        }
        char b[64];
        std::snprintf(b, sizeof b, " [%d t%d st%d f%02x]", i, tid, st, f);
        list += b;
    }
    Log("test: talkprobe: mask %#x (blocks when & 0xffffff37), button %u/%u cur %d btn %d, %d entries:%s", mask,
        on, want, cur, btn, n, list.substr(0, 380).c_str());
}

/// "lampwalk" (with lampprobe): stand the local player 1 m in front of the nearest probed lamp
/// object, facing it. The lamp's map object comes from 0x13C9AF0(entity, &index) (the lamp
/// update's own lookup, 0x2E0-byte record); its world matrix: 3x4 rows at +0x240..+0x26F, the
/// translation in column 3 (+0x24C, +0x25C, +0x26C; live: Central Yharnam's lamp 2411951 at
/// (-196.8, -28.6, 65.4), 5 m from the host's lamp arrival spot - out of the prompt's range).
bool WalkToLamp(const GameSnapshot& s) {
    float me[4] = {};
    if (!s.player || !SafeRead(s.player + 0xc0, me, sizeof me)) {
        return false;
    }
    float best_d = 1e9f, best[3] = {};
    std::int32_t best_ent = 0;
    for (auto& slot : g_lamp_objs) {
        const std::uint64_t o = slot.load(std::memory_order_relaxed);
        std::int32_t ent = 0;
        if (!o || !SafeGet(o + 0x20, &ent) || ent <= 0) {
            continue;
        }
        std::uint32_t idx = 0xffffffffu;
        using Find = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::uint32_t*);
        const std::uint64_t rec = reinterpret_cast<Find>(Guest(0x13c9af0))(ent, &idx);
        float m[16] = {};
        if (!rec || !SafeRead(rec + 0x230, m, sizeof m)) {
            continue;
        }
        const float t[3] = {m[7], m[11], m[15]};  // 3x4 rows at +0x240: translation in column 3
        const float dx = t[0] - me[0], dy = t[1] - me[1], dz = t[2] - me[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        Log("test: lampwalk: lamp %d record %p: rows %.1f %.1f %.1f %.1f | %.1f %.1f %.1f %.1f | %.1f %.1f %.1f %.1f | "
            "%.1f %.1f %.1f %.1f; player (%.1f, %.1f, %.1f), %.1f m", ent, reinterpret_cast<void*>(rec), m[0], m[1],
            m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], me[0], me[1],
            me[2], d);
        if (d < best_d) {
            best_d = d;
            best_ent = ent;
            std::memcpy(best, t, sizeof best);
        }
    }
    if (!best_ent || best_d > 30.0f) {
        return false;
    }
    float dx = me[0] - best[0], dz = me[2] - best[2];
    const float h = std::sqrt(dx * dx + dz * dz);
    if (h < 0.01f) {
        dx = 0, dz = 1;
    } else {
        dx /= h, dz /= h;
    }
    alignas(16) float pos[4] = {best[0] + dx * 1.0f, best[1], best[2] + dz * 1.0f, 1.0f};
    const float a = std::atan2(best[0] - pos[0], best[2] - pos[2]);
    alignas(16) float rot[4] = {0, a + 3.14159265f, 0, 0};
    std::uint64_t vtbl = 0, fn = 0;
    if (!SafeGet(s.player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) {
        return false;
    }
    using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
    Log("test: lampwalk: standing 1 m in front of lamp %d at (%.1f, %.1f, %.1f)", best_ent, pos[0], pos[1], pos[2]);
    return true;
}

struct LampPress {
    double next = 0, release = 0;
    int presses = 0;
    int base = -1;  // requests made before the fixture started (a Dream headstone before it)
    bool done = false;
    bool walked = false;
} g_lamp_press;

void LampPressTick(const GameSnapshot& s, double now) {
    LampPress& P = g_lamp_press;
    if (P.release > 0 && now >= P.release) {
        runtime_pad_director_press(0);
        P.release = 0;
    }
    // "ownlamp" (playtest 1 bug 22): the same presses in the player's OWN world (a Join-a-party
    // guest with no host, or any role): the lamp must work as in single player, i.e. its warp
    // starts a loading screen. Logs "ownlamp: the lamp warp ran" or FAILED.
    const bool own = g.test.find("ownlamp") != std::string::npos;
    if (own && !s.world_up && !s.loading && P.presses == 0 && now >= P.next) {
        // A guest whose host never answers stays on the title (auto-continue waits for the link):
        // "Failed to acquire server information" -> Right (CANCEL), then Cross (Continue).
        static int title_presses = 0;
        if (title_presses < 40) {
            const bool right = title_presses % 2 == 0;
            runtime_pad_director_press(right ? kPadRight : kPadCross);
            P.release = now + 0.12;
            P.next = now + 1.2;
            Log("test: ownlamp: title %s #%d", right ? "Right" : "Cross", ++title_presses);
        }
        return;
    }
    if (own && !P.done && P.presses > 0 && s.loading) {
        P.done = true;
        Log("test: ownlamp: the lamp warp ran (loading screen) after %d press(es)", P.presses);
        return;
    }
    if (P.done || g.test.find("lamppress") == std::string::npos || !s.world_up || s.loading ||
        (own ? s.session_role == kSessionRoleClient : s.session_role != kSessionRoleClient) || IsDreamMap(s.map_id) ||
        s.map_id == 0xffffffffu) {
        P.base = -1;  // the baseline is taken once the guest stands in a world map
        return;
    }
    if (now < P.next) {
        return;
    }
    if (P.base < 0) {
        P.base = g_lamp_requests_made.load();
    }
    if (g_lamp_requests_made.load() > P.base) {
        P.done = true;
        Log("test: lamppress: the guest's lamp use became a request after %d press(es)", P.presses);
        return;
    }
    P.next = now + 1.5;
    if (g.test.find("lampprobe") != std::string::npos) {
        LogLampObjs();
    }
    if (const std::size_t f = g.test.find("lampflag="); f != std::string::npos) {
        // The lamp's own "used" flag (the one its common event 7200 instance waits for), as the
        // lamp interaction would set it on this game (load mode 1: the host-world overlay).
        if (P.presses++ == 0) {
            const std::uint32_t flag = std::uint32_t(std::strtoul(g.test.c_str() + f + 9, nullptr, 10));
            const bool ok = WriteEventFlag(flag, true);
            Log("test: lamppress: lamp flag %u set on the guest%s (map %s); its 7200 runs the warp", flag,
                ok ? "" : " - FAILED", MapName(s.map_id).c_str());
        } else if (P.presses > 10) {
            P.done = true;
            Log("test: lamppress: FAILED: no lamp request 15 s after the lamp flag");
        }
        return;
    }
    if (g.test.find("lampwalk") != std::string::npos && !P.walked) {
        P.walked = WalkToLamp(s);  // then the prompt check on the next round
        if (P.walked && g.test.find("hurt") != std::string::npos) {
            // "hurt": half HP before the rest (does the party rest heal the guest?). Chr data module
            // chr +0x3B0 -> +0x20: HP +0xf8, max +0xFC.
            std::uint64_t cm = 0, dm = 0;
            std::int32_t mx = 0;
            if (SafeGet(s.player + 0x3b0, &cm) && cm && SafeGet(cm + 0x20, &dm) && dm && SafeGet(dm + 0xfc, &mx) &&
                mx > 1) {
                const std::int32_t half = mx / 2;
                SafeWrite(dm + 0xf8, &half, 4);
                Log("test: hurt: guest HP set to %d / %d before the rest", half, mx);
            }
        }
        if (P.walked) {
            return;
        }
    }
    std::uint32_t prompt = 0;
    const bool up = PromptUp(&prompt);
    if (!up && P.presses == 0 && P.next - now < 1.6) {
        static int waits = 0;
        if (++waits < 6) {
            Log("test: lamppress: in %s, no action prompt yet (%d)", MapName(s.map_id).c_str(), waits);
            return;
        }
    }
    if (++P.presses > 8) {
        P.done = true;
        Log("test: %s: FAILED: no lamp %s after 8 presses", own ? "ownlamp" : "lamppress", own ? "warp" : "request");
        return;
    }
    // With lampwalk: Cross (prompt), Left (YES: the confirm box starts on NO), Cross, repeated.
    const bool left = g.test.find("lampwalk") != std::string::npos && P.presses % 3 == 2;
    runtime_pad_director_press(left ? kPadLeft : kPadCross);
    P.release = now + 0.12;
    Log("test: lamppress: %s #%d (prompt %s %u, map %s)", left ? "Left" : "Cross", P.presses, up ? "up" : "not seen", prompt,
        MapName(s.map_id).c_str());
}

void LogLampTalks() {
    static double last = 0;
    if (Now() - last < 1.0) {
        return;
    }
    last = Now();
    // Counted silently (the scripts ask 30 times a second while near); logged in tests only.
    if (const std::uint32_t m = g_world_talk_overrides.exchange(0, std::memory_order_relaxed); m && !g.test.empty()) {
        Log("guest NPC talk in the host's world asked IsClientPlayer %u times (answered 0)", m);
    } else if (m) {
        // Outside tests: at most one line a minute (playtest 1 bug 5: the logs never told whether
        // a guest's knock / talk reached the override at all).
        static std::uint32_t pending = 0;
        static double last_normal = -1e9;
        pending += m;
        if (Now() - last_normal >= 60.0) {
            last_normal = Now();
            Log("guest NPC talk in the host's world: %u IsClientPlayer answer(s) overridden (answered 0) since the last line",
                pending);
            pending = 0;
        }
    }
    if (const std::uint32_t m = g_lamp_talk_overrides.exchange(0, std::memory_order_relaxed); m && !g.test.empty()) {
        Log("guest lamp talk asked IsClientPlayer %u times (answered 0)", m);
    }
    if (const std::uint32_t m = g_lamp_talk_held.exchange(0, std::memory_order_relaxed); m) {
        // A lamp kept closed during / right after the guest's NPC talk: one line a minute at most.
        static double last_held = -1e9;
        if (Now() - last_held >= 60.0 || !g.test.empty()) {
            last_held = Now();
            Log("guest lamp kept closed while an NPC talk plays or just stopped (%u IsClientPlayer answers left as before)", m);
        }
    }
    static std::uint32_t client_logged[16] = {};
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t t = g_client_talks[i].load(std::memory_order_relaxed);
        if (t && client_logged[i] != t) {
            client_logged[i] = t;
            const bool world_talk = g_world_talk.load(std::memory_order_relaxed) && worldact::WorldTalkAllowed(t);
            Log("talk t%06u asks IsClientPlayer (%s)", t,
                ChaliceLampTalk(t)               ? "Chalice Dungeon lamp: answered 0"
                : LampTalk(t) && !HeadstoneTalk(t) ? "world lamp: answered 0 with BB_PARTY_GUEST_LAMPS=1"
                : world_talk                     ? "NPC talk in the host's world: answered 0 (D8)"
                                                 : "as before");
        }
    }
    static std::uint32_t multi_logged[16] = {};
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t t = g_multi_talks[i].load(std::memory_order_relaxed);
        if (t && multi_logged[i] != t) {
            multi_logged[i] = t;
            Log("talk t%06u asks IsMultiplayerInProgress (%s)", t,
                HeadstoneTalk(t) ? "headstone"
                : LampTalk(t)    ? "lamp: answered 0"
                : ChaliceLampTalk(t) ? (ChaliceLampsOn() ? "Chalice Dungeon lamp: answered 0"
                                                                                     : "Chalice Dungeon lamp: left as is (BB_PARTY_CHALICE_LAMPS=0)")
                                 : "left as is");
        }
    }
}

void LampTick(const GameSnapshot& s, party::PartyLink* link) {
    g_session_role = s.session_role;
    LogLampTalks();
    if (const std::size_t ep = g.test.find("entprobe="); ep != std::string::npos && s.world_up && !s.loading) {
        // "entprobe=ENTITY": a map object's world matrix every 5 s (0x13C9AF0 record, 3x4 rows at
        // +0x240, translation in column 3) and the player's position: test spots for doors etc.
        static double last = 0;
        if (Now() - last >= 5.0) {
            last = Now();
            // "entprobe=A:B:C": several entities.
            for (const char* q = g.test.c_str() + ep + 9; *q >= '0' && *q <= '9';) {
            char* qe = nullptr;
            const std::int32_t ent = static_cast<std::int32_t>(std::strtol(q, &qe, 10));
            q = (qe && *qe == ':') ? qe + 1 : "";
            std::uint32_t idx = 0xffffffffu;
            using Find = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::uint32_t*);
            const std::uint64_t rec = reinterpret_cast<Find>(Guest(0x13c9af0))(ent, &idx);
            float m[12] = {}, me[4] = {};
            if (s.player) SafeRead(s.player + 0xc0, me, sizeof me);
            if (rec && SafeRead(rec + 0x240, m, sizeof m)) {
                Log("test: entprobe %d: pos (%.2f, %.2f, %.2f) rows x (%.2f %.2f %.2f) z (%.2f %.2f %.2f); player (%.2f, "
                    "%.2f, %.2f) map %s", ent, m[3], m[7], m[11], m[0], m[1], m[2], m[8], m[9], m[10], me[0], me[1],
                    me[2], MapName(s.map_id).c_str());
            } else {
                Log("test: entprobe %d: no map object here (map %s)", ent, MapName(s.map_id).c_str());
            }
            }
        }
    }
    if (g.test.find("promptlog") != std::string::npos && s.world_up && !s.loading) {
        // "promptlog" (with buttonprobe): once a second the prompt on screen, the action-button registrations
        // of that second and where the player stands (the NPC / lamp action-target tests).
        static double last_prompt = 0;
        if (Now() - last_prompt >= 1.0) {
            last_prompt = Now();
            float me[4] = {};
            if (s.player) SafeRead(s.player + 0xc0, me, sizeof me);
            std::uint32_t prompt = 0;
            const bool up = PromptUp(&prompt);
            Log("test: promptlog: map %s at (%.1f, %.1f, %.1f): prompt %s %u, conversation idle %.1f s", MapName(s.map_id).c_str(),
                me[0], me[1], me[2], up ? "up" : "not seen", prompt, std::min(ConversationIdleSeconds(), 999.0));
            if (g.test.find("buttonprobe") != std::string::npos) LogButtons();
        }
    }
    if (!g.test.empty()) {
        LampPressTick(s, Now());
        static double last_own_probe = 0;
        if (g.test.find("ownprobe") != std::string::npos && s.session_role != kSessionRoleClient && s.world_up &&
            !s.loading && Now() - last_own_probe >= 2.0) {
            // #22: the own-world view of a lamp (a Join-a-party guest with no host): the lamp
            // objects' gate bytes, the talk manager, the action buttons, the prompt, where we stand.
            last_own_probe = Now();
            float me[4] = {};
            if (s.player) SafeRead(s.player + 0xc0, me, sizeof me);
            std::uint32_t prompt = 0;
            const bool up = PromptUp(&prompt);
            Log("test: ownprobe: role %d map %s at (%.1f, %.1f, %.1f): prompt %s %u", s.session_role,
                MapName(s.map_id).c_str(), me[0], me[1], me[2], up ? "up" : "not seen", prompt);
            if (g.test.find("lampprobe") != std::string::npos) LogLampObjs();
            if (g.test.find("talkprobe") != std::string::npos) LogTalkMan();
            if (g.test.find("buttonprobe") != std::string::npos) LogButtons();
        }
        static double last_host_probe = 0;
        if (g.test.find("lampprobe") != std::string::npos && s.session_role == 3 && s.world_up && !s.loading &&
            !IsDreamMap(s.map_id) && Now() - last_host_probe >= 3.0) {
            last_host_probe = Now();  // the host's view of the same lamp objects (comparison)
            LogLampObjs();
            static bool host_walked = false;
            if (!host_walked && g.test.find("lampwalk") != std::string::npos) {
                host_walked = WalkToLamp(s);  // the host at its lamp too: is there a prompt?
            } else if (host_walked) {
                std::uint32_t prompt = 0;
                const bool up = PromptUp(&prompt);
                static int host_presses = 0;
                // Cross (the lamp's "Return to the Hunter's Dream"), Left (YES in the confirm box, the
                // cursor starts on NO), Cross.
                const bool press = g.test.find("hostlamppress") != std::string::npos && host_presses < 3;
                const std::uint32_t key = host_presses == 1 ? kPadLeft : kPadCross;
                Log("test: lampwalk: host at the lamp: prompt %s %u%s", up ? "up" : "not seen", prompt,
                    !press ? "" : key == kPadLeft ? "; key Left" : "; key Cross");
                if (g.test.find("talkprobe") != std::string::npos) {
                    LogTalkMan();
                }
                if (g.test.find("buttonprobe") != std::string::npos) {
                    LogButtons();
                }
                if (press) {
                    ++host_presses;
                    runtime_pad_director_press(key);
                    g_lamp_press.release = Now() + 0.12;
                }
            }
        }
    }
    if (!g_lamps.load()) {
        return;
    }
    const PartyRole role = PartyDirector::Get().Role();
    std::deque<LampRequest> out;
    std::deque<std::pair<int, LampRequest>> in;
    {
        std::lock_guard<std::mutex> lk(g_lamp_mu);
        if (role == PartyRole::Guest && link) {
            out.swap(g_lamp_out);
        }
        if (role == PartyRole::Host) {
            in.swap(g_lamp_in);
        }
    }
    for (const LampRequest& r : out) {
        link->send_event(0, kLampRequestEventName, LampRequestToJsonText(r));
        Log("lamp: request %u sent to the host", r.id);
    }
    if (role == PartyRole::Guest) {
        GuestLampLitTick(s);
        GuestOwnLampsTick(s);
        std::vector<std::uint32_t> lit, own;
        {
            std::lock_guard<std::mutex> lk(g_lit_mu);
            lit.swap(g_lit_out);
            own.swap(g_lit_own_out);
        }
        if (!lit.empty() && link) {
            link->send_event(0, kLampLitEventName, LampLitToJsonText(lit));
        }
        for (std::size_t at = 0; link && at < own.size(); at += kMaxLampLitIds) {
            const std::vector<std::uint32_t> part(own.begin() + at,
                                                  own.begin() + std::min(own.size(), at + kMaxLampLitIds));
            link->send_event(0, kLampLitEventName, LampLitToJsonText(part, true));
        }
    } else if (role == PartyRole::Host) {
        std::deque<std::pair<int, std::vector<std::uint32_t>>> lit;
        {
            std::lock_guard<std::mutex> lk(g_lit_mu);
            lit.swap(g_lit_in);
        }
        bool wrote_own = false, wrote_lit = false;
        for (const auto& [tagged, ids] : lit) {
            const bool own = tagged < 0;
            const int slot = own ? -1 - tagged : tagged;
            std::string list;
            std::vector<std::uint32_t> all = ids;
            if (!own) {
                // 0.7.2: the whole lamp at once (its object's base+10 / +11 come a second after the
                // lamp-lit flag from the guest), so the save requested below holds all of it.
                for (std::uint32_t id : ids) {
                    for (std::uint32_t f : lampshare::LitFlagsOfLampLit(id)) {
                        if (LampLitFlagAllowed(f) && std::find(all.begin(), all.end(), f) == all.end()) {
                            all.push_back(f);
                        }
                    }
                }
            }
            for (std::uint32_t id : all) {
                bool before = false;
                progress::ReadFlag(id, &before);
                const bool ok = progress::WriteFlagOwnSave(id, true);  // the host's world (load mode 0)
                list += " " + std::to_string(id) + (before ? "(was set)" : ok ? "" : "(NOT written)");
                wrote_own = wrote_own || (own && ok && !before);
                wrote_lit = wrote_lit || (!own && ok && !before);
                if (!own && ok && LampOfLitFlag(id)) {
                    g_guest_lit_rp = LampOfLitFlag(id);
                    g_guest_lit_at = Now();
                }
            }
            if (own) {
                Log("lamps: slot %d's own save has lamps this world lacked (shared lamps): set%s", slot, list.c_str());
            } else {
                Log("lamp: slot %d lit a lamp in the host's world: set%s", slot, list.c_str());
            }
        }
        if (wrote_own) {
            RequestGameSave("shared lamps from a guest's save");
        }
        if (wrote_lit) {
            // 0.7.2: the host's world saves the lamps anyone lit there (as the host's own lamp would be).
            RequestGameSave("a lamp a guest lit in the host's world");
        }
    }
    for (const auto& [slot, r] : in) {
        std::string why;
        const LampRequestAction act = ChooseLampRequestAction(r, b2::LampInLoadedMap(r.id));
        if (act == LampRequestAction::RestLoad) {
            // Its own gate (party_death_rules PlanRestLoad), run by the B2 tick.
            b2::HostRestRequest(slot, r.id);
            continue;
        }
        if (act == LampRequestAction::IgnoreSameMap) {
            // Playtest 1 bug 10: the host was yanked across the Dream for a guest's lamp in the
            // map both stood in. That guest loads alone (its own loading screen).
            Log("lamp: slot %d asked for lamp %u in this loaded map: the host does not move (the guest loads alone)", slot,
                r.id);
            continue;
        }
        if (split::OwnChoiceFor(r.id, 20.0)) {
            // P4 (quest_audit.md): the same scripted warp ran on this game too (Lecture Building
            // doors, a Tonsil Stone grab): the host's own question / answer moves the party.
            Log("lamp: request %u from slot %d dropped: the host has its own question for the same warp (P4)", r.id,
                slot);
            continue;
        }
        if (!g_lamp_gate.Accept(s.world_up && !s.loading, Now(), &why)) {
            Log("lamp: request %u from slot %d not run: %s", r.id, slot, why.c_str());
            continue;
        }
        split::NoteMemberRequestRun(r.id);
        const std::uint32_t id = r.id;
        const int from = slot;
        const auto run = [id, from] {
            split::NoteMemberRequestRun(id);
            Log("lamp: slot %d used a lamp / headstone: the host warps to %u, the party follows", from, id);
            // Another map (the split question is the guest's, already answered): the party travel.
            RunLampWarpDecided(id, false);
        };
        // 0.7.3 lamp travel voting (party_vote.h): with the host's rule on, the others vote first.
        if (!vote::HostBegin(id, slot, run, "a guest's lamp / headstone")) {
            run();
        }
    }
}

/// "hosttalk=N": the talk the host's fixture uses (default: headstone t210000).
std::uint32_t HostTalkTarget() {
    const std::size_t k = g.test.find("hosttalk=");
    if (k == std::string::npos) {
        const std::size_t q = g.test.find("guesttalk=");
        return q == std::string::npos ? 210000 : std::uint32_t(std::strtoul(g.test.c_str() + q + 10, nullptr, 10));
    }
    return std::uint32_t(std::strtoul(g.test.c_str() + k + 9, nullptr, 10));
}

/// Live test "watchtalk=N[:M[:A]][@D]" (either role): D s (default 8) into a steady Dream with
/// another player present, stand M m (default 1.5) from talk N, facing it, in its use range: A
/// degrees off its front (default: tries 60, -60, 90, -90, 40, -40 until its action prompt shows;
/// the player never presses it), then the camera behind the player (Lua CamReset). The player who
/// watches another player use that NPC (the Bath Messengers' shop). Again after each load.
void WatchTalkTick(const GameSnapshot& s) {
    static double steady_at = 0, warped_at = 0, cam_until = 0;
    static int tries = 0;
    static bool done = false;
    if (cam_until > 0 && Now() >= cam_until) {
        cam_until = 0;
        runtime_pad_director_sticks(-1, -1, -1, -1);
        Log("test: watchtalk: camera turned (right stick released)");
    }
    const std::size_t k = g.test.find("watchtalk=");
    if (k == std::string::npos) {
        return;
    }
    if (!s.world_up || s.loading || !IsDreamMap(s.map_id) ||
        (s.session_role != kSessionRoleClient && s.cooperators < 1)) {
        steady_at = warped_at = 0;
        tries = 0;
        done = false;
        return;
    }
    if (steady_at == 0) {
        steady_at = Now();
    }
    char* end = nullptr;
    const std::uint32_t target = std::uint32_t(std::strtoul(g.test.c_str() + k + 10, &end, 10));
    const float dist = end && *end == ':' ? std::strtof(end + 1, &end) : 1.5f;
    const bool fixed = end && *end == ':';
    const float angle = fixed ? std::strtof(end + 1, &end) : 0.0f;
    const double delay = end && *end == '@' ? std::strtod(end + 1, nullptr) : 8.0;
    static const float kAngles[] = {60, -60, 90, -90, 40, -40};
    if (done || Now() - steady_at < delay) {
        return;
    }
    if (warped_at > 0) {
        if (Now() - warped_at < 1.0) {
            return;
        }
        std::uint32_t prompt = 0;
        const bool up = PromptUp(&prompt);
        Log("test: watchtalk: watch_text %s%s", up ? "action prompt " : "no action prompt",
            up ? std::to_string(prompt).c_str() : "");
        if (up || fixed || tries >= int(std::size(kAngles))) {
            done = true;
            // 0x132F5A0(ctx, 10000 = the local player, 1): the camera behind the player (party_b2).
            using CamReset = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
            reinterpret_cast<CamReset>(Guest(0x132f5a0))(0, 10000, 1);
            // "cam=RX:RY:S": then the right stick held S s (default 170:215:0.5, a little right and
            // down: the bath is low and the player's own body hid it).
            int rx = 170, ry = 215;
            double secs = 0.5;
            if (const std::size_t c = g.test.find("cam="); c != std::string::npos) {
                std::sscanf(g.test.c_str() + c + 4, "%d:%d:%lf", &rx, &ry, &secs);
            }
            runtime_pad_director_sticks(-1, -1, rx, ry);
            cam_until = Now() + secs;
            Log("test: watchtalk: in place%s; camera put behind the player, right stick %d:%d for %.1f s",
                up ? " (in use range)" : "", rx, ry, secs);
            return;
        }
    }
    const std::uint64_t chr = FindChr(target);
    if (!chr) {
        return;
    }
    const float a = fixed ? angle : kAngles[tries++];
    const bool ok = StandAt(s, chr, dist, 0, a * 0.017453293f);
    warped_at = Now();
    Log("test: watchtalk: standing %.1f m from t%u, %.0f deg off its front, facing it%s", dist, target, a,
        ok ? "" : " - warp failed");
}

/// "hsdelay=S" (hostheadstone / guestheadstone): s after the first steady Dream (default 20).
double HeadstoneTestDelay() {
    const std::size_t k = g.test.find("hsdelay=");
    return k == std::string::npos ? 20.0 : std::strtod(g.test.c_str() + k + 8, nullptr);
}

/// 0.7.2 last-lamp guard (lampshare::RestoreLastLamp; BB_PARTY_LAMP_RECORD_GUARD=0 off): any role,
/// in a steady world. Watches this map's lamps' lit flags (the lamp-lit 7AAB02xx and the headstone
/// flag base+10) and this game's last-lamp / respawn records (GSM +0x1528 / +0x1538 / +0x153C). A
/// record move to a lamp lit by someone else just now is put back after 2.5 s unless a load came
/// (a travel) or this player stands at that lamp (its own lighting or rest, an in-world travel).
void LastLampGuardTick(const GameSnapshot& s) {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_LAMP_RECORD_GUARD");
        return !(v && v[0] == '0');
    }();
    struct Rec {
        std::uint64_t lamp = 0, respawn = 0;
        std::uint32_t mode = 0;
    };
    static Rec base, was;
    static bool have = false, pending = false;
    static double pending_at = 0, pending_lit_ago = -1;
    static std::uint32_t map = 0;
    static double next = 0;
    static std::vector<std::pair<std::uint32_t, bool>> lit;  // return point, lit
    static std::vector<std::pair<std::uint32_t, double>> rose;  // return point, when its flag rose
    if (!on) {
        return;
    }
    if (!s.world_up || s.loading || !s.player || s.map_id != map) {
        if (pending) {
            Log("lamp record guard: a load: the last lamp %u stands (a travel)", std::uint32_t(base.lamp));
        }
        have = pending = false;  // a load: the records are the travel's; baseline again after it
        lit.clear();
        rose.clear();
        map = s.world_up && !s.loading ? s.map_id : 0;
        return;
    }
    const double now = Now();
    if (now < next) {
        return;
    }
    next = now + 0.2;
    for (const lamptravel::Lamp& l : lamptravel::kLamps) {
        const std::uint32_t rp = lamptravel::ReturnPoint(l);
        if (lamptravel::MapOf(rp) != (s.map_id & 0xffff0000u)) {
            continue;
        }
        bool a = false, b = false;
        progress::ReadFlag(lamptravel::LitFlag(l), &a);
        progress::ReadFlag(70000000u + std::uint32_t(l.aab) * 10000u + 200u + l.n, &b);
        const bool v = a || b;
        auto it = std::find_if(lit.begin(), lit.end(), [&](const auto& e) { return e.first == rp; });
        if (it == lit.end()) {
            lit.emplace_back(rp, v);
        } else {
            if (v && !it->second) {
                rose.erase(std::remove_if(rose.begin(), rose.end(), [&](const auto& e) { return e.first == rp; }),
                           rose.end());
                rose.emplace_back(rp, now);
            }
            it->second = v;
        }
    }
    std::uint64_t gsm = 0;
    Rec cur;
    if (!SafeGet(Guest(0x5556678), &gsm) || !gsm || !SafeGet(gsm + 0x1528, &cur.lamp) ||
        !SafeGet(gsm + 0x1538, &cur.mode) || !SafeGet(gsm + 0x153c, &cur.respawn)) {
        return;
    }
    if (!have) {
        base = cur;
        have = true;
        return;
    }
    const std::uint32_t now_rp = std::uint32_t(cur.lamp);
    if (cur.lamp != base.lamp && !pending) {
        pending = true;
        pending_at = now;
        was = base;
        auto r = std::find_if(rose.begin(), rose.end(), [&](const auto& e) { return e.first == now_rp; });
        pending_lit_ago = r == rose.end() ? -1.0 : now - r->second;
    }
    if (!pending) {
        base = cur;
        return;
    }
    float me[3], sp[4];
    float dist = -1.0f;
    if (balance::ChrPosition(s.player, me) && LampSpotNow(now_rp, sp)) {
        dist = std::sqrt((me[0] - sp[0]) * (me[0] - sp[0]) + (me[1] - sp[1]) * (me[1] - sp[1]) +
                         (me[2] - sp[2]) * (me[2] - sp[2]));
    }
    const bool by_guest = g_guest_lit_rp.load() == now_rp && Now() - g_guest_lit_at.load() <= lampshare::kLitWindowS + 3.0;
    // BB_PARTY_LAMP_ARRIVAL_COUNTS=0: the 0.7.2 guard as it was (test A/B of the mensislamp fix).
    static const bool arrival_counts = [] {
        const char* v = std::getenv("BB_PARTY_LAMP_ARRIVAL_COUNTS");
        return !(v && v[0] == '0');
    }();
    const bool restore =
        lampshare::RestoreLastLamp(std::uint32_t(was.lamp), now_rp, pending_lit_ago, dist, false, by_guest, arrival_counts);
    if (!restore) {
        pending = false;
        base = cur;
        return;
    }
    if (now - pending_at < 2.5) {
        return;  // a travel's load may still start
    }
    pending = false;
    SafeWrite(gsm + 0x1528, &was.lamp, 8);
    SafeWrite(gsm + 0x1538, &was.mode, 4);
    SafeWrite(gsm + 0x153c, &was.respawn, 8);
    base = was;
    Log("lamp record guard: the last lamp moved to %u, a lamp lit %.1f s before by someone else (this player "
        "%.0f m from it): put back to %u (respawn %#llx, mode %u)",
        now_rp, pending_lit_ago, double(dist), std::uint32_t(was.lamp), static_cast<unsigned long long>(was.respawn),
        was.mode);
}

void GuestServicesTick(const GameSnapshot& s, party::PartyLink* link) {
    LastLampGuardTick(s);
    LampTick(s, link);
    WatchTalkTick(s);
    // Live test "hostheadstone[;keys=...]": the HOST uses a Dream headstone (t210000) with a guest
    // present (IsMultiplayerInProgress lift), 20 s after the first cooperator: the party follows.
    {
        static bool started = false;
        static double coop_at = 0;
        // "guestheadstone[;keys=...]": the same on the GUEST (a client in the host's Dream; needs
        // the headstones allowed for guests, BB_PARTY_GUEST_LAMPS=1).
        // "hosttalk=N[;open=TEXT][;keys=...]": the HOST uses talk N instead (a Bath Messenger's shop).
        // "guesttalk=N[;open=TEXT][;keys=...]" (0.7.6 chaliceshop): the same on the GUEST for talk N in the Dream or in
        // a Chalice Dungeon (the dungeon Bath Messenger is t290300).
        static const int role = g.test.find("guestheadstone") != std::string::npos ||
                                        g.test.find("guesttalk=") != std::string::npos
                                    ? kSessionRoleClient
                                : g.test.find("hostheadstone") != std::string::npos ||
                                          g.test.find("hosttalk=") != std::string::npos
                                    ? 3
                                    : -1;
        // "hsapart" (guestheadstone): the delay counts from the moment the host is in another area
        // (a guest left in the Dream by the host's alone travel, party_stay_rules.h).
        static const bool apart_only = g.test.find("hsapart") != std::string::npos;
        // "hsalone" (hostheadstone): no guest needed in the Dream (the host picks after a guest left alone).
        static const bool no_coop = g.test.find("hsalone") != std::string::npos;
        if (!started && apart_only && !b2::MemberApart(0)) {
            coop_at = 0;
        } else if (!started && role >= 0 && s.session_role == role &&
            ((HostTalkTarget() / 10000 == 29 ? (s.map_id >> 24) == 29 : IsDreamMap(s.map_id)) &&
             (HostTalkTarget() / 10000 != 29 || g.test.find("guesttalk=") != std::string::npos)) &&
            s.world_up && !s.loading && (role != 3 || no_coop || s.cooperators >= 1)) {
            if (coop_at == 0) {
                coop_at = Now();
            } else if (Now() - coop_at >= HeadstoneTestDelay() && FindChr(HostTalkTarget())) {
                started = true;
                g_lvl = LevelUpScript{};
                g_lvl.open = true;
                g_lvl.target = HostTalkTarget();
                // "hosttalk=N;open=TEXT": that talk menu row first (a shop's Purchase), else none.
                if (const std::size_t o = g.test.find("open="); o != std::string::npos && g_lvl.target != 210000) {
                    g_lvl.menu_text = std::uint32_t(std::strtoul(g.test.c_str() + o + 5, nullptr, 10));
                } else {
                    g_lvl.menu_text = 0;
                }
                if (const std::size_t k = g.test.find("keys="); k != std::string::npos) {
                    const std::size_t e = g.test.find(';', k);
                    g_lvl.keys = g.test.substr(k + 5, e == std::string::npos ? std::string::npos : e - k - 5);
                }
                g_lvl.stage = LevelUpScript::WaitPlaced;
                g_lvl.at = Now();
                Log("test: %s headstone: starting (cooperators %d)", role == 3 ? "host" : "guest", s.cooperators);
            }
        }
        if (started && s.session_role == role) {
            LevelUpTick(s, Now());
        }
    }
    static bool progress_logged = false;
    if (!g.test.empty() && s.world_up && !s.loading && IsDreamMap(s.map_id) && !progress_logged) {
        progress_logged = true;
        CheckDollProgress(s, s.session_role == kSessionRoleClient ? "guest (host world)" : "own world");
    } else if (!s.world_up || s.loading) {
        progress_logged = false;
    }
    static double last_probe = 0;
    if (g.test.find("probe") != std::string::npos && s.world_up && !s.loading && IsDreamMap(s.map_id) &&
        Now() - last_probe >= 10.0) {
        last_probe = Now();
        ProbeDoll();
    }
    if (!g.installed) {
        return;
    }
    GuestServicesInput in;
    in.enabled = g.enabled;
    in.world_up = s.world_up;
    in.loading = s.loading;
    in.session_role = s.session_role;
    in.map_id = s.map_id;
    const bool open = GuestServicesGate(in);
    if (open != g.open) {
        g.open = open;
        g_gate.store(open, std::memory_order_relaxed);
        if (open) {
            ++g.opens;
            g.open_at = Now();
            g.rec = RecordOf(s);
            Log("open: guest in the host's %s; level %d, echoes %d, Insight %d", MapName(s.map_id).c_str(), g.rec.level,
                g.rec.echoes, g.rec.insight);
        } else {
            Log("closed (role %s, map %s%s)", SessionRoleName(s.session_role), MapName(s.map_id).c_str(),
                s.loading ? ", loading" : "");
        }
    }
    {
        // Gehrman (t210305): his ordinary talk is a guest's too; his last event (the ending choice)
        // stays the host's. The flags are the host world's (1000-1999 come with the join snapshot
        // and the mirror); an unreadable flag keeps him vanilla.
        bool f1028 = true, f1029 = true, f1030 = false;
        const bool read = open && ReadEventFlag(1028, &f1028) && ReadEventFlag(1029, &f1029) && ReadEventFlag(1030, &f1030);
        const bool talk = read && !GehrmanLastEvent(f1028, f1029, f1030);
        if (talk != g_gehrman_talk.exchange(talk, std::memory_order_relaxed) && open) {
            Log("Gehrman (t210305): %s (flags 1028 %d, 1029 %d, 1030 %d)",
                talk ? "a guest may talk to him (his ordinary lines)"
                     : "kept vanilla for a guest (his last event: the ending choice is the host's)",
                int(f1028), int(f1029), int(f1030));
        }
    }
    const double now = Now();
    bool doll_ready = true;
    std::uint32_t target = 0;
    if (g.test.find("warp=doll") != std::string::npos) {
        target = 210304;
    } else if (const std::size_t t = g.test.find("warp=talk:"); t != std::string::npos) {
        target = std::uint32_t(std::strtoul(g.test.c_str() + t + 10, nullptr, 10));
    }
    if (target && g.test_step == 0) {
        // Wait until the character exists and stands somewhere (polled every tick).
        float d[3] = {};
        const std::uint64_t chr = FindChr(target);
        doll_ready = chr && SafeRead(chr + 0xc0, d, sizeof d) && (d[0] != 0.0f || d[2] != 0.0f);
    }
    int gate_n = 1;
    if (const std::size_t gn = g.test.find("gate="); gn != std::string::npos) {
        gate_n = std::atoi(g.test.c_str() + gn + 5);
    }
    // "ownworld" (0.7.6 phantomsave): the same fixture in the player's OWN world after it left a party
    // (the guest must be able to talk to the Doll there; the kicked / left guest report). "ownsolo": also
    // without ever having been summoned (a save that carries a stuck state).
    static double own_since = 0;
    const bool own_test = g.test.find("ownworld") != std::string::npos;
    const bool own_world = own_test && s.session_role == 0 && s.world_up && !s.loading && IsDreamMap(s.map_id) &&
                           (g.opens >= gate_n || g.test.find("ownsolo") != std::string::npos);
    if (!own_world) {
        own_since = 0;
    } else if (own_since == 0) {
        own_since = now;
    }
    if (((g.open && !own_test) || own_world) && !g.test.empty() && !g.test_done && doll_ready &&
        g.test.find("guesttalk=") == std::string::npos &&
        (own_world || (g.opens >= gate_n && now - g.open_at >= 5.0)) && (!own_world || now - own_since >= 8.0) &&
        now >= g.test_next) {
        if (!RunTestFixture(s, g.test_step)) {
            g.test_done = true;
            Log("test: fixture done");
        } else if (g.test_step == 0 && g.test.find("levelup") != std::string::npos) {
            g_lvl.stage = LevelUpScript::WaitPlaced;
            g_lvl.at = now;
            g_lvl.shop = false;
            g_lvl.menu_text = kTextChannel;
            g_lvl.target = target ? target : 210304;
        } else if (const std::size_t o = g.test.find("open"); g.test_step == 0 && o != std::string::npos) {
            g_lvl.stage = LevelUpScript::WaitPlaced;
            g_lvl.at = now;
            g_lvl.open = true;
            g_lvl.target = target;
            g_lvl.menu_text = g.test.compare(o, 5, "open=") == 0 ? std::uint32_t(std::strtoul(g.test.c_str() + o + 5, nullptr, 10)) : 0;
            if (const std::size_t k = g.test.find("keys="); k != std::string::npos) {
                const std::size_t e = g.test.find(';', k);
                g_lvl.keys = g.test.substr(k + 5, e == std::string::npos ? std::string::npos : e - k - 5);
            }
        } else if (g.test_step == 0 && g.test.find("buy") != std::string::npos) {
            g_lvl.stage = LevelUpScript::WaitPlaced;
            g_lvl.at = now;
            g_lvl.shop = true;
            g_lvl.menu_text = kTextPurchase;
            g_lvl.target = target;
        }
        ++g.test_step;
        g.test_next = now + 8.0;
        g.rec = RecordOf(ReadGameState(false)); // the fixture's echoes are not a purchase
    }
    if (g.open || own_world) {
        LevelUpTick(s, now);
    }
    if (g.test_done && !g.test_chain.empty() &&
        (g_lvl.stage == LevelUpScript::Off || g_lvl.stage == LevelUpScript::Done)) {
        g.test = g.test_chain.front();
        g.test_chain.erase(g.test_chain.begin());
        g.test_done = false;
        g.test_step = 0;
        g.test_next = now + 3.0;
        g_lvl = LevelUpScript{};
        Log("test: chain: next fixture '%s' (%zu more after it)", g.test.c_str(), g.test_chain.size());
    }
    if (!g.open || now - g.last_check < 1.0) {
        return;
    }
    g.last_check = now;
    for (int i = 0; i < kEnvSlots; ++i) {
        const std::uint64_t env = g_envs[i].load(std::memory_order_relaxed);
        if (!env || g.logged_envs[i] == env) {
            continue;
        }
        g.logged_envs[i] = env;
        LogEnv(env);
    }
    {
        std::string cmds;
        for (int i = 0; i < 96; ++i) {
            if (const std::uint32_t n = g_talk_cmds[i].exchange(0, std::memory_order_relaxed)) {
                cmds += " " + std::to_string(i) + "x" + std::to_string(n);
            }
        }
        if (!cmds.empty()) {
            Log("talk commands run:%s (31 = OpenSoul)", cmds.c_str());
        }
    }
    static std::uint32_t reqs_logged = 0;
    if (const std::uint32_t q = g_menu_requests.load(std::memory_order_relaxed); q != reqs_logged) {
        Log("menu requests %u (LevelUp %u)", q, g_levelup_requests.load());
        reqs_logged = q;
    }
    static std::uint32_t menus_logged = 0;
    if (const std::uint32_t m = g_levelup_menus.load(std::memory_order_relaxed); m != menus_logged) {
        Log("the game built the LevelUp menu (%u so far)", m);
        menus_logged = m;
    }
    if (const std::uint32_t d = g_doll_rewrites.exchange(0, std::memory_order_relaxed)) {
        Log("Doll level-up events (12100112-114): %u chr type test(s) set to the guest's own type", d);
    }
    if (const std::uint32_t m = g_multi_overrides.exchange(0, std::memory_order_relaxed)) {
        Log("headstones / lamps asked IsMultiplayerInProgress %u times (answered 0)", m);
    }
    const std::uint32_t n = g_overrides.load(std::memory_order_relaxed);
    if (n != g.overrides_logged) {
        Log("talk scripts asked IsClientPlayer %u times (answered 0)", n - g.overrides_logged);
        g.overrides_logged = n;
    }
    const PlayerRecord r = RecordOf(s);
    if (r != g.rec) {
        const std::string d = DescribeRecordChange(g.rec, r);
        if (!d.empty()) {
            Log("own character: %s", d.c_str());
            g.changed_at = now;
        }
        g.rec = r;
    } else if (g.changed_at > 0 && now - g.changed_at >= 15.0) {
        g.changed_at = 0;
        Log("own character steady for 15 s: level %d, echoes %d, Insight %d", r.level, r.echoes, r.insight);
    }
}

std::uint64_t GuestSvcFindChr(std::uint32_t id) {
    return FindChr(id);
}

bool GuestSvcPromptUp(std::uint32_t* id) {
    return PromptUp(id);
}

bool GuestSvcOnlyPrompt(std::uint32_t id, std::uint32_t* other) {
    std::uint32_t ids[256];
    const int n = BbGameMenu::RecentLookups(400, ids, 256);
    bool seen = false;
    *other = 0;
    for (int i = 0; i < n; ++i) {
        if ((ids[i] >= 10010000 && ids[i] < 10020000) || (ids[i] >= 70000000 && ids[i] < 70010000)) {
            if (ids[i] == id) {
                seen = true;
            } else if (!*other) {
                *other = ids[i];
            }
        }
    }
    return seen && !*other;
}

bool GuestServicesOpen() {
    return g.open;
}

} // namespace coop
#endif
