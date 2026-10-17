// SPDX-License-Identifier: GPL-3.0-or-later
// Split travel's choice dialog (see party_split.h; docs/party/split_travel.md).
#include "party_split.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_b2.h"
#include "party_death_rules.h"
#include "party_director.h"
#include "party_guestsvc.h"
#include "party_link.h"
#include "party_progress.h"
#include "party_status.h"
#include "party_travel.h"
#include "../bbport_game_menu_pages.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop::split {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kSlotMenuMan = 0x5562878;    // +0x78 dialog kind up (0 none), +0xD08 generic dialog struct
constexpr u64 kMenuBusyByte = 0x553b1ba;    // ShowGenDialog 0x1335110 opens nothing while set
constexpr u64 kOpenDialog = 0x16bebf0;      // (dialog struct, kind)
constexpr int kRoleHost = 3, kRoleClient = 6;
constexpr u32 kPadCross = 0x4000, kPadRight = 0x20, kPadCircle = 0x2000;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party split: ") + line + "\n");
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <class T>
T Field(u64 base, u64 off, T fallback = T{}) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

u64 Slot(u64 off) {
    u64 v = 0;
    const u64 at = Guest(off);
    return at && SafeGet(at, &v) ? v : 0;
}

int SessionRole() {
    return Field<int>(Slot(0x5540290), 0x124, -1);
}

Mode CurrentMode() {
    static const Mode m = ModeFromEnv(std::getenv("BB_PARTY_TRAVEL_CHOICE"));
    return m;
}

enum class Ask { None, Queued, Open, Answered };
struct Pending {
    Ask state = Ask::None;
    u32 id = 0;
    bool host = false;
    bool mark = false;  // a Hunter's Mark (id = the player's own last lamp)
    bool solo_load = false;  // a guest's lamp in the loaded map: its own load, no question (bug 10)
    double since = 0;
    u64 dialog = 0;
    int test_step = 0;
    int reopens = 0;  // the dialog did not show (menu +0x78 0): opened again
    double test_at = 0;
    bool after_scene = false;  // a story warp: ask once its cutscene is over (kAskAfterSceneDelay)
};
std::mutex g_mu;
std::atomic<double> g_story_at{-1};  // NoteStoryWarp
double g_remo_seen_at = -1;          // main thread: last tick a cutscene (remo) was playing

/// SprjRemo player +0x168 bit 0 (party_catchup's RemoPlaying).
bool SceneRunning() {
    u64 remo = 0, player = 0;
    std::uint8_t bits = 0;
    return SafeGet(Guest(0x5540058), &remo) && remo && SafeGet(remo + 8, &player) && player &&
           SafeGet(player + 0x168, &bits) && (bits & 1);
}
int g_asked = 0;  // questions opened (BB_PARTY_SPLIT_TEST answers them in order)
Pending g_p;  // under g_mu (OnLampWarp: game thread; Tick: main thread - the same thread in practice)

// ---- Whose lamp (playtest 1 bug 20) ----
// The lamp / headstone "used" flags (deathrules::LampTriggerFlag) this game set itself (game
// thread: the progress SetEventFlag / CompleteEvent hooks) and the ones a peer sent (packet 0x0f,
// drained by 0x1339B50 on the main thread: the read call sites below).
std::atomic<double> g_local_at{-1}, g_remote_at{-1};
std::atomic<u32> g_local_id{0}, g_remote_id{0};
constexpr u64 kPacketRead = 0x1789200;  // int (net man, peer, packet type, buffer, size)
// 0x1339B50 (SprjLuaEventMan update) drains packet 0x0e (network-synced EMEVD event state, 0x14
// bytes) at 0x1339C02 / 0x1339D0E and packet 0x0f (a peer's CompleteEvent flag, 4 bytes) at
// 0x1339D8E / 0x1339F2E.
constexpr u64 kPacketSites[] = {0x1339c02, 0x1339d0e, 0x1339d8e, 0x1339f2e};
// The lamp warp events (deathrules::LampTriggerFlag): world lamp 7200, Dream headstone 12107000,
// common 7300 (the Dream side of a headstone warp).
bool LampWarpEvent(u32 id) {
    return id == 7200 || id == 7300 || id == 12107000;
}
std::atomic<u32> g_ev_packets{0}, g_ev_any{0}, g_ev_last[5];  // 0x0e packets: of the lamp events, any; the last one
std::atomic<bool> g_ev_dump{false};

