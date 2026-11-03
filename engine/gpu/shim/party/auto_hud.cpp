// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-hide HUD, the game side: see auto_hud.h. RE (1.09, our offsets):
//   0x1BF41C0 (view, movie) builds the in-game HUD view (the "IngameTop" movie's top-level clips); the clip
//     "PlayerHUD" (HP / stamina gauges, the item slots, bullets, vials, the echoes / Insight counters) is the
//     component at view[0xc] (built by 0x1BF34E0). The boss bar ("BossTop"), the lock-on cursor, the prompts,
//     subtitles and item notices are other clips of the view: untouched.
//   The component is a Scaleform value wrapper (vtable 0x5399F90): +0x38 the object interface, +0x40 the value
//     type (& 0x8f == 10: a display object), +0x48 the display object. 0x1ACB760 (value, visible) shows the
//     interface's use: GetDisplayInfo is vtable +0xE0, SetDisplayInfo +0xE8 (pdata, DisplayInfo*). We set only
//     the alpha (flag 0x20) so nothing else of the clip changes.
#include "auto_hud.h"

#include "auto_hud_rules.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_balance.h"
#include "party_hud.h"
#include "../bbport_settings.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::autohud {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kViewCtor = 0x1bf41c0;      // (view, movie value)
constexpr u64 kCompVtable = 0x5399f90;    // the value wrapper's vtable
constexpr u64 kWorldChrMan = 0x553e878;   // +0x60 the local player
constexpr u64 kGameDataMan = 0x553b130;   // +8 the PlayerGameData
constexpr u64 kMenuMan = 0x5562878, kMenuFlow = 0x5562870, kMenuNewSystem = 0x5562882;
constexpr u64 kViewPlayerHud = 0x60;      // view[0xc]
constexpr u64 kIface = 0x38, kType = 0x40, kPdata = 0x48;
constexpr u64 kInfoGet = 0xe0, kInfoSet = 0xe8;
constexpr std::size_t kInfoSize = 0x100;
constexpr std::size_t kAlphaOff = 0x28;   // DisplayInfo.Alpha (double, 0..100); checked live (the log line)
constexpr std::size_t kFlagsOff = 0xd4;   // DisplayInfo.VarsSet (u16)
constexpr std::uint16_t kFlagAlpha = 0x20;

std::atomic<u64> g_view{0};
Machine g_machine;
bool g_on = false;
bool g_applied_any = false;
double g_world_since = -1;
std::string g_test;
std::vector<std::pair<double, std::string>> g_script;
std::size_t g_script_at = 0;

double Now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Auto HUD: ") + line + "\n");
}

u64 Rd(u64 a) {
    u64 v = 0;
    return SafeGet(a, &v) ? v : 0;
}
template <class T>
T Fld(u64 base, u64 off, T dflt) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : dflt;
}

BB_COOP_SYSV void ViewCtorEntry(u64 view, u64, u64, u64, u64, u64) {
    g_view.store(view, std::memory_order_release);
}

bool Enabled() {
    static const char* env = std::getenv("BB_AUTO_HUD");
    if (env && *env) return env[0] != '0';
    return BbSettings::Get().auto_hide_hud.load(std::memory_order_relaxed);
}

/// The PlayerHUD component, validated; 0 when the view is gone or not built yet.
u64 Component() {
    const u64 view = g_view.load(std::memory_order_acquire);
    if (!view) return 0;
    const u64 comp = Rd(view + kViewPlayerHud);
    if (!comp || Rd(comp) != Guest(kCompVtable)) return 0;
    if (!Rd(comp + kIface) || (Fld<u32>(comp, kType, 0) & 0x8f) != 10 || !Rd(comp + kPdata)) return 0;
    return comp;
}

void InitInfo(u8* info) {
    // As 0x1ACB760 prepares it before the get (3D defaults), then the get fills the rest.
    std::memset(info, 0, kInfoSize);
    const float one = 1.0f;
    for (std::size_t o : {0x60u, 0x7cu, 0x88u, 0x90u, 0xacu, 0xb8u, 0xccu}) std::memcpy(info + o, &one, 4);
}

