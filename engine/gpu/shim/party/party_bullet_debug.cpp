// SPDX-License-Identifier: GPL-3.0-or-later
// Bullet debug log (party_bullet_debug.h). Offsets are ours (raw ELF VA, 1.09):
//   0x553e8d0 SprjBulletManager slot: +0x08 first live bullet, bullet +0x868 next (the join send
//             0x1a04190 walks it so), +0x10 free list, pool stride 0x890
//   bullet +0x08 handle, +0x10 position, +0x40 velocity, +0x64 emitter handle, +0x2e0 stage 1..3
//          (4 = ended) of the stage records +0x7f8 / +0x820 / +0x848 (stride 0x28, param id at +8),
//          +0x2f4 root BulletParam id, +0x2fc owner chr handle, +0x734 life left of the stage
//   0x19f6ca0 init(bullet, descriptor, ...): descriptor +0x04 param id, +0x0c owner handle
//   0x19fbeb0 end(bullet, reason, const float* position): expiry passes reason -1
//   0x19fa730 character contact(...): one argument is the bullet; the attack record's +0x3c is
//             the AtkParam id
// Found by static reading of the eboot (task R1, 26 Oct); the runtime log is what proves them.
#include "party_bullet_debug.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_balance.h"
#include "party_enemy_census.h"
#include "world_warper_rules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop::bulletdebug {
namespace {

using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

// BB_DEBUG_USE_TOOL=GOODS[,GOODS...][@S]: S s (default 30) after the player first exists, each goods
// in turn into quick slot 0 and one Square press, 8 s apart (a headless tool test).
constexpr std::uint32_t kQuicksilver = 900;
// ":near=M" before the delay's end: each use first moves the player M m from the nearest living
// regular enemy, facing it, and locks on (R3); the enemy's HP is logged before and 8 s after.
constexpr std::uint32_t kSquare = 0x8000, kR3 = 0x4;
enum class Phase { Next, Lock, Use };
std::vector<std::uint32_t> g_tools;
double g_tool_delay = 30;
float g_near = 0;
float g_near_dy = 1.5f; // ":dy=H": max enemy height difference from the player (default 1.5 m)
bool g_pinned = false;
alignas(16) float g_pin_pos[4] = {0, 0, 0, 1}, g_pin_rot[4] = {0, 0, 0, 0};
std::size_t g_tool_next = 0;
Clock::time_point g_player_seen{}, g_tool_at{}, g_release_at{};
bool g_pressing = false;
Phase g_phase = Phase::Next;
int g_lock_tries = 0;
std::uint64_t g_target = 0;
std::uint32_t g_target_entity = 0;
int g_target_hp = -1;

constexpr u64 kSlotBullet = 0x553e8d0;
constexpr u64 kSlotWorldChrMan = 0x553e878; // +0x60 the local player
constexpr u64 kBulletInit = 0x19f6ca0;
constexpr u64 kBulletEnd = 0x19fbeb0;
constexpr u64 kBulletContact = 0x19fa730;
constexpr int kMaxLines = 4000; // a run's log budget; enemies fire a lot with "all"
constexpr int kMaxWalk = 1024;

bool g_on = false;
std::vector<std::pair<int, int>> g_ranges; // empty: every id
std::mutex g_mu;                           // the hooks can run off the main thread
int g_lines = 0;
// live bullet pointer -> {handle, last stage param id}
std::unordered_map<u64, std::pair<std::uint32_t, int>> g_seen;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    if (g_lines >= kMaxLines) return;
    if (++g_lines == kMaxLines) {
        std::printf("[coop] Bullet: log budget of %d lines used; no more bullet lines\n", kMaxLines);
        std::fflush(stdout);
        return;
    }
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("[coop] Bullet: %s\n", line);
    std::fflush(stdout);
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

bool Wanted(int id) {
    if (g_ranges.empty()) return true;
    for (const auto& [lo, hi] : g_ranges) {
        if (id >= lo && id <= hi) return true;
    }
    return false;
}

/// The param id of the bullet's current stage, else its root id.
int StageId(u64 b) {
    const int stage = Field<int>(b, 0x2e0);
    if (stage >= 1 && stage <= 3) {
        const int id = Field<int>(b, 0x7f8 + u64(stage - 1) * 0x28 + 8, -1);
        if (id > 0) return id;
    }
    return Field<int>(b, 0x2f4, -1);
}

std::string Vec(u64 at) {
    float v[3] = {};
    if (!at || !SafeRead(at, v, sizeof v)) return "(?)";
    char s[96];
    std::snprintf(s, sizeof s, "(%.2f, %.2f, %.2f)", v[0], v[1], v[2]);
    return s;
}

/// The local player's position (party_b2's ChrPos path) for comparing heights.
std::string PlayerPos() {
    const u64 me = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(me, 0x58), 8), 0x3b0), 0x68);
    return a ? Vec(a + 0x1e0) : "(?)";
}