void OnLocalFlagSet(u32 id) {
    if (deathrules::LampTriggerFlag(id)) {
        g_local_id = id;
        g_local_at = Now();
    }
}

BB_COOP_SYSV u64 FlagPacketRead(u64 man, u64 peer, u64 type, u64 buf, u64 size) {
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64);
    const u64 r = reinterpret_cast<Fn>(Guest(kPacketRead))(man, peer, type, buf, size);
    if (!u32(r) || !buf) {
        return r;
    }
    u32 v[5] = {};
    if (u32(type) == 0xf && u32(size) == 4 && SafeGet(buf, &v[0]) && deathrules::LampTriggerFlag(v[0])) {
        g_remote_id = v[0];
        g_remote_at = Now();
    } else if (u32(type) == 0xe && u32(size) == 0x14 && SafeRead(buf, v, sizeof v)) {
        // A peer's lamp warp event ran (its state replicated here): the same warp follows on
        // this game a moment later (playtest 1 bug 20, live splitq run 1: the guest asked 0.04 s
        // after the host's headstone, no 0x0f flag).
        u32 ev = 0;
        for (u32 x : v) {
            ev = ev ? ev : LampWarpEvent(x) ? x : 0;
        }
        const u32 n = g_ev_any.fetch_add(1);
        if (n < 12 || ev) {
            for (int i = 0; i < 5; ++i) {
                g_ev_last[i] = v[i];
            }
            g_ev_dump = true;
        }
        if (ev) {
            g_ev_packets.fetch_add(1);
            g_remote_id = ev;
            g_remote_at = Now();
        }
    }
    return r;
}

double Age(const std::atomic<double>& at, double now) {
    const double t = at.load();
    return t < 0 ? -1 : now - t;
}

// Outgoing / incoming EVENT split_choosing (main thread / callback thread).
std::atomic<u32> g_notify_lamp{0};
std::mutex g_note_mu;
struct Choosing {
    std::string who;
    int slot = -1;
    u32 lamp = 0;
    bool fresh = false;
};
// P4: this game's last answered question (main thread) and the last member request the host ran.
std::atomic<u32> g_decided_id{0}, g_member_run_id{0};
std::atomic<double> g_decided_at{-1}, g_member_run_at{-1};
Choosing g_choosing;  // under g_note_mu

/// Opens the game's YES / NO dialog with our question; the dialog struct, 0 when it cannot open.
u64 OpenQuestion(u32 text_override = 0) {
    const u64 mm = Slot(kSlotMenuMan);
    u8 busy = 0;
    if (!mm || Field<int>(mm, 0x78, -1) != 0 || !SafeGet(Guest(kMenuBusyByte), &busy) || busy) {
        return 0;
    }
    const u64 dlg = Field<u64>(mm, 0xd08);
    const u32 text = text_override ? text_override : BbGameMenu::Pages::SplitTravelText(0);
    if (!dlg || !text) {
        return 0;
    }
    // The talk command OpenGenericDialog (talk ESD command 0x11 in 0x203FFD0: args kind, message,
    // button 1, button 2, mode) the way the Lua OnYesNoDialog 0x132D2C0 writes it. Kind 7 with
    // buttons 1 / 2 is a one-button message box (live split2: "OK"); kind 8 with 3 / 4 is the
    // YES / NO question (BB_PARTY_SPLIT_DIALOG=kind,b1,b2 overrides, for probing).
    static u32 kind = 8, one = 3, two = 4;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* v = std::getenv("BB_PARTY_SPLIT_DIALOG"); v && v[0]) {
            std::sscanf(v, "%u,%u,%u", &kind, &one, &two);
        }
    }
    const u32 zero = 0, minus = 0xffffffffu;
    const u8 b1 = 1;
    SafeWrite(dlg + 0x00, &text, 4);
    SafeWrite(dlg + 0x04, &one, 4);
    SafeWrite(dlg + 0x08, &two, 4);
    SafeWrite(dlg + 0x14, &zero, 4);
    SafeWrite(dlg + 0x10, &b1, 1);
    SafeWrite(dlg + 0x18, &minus, 4);
    SafeWrite(dlg + 0x1c, &zero, 4);
    SafeWrite(mm + 0x254, &zero, 4);
    using Open = void(BB_COOP_SYSV*)(u64, u32);
    reinterpret_cast<Open>(Guest(kOpenDialog))(dlg, kind);
    SafeWrite(mm + 0x16c, &minus, 4);
    SafeWrite(mm + 0x170, &minus, 4);
    return dlg;
}

