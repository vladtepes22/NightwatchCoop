#include "../engine/engine_hooks.h"
#include "party_npctalk.h"
// SPDX-License-Identifier: GPL-3.0-or-later
// Direct lamp travel (see lamp_travel.h; ids and rules in lamp_travel_rules.h).
#include "lamp_travel.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "lamp_travel_rules.h"
#include "mirror_frame.h"
#include "party_async_io.h"
#include "party_balance.h"
#include "party_director.h"
#include "party_ids.h"
#include "party_split.h"
#include "seamless_rules.h"
#include "world_warper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop::lamptravel {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

constexpr u64 kTalkCommand = 0x203ffd0;   // talk command switch (ctx, call, ...)
constexpr u64 kSlotMenuMan = 0x5562878;   // +0xEC0 warp menu result, +0xD08 generic dialog
constexpr u64 kMenuFlags = 0x176b7e0;     // u32 (): open-menu bits; OpenWarpMenu needs & mask == 0
constexpr u32 kMenuMask = 0xeffffb33u;
constexpr u64 kLampWarp = 0x13cdf30;      // void (u32 ReturnPointParam id): lamp / headstone warp
constexpr u64 kFlipperUpdate = 0x2034770; // main-thread tick (as party_director)
constexpr u64 kMapObject = 0x13c9af0;     // (entity, u32* index) -> map object record (+0x240 3x4 rows)
constexpr u32 kPadCross = 0x4000, kPadDown = 0x40, kPadRight = 0x20, kPadLeft = 0x80;


void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Lamp travel: ") + line + "\n");
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

// ---- The talk command call object (the switch's rsi): vtable +0x18 id, +0x20 count (the id
// included), +0x28 get(i) -> {value, pad, i32 type}: type 1 a float (the switch truncates it), type
// 2 an int (the ESD's integer constants: flags, text ids; live: the lamp's 10010713 came as type 2).
struct Value {
    i32 v = 0, pad = 0, type = 2, pad2 = 0;
};
struct FakeCall {
    const void* const* vtbl;
    u32 id;
    int count;
    Value vals[6];
};
BB_COOP_SYSV u64 FcZero(FakeCall*) { return 0; }
BB_COOP_SYSV u64 FcId(FakeCall* c) { return c->id; }
BB_COOP_SYSV u64 FcCount(FakeCall* c) { return static_cast<u64>(c->count); }
BB_COOP_SYSV const Value* FcGet(FakeCall* c, int i) { return &c->vals[std::clamp(i, 0, 5)]; }
const void* const kFakeVtbl[16] = {
    reinterpret_cast<const void*>(&FcZero),  reinterpret_cast<const void*>(&FcZero),
    reinterpret_cast<const void*>(&FcZero),  reinterpret_cast<const void*>(&FcId),
    reinterpret_cast<const void*>(&FcCount), reinterpret_cast<const void*>(&FcGet),
    reinterpret_cast<const void*>(&FcZero),  reinterpret_cast<const void*>(&FcZero),
    reinterpret_cast<const void*>(&FcZero),  reinterpret_cast<const void*>(&FcZero),
    reinterpret_cast<const void*>(&FcZero),  reinterpret_cast<const void*>(&FcZero),
    reinterpret_cast<const void*>(&FcZero),  reinterpret_cast<const void*>(&FcZero),
    reinterpret_cast<const void*>(&FcZero),  reinterpret_cast<const void*>(&FcZero),
};
FakeCall MakeCall(u32 id, std::initializer_list<i32> args) {
    FakeCall c{};
    c.vtbl = kFakeVtbl;
    c.id = id;
    c.count = 1 + static_cast<int>(args.size());
    c.vals[0].v = static_cast<i32>(id);
    int i = 1;
    for (i32 a : args) {
        if (i < 6) c.vals[i++].v = a;
    }
    return c;
}

using CmdFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);
void* g_cmd_orig = nullptr;
std::atomic<bool> g_on{false};
bool g_own_tick = false;  // our own main-thread tick hook is in (solo)
std::atomic<bool> g_inside{false};
std::atomic<u32> g_cmd_counts[96];  // talk commands seen per id (the test's log)
std::atomic<bool> g_t_on{false};     // BB_LAMP_TRAVEL_TEST set