bool IsLiveBullet(u64 p) {
    if (!p) return false;
    u64 b = Field<u64>(Slot(kSlotBullet), 8);
    for (int n = 0; b && n < kMaxWalk; ++n) {
        if (b == p) return true;
        b = Field<u64>(b, 0x868);
    }
    return false;
}

void BB_COOP_SYSV OnInit(u64 bullet, u64 desc, u64, u64, u64 shot, u64) {
    const int id = Field<int>(desc, 4, -1);
    if (!Wanted(id)) return;
    std::lock_guard lock(g_mu);
    Log("new id %d owner 0x%x shot %d (bullet 0x%llx); player at %s", id, Field<std::uint32_t>(desc, 0xc),
        int(shot & 0xffffffff), static_cast<unsigned long long>(bullet), PlayerPos().c_str());
}

// At the end call the stage record already names the child (run 1: "end" of 494 read 495), so the
// bullet's own id is its +0x2f4.
void BB_COOP_SYSV OnEnd(u64 bullet, u64 reason, u64 pos, u64, u64, u64) {
    const int id = Field<int>(bullet, 0x2f4, -1), next = StageId(bullet);
    if (!Wanted(id)) return;
    std::lock_guard lock(g_mu);
    Log("end id %d (next %d, handle %u) reason %d at %s, life left %.2f; player at %s", id, next == id ? -1 : next,
        Field<std::uint32_t>(bullet, 8), int(reason & 0xffffffff), Vec(pos).c_str(), Field<float>(bullet, 0x734),
        PlayerPos().c_str());
}

// Run 1 showed the order: a0 the bullet, a1 the attack record (+0x3c AtkParam id), a2 the chr hit
// (+0x1f0 map entity id). The bullet is still searched for in case another caller differs.
void BB_COOP_SYSV OnContact(u64 a0, u64 a1, u64 a2, u64, u64, u64) {
    if (IsLiveBullet(a0)) {
        const int id = Field<int>(a0, 0x2f4, -1);
        if (!Wanted(id)) return;
        std::lock_guard lock(g_mu);
        const bool self = a2 == Field<u64>(Slot(kSlotWorldChrMan), 0x60);
        Log("hit: bullet id %d at %s, AtkParam %d, on %s (entity %d)%s", id, Vec(a0 + 0x10).c_str(),
            Field<int>(a1, 0x3c, -1), self ? "the PLAYER" : "chr", Field<int>(a2, 0x1f0, -1),
            self ? " (selfTarget)" : "");
        return;
    }
    const u64 args[3] = {a0, a1, a2};
    int which = -1;
    for (int i = 0; i < 3 && which < 0; ++i) {
        if (IsLiveBullet(args[i])) which = i;
    }
    const u64 b = which >= 0 ? args[which] : 0;
    const int id = b ? StageId(b) : -1;
    if (b && !Wanted(id) && !Wanted(Field<int>(b, 0x2f4, -1))) return;
    std::lock_guard lock(g_mu);
    // The other two arguments are the attack record (+0x3c AtkParam id) and the hit chr (+0x1f0
    // its map entity id); which is which is printed, not assumed.
    std::string others;
    for (int i = 0; i < 3; ++i) {
        if (i == which) continue;
        char s[128];
        std::snprintf(s, sizeof s, " arg%d 0x%llx (+0x3c %d, +0x1f0 %d)", i, static_cast<unsigned long long>(args[i]),
                      Field<int>(args[i], 0x3c, -1), Field<int>(args[i], 0x1f0, -1));
        others += s;
    }
    Log("hit: bullet id %d (root %d, arg%d) at %s;%s", id, b ? Field<int>(b, 0x2f4, -1) : -1, which,
        b ? Vec(b + 0x10).c_str() : "(?)", others.c_str());
}