/// Opens the game's one-button message box (kind 7, button 1: "OK") with port text `text`; false
/// while another dialog / menu is up.
u64 OpenMessage(u32 text) {
    const u64 mm = Slot(kSlotMenuMan);
    u8 busy = 0;
    if (!text || !mm || Field<int>(mm, 0x78, -1) != 0 || !SafeGet(Guest(kMenuBusyByte), &busy) || busy) {
        return 0;
    }
    const u64 dlg = Field<u64>(mm, 0xd08);
    if (!dlg) {
        return 0;
    }
    const u32 zero = 0, minus = 0xffffffffu, one = 1, two = 2;
    const u8 b1 = 1;
    SafeWrite(dlg + 0x00, &text, 4);
    SafeWrite(dlg + 0x04, &one, 4);
    SafeWrite(dlg + 0x08, &two, 4);
    SafeWrite(dlg + 0x14, &zero, 4);
    SafeWrite(dlg + 0x10, &b1, 1);
    SafeWrite(dlg + 0x18, &minus, 4);
    SafeWrite(dlg + 0x1c, &zero, 4);
    SafeWrite(mm + 0x254, &zero, 4);
    using Open = void(BB_COOP_SYSV*)(u64, u32);
    reinterpret_cast<Open>(Guest(kOpenDialog))(dlg, 7);
    SafeWrite(mm + 0x16c, &minus, 4);
    SafeWrite(mm + 0x170, &minus, 4);
    return dlg;
}

/// Runs the warp the chosen way (main thread).
void Carry(const Pending& p, bool alone, const char* how) {
    g_decided_id = p.id;
    g_decided_at = Now();
    Log("lamp %u (%s): %s -> %s", p.id, p.host ? "host" : "guest", how, alone ? "travel alone" : "travel with the party");
    if (p.host) {
        // Inside the loaded map: no load for anyone (the chr warp; guests of this map beside).
        if (alone ? b2::WarpToLampNoLoad(p.id, false) : b2::HostPartyWarpNoLoad(p.id)) {
            return;
        }
        if (alone) {
            b2::HostTravelAloneNext();
        }
        RunLampWarpDecided(p.id, alone);
        return;
    }
    // BB_PARTY_GUEST_LAMP_INWORLD=1 (opt-in since playtest 1): a guest's own move inside the
    // host's loaded map is a chr warp with no load. Default: its own loading screen.
    static const bool inworld = deathrules::OptIn(std::getenv("BB_PARTY_GUEST_LAMP_INWORLD"));
    if (alone && inworld && b2::WarpToLampNoLoad(p.id, false)) {
        return;
    }
    if (alone && p.solo_load) {
        // D9 (pard4): a lamp use in the loaded map is a rest too (not a Hunter's Mark).
        const bool rest = deathrules::GuestAloneRestsHostWorld(!p.mark, true);
        bool carried = false;
        if (b2::GuestSoloLampLoad(p.id, p.mark, rest, &carried)) {
            if (rest && !carried) {
                GuestRestRequestNow(p.id);
            }
            return;
        }
        Log("lamp %u: the guest's own load is not possible now; nothing runs", p.id);
        return;
    }
    if (alone) {
        TravelIntent t;
        const b2::AloneWhy why = p.mark ? b2::AloneWhy::HuntersMark
                                 : p.id == 2102950 ? b2::AloneWhy::Lamp : b2::AloneWhy::Headstone;
        // A rest at a lamp resets the world the guest leaves (the host's) for everyone there.
        const bool rest = deathrules::GuestAloneRestsHostWorld(!p.mark, false);
        bool carried = false;
        if (b2::LampTravelIntent(p.id, &t) && b2::TravelAlone(t, why, rest ? p.id : 0, &carried)) {
            // Back into the host's area the rest rode on the re-join (one load); else the request.
            if (rest && !carried) {
                GuestRestRequestNow(p.id);
            }
            return;
        }
        Log("lamp %u: travelling alone is not possible now; the party travels together", p.id);
    }
    // With the party: a guest's Hunter's Mark takes the party to the host's last lamp.
    const u32 id = p.mark ? GuestDeathRedirectLamp() : p.id;
    if (p.mark && (id == kTravelNone || id == 0)) {
        Log("Hunter's Mark: the host's last lamp is not known; nothing runs");
        return;
    }
    if (!GuestLampRequestNow(id)) {
        Log("lamp %u: no party request possible (guest lamps off); nothing runs", p.id);
    }
}

} // namespace