struct Pending {
    bool open = false;
    std::vector<Entry> list;
    u32 here = 0;
    float at[3] = {};      // where the player stood (walking away ends the wait)
    double opened = 0;
    u32 chosen = 0;        // return point picked; the warp runs once the menu has closed
    double chosen_at = 0;
};
std::mutex g_mu;
Pending g_p;  // under g_mu (talk hook: game thread; Tick: main thread)

struct Arrival {
    u32 id = 0;
    double at = 0;
    bool load_seen = false;
} g_arrival;  // main thread

u32 MenuFlags() {
    return reinterpret_cast<u32(BB_COOP_SYSV*)()>(Guest(kMenuFlags))();
}

bool PlayerPos(u64 player, float out[3]) {
    float p[4] = {};
    if (!player || !SafeRead(player + 0xc0, p, sizeof p)) return false;
    out[0] = p[0], out[1] = p[1], out[2] = p[2];
    return true;
}

std::vector<Spot> LampSpots() {
    std::vector<Spot> v;
    for (const Lamp& l : kLamps) {
        float sp[4];
        if (ids::ReturnSpot(ReturnPoint(l), sp)) v.push_back({ReturnPoint(l), sp[0], sp[1], sp[2]});
    }
    return v;
}

/// The lamp the player stands at (its return point; 0 unknown).
u32 HereLamp(const GameSnapshot& s) {
    float me[3];
    if (!PlayerPos(s.player, me)) return 0;
    static const std::vector<Spot> spots = LampSpots();
    return NearestLamp(spots, s.map_id, me, 15.0f);
}

bool Flag(u32 id) {
    bool v = false;
    return ReadEventFlag(id, &v) && v;
}

const char* GroupName(std::uint8_t g) {
    switch (g) {
    case Dream: return "Dream";
    case Yharnam: return "Yharnam";
    case Frontier: return "Frontier";
    case Unseen: return "Unseen";
    case Nightmare: return "Nightmare";
    case HuntersNightmare: return "Hunter's Nightmare";
    }
    return "?";
}