void ParseRanges(const char* s) {
    if (!std::strcmp(s, "1") || !std::strcmp(s, "all")) return;
    for (const char* p = s; *p;) {
        char* end = nullptr;
        const long lo = std::strtol(p, &end, 10);
        if (end == p) break;
        long hi = lo;
        p = end;
        if (*p == '-') {
            hi = std::strtol(p + 1, &end, 10);
            p = end;
        }
        g_ranges.emplace_back(int(lo), int(hi));
        while (*p == ',' || *p == ' ') ++p;
    }
}

void ParseTools(const char* s) {
    for (const char* p = s; *p && *p != '@';) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 10);
        if (end == p) break;
        g_tools.push_back(std::uint32_t(id));
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    if (const char* at = std::strchr(s, '@')) g_tool_delay = std::atof(at + 1);
    if (const char* n = std::strstr(s, ":near=")) g_near = float(std::atof(n + 6));
    if (const char* n = std::strstr(s, ":dy=")) g_near_dy = float(std::atof(n + 4));
}

void Press(std::uint32_t buttons, Clock::time_point now) {
    runtime_pad_director_press(buttons);
    g_pressing = true;
    g_release_at = now + std::chrono::milliseconds(200);
}

/// The local chr `dist` m from the nearest living regular enemy, facing it (the director's chr warp:
/// vfunc +0x5F8 = 0x18CD600(chr, pos, rot, 1)). False: no enemy or no warp.
bool MoveNearEnemy(u64 me, float dist) {
    float mp[3] = {};
    const u64 pm = Field<u64>(Field<u64>(Field<u64>(Field<u64>(me, 0x58), 8), 0x3b0), 0x68);
    if (!pm || !SafeRead(pm + 0x1e0, mp, sizeof mp)) return false;
    const EnemyKillCandidate* best = nullptr;
    float best_d = 1e9f;
    const std::vector<EnemyKillCandidate> enemies = balance::LoadedEnemies();
    for (const EnemyKillCandidate& e : enemies) {
        if (!e.alive || !e.have_pos || !e.chr || EnemyIsBossEntity(e.entity)) continue;
        // Only enemies near the player's own height: a ledge's height over the floor below drops the
        // pinned player into a fall (run 4: dead of fall damage, the later goods never fired).
        if (std::fabs(e.pos[1] - mp[1]) > g_near_dy) continue;
        const float d = std::hypot(e.pos[0] - mp[0], e.pos[2] - mp[2]);
        if (d < best_d) best_d = d, best = &e;
    }
    const u64 vt = Field<u64>(me, 0);
    const u64 fn = Field<u64>(vt, 0x5f8);
    if (!best || !fn) {
        Log("tool: no living enemy loaded (%zu in the census) or no warp: used where the player stands", enemies.size());
        return false;
    }
    // From the enemy back toward the player's side, `dist` m out, at the enemy's height.
    float dx = mp[0] - best->pos[0], dz = mp[2] - best->pos[2];
    const float len = std::hypot(dx, dz);
    if (len < 0.01f) dx = 0, dz = 1; else dx /= len, dz /= len;
    alignas(16) float pos[4] = {best->pos[0] + dx * dist, best->pos[1], best->pos[2] + dz * dist, 1.0f};
    // Yaw toward the enemy. Run fire2 used atan2(-dx, -dz) and the tool fired exactly away from the enemy:
    // the game's forward at yaw y is -(sin y, cos y), so the yaw is atan2(dx, dz) (dx/dz: enemy -> player).
    alignas(16) float rot[4] = {0, std::atan2(dx, dz), 0, 0};
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, const float*, const float*, std::uint32_t)>(fn)(me, pos, rot, 1);
    // Kept there every frame (like the director's ":pin"): a single warp drops the player through the
    // floor while the new spot's collision streams in (run 2). Released at the next move.
    std::memcpy(g_pin_pos, pos, sizeof pos);
    std::memcpy(g_pin_rot, rot, sizeof rot);
    g_pinned = true;
    g_target = best->chr;
    g_target_entity = best->entity;
    g_target_hp = balance::EnemyHp(best->chr);
    Log("tool: moved %.1f m from enemy entity %u (HP %d) at (%.2f, %.2f, %.2f); player to (%.2f, %.2f, %.2f) yaw %.2f",
        dist, best->entity, g_target_hp, best->pos[0], best->pos[1], best->pos[2], pos[0], pos[1], pos[2], rot[1]);
    return true;
}