/// Sets the clip's alpha (0..1); false when the interface refuses. `read_back`: the alpha as read first.
bool SetAlpha(u64 comp, double a01, double* read_back) {
    const u64 iface = Rd(comp + kIface), pdata = Rd(comp + kPdata);
    const u64 get = Rd(Rd(iface) + kInfoGet), set = Rd(Rd(iface) + kInfoSet);
    if (!iface || !pdata || !get || !set) return false;
    alignas(16) u8 info[kInfoSize];
    InitInfo(info);
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, void*);
    reinterpret_cast<Fn>(get)(iface, pdata, info);
    double alpha = 0;
    std::memcpy(&alpha, info + kAlphaOff, 8);
    if (read_back) *read_back = alpha;
    const double want = a01 * 100.0;
    std::memcpy(info + kAlphaOff, &want, 8);
    std::memcpy(info + kFlagsOff, &kFlagAlpha, 2);
    reinterpret_cast<Fn>(set)(iface, pdata, info);
    return true;
}

bool MenuUp() {
    const u64 mm = Rd(Guest(kMenuMan));
    int f[party::hud::kFullscreenMenuFields] = {};
    for (int i = 0; i < party::hud::kFullscreenMenuFields; ++i)
        f[i] = Fld<int>(mm, party::hud::kFullscreenMenuOffsets[i], 0);
    u8 ns = 0;
    u64 flow = 0;
    if (SafeGet(Guest(kMenuNewSystem), &ns) && ns) flow = Fld<u64>(Rd(Guest(kMenuFlow)), 0xa88, 0);
    return mm && (party::hud::GameMenuUp(flow != 0, f, -1) || Fld<int>(mm, 0x78, 0) != 0);
}

bool ReadInputs(Inputs* in) {
    const u64 player = Rd(Rd(Guest(kWorldChrMan)) + 0x60);
    const u64 rec = Rd(Rd(Guest(kGameDataMan)) + 8);
    if (!player || !rec) return false;
    const u64 dm = Rd(Rd(player + 0x3b0) + 0x20);
    if (!dm) return false;
    in->hp = Fld<int>(dm, 0xf8, -1);
    in->max_hp = Fld<int>(dm, 0xfc, -1);
    in->stamina = Fld<int>(dm, 0x134, -1);
    in->max_stamina = Fld<int>(dm, 0x138, -1);
    if (in->max_hp <= 0) return false;
    in->insight = Fld<int>(rec, 0x84, 0);
    in->echoes = Fld<int>(rec, 0x94, 0);
    in->lock_on = Fld<u8>(player, 0xf8, 0) != 0;
    // Equipment: the slot handles and ids, the quick items and the selected one.
    u8 buf[0x80 + 0x80 + 0x34] = {};
    SafeRead(rec + 0x264, buf, 0x80);
    SafeRead(rec + 0x2a4, buf + 0x80, 0x80);
    SafeRead(rec + 0x3ec, buf + 0x100, 0x34);
    in->equip_hash = Hash(buf, sizeof buf);
    in->menu_open = MenuUp();
    // A living enemy within kEnemyRadiusM (any: the game's aggro state is not read; a sleeping one counts).
    float me[3] = {};
    in->enemy_near = false;
    std::uint32_t near_entity = 0;
    float near_d = 0;
    if (balance::ChrPosition(player, me)) {
        for (const auto& e : balance::LoadedEnemies()) {
            float at[3] = {};
            if (e.entity / 100000 == 21) continue;  // the Hunter's Dream (m21): its practice dummy is no threat
            if (balance::EnemyHp(e.chr) <= 0 || !balance::ChrPosition(e.chr, at)) continue;
            const float dx = at[0] - me[0], dy = at[1] - me[1], dz = at[2] - me[2];
            if (dx * dx + dy * dy + dz * dz <= float(kEnemyRadiusM * kEnemyRadiusM)) {
                in->enemy_near = true;
                near_entity = e.entity;
                near_d = std::sqrt(dx * dx + dy * dy + dz * dz);
                break;
            }
        }
    }
    static bool last_near = false;
    if (in->enemy_near != last_near) {
        last_near = in->enemy_near;
        if (last_near) Log("an enemy is near: entity %u, %.1f m", near_entity, double(near_d));
        else Log("no enemy near");
    }
    return true;
}