/// Game thread, inside the lamp script's Dream confirmation: opens the warp menu instead. False =
/// the vanilla question runs.
bool TryOpen(u64 orig_ctx, u64 a2, u64 a3, u64 a4, u64 a5) {
    const GameSnapshot s = ReadGameState(false);
    Situation sit;
    sit.world_up = s.world_up;
    sit.loading = s.loading;
    sit.cutscene = CutscenePlaying();
    sit.boss = BossBarShown();
    sit.map = s.map_id;
    if (!Offer(sit)) {
        Log("the lamp's Dream question stays (%s)", sit.cutscene ? "cutscene" : sit.boss ? "boss fight" : "not in the world");
        return false;
    }
    const u32 here = HereLamp(s);
    if (!here) {
        Log("the lamp's Dream question stays: no lamp of the table within 15 m (map %s)", MapName(s.map_id).c_str());
        return false;
    }
    std::vector<Entry> list = BuildList(here, &Flag, [](u32 rp) { return ids::ReturnPointKnown(rp); });
    if (list.empty()) {
        Log("lamp %u: no other lit lamp; the vanilla Dream question", here);
        return false;
    }
    const u32 flags = MenuFlags();
    if (flags & kMenuMask) {
        Log("lamp %u: a menu is up (0x%x); the vanilla Dream question", here, flags);
        return false;
    }
    const u64 mm = Slot(kSlotMenuMan);
    if (!mm) return false;
    const i32 zero = 0, minus = -1;
    SafeWrite(mm + 0xec0, &zero, 4);
    const auto run = [&](FakeCall c) {
        reinterpret_cast<CmdFn>(g_cmd_orig)(orig_ctx, reinterpret_cast<u64>(&c), a2, a3, a4, a5);
    };
    g_inside = true;
    run(MakeCall(kCmdClearWarpList, {}));
    for (const Entry& e : list) {
        run(MakeCall(kCmdAddWarpEntry, {static_cast<i32>(e.index), static_cast<i32>(e.name_flag), static_cast<i32>(e.lit_flag)}));
    }
    // The game's list (MenuMan +0xEB8: {?, begin, end}, 0x40-byte entries, +4 the index): every
    // entry must be there before the menu opens (an empty warp menu faults on Cross, as the
    // guest headstone did before the lamp flag mirror).
    const u64 vec = Field<u64>(mm, 0xeb8);
    const u64 begin = Field<u64>(vec, 8), end = Field<u64>(vec, 0x10);
    const std::size_t have = vec && end >= begin ? static_cast<std::size_t>((end - begin) / 0x40) : 0;
    const i32 first_index = Field<i32>(begin, 4, -1);
    if (have != list.size()) {
        run(MakeCall(kCmdClearWarpList, {}));
        g_inside = false;
        Log("lamp %u: the game took %zu of %zu warp entries (first index %d); the vanilla Dream question", here, have,
            list.size(), first_index);
        return false;
    }
    // OpenWarpMenu with the lamp's own talk (as the headstones' scripts): the talk keeps the menu's
    // handle, and the command records the lamp as the last lamp (its chr id + 1000, a rest here).
    run(MakeCall(kCmdOpenWarpMenu, {}));
    g_inside = false;
    balance::NoteLampMenuOpened();  // the lamp's menu opened: a rest for the rot (#21), as the Dream question
    // The script's next test reads the generic dialog's result: none (-1), so it takes its CANCEL
    // path instead of a stale YES that would start the Dream trip.
    if (const u64 dlg = Field<u64>(mm, 0xd08)) SafeWrite(dlg + 0x18, &minus, 4);
    std::string names;
    for (const Entry& e : list) {
        char b[48];
        std::snprintf(b, sizeof b, "%s%u:%u(%s)", names.empty() ? "" : " ", e.index, e.return_point, GroupName(e.group));
        names += b;
    }
    const std::size_t n = list.size();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_p = Pending{};
        g_p.open = true;
        g_p.list = std::move(list);
        g_p.here = here;
        PlayerPos(s.player, g_p.at);
        g_p.opened = Now();
    }
    Log("lamp %u (%s): the warp menu instead of the Dream question; %zu entries: %s (menu bits now 0x%x)", here,
        MapName(s.map_id).c_str(), n, names.c_str(), MenuFlags());
    return true;
}

__attribute__((noinline)) BB_COOP_SYSV u64 TalkCommandHook(u64 ctx, u64 call, u64 a2, u64 a3, u64 a4, u64 a5) {
    if (PartyRequested()) npctalk::ObserveCommand(ctx,call);
    const auto orig = reinterpret_cast<CmdFn>(g_cmd_orig);
    if (!g_on.load(std::memory_order_relaxed) || g_inside.load(std::memory_order_relaxed) || !call) {
        return orig(ctx, call, a2, a3, a4, a5);
    }
    u64 vtbl = 0, fid = 0, fcount = 0, fget = 0;
    if (!SafeGet(call, &vtbl) || !vtbl || !SafeGet(vtbl + 0x18, &fid) || !fid) {
        return orig(ctx, call, a2, a3, a4, a5);
    }
    const u32 id = static_cast<u32>(reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(fid)(call));
    if (id < 96 && g_t_on.load(std::memory_order_relaxed)) g_cmd_counts[id].fetch_add(1, std::memory_order_relaxed);
    if (id != kCmdGenericDialog || !SafeGet(vtbl + 0x20, &fcount) || !SafeGet(vtbl + 0x28, &fget) || !fcount || !fget) {
        return orig(ctx, call, a2, a3, a4, a5);
    }
    const int count = static_cast<int>(reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(fcount)(call)) - 1;
    i32 args[5] = {}, alt[5] = {}, raw[5] = {}, types[5] = {};
    if (count == 5) {
        for (int i = 0; i < 5; ++i) {
            const u64 v = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, int)>(fget)(call, i + 1);
            i32 val = 0, type = 0;
            if (!v || !SafeGet(v, &val) || !SafeGet(v + 8, &type)) break;
            raw[i] = val;
            types[i] = type;
            float f = 0;
            std::memcpy(&f, &val, 4);
            // Type 1 a float, 2 an int (the switch's own reads); the other view as a fallback.
            const i32 as_float = std::isfinite(f) && std::fabs(f) < 2.0e9f ? static_cast<i32>(f) : val;
            args[i] = type == 1 ? as_float : val;
            alt[i] = type == 1 ? val : as_float;
        }
    }
    if ((IsDreamConfirm(id, count, args) || IsDreamConfirm(id, count, alt)) && TryOpen(ctx, a2, a3, a4, a5)) {
        return 0;
    }
    if (g_t_on.load(std::memory_order_relaxed)) {
        // Test diagnostics: each distinct generic dialog once (at most 32), with its raw arguments.
        static std::mutex mu;
        static std::vector<std::uint64_t> seen;
        const std::uint64_t key = static_cast<std::uint64_t>(static_cast<u32>(raw[1])) << 8 ^ static_cast<u32>(count) ^
                                  static_cast<std::uint64_t>(static_cast<u32>(raw[0])) << 40;
        std::lock_guard<std::mutex> lk(mu);
        if (seen.size() < 32 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            Log("talk dialog command: %d args: %d/%d %d/%d %d/%d %d/%d %d/%d (value/type)%s", count, raw[0], types[0], raw[1],
                types[1], raw[2], types[2], raw[3], types[3], raw[4], types[4],
                IsDreamConfirm(id, count, args) || IsDreamConfirm(id, count, alt) ? ": the lamp's Dream question" : "");
        }
    }
    return orig(ctx, call, a2, a3, a4, a5);
}