void ToolTick() {
    const auto now = Clock::now();
    if (g_pressing && now >= g_release_at) {
        runtime_pad_director_press(0);
        g_pressing = false;
    }
    const u64 me = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    if (!me) {
        g_player_seen = {}; // a load: wait for the player again
        g_pinned = false;
        return;
    }
    if (g_pinned) {
        if (const u64 fn = Field<u64>(Field<u64>(me, 0), 0x5f8))
            reinterpret_cast<void(BB_COOP_SYSV*)(u64, const float*, const float*, std::uint32_t)>(fn)(me, g_pin_pos,
                                                                                                    g_pin_rot, 1);
    }
    if (g_pressing) return;
    if (g_player_seen == Clock::time_point{}) {
        g_player_seen = now;
        g_tool_at = now + std::chrono::milliseconds(long(g_tool_delay * 1000));
    }
    if (now < g_tool_at) return;
    std::lock_guard lock(g_mu);
    switch (g_phase) {
    case Phase::Next: {
        if (g_target) { // the previous use's result
            Log("tool: enemy entity %u HP %d -> %d after goods %u", g_target_entity, g_target_hp, balance::EnemyHp(g_target),
                g_tools[g_tool_next - 1]);
            g_target = 0;
        }
        if (g_tool_next >= g_tools.size()) return;
        g_pinned = false;
        if (g_near > 0 && MoveNearEnemy(me, g_near)) {
            g_phase = Phase::Lock;
            g_lock_tries = 0;
            g_tool_at = now + std::chrono::milliseconds(3000); // collision streams in, the chr settles
            return;
        }
        g_phase = Phase::Use;
        return;
    }
    case Phase::Lock: {
        std::uint8_t locked = 0;
        SafeGet(me + 0xf8, &locked);
        if (!locked && g_lock_tries++ < 2) {
            Press(kR3, now);
            g_tool_at = now + std::chrono::milliseconds(600);
            return;
        }
        Log("tool: lock-on %s after %d R3 press(es)", locked ? "on" : "OFF", g_lock_tries);
        g_phase = Phase::Use;
        return;
    }
    case Phase::Use: {
        const std::uint32_t id = g_tools[g_tool_next++];
        if (const int held = GoodsCount(kQuicksilver); held >= 0 && held < 12) GiveGoods(kQuicksilver, 20 - held);
        const bool ok = SetQuickItem(0, id, true);
        bool selected = false;
        const std::uint32_t in_slot = QuickItemId(0, &selected);
        Log("tool: goods %u into quick slot 0 %s (slot holds %u%s, Quicksilver %d); Square pressed; player at %s", id,
            ok ? "ok" : "FAILED", in_slot, selected ? ", selected" : "", GoodsCount(kQuicksilver), PlayerPos().c_str());
        Press(kSquare, now);
        g_phase = Phase::Next;
        g_tool_at = now + std::chrono::seconds(8);
        return;
    }
    }
}