void ParseScript() {
    // BB_AUTO_HUD_TEST="S:hp=40,S:hp=100,S:stam=20,S:echoes=+500,S:peek": S seconds after the world is up.
    const char* e = std::getenv("BB_AUTO_HUD_TEST");
    if (!e || !*e) return;
    g_test = e;
    std::size_t i = 0;
    while (i < g_test.size()) {
        std::size_t j = g_test.find(',', i);
        if (j == std::string::npos) j = g_test.size();
        const std::string item = g_test.substr(i, j - i);
        const std::size_t c = item.find(':');
        if (c != std::string::npos) g_script.emplace_back(std::atof(item.c_str()), item.substr(c + 1));
        i = j + 1;
    }
}

void RunScript(double since) {
    while (g_script_at < g_script.size() && since >= g_script[g_script_at].first) {
        const std::string a = g_script[g_script_at++].second;
        const u64 player = Rd(Rd(Guest(kWorldChrMan)) + 0x60);
        const u64 dm = Rd(Rd(player + 0x3b0) + 0x20);
        if (!dm) continue;
        if (a.rfind("hp=", 0) == 0) {  // own HP to N % of the max
            const int v = Fld<int>(dm, 0xfc, 0) * std::atoi(a.c_str() + 3) / 100;
            SafeWrite(dm + 0xf8, &v, 4);
        } else if (a.rfind("stam=", 0) == 0) {
            const int v = Fld<int>(dm, 0x138, 0) * std::atoi(a.c_str() + 5) / 100;
            SafeWrite(dm + 0x134, &v, 4);
        } else if (a.rfind("echoes=+", 0) == 0) {
            const u64 rec = Rd(Rd(Guest(kGameDataMan)) + 8);
            WritePlayerEchoes(Fld<int>(rec, 0x94, 0) + std::atoi(a.c_str() + 8));
        } else if (a.rfind("quick=", 0) == 0) {  // select quick slot N (the game's own selection record)
            const int slot = std::atoi(a.c_str() + 6);  // "quick=SLOT" or "quick=SLOT:GOODS" (given when not held)
            const std::size_t colon = a.find(':');
            const std::uint32_t id = colon == std::string::npos ? QuickItemId(slot, nullptr)
                                                                 : std::uint32_t(std::atoi(a.c_str() + colon + 1));
            bool selected = false;
            QuickItemId(slot, &selected);
            const bool ok = id && SetQuickItem(slot, id, true);
            Log("test: quick slot %d holds goods %u (was selected %d), select %s", slot, id, selected ? 1 : 0, ok ? "ok" : "failed");
        } else if (a == "peek") {
            g_machine.Peek(Now(), 3.0);
        }
        Log("test: %s (%.1f s after the world is up)", a.c_str(), since);
    }
}

}  // namespace

void Install() {
    ParseScript();
    const bool code = Matches(kViewCtor, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57});
    g_on = code && HookPrologue(kViewCtor, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &ViewCtorEntry, "auto HUD: the HUD view");
    Log("%s", g_on ? "installed (setting Auto-hide HUD; off by default)" : "NOT installed: the code differs from 1.09");
}

void Tick() {
    if (!g_on) return;
    const bool enabled = Enabled();
    const double now = Now();
    Inputs in;
    in.now = now;
    const u64 comp = Component();
    in.valid = comp && ReadInputs(&in);
    if (!in.valid) {
        g_world_since = -1;
        g_machine.Update(in);
        return;
    }
    if (g_world_since < 0) g_world_since = now;
    RunScript(now - g_world_since);
    if (!enabled) {
        if (g_applied_any) {  // switched off: the HUD back to full
            SetAlpha(comp, 1.0, nullptr);
            g_applied_any = false;
            g_machine.Reset();
            Log("off: the HUD is back to normal");
        }
        return;
    }
    const double a = g_machine.Update(in);
    static double last_a = -1;
    static unsigned last_r = ~0u;
    if (std::fabs(a - last_a) > 1e-3 || !g_applied_any) {
        double read = -1;
        if (SetAlpha(comp, a, &read)) {
            g_applied_any = true;
            static int logged = 0;
            if (logged < 6) {
                ++logged;
                Log("alpha %.2f set (the clip's alpha read before: %.1f)", a, read);
            }
        }
        last_a = a;
    }
    if (g_machine.reason() != last_r) {
        last_r = g_machine.reason();
        Log("%s (reason bits 0x%x, alpha %.2f)", last_r ? "shown" : "hidden", last_r, a);
    }
}

}  // namespace coop::autohud