void Act(u32 here, u32 id) {
    if (id == kDreamReturnPoint) {
        // The lamp's own YES: its common event 7200 (turn, light, warp) - the vanilla rest trip.
        const u32 flag = DreamFlag(here);
        const bool ok = flag && WriteEventFlag(flag, true);
        Log("chose the Hunter's Dream: lamp %u's Dream flag %u %s", here, flag, ok ? "set (the lamp's own warp)" : "NOT set");
        if (!ok) {
            reinterpret_cast<void(BB_COOP_SYSV*)(u32)>(Guest(kLampWarp))(id);
        }
    } else {
        Log("chose lamp %u (%s): lamp warp 0x13CDF30(%u) from lamp %u", id, MapName(MapOf(id)).c_str(), id, here);
        // In a party: this player's own lamp use (no lamp flag is set for it), so the split
        // question asks here and nowhere else (party_split PlanLampOrigin).
        split::NoteLocalUse(id);
        reinterpret_cast<void(BB_COOP_SYSV*)(u32)>(Guest(kLampWarp))(id);
    }
    g_arrival = Arrival{id, Now(), false};
}

BB_COOP_SYSV void OwnTickEntry(u64, u64, u64, u64, u64, u64) {
    Tick();
}

// ---- Test driver (BB_LAMP_TRAVEL_TEST) ----

struct TestState {
    bool on = false, cont = false, members = false;
    bool any_title = false;
    bool dream = false;
    u32 walked = 0;        // the lamp walked to
    int rewalks = 0;
    bool start_any = false;  // startany: the start= warp also when the save is in that map  // dream: Cross, Left (YES), Cross at the lamp; done when a load starts (#22)  // titleany: also in a party process whose host never answers (#22)
    u32 lamp = 0;          // lamp=ID (0: the nearest lit one)
    u32 to = 0;            // to=ID (0: "other": the first lamp of another map)
    u32 start = 0;         // start=ID: first a plain lamp warp there when the save is in another map
    bool started = false;
    enum Stage { Title, Settle, Walk, Press, Choose, Wait, Done } stage = Title;
    double at = 0, steady_since = 0, release_at = 0, members_at = 0;
    int presses = 0, downs = 0, want_downs = -1, confirms = 0;
    u32 target = 0;
} g_t;