// BB_DEBUG_ATTACK=WEAPON[,WEAPON...][@S]: S s (default 30) after the player first exists, each weapon in
// turn into the right hand, then R1, L1 (transform), R1, L1. Logs every weapon clip selection and every
// change of the hkb variables WeaponRightType / WeaponLeftType.
//   0xacd670 picks a CustomManualSelectorGenerator's child: (selector, context); selector +0x88 children,
//            +0x90 count, +0x98 offsetType, +0x9c animId; a child's name is at +0x38 (low bit masked)
//   0xacf9d0 (context, offsetType, -1, animId, 0.0f) reads the variable behind offsetType x 1000000
//            (13 WeaponRightType, 14 WeaponLeftType); it uses only context +0 and +0x10
constexpr u64 kCmsgPick = 0xacd670;
constexpr u64 kCmsgGroup = 0xacf9d0;
constexpr std::uint32_t kR1 = 0x800, kL1 = 0x400;
std::vector<std::uint32_t> g_attack_weapons;
double g_attack_delay = 30;
std::size_t g_attack_next = 0;
int g_attack_step = 0;
Clock::time_point g_attack_seen{}, g_attack_at{}, g_attack_release{};
bool g_attack_pressing = false;
std::uint32_t g_attack_weapon = 0;
u64 g_ctx[3] = {}; // the player's context fields the group read uses (the context itself is short-lived)
int g_last_right = -1, g_last_left = -1;

int ReadGroup(const u64* ctx, int offset_type) {
    const auto fn = reinterpret_cast<int(BB_COOP_SYSV*)(const u64*, int, int, int, float)>(Guest(kCmsgGroup));
    return fn(ctx, offset_type, -1, 0, 0.0f) / 1000000;
}

void BB_COOP_SYSV OnCmsgPick(u64 selector, u64 context, u64, u64, u64, u64) {
    if (Field<int>(selector, 0x98) != 13 || !context) return;
    const u64 ctx[3] = {Field<u64>(context, 0), Field<u64>(context, 8), Field<u64>(context, 0x10)};
    const int right = ReadGroup(ctx, 13);
    if (right == 0) return; // a graph without the variable (enemies)
    std::memcpy(g_ctx, ctx, sizeof g_ctx);
    const int anim = Field<int>(selector, 0x9c);
    char want[32];
    std::snprintf(want, sizeof want, "a%03d_%06d", right, anim % 1000000);
    std::string picked = "(no child, fallback)";
    const u64 kids = Field<u64>(selector, 0x88);
    const int count = Field<int>(selector, 0x90);
    for (int i = 0; i < count; ++i) {
        char name[64] = {};
        const u64 at = Field<u64>(Field<u64>(kids, u64(i) * 8), 0x38) & ~u64(1);
        if (!at || !SafeRead(at, name, sizeof name - 1)) continue;
        if (!std::strncmp(name, want, std::strlen(want))) {
            picked = name;
            break;
        }
    }
    Log("attack: weapon %u selector anim %d: WeaponRightType %d WeaponLeftType %d -> %s", g_attack_weapon, anim, right,
        ReadGroup(ctx, 14), picked.c_str());
}

// 0x1a27fb0 (id, chr) is the TAE adapter: chr +0x3b0 +0x18 = TAE owner, owner +0x10 = table, table +0x30 count, +0x38 records
// sorted by id, record +0x10 = its events. Logs each weapon-group id once, and the whole table's a144 / a159 ids once.
constexpr u64 kTaeAdapter = 0x1a27fb0;
std::vector<int> g_tae_seen;
bool g_tae_dumped = false;