Mode ModeFromEnv(const char* v) {
    if (v && std::strcmp(v, "party") == 0) {
        return Mode::Party;
    }
    if (v && std::strcmp(v, "alone") == 0) {
        return Mode::Alone;
    }
    return Mode::Ask;
}

const char* ModeName(Mode m) {
    return m == Mode::Party ? "party" : m == Mode::Alone ? "alone" : "ask";
}

namespace {
bool Hold(std::uint32_t id, bool mark);
bool OnMark(std::uint32_t last_lamp) {
    return Hold(last_lamp, true);
}
} // namespace

void Init() {
    if (!b2::Enabled()) {
        return;
    }
    SetMarkInterceptor(&OnMark);
    progress::SetLocalFlagSetObserver(&OnLocalFlagSet);
    int sites = 0;
    for (u64 at : kPacketSites) {
        sites += HookCallSite(at, kPacketRead, reinterpret_cast<const void*>(&FlagPacketRead),
                              "split: peer event / flag packet 0x0e / 0x0f (whose lamp)") ? 1 : 0;
    }
    Log("whose lamp: own lamp flags from the SetEventFlag / CompleteEvent hooks, a peer's from %d of 4 packet "
        "0x0e / 0x0f reads; a warp started by another member's lamp asks nothing here", sites);
    Log("on: a lamp, headstone or Hunter's Mark in a party asks \"Travel with the party?\" (mode %s)",
        ModeName(CurrentMode()));
}

void NoteLocalUse(std::uint32_t id) {
    g_local_id = id;
    g_local_at = Now();
}

void NoteStoryWarp(std::uint32_t id) {
    NoteLocalUse(id);
    g_story_at = Now();
}

bool OnLampWarp(std::uint32_t id) {
    return Hold(id, false);
}

bool OwnChoiceFor(std::uint32_t id, double within_s) {
    if (id == 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_p.state != Ask::None && g_p.id == id && !g_p.mark) {
            return true;
        }
    }
    const double a = Age(g_decided_at, Now());
    return g_decided_id.load() == id && a >= 0 && a <= within_s;
}

void NoteMemberRequestRun(std::uint32_t id) {
    g_member_run_id = id;
    g_member_run_at = Now();
}