void TestParse() {
    const char* v = std::getenv("BB_LAMP_TRAVEL_TEST");
    if (!v || !v[0]) return;
    g_t.on = true;
    g_t_on = true;
    std::string s = v;
    for (char& c : s) {
        if (c == ';') c = ',';
    }
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item == "continue") g_t.cont = true;
        else if (item == "titleany") g_t.cont = g_t.any_title = true;
        else if (item == "dream") g_t.dream = true;
        else if (item == "startany") g_t.start_any = true;
        else if (item == "members") g_t.members = true;
        else if (item.rfind("lamp=", 0) == 0) g_t.lamp = static_cast<u32>(std::strtoul(item.c_str() + 5, nullptr, 10));
        else if (item.rfind("start=", 0) == 0) g_t.start = static_cast<u32>(std::strtoul(item.c_str() + 6, nullptr, 10));
        else if (item.rfind("to=", 0) == 0) g_t.to = item == "to=other" ? 0 : static_cast<u32>(std::strtoul(item.c_str() + 3, nullptr, 10));
    }
    Log("test: %s", v);
}

void Press(u32 buttons, double now) {
    runtime_pad_director_press(buttons);
    g_t.release_at = now + 0.12;
}

/// Stands the player 1 m in front of lamp `rp`'s object, facing it (test aid, as guestsvc's lampwalk).
bool WalkTo(const GameSnapshot& s, u32 rp) {
    u32 idx = 0xffffffffu;
    const u64 rec = reinterpret_cast<u64(BB_COOP_SYSV*)(i32, u32*)>(Guest(kMapObject))(static_cast<i32>(rp - 1000), &idx);
    float m[12] = {}, me[3] = {};
    if (!rec || !SafeRead(rec + 0x240, m, sizeof m) || !PlayerPos(s.player, me)) return false;
    const float t[3] = {m[3], m[7], m[11]};
    float dx = me[0] - t[0], dz = me[2] - t[2];
    const float h = std::sqrt(dx * dx + dz * dz);
    if (h < 0.01f) dx = 0, dz = 1;
    else dx /= h, dz /= h;
    alignas(16) float pos[4] = {t[0] + dx * 1.0f, t[1], t[2] + dz * 1.0f, 1.0f};
    const float a = std::atan2(t[0] - pos[0], t[2] - pos[2]);
    alignas(16) float rot[4] = {0, a + 3.14159265f, 0, 0};
    u64 vtbl = 0, fn = 0;
    if (!SafeGet(s.player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) return false;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, const float*, const float*, u64)>(fn)(s.player, pos, rot, 0);
    Log("test: standing 1 m in front of lamp %u (object %d at %.1f, %.1f, %.1f; was %.1f m away)", rp,
        static_cast<int>(rp - 1000), t[0], t[1], t[2], std::sqrt(h * h + (me[1] - t[1]) * (me[1] - t[1])));
    return true;
}