void BB_COOP_SYSV OnTaeAdapter(u64 id64, u64 chr, u64, u64, u64, u64) {
    const int id = int(id64);
    if (id / 1000000 != 144 && id / 1000000 != 159) return;
    if (std::find(g_tae_seen.begin(), g_tae_seen.end(), id) != g_tae_seen.end()) return;
    g_tae_seen.push_back(id);
    const u64 owner = Field<u64>(Field<u64>(chr, 0x3b0), 0x18);
    const u64 table = Field<u64>(owner, 0x10);
    const int count = Field<int>(table, 0x30);
    const u64 records = Field<u64>(table, 0x38);
    u64 data = 0;
    bool found = false;
    std::string ids144, ids159;
    int n144 = 0, n159 = 0;
    for (int i = 0; i < count; ++i) {
        const u64 record = Field<u64>(records, u64(i) * 8);
        const int rid = Field<int>(record, 0);
        const u64 events = Field<u64>(record, 0x10);
        if (rid == id) {
            found = true;
            data = events;
        }
        if (rid / 1000000 == 144) ++n144;
        if (rid / 1000000 == 159) {
            ++n159;
            ids159 += " " + std::to_string(rid % 1000000) + (events ? "" : "(null)");
        }
    }
    Log("attack: TAE id %d: weapon %u, record %s, events 0x%llx, table %d records", id, g_attack_weapon, found ? "found" : "MISSING",
        static_cast<unsigned long long>(data), count);
    if (!g_tae_dumped) {
        g_tae_dumped = true;
        Log("attack: TAE table: %d a144 records, %d a159 records:%s", n144, n159, ids159.c_str());
    }
}

// 0x977800 is the hks hkbSetVariable; its first name lookup 0x94a2f0(table, name) names the variable.
constexpr u64 kHksSetVariable = 0x977800;
constexpr u64 kVariableIndex = 0x94a2f0;
thread_local bool t_in_set = false;
int g_set_lines = 0;

void BB_COOP_SYSV OnHksSet(u64, u64, u64, u64, u64, u64) { t_in_set = true; }

void BB_COOP_SYSV OnVariableIndex(u64, u64 name, u64, u64, u64, u64) {
    if (!t_in_set) return;
    t_in_set = false;
    char s[32] = {};
    if (!g_ctx[0] || !SafeRead(name, s, sizeof s - 1) || std::strncmp(s, "Weapon", 6) || g_set_lines >= 4) return;
    ++g_set_lines;
    Log("attack: hks hkbSetVariable(\"%s\") arrived; before it WeaponRightType %d WeaponLeftType %d", s,
        g_ctx[0] ? ReadGroup(g_ctx, 13) : -1, g_ctx[0] ? ReadGroup(g_ctx, 14) : -1);
}

/// One variable in one graph: value, bounds and type, as the setter 0x97b0a0 sees them
/// (graph +0xf8 +0x20 external -> internal map, +0x108 +0x10 values, +0xc0 +0x20 infos, +0xc0 +0x50 bounds).
void DumpVar(const char* label, u64 graph, const char* name) {
    if (!graph) return;
    const auto index = reinterpret_cast<int(BB_COOP_SYSV*)(u64, const char*)>(Guest(kVariableIndex));
    const auto map = reinterpret_cast<int(BB_COOP_SYSV*)(u64, long, long)>(Guest(0x24c7fe0));
    const int external = index(Field<u64>(g_ctx[0], 0xa0), name);
    if (external < 0) return;
    const int in = map(Field<u64>(graph, 0xf8) + 0x20, external, -1);
    const u64 data = Field<u64>(graph, 0xc0);
    Log("attack: %s graph 0x%llx %s external %d internal %d value %d min %d max %d type %d", label,
        static_cast<unsigned long long>(graph), name, external, in,
        Field<int>(Field<u64>(Field<u64>(graph, 0x108), 0x10), u64(in) * 4, -999),
        Field<int>(Field<u64>(data, 0x50), u64(in) * 8, -999), Field<int>(Field<u64>(data, 0x50), u64(in) * 8 + 4, -999),
        int(Field<std::int8_t>(Field<u64>(data, 0x20), u64(in) * 6 + 4, -1)));
}