namespace {
bool Hold(std::uint32_t id, bool mark) {
    if (!b2::Enabled()) {
        return false;
    }
    const PartyRole role = PartyDirector::Get().Role();
    const int sr = SessionRole();
    const bool guest = role == PartyRole::Guest && sr == kRoleClient;
    const bool host = role == PartyRole::Host && sr == kRoleHost && b2::HostHasMembers();
    if (!guest && !host) {
        return false;  // nobody to travel with: the plain warp
    }
    if (!mark) {
        u64 c[6];
        LampWarpCallers(c);
        Log("lamp %u (%s): warp called from +%llx < +%llx < +%llx < +%llx < +%llx < +%llx", id, host ? "host" : "guest",
            (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3],
            (unsigned long long)c[4], (unsigned long long)c[5]);
    }
    if (host && id / 100000 == 29) {
        // G1: the host enters a Chalice Dungeon from its altar (the awaken point's warp 29xxxxx runs
        // from the altar's EMEVD, no lamp flag of the host's: live g1l dropped it as "another
        // member's lamp"). No question: the guests cannot follow into a dungeon by travel, they are
        // summoned into it (chalice.md 4).
        Log("lamp %u (host): a Chalice Dungeon awaken point: the host enters alone, the party is summoned there", id);
        return false;
    }
    if (!mark) {
        // Bug 20: another member's lamp / headstone flag (packet 0x0f) runs the same warp event
        // here. Only the member who used it is asked; here the warp is dropped (its outcome comes
        // as the party travel, or nothing).
        const double now = Now(), own = Age(g_local_at, now), peer = Age(g_remote_at, now);
        if (deathrules::PlanLampOrigin(own, peer, progress::LocalFlagSetHooked()) == deathrules::LampOrigin::Other) {
            char why[96];
            if (own >= 0 && own <= deathrules::kLampOriginWindow) {
                std::snprintf(why, sizeof why, "a member's lamp flag / event %u came %.1f s after this game's own %u",
                              g_remote_id.load(), own - peer, g_local_id.load());
            } else {
                std::snprintf(why, sizeof why, "this game set no lamp / headstone flag in the last %.0f s%s",
                              deathrules::kLampOriginWindow, peer >= 0 && peer <= deathrules::kLampOriginWindow
                                                                 ? "; a member's came just before" : "");
            }
            Log("lamp %u (%s): not this player's use (%s): the same warp event of another member's lamp / headstone; "
                "no question here, the warp is dropped (that member chooses)",
                id, host ? "host" : "guest", why);
            party::status::SetLastEvent("A party member is choosing travel");
            return true;
        }
    }
    if (!mark && host && g_member_run_id.load() == id && Age(g_member_run_at, Now()) >= 0 &&
        Age(g_member_run_at, Now()) <= 15.0) {
        // P4: a guest's scripted warp (its question answered there) that also runs here: the
        // party travel for it is already running; no second question.
        Log("lamp %u (host): a member's request for this warp already moves the party (%.1f s ago); no second "
            "question", id, Age(g_member_run_at, Now()));
        return true;
    }
    if (!mark && b2::AtLampSpot(id)) {
        return false;  // standing at it already: no question, no move (the same-map hook: last lamp only)
    }
    const deathrules::GuestLamp gl =
        guest ? deathrules::PlanGuestLamp(true, b2::AtLampSpot(id), b2::LampInLoadedMap(id), mark) : deathrules::GuestLamp::Plain;
    if (gl == deathrules::GuestLamp::SoloLoad) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_p.state != Ask::None) {
            Log("lamp %u: a choice is already pending (lamp %u); this warp is dropped", id, g_p.id);
            return true;
        }
        g_p = Pending{};
        g_p.state = Ask::Answered;
        g_p.id = id;
        g_p.mark = mark;
        g_p.solo_load = true;
        g_p.since = Now();
        Log("%s %u (guest): in the loaded map: no question, the guest's own loading screen (the host stays)",
            mark ? "Hunter's Mark to lamp" : "lamp", id);
        return true;
    }
    if (CurrentMode() == Mode::Party && host) {
        return false;  // the host's warp is the party move (the funnel capture)
    }
    if (CurrentMode() == Mode::Party && guest) {
        return false;  // the guest's request (party_guestsvc), as before
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_p.state != Ask::None) {
        Log("lamp %u: a choice is already pending (lamp %u); this warp is dropped", id, g_p.id);
        return true;
    }
    g_p = Pending{};
    g_p.state = Ask::Queued;
    g_p.id = id;
    g_p.host = host;
    g_p.mark = mark;
    g_p.since = Now();
    g_p.after_scene = !mark && Age(g_story_at, g_p.since) >= 0 && Age(g_story_at, g_p.since) <= 2.0;
    Log("%s %u (%s): held for the choice (mode %s)", mark ? "Hunter's Mark to lamp" : "lamp", id, host ? "host" : "guest",
        ModeName(CurrentMode()));
    return true;
}
} // namespace