void TestTick(const GameSnapshot& s, double now) {
    if (!g_t.on) return;
    if (g_t.release_at > 0 && now >= g_t.release_at) {
        runtime_pad_director_press(0);
        g_t.release_at = 0;
    }
    if (g_t.release_at > 0) return;
    const bool steady_world = s.world_up && !s.loading;
    switch (g_t.stage) {
    case TestState::Title:
        if (g_t.started && now < g_t.at) return;
        if (steady_world) {
            g_t.stage = TestState::Settle;
            g_t.steady_since = now;
            Log("test: in the world (%s)", MapName(s.map_id).c_str());
            return;
        }
        if (g_t.cont && (!PartyRequested() || g_t.any_title) && now - g_t.at >= (g_t.any_title ? 1.2 : 2.5)) {
            // titleany: a Join-a-party guest with no host stays on "Failed to acquire server
            // information" (auto-continue waits for the link): Right (CANCEL), Cross, alternately.
            g_t.at = now;
            const bool right = g_t.any_title && g_t.presses % 2 == 0;
            Press(right ? kPadRight : kPadCross, now);
            if (++g_t.presses <= 40) Log("test: title %s #%d", right ? "Right" : "Cross", g_t.presses);
        }
        return;
    case TestState::Settle:
        if (!steady_world) {
            g_t.steady_since = now;
            return;
        }
        if (now - g_t.steady_since < 3.0) return;
        if (g_t.start && !g_t.started && (g_t.start_any || MapOf(g_t.start) != (s.map_id & 0xffff0000u))) {
            g_t.started = true;
            Log("test: setup: the save is in %s; lamp warp to %u first", MapName(s.map_id).c_str(), g_t.start);
            split::NoteLocalUse(g_t.start);
            reinterpret_cast<void(BB_COOP_SYSV*)(u32)>(Guest(kLampWarp))(g_t.start);
            g_t.stage = TestState::Title;
            g_t.at = now + 4.0;  // the load starts first
            return;
        }
        if (g_t.members) {
            if (now - g_t.at >= 1.0) {
                g_t.at = now;
                const int coop = ReadGameState(true).cooperators;
                if (coop >= 1 && g_t.members_at == 0) {
                    g_t.members_at = now;
                    Log("test: %d cooperator(s) in the world", coop);
                }
            }
            if (g_t.members_at == 0 || now - g_t.members_at < 12.0) return;
        } else if (now - g_t.steady_since < 5.0) {
            return;
        }
        g_t.stage = TestState::Walk;
        return;
    case TestState::Walk: {
        u32 lamp = g_t.lamp;
        if (!lamp) {
            float me[3];
            if (!PlayerPos(s.player, me)) return;
            float best = 1e18f;
            for (const Lamp& l : kLamps) {
                float sp[4];
                const u32 rp = ReturnPoint(l);
                if (MapOf(rp) != (s.map_id & 0xffff0000u) || !Flag(LitFlag(l)) || !ids::ReturnSpot(rp, sp)) continue;
                const float d = (sp[0] - me[0]) * (sp[0] - me[0]) + (sp[2] - me[2]) * (sp[2] - me[2]);
                if (d < best) best = d, lamp = rp;
            }
        }
        g_t.walked = lamp;
        if (!lamp || !WalkTo(s, lamp)) {
            Log("test: FAILED: no lit lamp to stand at in %s", MapName(s.map_id).c_str());
            g_t.stage = TestState::Done;
            return;
        }
        g_t.stage = TestState::Press;
        g_t.at = now + 1.5;
        g_t.presses = 0;
        return;
    }
    case TestState::Press: {
        if (g_t.dream && g_t.presses > 0 && (s.loading || !s.world_up)) {
            Log("test: done: the lamp's travel loads after %d press(es)", g_t.presses);
            g_t.stage = TestState::Done;
            return;
        }
        if (now < g_t.at) return;
        if (g_t.dream) {
            // The vanilla lamp: "Return to the Hunter's Dream" (Cross), the box starts on NO (Left
            // = YES), Cross; or the travel list's first entry (the Dream) with the same keys.
            float me[3] = {};
            if (PlayerPos(s.player, me)) {
                u32 idx = 0xffffffffu;
                float m[12] = {};
                const u64 rec = reinterpret_cast<u64(BB_COOP_SYSV*)(i32, u32*)>(Guest(kMapObject))(
                    static_cast<i32>(g_t.walked - 1000), &idx);
                float d = -1.0f;
                if (rec && SafeRead(rec + 0x240, m, sizeof m)) {
                    d = std::sqrt((m[3] - me[0]) * (m[3] - me[0]) + (m[11] - me[2]) * (m[11] - me[2]));
                }
                Log("test: at (%.1f, %.1f, %.1f) in %s, %.1f m from lamp %u", me[0], me[1], me[2],
                    MapName(s.map_id).c_str(), d, g_t.walked);
                if (d > 3.0f && g_t.rewalks < 4 && g_t.presses == 0) {
                    ++g_t.rewalks;
                    WalkTo(s, g_t.walked);  // the first walk did not take (the world still settling)
                    g_t.at = now + 1.5;
                    return;
                }
            }
            if (++g_t.presses > 9) {
                Log("test: FAILED: no load after 9 presses at the lamp");
                g_t.stage = TestState::Done;
                return;
            }
            const bool left = g_t.presses % 3 == 2;
            Log("test: %s at the lamp #%d", left ? "Left" : "Cross", g_t.presses);
            Press(left ? kPadLeft : kPadCross, now);
            g_t.at = now + 2.0;
            return;
        }
        bool open = false;
        std::vector<Entry> list;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            open = g_p.open;
            list = g_p.list;
        }
        if (open) {
            u32 target = g_t.to;
            if (!target) {
                for (const Entry& e : list) {
                    if (e.return_point != kDreamReturnPoint && MapOf(e.return_point) != (s.map_id & 0xffff0000u)) {
                        target = e.return_point;
                        break;
                    }
                }
            }
            int pos = -1;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].return_point == target) pos = static_cast<int>(i);
            }
            if (pos < 0) {
                Log("test: FAILED: lamp %u is not in the warp list", target);
                g_t.stage = TestState::Done;
                return;
            }
            g_t.target = target;
            g_t.want_downs = pos;
            g_t.downs = 0;
            g_t.stage = TestState::Choose;
            g_t.at = now + 2.0;
            Log("test: warp menu up; choosing lamp %u (entry %d of %zu)", target, pos + 1, list.size());
            return;
        }
        if (++g_t.presses > 6) {
            Log("test: FAILED: no warp menu after 6 Cross presses at the lamp");
            g_t.stage = TestState::Done;
            return;
        }
        Log("test: Cross at the lamp #%d", g_t.presses);
        Press(kPadCross, now);
        g_t.at = now + 2.0;
        return;
    }
    case TestState::Choose:
        if (now < g_t.at) return;
        if (g_t.downs < g_t.want_downs) {
            ++g_t.downs;
            Log("test: Down #%d in the warp menu", g_t.downs);
            Press(kPadDown, now);
            g_t.at = now + 0.6;
            return;
        }
        if (g_arrival.id) {
            g_t.stage = TestState::Done;  // the arrival (Tick) moves on: Wait (party) or done
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_p.chosen) return;  // chosen: the warp follows
        }
        if (++g_t.confirms > 3) {
            Log("test: FAILED: no choice after 3 Cross presses in the warp menu");
            g_t.stage = TestState::Done;
            return;
        }
        Log("test: Cross in the warp menu #%d", g_t.confirms);
        Press(kPadCross, now);
        g_t.at = now + 2.5;
        return;
    case TestState::Wait: {
        // Party: the members followed (one load each) and are back in this world.
        if (!steady_world) return;
        static double last = 0;
        if (now - last < 1.0) return;
        last = now;
        const int coop = ReadGameState(true).cooperators;
        if (coop >= 1 && g_t.members_at == 0) g_t.members_at = now;
        if (coop < 1) g_t.members_at = 0;
        if (g_t.members_at > 0 && now - g_t.members_at >= 8.0) {
            Log("test: done: %d cooperator(s) in the world again, %.0f s after the arrival", coop, now - g_t.at);
            g_t.stage = TestState::Done;
        } else if (now - g_t.at > 150.0) {
            Log("test: FAILED: the party did not come back within 150 s (cooperators %d)", coop);
            g_t.stage = TestState::Done;
        }
        return;
    }
    case TestState::Done:
        return;
    }
}

} // namespace