void DumpVars() {
    if (!g_ctx[0]) return;
    const u64 write_graph = g_ctx[1] ? g_ctx[1] : Field<u64>(g_ctx[0], 0x80);
    for (const char* name : {"WeaponRightType", "WeaponLeftType", "ItemAnime"}) {
        DumpVar("setter", write_graph, name);
        DumpVar("selector", g_ctx[2], name);
    }
}

void ParseAttack(const char* s) {
    for (const char* p = s; *p && *p != '@';) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 10);
        if (end == p) break;
        g_attack_weapons.push_back(std::uint32_t(id));
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    if (const char* at = std::strchr(s, '@')) g_attack_delay = std::atof(at + 1);
    if (const char* n = std::strstr(s, ":near=")) g_near = float(std::atof(n + 6));
    if (const char* n = std::strstr(s, ":dy=")) g_near_dy = float(std::atof(n + 4));
}

void AttackTick() {
    const auto now = Clock::now();
    if (g_ctx[0]) {
        const int right = ReadGroup(g_ctx, 13), left = ReadGroup(g_ctx, 14);
        if (right != g_last_right || left != g_last_left) {
            Log("attack: variables now WeaponRightType %d WeaponLeftType %d (weapon %u)", right, left, g_attack_weapon);
            g_last_right = right;
            g_last_left = left;
        }
    }
    if (g_attack_pressing && now >= g_attack_release) {
        runtime_pad_director_press(0);
        g_attack_pressing = false;
    }
    const u64 me = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    if (!me) {
        g_attack_seen = {};
        return;
    }
    if (g_pinned) {
        if (const u64 fn = Field<u64>(Field<u64>(me, 0), 0x5f8))
            reinterpret_cast<void(BB_COOP_SYSV*)(u64, const float*, const float*, std::uint32_t)>(fn)(me, g_pin_pos, g_pin_rot, 1);
    }
    if (g_attack_pressing) return;
    if (g_attack_seen == Clock::time_point{}) {
        g_attack_seen = now;
        g_attack_at = now + std::chrono::milliseconds(long(g_attack_delay * 1000));
    }
    if (now < g_attack_at) return;
    const auto press = [&](std::uint32_t button, const char* name) {
        Log("attack: %s pressed (weapon %u), enemy HP %d", name, g_attack_weapon, g_target ? balance::EnemyHp(g_target) : -1);
        runtime_pad_director_press(button);
        g_attack_pressing = true;
        g_attack_release = now + std::chrono::milliseconds(200);
    };
    switch (g_attack_step) {
    case 0: {
        if (g_attack_next >= g_attack_weapons.size()) return;
        g_attack_weapon = g_attack_weapons[g_attack_next++];
        const bool ok = EquipSlotItem(warper::kSlotRight1, kItemWeapon, g_attack_weapon);
        Log("attack: equip weapon %u right hand %s (slot holds %u)", g_attack_weapon, ok ? "ok" : "FAILED",
            EquippedSlotId(warper::kSlotRight1));
        g_attack_at = now + std::chrono::seconds(4);
        break;
    }
    case 1:
        if (g_near > 0) MoveNearEnemy(me, g_near);
        g_attack_at = now + std::chrono::seconds(3); // collision streams in, the chr settles
        break;
    case 2: case 3: press(kR1, "R1"); g_attack_at = now + std::chrono::milliseconds(1100); break;
    case 4: press(kR1, "R1"); g_attack_at = now + std::chrono::seconds(5); break;
    case 5: press(kL1, "L1"); g_attack_at = now + std::chrono::seconds(4); break;
    case 6:
        if (g_near > 0) MoveNearEnemy(me, g_near);
        g_attack_at = now + std::chrono::seconds(3);
        break;
    case 7: case 8: press(kR1, "R1"); g_attack_at = now + std::chrono::milliseconds(1100); break;
    case 9: press(kR1, "R1"); g_attack_at = now + std::chrono::seconds(5); break;
    case 10: press(kL1, "L1"); g_attack_at = now + std::chrono::seconds(4); break;
    }
    g_attack_step = (g_attack_step + 1) % 11;
}

} // namespace