void OnChoosingEvent(int slot, const std::string& who, const std::string& body) {
    u32 lamp = 0;
    if (const std::size_t k = body.find("\"lamp\":"); k != std::string::npos) {
        lamp = static_cast<u32>(std::strtoul(body.c_str() + k + 7, nullptr, 10));
    }
    std::lock_guard<std::mutex> lk(g_note_mu);
    g_choosing.who = who.empty() ? (slot == 0 ? "the host" : "slot " + std::to_string(slot)) : who;
    g_choosing.slot = slot;
    g_choosing.lamp = lamp;
    g_choosing.fresh = true;
}

void Tick(const GameSnapshot& s, party::PartyLink* link) {
    static double release_at = 0;  // a test / cancel press is a tap
    {
        // Diagnostics (bug 20): every lamp flag, own or a peer's, once.
        static double local_seen = -1, remote_seen = -1;
        const double l = g_local_at.load(), r = g_remote_at.load();
        if (l != local_seen) {
            local_seen = l;
            Log("lamp flag %u set by this game (this player's lamp / headstone use)", g_local_id.load());
        }
        if (g_ev_dump.exchange(false)) {
            Log("packet 0x0e #%u (lamp events %u): %u %u %u %u %u", g_ev_any.load(), g_ev_packets.load(),
                g_ev_last[0].load(), g_ev_last[1].load(), g_ev_last[2].load(), g_ev_last[3].load(), g_ev_last[4].load());
        }
        if (r != remote_seen) {
            remote_seen = r;
            if (LampWarpEvent(g_remote_id.load())) {
                Log("lamp warp event %u from a party member (packet 0x0e #%u: %u %u %u %u %u): their lamp / headstone use",
                    g_remote_id.load(), g_ev_packets.load(), g_ev_last[0].load(), g_ev_last[1].load(),
                    g_ev_last[2].load(), g_ev_last[3].load(), g_ev_last[4].load());
            } else {
                Log("lamp flag %u from a party member (packet 0x0f): their lamp / headstone use", g_remote_id.load());
            }
        }
        Choosing c;
        {
            std::lock_guard<std::mutex> lk(g_note_mu);
            c = g_choosing;
            g_choosing.fresh = false;
        }
        if (c.fresh) {
            const char* role = PartyDirector::Get().Role() == PartyRole::Host ? "guest" : "host";
            Log("%s %s is choosing travel (\"Travel with the party?\" for lamp %u on their screen)", role,
                c.who.c_str(), c.lamp);
            party::status::SetLastEvent(c.who + " is choosing travel");
            // P4: the same scripted warp asks on two games: the lower party slot (the host first)
            // keeps its question, this one closes.
            const int mine = link ? link->local_slot() : -1;
            Pending q;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                q = g_p;
            }
            if (!q.mark && deathrules::DropSameWarpQuestion(q.state == Ask::Queued || q.state == Ask::Open, q.id, mine,
                                                            c.lamp, c.slot)) {
                const bool up = q.state == Ask::Open && Field<int>(Slot(kSlotMenuMan), 0x78, 0) != 0;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    g_p = Pending{};
                }
                if (up) {
                    runtime_pad_director_press(kPadCircle);
                    release_at = Now() + 0.25;
                }
                Log("lamp %u: %s asks the same question (slot %d < %d): this one is dropped%s (P4)", c.lamp,
                    c.who.c_str(), c.slot, mine, up ? ", dialog cancelled" : "");
            }
        }
        if (const u32 lamp = g_notify_lamp.exchange(0); lamp && link) {
            char body[48];
            std::snprintf(body, sizeof body, "{\"lamp\":%u}", lamp);
            link->post_event(PartyDirector::Get().Role() == PartyRole::Host ? party::kBroadcast : 0, kChoosingEventName,
                             body);
        }
    }
    if (release_at > 0 && Now() >= release_at) {
        runtime_pad_director_press(0);  // the test press is a tap
        release_at = 0;
    }
    Pending p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        p = g_p;
    }
    if (p.state == Ask::None) {
        return;
    }
    const double now = Now();
    const auto finish = [&] {
        std::lock_guard<std::mutex> lk(g_mu);
        g_p = Pending{};
    };
    const int sr = SessionRole();
    if ((p.host && sr != kRoleHost) || (!p.host && sr != kRoleClient)) {
        // The session ended while asking: the plain warp, as without a party.
        Log("lamp %u: the session changed (role %d) before the choice; the plain warp runs", p.id, sr);
        finish();
        RunLampWarpDecided(p.id, false);
        return;
    }
    if (p.solo_load) {
        if (!s.world_up || s.loading) {
            return;
        }
        finish();
        Carry(p, true, "a lamp of the loaded map");
        return;
    }
    {
        // Playtest 1 bug 12: a question stayed open 120 s across the host's travel load and its
        // death. Closed now on a load, the player's death, or 30 s without an answer.
        const bool dead = s.player && Field<int>(Field<u64>(Field<u64>(s.player, 0x3b0), 0x20), 0xf8, 1) <= 0;
        const deathrules::AskClose c =
            deathrules::PlanAskClose(p.state == Ask::Open, s.loading || !s.world_up, dead, now - p.since);
        if (c != deathrules::AskClose::Keep) {
            finish();
            const bool up = p.state == Ask::Open && Field<int>(Slot(kSlotMenuMan), 0x78, 0) != 0;
            if (up && c != deathrules::AskClose::Load) {
                runtime_pad_director_press(kPadCircle);  // the dialog's own cancel: it closes
                release_at = now + 0.25;
            }
            Log("lamp %u: the question is closed (%s)%s; nothing runs", p.id, deathrules::AskCloseName(c),
                up && c != deathrules::AskClose::Load ? ", dialog cancelled" : "");
            return;
        }
    }
    if (CurrentMode() == Mode::Alone) {
        finish();
        Carry(p, true, "BB_PARTY_TRAVEL_CHOICE=alone");
        return;
    }
    if (SceneRunning()) {
        g_remo_seen_at = now;
    }
    if (p.state == Ask::Queued && p.after_scene &&
        (now - p.since < deathrules::kAskAfterSceneDelay ||
         (g_remo_seen_at >= 0 && now - g_remo_seen_at < deathrules::kAskAfterSceneDelay))) {
        static double logged = -1;
        if (logged != p.since) {
            logged = p.since;
            Log("lamp %u: a story warp; the question waits until its scene is over (+%.1f s)", p.id,
                deathrules::kAskAfterSceneDelay);
        }
        return;
    }
    if (p.state == Ask::Queued) {
        const u64 dlg = OpenQuestion();
        if (!dlg) {
            if (now - p.since > 5.0) {
                finish();
                Carry(p, false, "no dialog could open in 5 s");
            }
            return;
        }
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_asked;
        g_p.state = Ask::Open;
        g_p.dialog = dlg;
        g_p.since = now;
        Log("lamp %u: asking \"Travel with the party?\" (dialog %#llx)", p.id, static_cast<unsigned long long>(dlg));
        g_notify_lamp = p.id;  // the others log who is choosing (EVENT split_choosing)
        return;
    }
    if (p.state == Ask::Open) {
        const int r = Field<int>(p.dialog, 0x18, -1);
        {
            // b6a (live): a question opened in the frames the area name of a new map was shown
            // ("Forsaken Cainhurst Castle", right after a story scene) never appeared: menu +0x78
            // stayed 0, three Cross presses did nothing and it closed after 30 s with no travel.
            // Not up 2.5 s after opening: open it again (3 times at most).
            const int up = Field<int>(Slot(kSlotMenuMan), 0x78, -1);
            if (r == -1 && up == 0 && now - p.since > 2.5 && p.reopens < 3) {
                std::lock_guard<std::mutex> lk(g_mu);
                g_p.state = Ask::Queued;
                g_p.reopens = p.reopens + 1;
                g_p.test_step = 0;
                g_p.since = now;
                --g_asked;  // the same question (BB_PARTY_SPLIT_TEST counts questions)
                Log("lamp %u: the question did not show (menu +0x78 0 after %.1f s); opening it again (%d)", p.id,
                    now - p.since, p.reopens + 1);
                return;
            }
        }
        // Test answer (BB_PARTY_SPLIT_TEST=yes|no): YES = Cross; NO = Right, then Cross.
        // BB_PARTY_SPLIT_TEST=no,yes,...: one answer per question in order (the last repeats).
        static const char* test = std::getenv("BB_PARTY_SPLIT_TEST");
        const int asked = g_asked;
        {
            static double last_dump = 0;
            if (p.test_step == 0 && now - last_dump > 5.0) {
                last_dump = now;
                std::string d;
                for (u64 o = 0; o < 0x40; o += 4) {
                    char b[16];
                    std::snprintf(b, sizeof b, " %x", Field<u32>(p.dialog, o));
                    d += b;
                }
                Log("dialog %#llx:%s", static_cast<unsigned long long>(p.dialog), d.c_str());
            }
        }
        std::string answer;
        if (test && test[0]) {
            std::string all = test;
            int k = 1;
            std::size_t pos = 0;
            while (k < asked && all.find(',', pos) != std::string::npos) {
                pos = all.find(',', pos) + 1;
                ++k;
            }
            answer = all.substr(pos, all.find(',', pos) == std::string::npos ? std::string::npos : all.find(',', pos) - pos);
        }
        if (!answer.empty() && r == -1 && now - p.since > 1.5 && now - p.test_at > 1.2 && p.test_step < 6) {
            const bool yes = answer == "yes";
            // The cursor starts on YES (live inworld1: Left + Cross and Right + Cross both gave NO).
            const u32 b = yes || p.test_step % 2 == 1 ? kPadCross : kPadRight;
            runtime_pad_director_press(b);
            release_at = now + 0.25;  // held long enough for a slow frame (a 0.12 s tap was missed live)
            Log("test: dialog answer %s: press %#x (step %d)", answer.c_str(), b, p.test_step);
            std::lock_guard<std::mutex> lk(g_mu);
            g_p.test_step = p.test_step + 1;
            g_p.test_at = now;
        }
        if (r == -1) {
            return;  // the 30 s timeout: above
        }
        Log("lamp %u: dialog result %d (menu +0x78 %d, +0x16c %d, +0x170 %d)", p.id, r,
            Field<int>(Slot(kSlotMenuMan), 0x78, -1), Field<int>(Slot(kSlotMenuMan), 0x16c, -1),
            Field<int>(Slot(kSlotMenuMan), 0x170, -1));
        finish();
        // GetGenericDialogButtonResult (talk condition at 0x203BB1C): [6] when 0, 1 or 2.
        if (r == 1) {
            Carry(p, false, "YES");
        } else if (r == 2) {
            Carry(p, true, "NO");
        } else {
            Log("lamp %u: the question was closed (result %d); no travel", p.id, r);
        }
    }
}

std::uint64_t AskYesNo(std::uint32_t text) {
    return text ? OpenQuestion(text) : 0;
}

int QuestionResult(std::uint64_t dialog) {
    return dialog ? Field<int>(dialog, 0x18, -1) : -1;
}

bool DialogUp() {
    return Field<int>(Slot(kSlotMenuMan), 0x78, -1) != 0;
}

bool ShowMessage(std::uint32_t text) {
    return OpenMessage(text) != 0;
}

} // namespace coop::split