bool ObservesTalkCommands() { return g_cmd_orig != 0; }

void Install(unsigned char* image, std::uint64_t size) {
    if (!SwitchOn(std::getenv("BB_LAMP_TRAVEL"))) {
        std::printf("Lamp travel: off (BB_LAMP_TRAVEL=0): lamps only return to the Hunter's Dream\n");
        return;
    }
    if (!HooksInit(image, size)) return;
    TestParse();
    // push rbp; mov rbp, rsp; push r15..r12, rbx; sub rsp, 0x3c8 (20 bytes; then mov rbx, rsi).
    // "observer" in the name: a local menu choice, not part of the party's hook set (A9): a
    // player with the switch off can still join one with it on.
    const bool ok = ReplacePrologue(kTalkCommand,
                                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                     0x81, 0xec, 0xc8, 0x03, 0x00, 0x00},
                                    reinterpret_cast<const void*>(&TalkCommandHook), &g_cmd_orig,
                                    "direct lamp travel (talk command observer)");
    const bool code_ok = MatchesOriginal(kMenuFlags, {0x55, 0x48, 0x89, 0xe5}) &&
                         MatchesOriginal(kLampWarp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53});
    g_on = ok && code_ok;
    if (g_on && !PartyRequested() && !BbEngine::FrameTickInstalled()) {
        // No party: no CoopTick; our own main-thread tick (the director's hook site).
        g_own_tick = HookPrologue(kFlipperUpdate,
                     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
                      0x38},
                     &OwnTickEntry, "lamp travel main-thread tick (observer)");
    }
    Log("%s: a lamp offers the headstones' travel list (the Hunter's Dream and every lit lamp)%s",
        g_on ? "on" : "NOT installed", g_on ? "" : " - the code differs from 1.09");
}