void Init() {
    if (const char* attack = std::getenv("BB_DEBUG_ATTACK"); attack && *attack) {
        ParseAttack(attack);
        const bool pick = HookPrologue(kCmsgPick, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55}, &OnCmsgPick,
                                       "attack debug: selector pick");
        HookPrologue(kTaeAdapter, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53}, &OnTaeAdapter,
                     "attack debug: TAE adapter");
        HookPrologue(kHksSetVariable, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55}, &OnHksSet,
                     "attack debug: hks set");
        HookPrologue(kVariableIndex, {0x48, 0x83, 0xc7, 0x78, 0x48, 0xc7, 0xc2, 0xff, 0xff, 0xff, 0xff}, &OnVariableIndex,
                     "attack debug: variable index");
        std::printf("[coop] Bullet: attack test: %zu weapons, first %.0f s after the player exists (hook %d)\n",
                    g_attack_weapons.size(), g_attack_delay, pick);
        std::fflush(stdout);
    }
    if (const char* tools = std::getenv("BB_DEBUG_USE_TOOL"); tools && *tools) {
        ParseTools(tools);
        std::printf("[coop] Bullet: tool test: %zu goods, first %.0f s after the player exists\n", g_tools.size(),
                    g_tool_delay);
        std::fflush(stdout);
    }
    const char* env = std::getenv("BB_DEBUG_BULLETS");
    if (!env || !*env || !std::strcmp(env, "0")) return;
    ParseRanges(env);
    const bool init = HookPrologue(kBulletInit, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54},
                                   &OnInit, "bullet debug: init");
    const bool end = HookPrologue(kBulletEnd, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54},
                                  &OnEnd, "bullet debug: end");
    const bool hit = HookPrologue(kBulletContact, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53},
                                  &OnContact, "bullet debug: character contact");
    g_on = true;
    std::printf("[coop] Bullet: debug log on (%s; hooks: init %d, end %d, contact %d)\n", env, init, end, hit);
    std::fflush(stdout);
}

void Tick() {
    if (!g_tools.empty()) ToolTick();
    if (!g_attack_weapons.empty()) AttackTick();
    if (!g_on) return;
    std::lock_guard lock(g_mu);
    std::unordered_map<u64, std::pair<std::uint32_t, int>> now;
    u64 b = Field<u64>(Slot(kSlotBullet), 8);
    for (int n = 0; b && n < kMaxWalk; ++n, b = Field<u64>(b, 0x868)) {
        const std::uint32_t handle = Field<std::uint32_t>(b, 8);
        const int id = StageId(b);
        now[b] = {handle, id};
        const auto it = g_seen.find(b);
        const bool fresh = it == g_seen.end() || it->second.first != handle;
        if (!fresh && it->second.second == id) continue;
        if (!Wanted(id) && !Wanted(Field<int>(b, 0x2f4, -1))) continue;
        Log("%s id %d (root %d, handle %u, emitter %u, stage %d) at %s velocity %s life %.2f", fresh ? "live" : "stage",
            id, Field<int>(b, 0x2f4, -1), handle, Field<std::uint32_t>(b, 0x64), Field<int>(b, 0x2e0),
            Vec(b + 0x10).c_str(), Vec(b + 0x40).c_str(), Field<float>(b, 0x734));
    }
    g_seen.swap(now);
}

} // namespace coop::bulletdebug