bool OwnsTick() {
    return g_own_tick;
}

void Tick() {
    warper::Tick();  // the world warper (solo; nothing while it is off)
    mirror::Tick();  // the Dream mirror's editor frame (solo and party)
    if (!g_on.load(std::memory_order_relaxed)) return;
    const double now = Now();
    const GameSnapshot s = ReadGameState(false);
    TestTick(s, now);
    if (g_t.on) {
        static double last = 0;
        if (now - last >= 5.0) {
            last = now;
            std::string ids;
            for (u32 i = 0; i < 96; ++i) {
                if (const u32 n = g_cmd_counts[i].exchange(0)) {
                    char b[24];
                    std::snprintf(b, sizeof b, " 0x%x:%u", i, n);
                    ids += b;
                }
            }
            if (!ids.empty()) Log("test: talk commands in 5 s:%s", ids.c_str());
        }
    }
    if (g_arrival.id) {
        if (s.loading || !s.world_up) {
            g_arrival.load_seen = true;
        } else if (g_arrival.load_seen && now - g_arrival.at > 3.0) {
            static double steady = 0;
            if (steady == 0) steady = now;
            if (now - steady >= 3.0) {
                float sp[4] = {}, me[3] = {};
                const bool spot = ids::ReturnSpot(g_arrival.id, sp) && PlayerPos(s.player, me);
                const float d = spot ? std::sqrt((sp[0] - me[0]) * (sp[0] - me[0]) + (sp[1] - me[1]) * (sp[1] - me[1]) +
                                                 (sp[2] - me[2]) * (sp[2] - me[2]))
                                     : -1.0f;
                const bool party_wait = g_t.on && g_t.members;
                Log("arrived: lamp %u, map %s, %.1f m from its spot%s", g_arrival.id, MapName(s.map_id).c_str(), d,
                    !g_t.on ? "" : party_wait ? " (test: waiting for the party)" : " (test: done)");
                g_arrival = Arrival{};
                steady = 0;
                if (g_t.on) {
                    g_t.stage = party_wait ? TestState::Wait : TestState::Done;
                    g_t.at = now;
                    g_t.members_at = 0;
                }
            }
        } else if (!g_arrival.load_seen && now - g_arrival.at > 60.0) {
            Log("no load within 60 s after the warp to %u (held or refused: see the party log)", g_arrival.id);
            g_arrival = Arrival{};
        }
    }
    u32 act_here = 0, act_id = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_p.open) return;
        if (s.loading || !s.world_up) {
            Log("warp menu: a load came first; dropped");
            g_p = Pending{};
            return;
        }
        const u64 mm = Slot(kSlotMenuMan);
        // The menu writes the chosen entry's index on OK (-1 while open / on Return).
        const i32 r = Field<i32>(mm, 0xec0, 0);
        if (!g_p.chosen && r > 0) {
            const i32 zero = 0;
            SafeWrite(mm + 0xec0, &zero, 4);
            g_p.chosen = Chosen(g_p.list, static_cast<u32>(r));
            g_p.chosen_at = now;
            Log("warp menu: entry %d chosen -> lamp %u", r, g_p.chosen);
            if (!g_p.chosen) {
                g_p = Pending{};
                return;
            }
        }
        float me[3] = {};
        const bool away = PlayerPos(s.player, me) &&
                          (me[0] - g_p.at[0]) * (me[0] - g_p.at[0]) + (me[2] - g_p.at[2]) * (me[2] - g_p.at[2]) > 25.0f;
        if (!g_p.chosen && (away || now - g_p.opened > 900.0)) {
            Log("warp menu: no choice (%s)", away ? "the player left the lamp" : "15 minutes");
            g_p = Pending{};
            return;
        }
        if (g_p.chosen && now - g_p.chosen_at >= 0.5) {
            act_here = g_p.here;
            act_id = g_p.chosen;
            g_p = Pending{};
        }
    }
    if (act_id) {
        Act(act_here, act_id);
    }
}

} // namespace coop::lamptravel
