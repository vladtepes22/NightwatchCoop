// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance, the game side (see party_balance.h, docs/party/balance.md). Offsets are ours
// (raw ELF VA of out/eboot.elf, 1.09), each read in the eboot (capstone / Ghidra):
//   0x17e46e0 float MaxHpRate(SpEffect list, bool independent): the product of the active
//             effects' maxHpRate (node +0x48 -> param row +0x10, read live), NG+ table, 9200 sync.
//             Called by the per-frame HP update 0x1a0d7d0 (max = base * rate; current HP grows
//             with the "independent" rate) and the boss-bar refresh (EMEVD 2004[53]).
//   0x17e1d90 RemoveSpEffect(list, node, 1) (EMEVD 2004[21]): not used; bosses are kept out of 7503
//             by entity (0x13c97a0) instead
//   chr vfunc +0x3f0 ApplySpEffect(chr, id, chr, 0, 1, 0, 0, 1.0 x5) (party_director / party_b2)
//   ChrIns +0x1c8 SpEffect list (+8 first node; node +0x40 id, +0x48 row, +0x58 next)
//   ChrIns +0x3c8 -> +8 NpcParam row (0x18e4fa6): teamType +0x12f, isSoulGetByBoss 0x149 bit 4
//   WorldChrMan 0x553e878: +0x60 local player, +0x30 / +0x38 player set (stride 0x38),
//             +0x850 + set * 8 chr set tables ({int count, pad, entries}; entries 0x38 apart,
//             chr at +0); sets < *(*(+8) + 0x18) + 2 (party_b2 ChrOfHandle, the game's 0x15bdc20)
//   ChrIns +0x3b0 -> +0x20 data module: HP +0xf8, max HP +0xfc, base max +0x100 (0x18bf9a0, the
//             kill, writes HP +0xf8 = 0; 0x1a0d7d0 is the HP struct at +0xf0)
#include "party_balance.h"

#include "../../party_env.h"
#include "../bbport_settings.h"
#include "../net/bbnet_internal.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_enemy_census.h"
#include "party_ids.h"
#include "party_levelsync_rules.h"
#include "party_director.h"
#include "party_droproll.h"
#include "party_flow.h"
#include "party_gamedata.h"
#include "party_guestsvc.h"
#include "party_link.h"
#include "party_npc_mirror.h"
#include "party_perf.h"
#include "party_phantom.h"
#include "party_start.h"
#include "party_rot_rules.h"
#include "party_status.h"
#include "party_travel.h"
#include "party_worldact.h"
#include "seamless_rules.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace coop::balance {

namespace {

std::atomic<bool> g_lamp_menu_opened{false};  // NoteLampMenuOpened (direct lamp travel)

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using Clock = std::chrono::steady_clock;

constexpr u64 kSlotWorldChrMan = 0x553e878;
constexpr u64 kMaxHpRate = 0x17e46e0;
constexpr u64 kMaxStaminaRate = 0x17e4a20;  // float(list): the SpEffects' maxStaminaRate (+0x18)
constexpr u32 kBossRow0 = 7500;   // 7500 + k: k + 1 cooperators
constexpr u32 kEnemyRow = 7503;   // unused in 1.09
constexpr float kVanillaRowHp[4] = {1.5f, 2.0f, 2.5f, 3.0f};
constexpr int kEnemyTeam = 23;
constexpr int kMaxSets = 64;
constexpr int kMaxPerSet = 4096;
constexpr int kMaxAppliesPerPass = 256;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party balance: ") + line + "\n");
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

bool EnvIsZero(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

bool On() {
    static const bool on = bb_party_enabled() && !EnvIsZero("BB_PARTY_BALANCE");
    return on;
}

// ---- State ----
std::mutex g_mu;  // the received rules
bool g_have_host = false;
Rules g_host;

std::atomic<u64> g_rot_list{0};         // the local player's SpEffect list (rate hook)
// Rot kinds (ERSC: five, one at random per death). Each is a rate function of the player's
// SpEffect list, its prologue replaced; the result is changed for the local player's list only.
enum RotKind { kRotHealth, kRotStamina, kRotStaminaCost, kRotRally, kRotDiscovery, kRotKinds };
const char* const kRotKindName[kRotKinds] = {"health", "stamina", "stamina cost", "rally", "discovery"};
std::atomic<int> g_kind[kRotKinds];          // stacks per kind (sum = g_stacks)
std::atomic<float> g_kind_value[kRotKinds];  // the factor (or, discovery, the points) applied now
bool g_kind_hooked[kRotKinds]{};
void* g_kind_orig[kRotKinds]{};
std::atomic<float> g_rot_factor{1.0f};  // max HP (the HP hook)
std::atomic<int> g_stacks{0}, g_deaths{0};  // all stacks
std::atomic<bool> g_rest_pending{false};
std::atomic<bool> g_rest_alive{true};  // the local player was alive when that lamp warp ran
std::atomic<int> g_hp_now{-1};          // the local HP at the last in-world tick (-1 unknown)
std::atomic<bool> g_hooked{false};
constexpr u64 kSlotMenuManBal = 0x5562878;  // MenuMan: +0x78 the dialog kind up (0 none), as party_split
using RateFn = float(BB_COOP_SYSV*)(u64, u64);
void* g_rate_orig = nullptr;

struct TickState {
    bool loaded = false;
    Clock::time_point next_params{}, next_enemies{}, next_send{}, last_death{};
    std::string last_sent;
    int last_members = 0;  // the host's connected guests at the last send
    int last_n = 0;
    u64 last_player = 0;
    int last_hp = -1;
    bool log = false;
    bool row_refused[4]{};
    bool rows_logged = false;
    Doping written[4];
    bool have_written[4]{};
    int hp_logged_factor = -1, hp_logged_max = -1;
    int hits = 0;
    Clock::time_point next_rot_log{};
    bool up_since_death = true;   // alive in the world again since the last death
    bool dialog_was = false;      // a dialog was up at the last in-world tick (#21 lamp menu)
    int last_level = -1;          // the own level (a level up at the Doll is a rest too)
    Clock::time_point next_rest_note{};
    std::string toast;            // a rot notice waiting for the respawn
    Clock::time_point toast_at{};  // alive in the world since  // rot factor (percent) of the last "max HP now" line
    Clock::time_point factor_since{};
    int last_factor = 100;
    Clock::time_point prologue_since{};  // in the own prologue since (stale rot)
};
TickState& S() {
    static TickState s;
    return s;
}

// ---- 0.7.2: what else cleanses rot (party_rot_rules.h) ----
// The ledger (one entry per death that added a stack) is under g_rot_mu (party_rot.txt keeps its
// stain's kind); events from other threads (the host's boss notice) wait in g_cleanse_q for the tick.
rot::Ledger g_ledger;  // under g_rot_mu
struct CleanseEvent {
    rot::Why why = rot::Why::Killer;
    u32 entity = 0;  // Boss / Killer: the entity that died (0 unknown)
    u64 chr = 0;     // Killer: its chr on this game (0: by its entity id only)
    bool reset = false;  // the world reset instead (the kill record's new world)
    std::string from;
};
std::mutex g_cleanse_mu;
std::vector<CleanseEvent> g_cleanse_q;  // under g_cleanse_mu
std::atomic<bool> g_kill_world_followed{false};  // a guest whose kill record follows the host's world
// The lethal hit's attacker (the game's kill log 0x1FEB2A0(victim, attacker, ...), called by the
// damage step 0x1A2A600 when a hit leaves the victim at HP 0; attacker 0 there goes to 0x1FEBFF0).
constexpr u64 kKilledByLog = 0x1feb2a0;
using KilledByFn = void(BB_COOP_SYSV*)(u64, u64, u32);
void* g_killed_by_orig = nullptr;
std::atomic<u64> g_local_chr{0};     // the local player's chr (the hook compares the victim)
std::atomic<u64> g_killer_chr{0};    // the attacker of the local player's lethal hit
std::atomic<std::int64_t> g_killer_at{0};  // when (steady clock ticks; 0 none)
std::atomic<std::uint32_t> g_killed_by_calls{0};

BB_COOP_SYSV void KilledByHook(u64 victim, u64 attacker, u32 x) {
    if (victim && victim == g_local_chr.load(std::memory_order_relaxed)) {
        g_killer_chr.store(attacker);
        g_killer_at.store(Clock::now().time_since_epoch().count());
    }
    g_killed_by_calls.fetch_add(1, std::memory_order_relaxed);
    reinterpret_cast<KilledByFn>(g_killed_by_orig)(victim, attacker, x);
}

void QueueCleanse(CleanseEvent e) {
    std::lock_guard<std::mutex> lk(g_cleanse_mu);
    if (g_cleanse_q.size() < 256) g_cleanse_q.push_back(std::move(e));
}

// ---- Rot persistence ----
std::filesystem::path RotPath() {
    std::string dir = bbnet::settings().user_dir;
    if (dir.empty()) {
        if (const char* u = std::getenv("BB_GPU_USER_DIR")) dir = u;
        else if (const char* u2 = std::getenv("BB_USER_DIR")) dir = u2;
        else dir = std::string(std::getenv("BB_DATA_DIR") ? std::getenv("BB_DATA_DIR") : ".") + "/user";
    }
    return std::filesystem::path(dir) / "party_rot.txt";
}

// Playtest 1 save audit S4: party_rot.txt holds every character's rot by save slot (RotFile);
// g_stacks / g_kind are the loaded character's (g_rot_slot), switched when another save is loaded.
std::mutex g_rot_mu;
RotFile g_rot_file;  // under g_rot_mu
int g_rot_slot = -1;  // the save slot g_stacks / g_kind belong to (-1: no character loaded yet)

SlotRot ActiveRot(int slot) {
    SlotRot r;
    r.slot = slot;
    for (int k = 0; k < kRotKinds; ++k) r.kind[k] = g_kind[k].load();
    r.stain = g_ledger.StainKind();
    return r;
}

void UseRot(const SlotRot& r) {
    int sum = 0;
    for (int k = 0; k < kRotKinds; ++k) {
        g_kind[k] = r.kind[k];
        sum += r.kind[k];
    }
    g_stacks = sum;
}

/// Caller holds g_rot_mu.
void WriteRotLocked() {
    std::error_code ec;
    const auto p = RotPath();
    const std::string text = FormatRotFile(g_rot_file);
    if (text.empty()) {
        std::filesystem::remove(p, ec);
        return;
    }
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    f << text;
}

void LoadRot() {
    std::string text;
    {
        std::ifstream f(RotPath());
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    std::lock_guard<std::mutex> lk(g_rot_mu);
    g_rot_file = ParseRotFile(text);
    g_rot_slot = -1;
    UseRot(SlotRot{});  // nothing until the character's save slot is known
    if (g_rot_file.legacy_stacks > 0) {
        Log("rot: %d stack(s) in an old party_rot.txt without a save slot dropped (whose character's they were is "
            "unknown; a lamp would have cleansed them)",
            g_rot_file.legacy_stacks);
        g_rot_file.legacy_stacks = 0;
        WriteRotLocked();
    }
}

/// The loaded character's rot to party_rot.txt (kept in memory only while its save slot is unknown).
void SaveRot() {
    std::lock_guard<std::mutex> lk(g_rot_mu);
    if (g_rot_slot < 0) return;
    g_rot_file.Set(ActiveRot(g_rot_slot));
    WriteRotLocked();
}

/// Main thread, in the world: the character in save slot `slot` is loaded; its own rot comes in.
void RotForSlot(int slot) {
    std::lock_guard<std::mutex> lk(g_rot_mu);
    if (!RotSlotChanged(g_rot_slot, slot)) return;
    const SlotRot before = ActiveRot(g_rot_slot);
    SlotRot next = g_rot_file.Get(slot);
    if (g_rot_slot >= 0) {
        g_rot_file.Set(before);
    } else if (before.Stacks() > 0) {
        // Earned before the slot could be read (this character's): added to its own.
        int left = kRotMaxStacks - next.Stacks();
        for (int k = 0; k < kRotKinds; ++k) {
            const int add = std::clamp(before.kind[k], 0, std::max(0, left));
            next.kind[k] += add;
            left -= add;
        }
        g_rot_file.Set(next);
    }
    Log("rot: save slot %d loaded: its own %d stack(s) (slot %d had %d)", slot, next.Stacks(), g_rot_slot,
        before.Stacks());
    g_rot_slot = slot;
    UseRot(next);
    // The deaths' links: only the stain outlives a restart / another save (its killers respawned).
    g_ledger.Restore(next.stain);
    WriteRotLocked();
}

void UpdateRotFactor(const Rules& r) {
    for (int k = 0; k < kRotKinds; ++k) {
        const int n = r.rot ? g_kind[k].load() : 0;
        float v = 1.0f;
        switch (k) {
        case kRotHealth:
        case kRotStamina: v = RotHpFactor(n); break;                            // max -10 % a stack
        case kRotStaminaCost: v = 1.0f + 0.1f * static_cast<float>(n); break;   // costs +10 % a stack
        case kRotRally: v = std::max(0.0f, 1.0f - 0.15f * static_cast<float>(n)); break;  // rally -15 %
        case kRotDiscovery: v = -10.0f * static_cast<float>(n); break;           // discovery -10 points
        }
        g_kind_value[k] = v;
    }
    g_rot_factor = g_kind_value[kRotHealth].load();
}

// ---- Hooks ----
BB_COOP_SYSV float RateHook(u64 list, u64 independent) {
    float r = reinterpret_cast<RateFn>(g_rate_orig)(list, independent);
    if (list && list == g_rot_list.load(std::memory_order_relaxed)) r *= g_rot_factor.load(std::memory_order_relaxed);
    return r;
}

// Evidence: how often the game asked each rot rate for the local player, and the last answer.
std::atomic<std::uint32_t> g_kind_calls[kRotKinds];
std::atomic<float> g_kind_last[kRotKinds];
float Note(int kind, float r) {
    g_kind_calls[kind].fetch_add(1, std::memory_order_relaxed);
    g_kind_last[kind].store(r, std::memory_order_relaxed);
    return r;
}
bool Mine(u64 list) {
    return list && list == g_rot_list.load(std::memory_order_relaxed);
}
BB_COOP_SYSV float StaminaRateHook(u64 list) {  // 0x17e4a20 max stamina rate (maxStaminaRate +0x18)
    const float r = reinterpret_cast<float(BB_COOP_SYSV*)(u64)>(g_kind_orig[kRotStamina])(list);
    return Mine(list) ? Note(kRotStamina, r * g_kind_value[kRotStamina].load(std::memory_order_relaxed)) : r;
}
BB_COOP_SYSV float DiscoveryHook(u64 list) {  // 0x17e8010 the sum of itemDropRate +0x1fc (points), RE only
    const float r = reinterpret_cast<float(BB_COOP_SYSV*)(u64)>(g_kind_orig[kRotDiscovery])(list);
    return Mine(list) ? Note(kRotDiscovery, r) : r;
}

void OnLampUse(u32 id) {
    g_rest_alive = g_hp_now.load() != 0;  // unknown counts as alive
    g_rest_pending = true;
    (void)id;
}

/// The local chr's physics position (+0x58 -> +8 -> +0x3B0 -> +0x68, +0x1E0).
bool LocalPos(u64 player, float out[3]) {
    const u64 pm = Field<u64>(Field<u64>(Field<u64>(Field<u64>(player, 0x58), 8), 0x3b0), 0x68);
    return pm && SafeRead(pm + 0x1e0, out, 12);
}

/// The nearest lamp revive spot of the loaded map to `pos` (its ReturnPointParam id, 0 = none).
u32 NearestLampSpot(u32 map_id, const float pos[3], float d[3]) {
    u32 best = 0;
    float best2 = 1e30f;
    const std::size_t n = ids::ReturnSpotIds().size();
    for (std::size_t i = 0; i < n; ++i) {
        const u32 id = ids::ReturnSpotIds()[i];
        if (ids::EntityMap(id) != (map_id & 0xffff0000u)) continue;
        const float* p = &ids::ReturnSpots()[i * 4];
        const float dx = pos[0] - p[0], dy = pos[1] - p[1], dz = pos[2] - p[2];
        if (dx * dx + dz * dz < best2) {
            best2 = dx * dx + dz * dz;
            best = id;
            d[0] = dx, d[1] = dy, d[2] = dz;
        }
    }
    return best;
}

/// The rot goes (a rest: the lamp's menu, a lamp warp that is no respawn, a level up).
void CleanseRot(const Rules& r, const char* why, Clock::time_point now, TickState& st) {
    if (g_stacks.load() > 0) {
        Log("%s: rot cleansed (%d stack(s))", why, g_stacks.load());
        g_stacks = 0;
        for (auto& c : g_kind) c = 0;
        {
            std::lock_guard<std::mutex> lk(g_rot_mu);
            g_ledger.Clear();
        }
        SaveRot();
        if (r.rot) party::status::SetLastEvent("The rot is cleansed");
    } else if (now >= st.next_rest_note) {
        st.next_rest_note = now + std::chrono::seconds(10);
        Log("%s: a rest (no rot to cleanse)", why);
    }
}

// ---- SpEffects ----
bool HasEffect(u64 chr, int id, u64* node_out = nullptr) {
    u64 node = Field<u64>(Field<u64>(chr, 0x1c8), 8);
    for (int guard = 0; node && guard < 256; ++guard) {
        if (Field<int>(node, 0x40, -1) == id) {
            if (node_out) *node_out = node;
            return true;
        }
        node = Field<u64>(node, 0x58);
    }
    return false;
}

bool ApplyEffect(u64 chr, int id) {
    u64 vt = 0, fn = 0;
    if (!chr || !SafeGet(chr, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) return false;
    using Fn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, float, float, float, float, float);
    reinterpret_cast<Fn>(fn)(chr, u64(id), chr, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    return true;
}


// ---- Param rows ----
Doping ReadRow(u64 row) {
    Doping d;
    d.hp_rate = Field<float>(row, 0x10, -1.0f);
    d.attack_rate = Field<float>(row, 0x48, -1.0f);
    d.resist_rate = Field<float>(row, 0x108, -1.0f);
    d.super_armor = Field<std::int16_t>(row, 0x13a, 0);
    if (d.super_armor != 0 && Field<std::uint16_t>(row, 0x156, 0) != 155) d.super_armor = -99999;  // not counted yet
    return d;
}

void WriteRow(u64 row, const Doping& d) {
    for (u64 off : {0x10ull}) SafeWrite(row + off, &d.hp_rate, 4);
    for (u64 off : {0x48ull, 0x4cull, 0x50ull, 0x54ull}) SafeWrite(row + off, &d.attack_rate, 4);
    for (u64 off : {0x108ull, 0x10cull, 0x110ull, 0x114ull}) SafeWrite(row + off, &d.resist_rate, 4);
    const std::int16_t sa = static_cast<std::int16_t>(std::clamp(d.super_armor, -30000, 30000));
    SafeWrite(row + 0x13a, &sa, 2);
    // The engine counts changeSuperArmorPoint only on effects whose stateInfo (+0x156) is 155 (the
    // poise sums 0x17e8590 / 0x18d0351 / 0x18fd400: `cmp word [row+0x156], 0x9b`). The 1.09 doping
    // rows carry +30 / +60 with stateInfo 0, so the game's own boss poise bonus never applied.
    const std::uint16_t state = sa != 0 ? 155 : 0;
    SafeWrite(row + 0x156, &state, 2);
}

void WriteParams(const Rules& r, int n) {
    TickState& st = S();
    for (int k = 0; k < 4; ++k) {
        const u32 id = kBossRow0 + u32(k);
        const u64 row = GameParamRow("SpEffectParam", "SP_EFFECT_PARAM_ST", id);
        if (!row) {
            st.have_written[k] = false;  // params not loaded (or reloaded): the next pass writes again
            continue;
        }
        const Doping want = k < 3 ? Boss(r, k + 1) : Enemy(r, n);
        const Doping now = ReadRow(row);
        if (now == want) {
            st.have_written[k] = true;
            st.written[k] = want;
            continue;
        }
        // Only a row that still holds the 1.09 value or what we wrote last.
        const bool vanilla = now.hp_rate == kVanillaRowHp[k] && now.attack_rate == 1.0f;
        const bool ours = st.have_written[k] && now == st.written[k];
        if (!vanilla && !ours) {
            if (!st.row_refused[k]) {
                st.row_refused[k] = true;
                Log("SpEffect %u: maxHpRate %.3f attack %.3f is neither 1.09's nor ours; left alone", id, now.hp_rate,
                    now.attack_rate);
            }
            continue;
        }
        WriteRow(row, want);
        st.written[k] = want;
        st.have_written[k] = true;
        Log("SpEffect %u (%s): HP x%.3f, resist x%.3f, damage x%.3f, poise %+d", id,
            k < 3 ? (k == 0 ? "boss, 1 cooperator" : k == 1 ? "boss, 2 cooperators" : "boss, 3 cooperators")
                  : "normal enemies",
            want.hp_rate, want.resist_rate, want.attack_rate, want.super_armor);
    }
}

// ---- Enemies ----
// Every entity the boss events dope (EMEVD 2004[8] SpEffect 7500 / 7501 in the 1.09 maps;
// the chalice bosses in m29 take theirs from event parameters / Lua_MultiDoping: isSoulGetByBoss).
// The bosses' and the chalice bosses' entity ids come from the party tables (gamedata section bossEntities: derived
// from the event scripts, docs/party/gamedata.md); none without them, so no chr is taken for a boss.
constexpr u64 kGetChrByEntityId = 0x13c97a0;

u64 ChrByEntity(int entity) {
    static const bool code_ok = MatchesOriginal(kGetChrByEntityId, {0x55, 0x48, 0x89, 0xe5});
    if (!code_ok) return 0;
    using Fn = u64(BB_COOP_SYSV*)(int, u64, u64);
    return reinterpret_cast<Fn>(Guest(kGetChrByEntityId))(entity, u64(unsigned(entity)), 0);
}

// The last boss line logged per boss chr handle (only changes are logged; one string for all of
// them logged every line again when a fight had several boss chrs, live pard4 B2: 450 lines).
std::map<std::uint32_t, std::string> g_boss_line;
u64 g_sample = 0;
u32 g_sample_handle = ~0u;
int g_sample_logs = 0;

bool IsPlayer(u64 wcm, u64 chr) {
    const int n = Field<int>(wcm, 0x30, 0);
    const u64 entries = Field<u64>(wcm, 0x38);
    for (int i = 0; entries && n > 0 && n <= 8 && i < n; ++i)
        if (Field<u64>(entries + u64(i) * 0x38, 0) == chr) return true;
    return chr == Field<u64>(wcm, 0x60);
}

// The loaded map's enemies (team 23, entity id chr +0x3B8, HP +0xF8 > 0 = alive, position the
// physics chain ChrPos reads): the census and the host_kill / guest_kill target list.
std::vector<EnemyKillCandidate> LoadedEnemyList(u64 wcm) {
    std::vector<EnemyKillCandidate> out;
    const int sets = std::min(Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2, kMaxSets);
    for (int set = 0; wcm && set < sets; ++set) {
        const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
        const int count = Field<int>(tab, 0, 0);
        const u64 entries = Field<u64>(tab, 8);
        if (!tab || !entries || count <= 0 || count > kMaxPerSet) continue;
        for (int i = 0; i < count; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            if (!chr || IsPlayer(wcm, chr)) continue;
            const u64 npc = Field<u64>(Field<u64>(chr, 0x3c8), 8);
            if (!npc || Field<u8>(npc, 0x12f, 0xff) != kEnemyTeam) continue;
            const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
            const int hp = dm ? Field<int>(dm, 0xf8, -1) : -1;
            if (hp < 0) continue;
            EnemyKillCandidate e;
            e.id = Field<u32>(chr, 0x3b8, 0);
            e.entity = Field<u32>(chr, 0x1f0, 0);
            e.alive = hp > 0;
            e.chr = chr;
            const u64 phys = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
            e.have_pos = phys && SafeRead(phys + 0x1e0, e.pos, 12);
            out.push_back(e);
        }
    }
    return out;
}

// H4 (BB_PARTY_ENEMY_CENSUS=1): every second in a steady world, the loaded map's enemies (team 23,
// entity id chr +0x3B8, alive = HP > 0); a change is logged, and an enemy that was dead and is alive
// again without a load in between is logged as a RESPAWN (only a rest / wipe load may do that).
void CensusTick(const GameSnapshot& s) {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_ENEMY_CENSUS");
        return v && *v && *v != '0';
    }();
    if (!on) return;
    static std::vector<EnemyCensusEntry> prev;
    static bool have = false;
    static u32 map = 0;
    static Clock::time_point next{};
    static int quiet = 0;
    const auto now = Clock::now();
    if (!s.world_up || s.loading || !s.player) {
        if (have && s.loading) Log("enemy census: a load: the next census is a new baseline");
        if (s.loading) have = false;
        return;
    }
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm) return;
    std::vector<EnemyCensusEntry> cur;
    const auto list = LoadedEnemyList(wcm);
    for (const auto& e : list) cur.push_back({e.id, e.alive});
    if (!have || map != s.map_id) {
        const auto d = DiffEnemyCensus({}, cur);
        Log("enemy census: baseline in %s: %d alive, %d dead", MapName(s.map_id).c_str(), d.alive, d.dead);
        prev = cur;
        have = true;
        map = s.map_id;
        quiet = 0;
        return;
    }
    const auto d = DiffEnemyCensus(prev, cur);
    auto ids = [](const std::vector<u32>& v) {
        std::string o;
        for (size_t i = 0; i < v.size() && i < 12; ++i) o += (i ? " " : "") + std::to_string(v[i]);
        if (v.size() > 12) o += " ...";
        return o.empty() ? std::string("-") : o;
    };
    if (!d.respawned.empty()) {
        Log("enemy census: RESPAWN without a load: %zu enemies alive again (%s); %d alive, %d dead",
            d.respawned.size(), ids(d.respawned).c_str(), d.alive, d.dead);
    }
    if (!d.died.empty()) {
        Log("enemy census: %zu died (%s); %d alive, %d dead", d.died.size(), ids(d.died).c_str(), d.alive, d.dead);
        for (size_t i = 0; i < d.died.size() && i < 4; ++i)
            for (const auto& e : list)
                if (e.id == d.died[i])
                    Log("enemy census: %u (entity %u) kill echoes: %s; own Echoes %d", e.id, e.entity,
                        DescribeKillEchoGates(e.chr).c_str(), s.echoes);
    }
    if (d.Changed() || ++quiet >= 15) {
        if (!d.Changed()) Log("enemy census: steady: %d alive, %d dead, 0 respawned", d.alive, d.dead);
        quiet = 0;
    }
    prev = CarryEnemyCensus(prev, cur);
}

// BB_PARTY_AI_LOG=1 (diagnostics, off by default; launcher Diagnostics): who runs an enemy's AI on this
// game. NpcIns::SetManipulator 0x18E6540 picks the chr's active manipulator (+0x60): the local
// ComManipulator (+0x3B8: this game runs the AI) or the NetAIManipulator (+0x3C0: the AI runs on the
// owner's game, this one replays it) from the WorldChrMan +0xA40 owner table and the chr's vfunc +0x4C8
// (enemyai.md). Logs the nearest enemies when their state changes, at most every 2 s.
void AiProbeTick(const GameSnapshot& s) {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_AI_LOG");
        return v && *v && *v != '0';
    }();
    if (!on || !s.world_up || s.loading || !s.player) return;
    static Clock::time_point next{};
    static std::map<u64, std::string> last;
    const auto now = Clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(2);
    const u64 wcm = Slot(kSlotWorldChrMan);
    float me[3];
    if (!wcm || !ChrPosition(s.player, me)) return;
    struct Row { float d; u64 chr; };
    std::vector<Row> rows;
    for (const auto& e : LoadedEnemyList(wcm)) {
        float p[3];
        if (!e.alive || !ChrPosition(e.chr, p)) continue;
        // only the enemies the game has activated (chr entry +0x10 == 4): the others sleep until a player is near
        if (Field<int>(Field<u64>(e.chr, 0x18), 0x10, -1) != 4) continue;
        rows.push_back({std::hypot(p[0] - me[0], p[2] - me[2]), e.chr});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
    // how many of the 12 nearest enemies moved (> 0.3 m) since the last look: the evidence that their AI runs
    {
        static std::map<u64, std::array<float, 3>> seen;
        int moved = 0, n = 0;
        for (const Row& r : rows) {
            if (n >= 12) break;
            float p[3];
            if (!ChrPosition(r.chr, p)) continue;
            ++n;
            auto it = seen.find(r.chr);
            if (it != seen.end() && std::hypot(p[0] - it->second[0], p[2] - it->second[2]) > 0.3f) ++moved;
            seen[r.chr] = {p[0], p[1], p[2]};
        }
        static int last_moved = -1, quiet = 0;
        if (moved != last_moved || ++quiet >= 5) {
            Log("ai: %d of the %d nearest active enemies moved in the last 2 s (nearest %.0f m)", moved, n, rows.empty() ? 0.0 : double(rows[0].d));
            last_moved = moved;
            quiet = 0;
        }
    }
    int logged = 0;
    for (const Row& r : rows) {
        if (logged++ >= 6) break;
        const u64 chr = r.chr;
        const u64 cur = Field<u64>(chr, 0x60), com = Field<u64>(chr, 0x3b8), net = Field<u64>(chr, 0x3c0);
        u64 vt = Field<u64>(chr, 0);
        const u64 fn = vt ? Field<u64>(vt, 0x4c8) : 0;
        const int v4c8 = fn ? int(reinterpret_cast<bool(BB_COOP_SYSV*)(u64)>(fn)(chr)) : -1;
        // the owner table lookup of 0x18E6540 (handle chr +0x90: area 5 bits, index 11 bits)
        int tab = -1;
        const u32 h = Field<u32>(chr, 0x90, ~0u);
        if (h != ~0u && int(h) >= 0) {
            const u64 t = Field<u64>(wcm, 0xa40);
            const u32 a = (h >> 11) & 0x1f;
            tab = 0;
            if (t && int(a) < Field<int>(t, 8)) {
                const u64 t2 = Field<u64>(t, 0x10 + u64(a) * 8);
                if (t2 && int(h & 0x7ff) < Field<int>(t2, 0x10) &&
                    Field<std::int16_t>(Field<u64>(t2, 0x18), (h & 0x7ff) * 2, -1) >= 0)
                    tab = 1;
            }
        }
        // chr +0x18 record: +0x10 / +0x14 modes, +0x20 bit 0 disabled / bit 1 net-controlled, +0x30 sync object (+0x18 bit 0 remote)
        const u64 rec = Field<u64>(chr, 0x18), sync = Field<u64>(rec, 0x30);
        char b[260];
        std::snprintf(b, sizeof b, "manip %s (+0x1ec %u) owner table %d vfunc4c8 %d rec %d/%d/%02x sync %02x anim %d",
                      cur == com ? "Com" : cur == net ? "NetAI" : "other", Field<u32>(chr, 0x1ec, 0), tab, v4c8,
                      Field<int>(rec, 0x10, -1), Field<int>(rec, 0x14, -1), unsigned(Field<u8>(rec, 0x20, 0)),
                      unsigned(Field<u8>(sync, 0x18, 0)),
                      Field<std::int32_t>(Field<u64>(Field<u64>(chr, 0x3b0), 0x18), 0xc0, -9));
        std::string& l = last[chr];
        if (l != b) {
            l = b;
            Log("ai: enemy %u (chr %08x) %.0f m: %s", Field<u32>(chr, 0x1f0, 0), h, double(r.d), b);
        }
    }
}

// 0.7.8 (CL-7fc9fe): the enemy pass of a freshly loaded map (hundreds of chrs, a SpEffect applied to each enemy)
// runs in slices of at most LoadBudgetUs() per frame instead of in one frame; chrs within kNearM of the player
// are always done in the first slice (an enemy at the spawn is scaled before control returns), the far
// ones follow on the next frames. Opt-in (no measurable gain in the 2-client A/B): BB_PARTY_LOAD_SPREAD=1 (or
// bbport.ini party_load_spread=1); BB_PARTY_LOAD_BUDGET_US sets the slice (default 400, 0 = unlimited).
bool LoadSpreadOn() {
    static const bool on = [] {
        if (const char* e = std::getenv("BB_PARTY_LOAD_SPREAD"); e && e[0]) return e[0] != '0';
        const char* cfg = std::getenv("BB_CONFIG");
        if (FILE* f = std::fopen(cfg && cfg[0] ? cfg : "bbport.ini", "r")) {
            char line[256];
            bool v = false;
            while (std::fgets(line, sizeof line, f)) {
                if (std::strncmp(line, "party_load_spread", 17) != 0) continue;
                const char* q = line + 17;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q != '=') continue;
                ++q;
                while (*q == ' ' || *q == '\t') ++q;
                v = *q != '0';
            }
            std::fclose(f);
            return v;
        }
        return false;
    }();
    return on;
}
int LoadBudgetUs() {
    static const int us = [] {
        const char* e = std::getenv("BB_PARTY_LOAD_BUDGET_US");
        return e && e[0] ? std::max(0, std::atoi(e)) : 400;
    }();
    return LoadSpreadOn() ? us : 0;
}
constexpr float kNearM = 40.0f;

// The carried state of a sliced pass (one map): where it stopped and what it counted so far.
struct PassCarry {
    bool active = false;
    u32 map = 0;
    int set = 0, index = 0;
    int seen = 0, enemies = 0, applied = 0, removed = 0, bosses = 0, carrying = 0, counted = 0;
    bool sample_live = false;
    int n_boss = 0;
    u64 boss_chrs[128];
    int slices = 0;
};
PassCarry g_carry;

bool EnemyPass(const GameSnapshot& s, bool scale) {
    PARTY_PERF_SECTION("balance.enemy_pass");
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm || !s.player) return false;
    const int sets = std::min(Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2, kMaxSets);
    const int budget_us = LoadBudgetUs();
    const auto t0 = Clock::now();
    if (!(g_carry.active && g_carry.map == s.map_id)) g_carry = PassCarry{};  // a fresh pass (or another map)
    PassCarry& cy = g_carry;
    int& seen = cy.seen; int& enemies = cy.enemies; int& applied = cy.applied; int& removed = cy.removed;
    int& bosses = cy.bosses; int& carrying = cy.carrying; int& counted = cy.counted;
    bool& sample_live = cy.sample_live;  // the sample chr is still in a chr table (a freed chr keeps its old handle)
    if (!cy.active) {
        // The bosses of the loaded maps, by the entity ids the boss events dope (EMEVD 2004[8] with
        // SpEffect 7500 / 7501, every map; GetChrByEntityId 0x13c97a0, as the events resolve them).
        const gamedata::Tables& gd = gamedata::Get();
        const int cap = int(sizeof cy.boss_chrs / sizeof cy.boss_chrs[0]);
        for (std::uint32_t e : gd.boss_entities)
            if (cy.n_boss < cap)
                if (const u64 c = ChrByEntity(int(e))) cy.boss_chrs[cy.n_boss++] = c;
        if (((s.map_id >> 24) & 0xff) == 29) {
            for (std::uint32_t e : gd.chalice_boss_entities)
                if (cy.n_boss < cap)
                    if (const u64 c = ChrByEntity(int(e))) cy.boss_chrs[cy.n_boss++] = c;
        }
        cy.active = true;
        cy.map = s.map_id;
    }
    const u64* const boss_chrs = cy.boss_chrs;
    const int n_boss = cy.n_boss;
    float me[3] = {0, 0, 0};
    bool have_me = false, have_me_tried = false;
    for (int set = cy.set; set < sets; ++set) {
        const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
        const int count = Field<int>(tab, 0, 0);
        const u64 entries = Field<u64>(tab, 8);
        if (!tab || !entries || count <= 0 || count > kMaxPerSet) { cy.index = 0; continue; }
        for (int i = set == cy.set ? cy.index : 0; i < count; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            if (!chr) continue;
            if (budget_us > 0 && (seen & 7) == 0 &&
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count() >= budget_us) {
                // Over this frame's budget: what is near the player is still done now, the rest next frame.
                if (!have_me_tried) { have_me_tried = true; have_me = LocalPos(s.player, me); }
                float p[3];
                const bool near = have_me && ChrPosition(chr, p) &&
                                  (p[0] - me[0]) * (p[0] - me[0]) + (p[2] - me[2]) * (p[2] - me[2]) <= kNearM * kNearM;
                if (!near) {
                    cy.set = set;
                    cy.index = i;
                    ++cy.slices;
                    return true;
                }
            }
            ++seen;
            if (chr == g_sample) sample_live = true;
            if (IsPlayer(wcm, chr)) continue;
            const u64 npc = Field<u64>(Field<u64>(chr, 0x3c8), 8);
            if (!npc) continue;
            const int team = Field<u8>(npc, 0x12f, 0xff);
            const bool boss_soul = (Field<u8>(npc, 0x149, 0) & 0x10) != 0;
            if (team != kEnemyTeam) continue;
            ++enemies;
            const bool doped = HasEffect(chr, 7500) || HasEffect(chr, 7501) || HasEffect(chr, 7502);
            const bool boss_entity = std::find(boss_chrs, boss_chrs + n_boss, chr) != boss_chrs + n_boss;
            if (doped || boss_soul || boss_entity) {
                ++bosses;
                if (doped) {  // evidence: the boss's HP with the party's doping row (same on every peer)
                    const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
                    const int dope = HasEffect(chr, 7502) ? 7502 : HasEffect(chr, 7501) ? 7501 : 7500;
                    u64 node = 0;
                    HasEffect(chr, dope, &node);
                    const u64 list = Field<u64>(chr, 0x1c8);
                    int poise = 0;  // the engine's poise sum (stateInfo 155 effects only)
                    for (u64 n = Field<u64>(list, 8); n; n = Field<u64>(n, 0x58)) {
                        const u64 row = Field<u64>(n, 0x48);
                        if ((Field<u32>(n, 0x1c) & 0x800c0003u) == 0 && row && Field<std::uint16_t>(row, 0x156) == 155)
                            poise += Field<std::int16_t>(row, 0x13a);
                    }
                    // A boss chr that has not ticked (the guest is far away) keeps its unscaled max HP for
                    // a minute: write the doped max / full health now, so its bar and HP are right at the wake.
                    if (dm && list && g_rate_orig) {
                        int full = 0;
                        const int old_max = Field<int>(dm, 0xfc, -1);
                        // Live (queen3077): on the far guest the 7500 node carries flags 2 (not counted yet), so the
                        // game's rate (14.485) lacks its row's maxHpRate (x1.75); the host counts it (flags 4).
                        float rate = reinterpret_cast<RateFn>(g_rate_orig)(list, 0);
                        const u64 row = Field<u64>(node, 0x48);
                        if ((Field<u32>(node, 0x1c) & 0x800c0003u) != 0 && row) rate *= Field<float>(row, 0x10, 1.0f);
                        const int want = BossHpCatchUp(Field<int>(dm, 0xf8, -1), old_max, Field<int>(dm, 0x100, -1),
                                                       rate, &full);
                        if (want > 0 && SafeWrite(dm + 0xfc, &want, 4) && SafeWrite(dm + 0xf8, &full, 4))
                            Log("boss chr %x: stale max HP %d -> %d (SpEffect %d, the chr has not ticked)",
                                Field<u32>(chr, 8, ~0u), old_max, want, dope);
                    }
                    char b[240];
                    std::snprintf(b, sizeof b, "boss chr %x: SpEffect %d (flags %x): HP %d / %d base %d, rate %.3f, "
                                               "poise bonus %+d (counted)",
                                  Field<u32>(chr, 8, ~0u), dope, Field<u32>(node, 0x1c), Field<int>(dm, 0xf8, -1),
                                  Field<int>(dm, 0xfc, -1), Field<int>(dm, 0x100, -1),
                                  double(list && g_rate_orig ? reinterpret_cast<RateFn>(g_rate_orig)(list, 0) : -1.0f),
                                  poise);
                    if (g_boss_line.size() > 64) {
                        g_boss_line.clear();
                    }
                    std::string& last = g_boss_line[Field<u32>(chr, 8, ~0u)];
                    if (last != b) {
                        last = b;
                        Log("%s", b);
                    }
                }
                // No removal mid-fight (0x17e1d90): bosses never get 7503 (the boss entity tables);
                // one that still carries it is only counted.
                if (HasEffect(chr, int(kEnemyRow))) ++removed;
                continue;
            }
            if (scale && applied < kMaxAppliesPerPass && !HasEffect(chr, int(kEnemyRow))) {
                const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
                const int hp_before = Field<int>(dm, 0xf8, -1), max_before = Field<int>(dm, 0xfc, -1);
                PARTY_PERF_SECTION("balance.enemy_pass.apply");
                if (ApplyEffect(chr, int(kEnemyRow))) {
                    if (!applied) {  // one sample, followed in the log (evidence: max HP before / after)
                        g_sample = chr;
                        sample_live = true;
                        g_sample_handle = Field<u32>(chr, 8, ~0u);
                        Log("sample chr %x before SpEffect %u: HP %d / %d", g_sample_handle, kEnemyRow, hp_before,
                            max_before);
                    }
                    ++applied;
                }
            }
            u64 node = 0;
            if (HasEffect(chr, int(kEnemyRow), &node)) {
                ++carrying;
                if ((Field<u32>(node, 0x1c) & 0x800c0003u) == 0) ++counted;
            }
        }
    }
    char sample[480] = "";
    if (g_sample && sample_live && Field<u32>(g_sample, 8, ~0u) == g_sample_handle) {
        const u64 dm = Field<u64>(Field<u64>(g_sample, 0x3b0), 0x20);
        const u64 npc = Field<u64>(Field<u64>(g_sample, 0x3c8), 8);
        u64 node = 0;
        const bool on = HasEffect(g_sample, int(kEnemyRow), &node);
        const u64 list = Field<u64>(g_sample, 0x1c8);
        const float rate = list && g_rate_orig ? reinterpret_cast<RateFn>(g_rate_orig)(list, 0) : -1.0f;
        std::string effects;
        for (u64 n = Field<u64>(list, 8); n && effects.size() < 120; n = Field<u64>(n, 0x58)) {
            char e[24];
            std::snprintf(e, sizeof e, " %d/%x", Field<int>(n, 0x40, -1), Field<u32>(n, 0x1c));
            effects += e;
        }
        std::snprintf(sample, sizeof sample,
                      "; sample chr %x (NpcParam HP %u): HP %d / %d base %d, rate %.3f, 7503 %s (flags %x, row %s); effects%s",
                      g_sample_handle, Field<u32>(npc, 0x20), Field<int>(dm, 0xf8, -1), Field<int>(dm, 0xfc, -1),
                      Field<int>(dm, 0x100, -1), double(rate), on ? "on" : "off", Field<u32>(node, 0x1c),
                      !node ? "-" : Field<u64>(node, 0x48) == GameParamRow("SpEffectParam", "SP_EFFECT_PARAM_ST", kEnemyRow) ? "7503" : "other", effects.c_str());
    } else {
        g_sample = 0;
    }
    const bool any = applied || removed;
    if (S().log || any || (sample[0] && ++g_sample_logs <= 3))
        Log("enemy pass: %d chrs, %d enemies (%d bosses), SpEffect %u applied to %d, on %d boss(es) from before, carried by %d "
            "(%d counted)%s%s",
            seen, enemies, bosses, kEnemyRow, applied, removed, carrying, counted, sample,
            cy.slices ? (" [spread over " + std::to_string(cy.slices + 1) + " frames]").c_str() : "");
    g_carry = PassCarry{};
    return false;
}

}  // namespace

Rules FromSettings() {
    const auto& v = BbSettings::Get();
    Rules r;
    r.rot = v.party_rot;
    r.boss_hp = v.party_boss_hp;
    r.boss_damage = v.party_boss_damage;
    r.boss_poise = v.party_boss_poise;
    r.enemy_hp = v.party_enemy_hp;
    r.enemy_damage = v.party_enemy_damage;
    r.enemy_poise = v.party_enemy_poise;
    r.friendly_fire = v.party_friendly_fire;
    r.share_lamps = v.party_share_lamps;
    r.guests_bring_lamps = v.party_guests_bring_lamps;
    r.host_revive = v.party_host_revive;
    r.host_limbo_revive = v.party_host_limbo_revive;
    r.kill_record = v.party_kill_record;
    r.npc_mirror = v.party_npc_mirror;
    r.npc_fights = v.party_npc_fights;
    r.skip_prologue = v.party_skip_prologue;
    r.any_kidnap = v.party_any_kidnap;
    r.level_sync = v.party_level_sync;
    r.phantom_hp = v.party_phantom_hp;
    r.pvp_full_hp = v.party_pvp_full_hp;
    r.boss_drops = v.party_boss_drops;
    r.gather = v.party_gather;
    r.guest_gather = v.party_guest_gather;
    r.layer_travel = v.party_layer_travel;
    r.covenant_per_player = v.party_covenant_per_player;
    r.travel_vote = v.party_travel_vote;
    r.rot_cleanse_boss = v.party_rot_cleanse_boss;
    r.rot_cleanse_killer = v.party_rot_cleanse_killer;
    r.rot_cleanse_echoes = v.party_rot_cleanse_echoes;
    // 0.7.4 session rules (party_balance_rules.h): live ones from bbport.ini (env overrides in),
    // the ones read once at start as their modules took them (StartupSessionRules).
    r.open_world = v.party_open_world;
    r.guest_revive_inplace = v.party_guest_revive_inplace;
    r.guest_respawn = v.party_guest_respawn;
    r.guest_refill = v.party_guest_refill;
    r.chalice_rejoin = v.party_chalice_rejoin;
    r.story_mirror = v.party_story_mirror;
    const StartupRules st = StartupSessionRules();
    r.guest_lamps = st.guest_lamps;
    r.guest_mark = st.guest_mark;
    r.full_rewards = st.full_rewards;
    return Clamped(r);
}

StartupRules StartupSessionRules() {
    // As the modules read them at start: seamless_rules' Hunter's Mark rows and party_items' parity
    // patches the env BbSettings::Load exported (on unless "0"), party_phantom's boss Insight,
    // party_director's start mode, party_guestsvc's lamps (its own startup value).
    const auto env_on = [](const char* name) {
        const char* e = std::getenv(name);
        return !(e && e[0] == '0');
    };
    static const StartupRules once = [&] {
        StartupRules r;
        r.guest_lamps = coop::GuestLampsAtStart();
        r.guest_mark = env_on("BB_PARTY_GUEST_MARK");
        r.full_rewards = env_on("BB_PARTY_FULL_REWARDS");
        const coop::InsightMode m = coop::InsightModeFromString(std::getenv("BB_PARTY_GUEST_INSIGHT"));
        r.guest_insight = m == coop::InsightMode::Full ? 1 : m == coop::InsightMode::Off ? 2 : 0;
        r.start_immediate = coop::ParseStartMode(std::getenv("BB_PARTY_START")) == coop::StartMode::Immediate;
        return r;
    }();
    return once;
}

bool HostRules(Rules* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_have_host) return false;
    if (out) *out = g_host;
    return true;
}

Rules InForce() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_have_host) return g_host;
    }
    return FromSettings();
}

bool OnHostRules(const std::string& json, std::string* error) {
    Rules r;
    if (!FromJson(json, &r, error)) return false;
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        changed = !g_have_host || g_host != r;
        g_host = r;
        g_have_host = true;
    }
    if (changed) Log("the host's rules: %s", ToJson(r).c_str());
    return true;
}

int LocalDeaths() {
    return g_deaths.load();
}

int LocalRotStacks() {
    return InForce().rot ? g_stacks.load() : 0;
}

float RotDropDiscovery(float own) {
    const int n = g_kind[kRotDiscovery].load(std::memory_order_relaxed);
    if (n <= 0 || !g_kind_hooked[kRotDiscovery] || !InForce().rot) {
        return own;
    }
    return Note(kRotDiscovery, RotItemDiscovery(own, n));
}

void NoteLampMenuOpened() {
    g_lamp_menu_opened = true;
}

void NoteBossDefeated(std::uint32_t entity, const char* from) {
    if (!On()) return;
    CleanseEvent e;
    e.why = rot::Why::Boss;
    e.entity = entity;
    e.from = from ? from : "";
    QueueCleanse(std::move(e));
}

void NotePartyKill(std::uint32_t entity, const char* from) {
    if (!On() || !entity) return;
    {
        // Only a killer's death matters (most kills are someone else's enemy).
        std::lock_guard<std::mutex> lk(g_rot_mu);
        bool killer = false;
        for (const rot::Death& d : g_ledger.deaths()) killer = killer || (d.killer_link && d.killer.entity == entity);
        if (!killer) return;
    }
    CleanseEvent e;
    e.why = rot::Why::Killer;
    e.entity = entity;
    e.from = from ? from : "";
    QueueCleanse(std::move(e));
}

void NoteKillWorld(bool follows_host_world, bool new_world, const char* from) {
    g_kill_world_followed = follows_host_world;
    if (!On() || !new_world) return;
    CleanseEvent e;
    e.reset = true;
    e.from = from ? from : "";
    QueueCleanse(std::move(e));
}

void NoteEchoesRetrieved(const char* from) {
    if (!On()) return;
    CleanseEvent e;
    e.why = rot::Why::Echoes;
    e.from = from ? from : "";
    QueueCleanse(std::move(e));
}

void Init() {
    if (!On()) {
        Log("off (BB_PARTY_BALANCE=0 or no party)");
        return;
    }
    LoadRot();
    S().log = std::getenv("BB_PARTY_BALANCE_LOG") && std::getenv("BB_PARTY_BALANCE_LOG")[0] == '1';
    if (ReplacePrologue(kMaxHpRate, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50}, reinterpret_cast<const void*>(&RateHook),
                        &g_rate_orig, "balance: max HP rate (rot)")) {
        g_hooked = true;
    }
    g_kind_hooked[kRotHealth] = g_hooked;
    // The other rot kinds (ERSC has five): rate functions of the SpEffect list (1.09 eboot).
    g_kind_hooked[kRotStamina] = ReplacePrologue(kMaxStaminaRate, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10},
                                                 reinterpret_cast<const void*>(&StaminaRateHook),
                                                 &g_kind_orig[kRotStamina], "balance: max stamina rate (rot)");
    // Stamina cost (0.6.2): charged by the tick (RotExtraStaminaDrain), not by a game hook: the
    // SpEffect consumeStaminaRate getter 0x17e4c50 is never asked for a roll or a swing (live
    // 2026-10-15: 0 calls). BB_PARTY_ROT_STAMINA_COST=0 leaves the kind out.
    g_kind_hooked[kRotStaminaCost] = !EnvIsZero("BB_PARTY_ROT_STAMINA_COST");
    // Rally (0.6.2): the SpEffect regainRate getter 0x17e5590 was never asked while a guest fought
    // and killed (live 2026-10-15, 0 calls), so the tick takes back part of each HP gain instead
    // (RotRallyTakeBack), only for gains inside the rally window after damage
    // (RotRallyGainInWindow). Live 2026-10-15 (parfin c4, 2 stacks): 44 HP regained in 4 gains,
    // 13 taken back (30 %). On by default; BB_PARTY_ROT_RALLY=0 leaves the kind out.
    g_kind_hooked[kRotRally] = !EnvIsZero("BB_PARTY_ROT_RALLY");
    // Item discovery (0.6.2): the itemDropRate sum 0x17e8010 is never asked (0 calls over guest
    // kills with drops), but every enemy drop roll's request carries the player's discovery
    // (party_droproll, request +4): RotDropDiscovery takes the stacks off there. On by default;
    // BB_PARTY_ROT_DISCOVERY=0 leaves the kind out. BB_PARTY_ROT_ALL_KINDS=1 still hooks the sum
    // (RE only: it adds nothing the roll uses).
    g_kind_hooked[kRotDiscovery] = !EnvIsZero("BB_PARTY_ROT_DISCOVERY");
    const char* all = std::getenv("BB_PARTY_ROT_ALL_KINDS");
    if (all && all[0] == '1' && g_kind_hooked[kRotDiscovery]) {
        g_kind_hooked[kRotDiscovery] = ReplacePrologue(0x17e8010, {0x48, 0x8b, 0x47, 0x08, 0xc5, 0xf8, 0x57, 0xc0},
                                                       reinterpret_cast<const void*>(&DiscoveryHook),
                                                       &g_kind_orig[kRotDiscovery], "balance: item discovery sum (rot)");
    }
    SetLampUseObserver(&OnLampUse);
    // 0.7.2 "cleansed by killing your killer": who dealt the local player's lethal hit.
    const bool killer_hooked = ReplacePrologue(kKilledByLog, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56},
                                               reinterpret_cast<const void*>(&KilledByHook), &g_killed_by_orig,
                                               "balance: kill log (rot: the killer)");
    if (!killer_hooked) Log("the kill log is not hooked: no killer is known (rot cleansed by the killer's death: never)");
    std::string kinds;
    for (int k = 0; k < kRotKinds; ++k)
        kinds += std::string(" ") + kRotKindName[k] + (g_kind_hooked[k] ? "" : "(NOT hooked)") + "=" +
                 std::to_string(g_kind[k].load());
    std::size_t characters = 0;
    {
        std::lock_guard<std::mutex> lk(g_rot_mu);
        characters = g_rot_file.slots.size();
    }
    Log("on: rot kinds%s; rot kept for %zu character(s) (by save slot; it comes in when a save is loaded)",
        kinds.c_str(), characters);
}

// H3 (party_enemy_census.h KillEchoTracker): every 0.5 s in a party session, an enemy seen alive
// and then dead whose kill-echo payer has not run 3 s later gets it called here, on each player's
// game: the payer pays its own player (a guest the full kill echoes, items.md 5) and marks the chr
// paid, so the game's own later call pays nothing twice. BB_PARTY_KILL_ECHOES=0: off.
void KillEchoTick(const GameSnapshot& s) {
    static const bool on = !EnvIsZero("BB_PARTY_KILL_ECHOES");
    static KillEchoTracker tracker;
    static Clock::time_point next{};
    static const auto t0 = Clock::now();
    const bool party = (s.session_role == RoleHost && s.cooperators > 0) || s.session_role == RoleClient;
    if (!on) return;
    if (!s.world_up || s.loading || !s.player || !party) {
        tracker.Reset();
        return;
    }
    const auto now = Clock::now();
    if (now < next) return;
    next = now + std::chrono::milliseconds(500);
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm) return;
    std::vector<KillEchoObs> obs;
    for (const auto& e : LoadedEnemyList(wcm)) {
        KillEchoObs o;
        o.chr = e.chr;
        o.alive = e.alive;
        SafeRead(e.chr + 0x1e1, &o.flags, 5);
        obs.push_back(o);
    }
    // H3 diagnostics (0.6.2): every kill seen here (alive -> HP 0), its payer gates and the own
    // Echoes at the death and 3.5 s later, whoever paid (a real weapon kill by another player).
    {
        static std::map<u64, bool> was_alive;
        struct Later {
            Clock::time_point at;
            u64 chr;
            int echoes;  // at the death (for the log)
        };
        static std::vector<Later> later;
        std::map<u64, bool> now_alive;
        for (const auto& o : obs) {
            now_alive[o.chr] = o.alive;
            const auto w = was_alive.find(o.chr);
            if (!o.alive && w != was_alive.end() && w->second) {
                Log("kill seen: entity %u died here: %s; own Echoes %d", Field<u32>(o.chr, 0x1f0, 0),
                    DescribeKillEchoGates(o.chr).c_str(), ReadGameState(false).echoes);
                later.push_back({now + std::chrono::milliseconds(3500), o.chr, ReadGameState(false).echoes});
            }
        }
        was_alive.swap(now_alive);
        for (auto it = later.begin(); it != later.end();) {
            if (now >= it->at) {
                const int e_now = ReadGameState(false).echoes;
                Log("kill seen: entity %u 3.5 s later: %s; own Echoes %d (%+d since)", Field<u32>(it->chr, 0x1f0, 0),
                    DescribeKillEchoGates(it->chr).c_str(), e_now, e_now - it->echoes);
                it = later.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const u64 chr : tracker.Update(obs, std::chrono::duration<double>(now - t0).count())) {
        const int before = ReadGameState(false).echoes;
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(Guest(0x18c5070))(chr);
        Log("kill echoes: entity %u paid by the party (not paid 3 s after its death): own Echoes %d -> %d; %s",
            Field<u32>(chr, 0x1f0, 0), before, ReadGameState(false).echoes, DescribeKillEchoGates(chr).c_str());
    }
}

// Test aid (ngfix, BB_PARTY_TEST_ENEMY_HP=1): every 5 s in a steady world, the max HP of the loaded
// enemies with the lowest map entity ids (the same ids on every game) and the live NG cycle
// (GameDataMan +0x68), logged when it changes: the same enemy's HP compared across the party.
void EnemyHpTestTick(const GameSnapshot& s) {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_TEST_ENEMY_HP");
        return v && *v == '1';
    }();
    static Clock::time_point next{};
    static std::string last;
    if (!on || !s.world_up || s.loading || !s.player) return;
    const auto now = Clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(5);
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm) return;
    std::vector<std::pair<u32, u64>> list;
    for (const auto& e : LoadedEnemyList(wcm))
        if (e.entity) list.push_back({e.entity, e.chr});
    std::sort(list.begin(), list.end());
    std::string line;
    for (std::size_t i = 0; i < list.size() && i < 6; ++i) {
        const u64 dm = Field<u64>(Field<u64>(list[i].second, 0x3b0), 0x20);
        char b[64];
        std::snprintf(b, sizeof b, " %u max HP %d (base %d);", list[i].first, Field<int>(dm, 0xfc, -1),
                      Field<int>(dm, 0x100, -1));
        line += b;
    }
    if (line.empty() || line == last) return;
    last = line;
    Log("test: enemy HP in %s, live NG cycle %d:%s", MapName(s.map_id).c_str(),
        int(Field<u32>(s.game_data_man, 0x68, 0xffffffffu)), line.c_str());
}

/// Test aid (BB_PARTY_CHR_DUMP=R, prologue NPC check): every non-player chr within R m (default
/// 40) of the local player - entity id (+0x1F0), handle, team, HP, position, disabled / draw bits -
/// once the world has stood 4 s, again whenever that set changes (a chr enabled, disabled, killed,
/// added, hurt): the host's and a guest's lists side by side show a chr only one game has.
void ChrDumpTick(const GameSnapshot& s) {
    static const float radius = [] {
        const char* v = std::getenv("BB_PARTY_CHR_DUMP");
        return v && v[0] && v[0] != '0' ? (std::atof(v) > 1.0 ? float(std::atof(v)) : 40.0f) : 0.0f;
    }();
    if (radius <= 0) return;
    static Clock::time_point steady_since{}, next{};
    static std::string last_sig;
    const auto now = Clock::now();
    if (!s.world_up || s.loading || !s.player) {
        steady_since = {};
        return;
    }
    if (steady_since == Clock::time_point{}) steady_since = now;
    if (now - steady_since < std::chrono::seconds(4) || now < next) return;
    next = now + std::chrono::seconds(1);
    const u64 wcm = Slot(kSlotWorldChrMan);
    float me[3] = {};
    if (!wcm || !ChrPosition(s.player, me)) return;
    const int sets = std::min(Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2, kMaxSets);
    std::vector<std::string> lines;
    std::string sig;
    for (int set = 0; set < sets; ++set) {
        const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
        const int count = Field<int>(tab, 0, 0);
        const u64 entries = Field<u64>(tab, 8);
        if (!tab || !entries || count <= 0 || count > kMaxPerSet) continue;
        for (int i = 0; i < count; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            float p[3] = {};
            if (!chr || IsPlayer(wcm, chr) || !ChrPosition(chr, p)) continue;
            const float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!(d <= radius)) continue;
            const u64 npc = Field<u64>(Field<u64>(chr, 0x3c8), 8);
            const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
            const int entity = Field<int>(chr, 0x1f0, -1), hp = Field<int>(dm, 0xf8, -1);
            const bool off = (Field<u8>(Field<u64>(chr, 0x18), 0x20) & 1) || (Field<std::uint16_t>(Field<u64>(chr, 0x58), 0x114) & 0x40);
            char b[240];
            // team: the live one (ChrIns +0x88, EMEVD 2004[2] changes it; pvp.md), row: the NpcParam's
            std::snprintf(b, sizeof b, "entity %d chr %x team %d (row %d) type %d HP %d/%d at (%.1f, %.1f, %.1f) %.1f m%s",
                          entity, Field<u32>(chr, 8, ~0u), Field<int>(chr, 0x88, -1),
                          npc ? int(Field<u8>(npc, 0x12f, 0xff)) : -1, Field<int>(chr, 0x78, -1), hp, Field<int>(dm, 0xfc, -1),
                          double(p[0]), double(p[1]), double(p[2]), double(d), off ? " DISABLED" : "");
            lines.push_back(b);
            char k[48];
            // HP and team too (D11: an NPC hunter's HP on every game during a fight, its team switch
            // 2004[2] mid-fight), at most one dump a second
            // and the height in 4 m steps (0.7.6: a chr falling out of the world, Annalise's chamber)
            std::snprintf(k, sizeof k, "%d/%d/%d/%d/%d;", entity, hp, off ? 1 : 0, Field<int>(chr, 0x88, -1),
                          int(std::floor(p[1] / 4.0f)));
            sig += k;
        }
    }
    if (sig == last_sig) return;
    last_sig = sig;
    Log("chr dump (%s, within %.0f m of (%.1f, %.1f, %.1f)): %zu chr(s)", MapName(s.map_id).c_str(), double(radius),
        double(me[0]), double(me[1]), double(me[2]), lines.size());
    for (const std::string& l : lines) Log("chr dump: %s", l.c_str());
}

// ---- NPC presence mirror (party_npc_mirror.h) ----
bool ChrOff(u64 chr) {
    return (Field<u8>(Field<u64>(chr, 0x18), 0x20) & 1) || (Field<std::uint16_t>(Field<u64>(chr, 0x58), 0x114) & 0x40);
}

struct NpcMirrorState {
    std::mutex mu;
    std::uint32_t host_map = 0;  // the host's last list (guest), under mu
    std::vector<std::uint32_t> host_off;
    Clock::time_point host_at{};
    std::vector<std::uint32_t> ours;  // main thread: chrs this mirror turned off
    std::uint32_t ours_map = 0;
    Clock::time_point steady_since{}, next{}, next_send{};
    std::string last_sent;
};
NpcMirrorState& NM() {
    static NpcMirrorState st;
    return st;
}

void NpcMirrorTick(const GameSnapshot& s, party::PartyLink* link, bool host) {
    // The host's rule (System > Party rules: World, bbport.ini party_npc_mirror, BB_PARTY_NPC_MIRROR at
    // start): the host's own setting, a guest follows the host's (party_balance Rules). Live.
    const bool on = host ? BbSettings::Get().party_npc_mirror.load()
                         : party::status::HostRuleOn(party::status::HostRule::NpcMirror);
    NpcMirrorState& st = NM();
    const auto now = Clock::now();
    if (!on && !st.ours.empty()) {
        // Turned off: the NPCs this mirror turned off stand again (the game's own state).
        using SetDisableFn = void(BB_COOP_SYSV*)(u64, u8);
        const u64 wcm = Slot(kSlotWorldChrMan);
        const bool here = wcm && s.world_up && !s.loading && s.map_id == st.ours_map;
        for (std::uint32_t id : st.ours)
            if (const u64 chr = here ? ChrByEntity(int(id)) : 0; chr && !IsPlayer(wcm, chr))
                reinterpret_cast<SetDisableFn>(Guest(0x18c6390))(chr, 0);
        Log("NPC mirror: off: %zu NPC(s) it turned off are on again%s", st.ours.size(), here ? "" : " (after a load)");
        st.ours.clear();
    }
    const bool steady = on && link && s.world_up && !s.loading && s.player && s.map_id != 0xffffffffu;
    if (!steady || (!host && s.session_role != 6) || s.map_id != st.ours_map) {
        st.steady_since = {};
        st.ours.clear();  // a load rebuilds the chrs
        st.ours_map = s.map_id;
        if (!steady || (!host && s.session_role != 6)) return;
    }
    if (st.steady_since == Clock::time_point{}) st.steady_since = now;
    if (now - st.steady_since < std::chrono::seconds(3) || now < st.next) return;
    st.next = now + std::chrono::milliseconds(500);
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm) return;
    if (host) {
        // The non-player, non-enemy chrs with a map entity id that are off in the host's world.
        std::vector<std::uint32_t> off;
        const int sets = std::min(Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2, kMaxSets);
        for (int set = 0; set < sets && off.size() < npcmirror::kMaxIds; ++set) {
            const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
            const int count = Field<int>(tab, 0, 0);
            const u64 entries = Field<u64>(tab, 8);
            if (!tab || !entries || count <= 0 || count > kMaxPerSet) continue;
            for (int i = 0; i < count && off.size() < npcmirror::kMaxIds; ++i) {
                const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
                if (!chr || IsPlayer(wcm, chr) || !ChrOff(chr)) continue;
                const int entity = Field<int>(chr, 0x1f0, -1);
                const u64 npc = Field<u64>(Field<u64>(chr, 0x3c8), 8);
                if (entity < 1000000 || entity > 99999999 || (npc && Field<u8>(npc, 0x12f, 0xff) == kEnemyTeam)) continue;
                if (!npcmirror::Mirrored(std::uint32_t(entity))) continue;  // NPCs only: never a lamp's talk chr
                off.push_back(std::uint32_t(entity));
            }
        }
        std::sort(off.begin(), off.end());
        const std::string j = npcmirror::ToJson(s.map_id, off);
        if (j != st.last_sent || now >= st.next_send) {
            link->post_event(party::kBroadcast, npcmirror::kEventName, j);
            if (j != st.last_sent) Log("NPC mirror: %zu NPC(s) off in the host's world (%s) sent to the party", off.size(), MapName(s.map_id).c_str());
            st.last_sent = j;
            st.next_send = now + std::chrono::seconds(10);
        }
        return;
    }
    std::vector<std::uint32_t> host_off;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.host_map != s.map_id || now - st.host_at > std::chrono::seconds(30)) return;
        host_off = st.host_off;
    }
    std::vector<npcmirror::LocalNpc> local;
    std::vector<u64> chrs;
    auto add = [&](std::uint32_t id, bool ours) {
        for (const auto& n : local)
            if (n.entity == id) return;
        const u64 chr = ChrByEntity(int(id));
        if (!chr || IsPlayer(wcm, chr)) return;
        const u64 npc = Field<u64>(Field<u64>(chr, 0x3c8), 8);
        if (npc && Field<u8>(npc, 0x12f, 0xff) == kEnemyTeam) return;  // enemies: the census / kill record
        local.push_back({id, !ChrOff(chr), ours});
        chrs.push_back(chr);
    };
    for (std::uint32_t id : st.ours) add(id, true);
    for (std::uint32_t id : host_off) add(id, false);
    const npcmirror::Plan plan = npcmirror::PlanMirror(host_off, local);
    using SetDisableFn = void(BB_COOP_SYSV*)(u64, u8);
    const auto set = [&](std::uint32_t id, bool off) {
        for (std::size_t k = 0; k < local.size(); ++k)
            if (local[k].entity == id) reinterpret_cast<SetDisableFn>(Guest(0x18c6390))(chrs[k], off ? 1 : 0);
    };
    for (std::uint32_t id : plan.disable) {
        set(id, true);
        if (std::find(st.ours.begin(), st.ours.end(), id) == st.ours.end()) st.ours.push_back(id);
        Log("NPC mirror: NPC %u is not in the host's world (%s): turned off here too", id, MapName(s.map_id).c_str());
    }
    for (std::uint32_t id : plan.enable) {
        set(id, false);
        st.ours.erase(std::remove(st.ours.begin(), st.ours.end(), id), st.ours.end());
        Log("NPC mirror: NPC %u is back in the host's world: on again here", id);
    }
}

/// 0.7.2 (party_rot_rules.h): the loads, the killers this game sees die, and the queued cleanse
/// events (a boss, a party kill, the bloodstain), applied to the stacks.
void RotCleanseTick(const GameSnapshot& s, const Rules& r, bool host, bool in_world, Clock::time_point now) {
    static bool was_loading = false;
    if (s.loading && !was_loading) {
        // A load on this game: new chr instances. The host's load is a new world for everyone (a
        // rest, a death, a travel: every enemy respawned); a guest's own load is not, unless its
        // kill record does not follow the host's world (then every load counts: no way to tell).
        std::lock_guard<std::mutex> lk(g_rot_mu);
        const std::size_t had = g_ledger.deaths().size();
        g_ledger.OnLocalLoad();
        const bool reset = host || !g_kill_world_followed.load();
        if (reset) g_ledger.OnWorldReset();
        if (had) Log("rot: a load (%s): %zu death link(s) -> %zu", reset ? "a new world" : "the host's world stays", had,
                     g_ledger.deaths().size());
    }
    was_loading = s.loading;
    std::vector<CleanseEvent> due;
    {
        std::lock_guard<std::mutex> lk(g_cleanse_mu);
        due.swap(g_cleanse_q);
    }
    // A killer this game has loaded and sees dead now (its own chr, the same census key).
    static Clock::time_point next_poll{};
    if (in_world && now >= next_poll) {
        next_poll = now + std::chrono::milliseconds(250);
        std::vector<rot::Death> deaths;
        {
            std::lock_guard<std::mutex> lk(g_rot_mu);
            deaths = g_ledger.deaths();
        }
        for (const rot::Death& d : deaths) {
            if (!d.killer_link || !d.killer.chr) continue;
            if (Field<u32>(d.killer.chr, 0x3b8, 0) != d.killer.key || EnemyHp(d.killer.chr) != 0) continue;
            CleanseEvent e;
            e.why = rot::Why::Killer;
            e.entity = d.killer.entity;
            e.chr = d.killer.chr;
            e.from = "dead on this game";
            due.push_back(e);
        }
    }
    for (const CleanseEvent& e : due) {
        if (e.reset) {
            std::lock_guard<std::mutex> lk(g_rot_mu);
            const std::size_t had = g_ledger.deaths().size();
            g_ledger.OnWorldReset();
            if (had != g_ledger.deaths().size()) Log("rot: %s: a new world, the killers respawned", e.from.c_str());
            continue;
        }
        rot::Toggles t;
        t.rot = r.rot;
        t.boss = r.rot_cleanse_boss;
        t.killer = r.rot_cleanse_killer;
        t.echoes = r.rot_cleanse_echoes;
        int taken = 0;
        int links = 0;  // the death links this event used up
        const auto count_links = [] {
            int n = 0;
            for (const rot::Death& d : g_ledger.deaths()) n += int(d.killer_link) + int(d.stain_link);
            return n;
        };
        {
            std::lock_guard<std::mutex> lk(g_rot_mu);
            links = count_links();
            int stacks[kRotKinds];
            for (int k = 0; k < kRotKinds; ++k) stacks[k] = g_kind[k].load();
            taken = rot::Apply(e.why, t, &g_ledger, stacks, e.entity, e.chr);
            int sum = 0;
            for (int k = 0; k < kRotKinds; ++k) {
                g_kind[k] = stacks[k];
                sum += stacks[k];
            }
            g_stacks = sum;
            links -= count_links();
        }
        const char* what = e.why == rot::Why::Boss     ? "a boss died"
                           : e.why == rot::Why::Killer ? "an enemy died"
                                                       : "the bloodstain was picked up";
        if (taken > 0) {
            char b[96];
            std::snprintf(b, sizeof b, rot::NoticeText(e.why, taken), taken);
            party::status::SetLastEvent(b);
            Log("rot: %s (entity %u, %s): %d stack(s) off, %d left; notice: %s", what, e.entity, e.from.c_str(), taken,
                g_stacks.load(), b);
            SaveRot();
        } else if (links > 0 || (e.why == rot::Why::Boss && g_stacks.load() > 0)) {
            Log("rot: %s (entity %u, %s): nothing off (rot %s; cleanse boss %s, killer %s, echoes %s; %d stack(s))", what,
                e.entity, e.from.c_str(), r.rot ? "on" : "off", r.rot_cleanse_boss ? "on" : "off",
                r.rot_cleanse_killer ? "on" : "off", r.rot_cleanse_echoes ? "on" : "off", g_stacks.load());
            SaveRot();  // a link used up (the stain's kind in party_rot.txt)
        }
    }
}


// ---- 0.7.5 level sync (party_levelsync_rules.h) ----
// Per player chr every 0.5 s: does it carry SpEffect 9200 (the game's level sync) as the host's rule
// says? BB_PARTY_LEVELSYNC_LOG=1 logs each player's level, the range, the effect and the stats every
// 5 s; BB_PARTY_LEVELSYNC_OBSERVE=1 only logs (the game's own behaviour, for the A/B baseline).
constexpr u64 kSlotGameDataMan = 0x553b130, kSlotClientMan = 0x5540288;

u64 PlayerGameDataOf(u64 chr) {
    u64 vt = 0, fn = 0;
    if (!chr || !SafeGet(chr, &vt) || !vt || !SafeGet(vt + 0x1c8, &fn) || !fn) return 0;
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(fn)(chr);
}

/// The first game-data record with +0x10 == 1 (the game's "host" record, 0x15077c0): the local one, the
/// four session records (stride 0x6a0), then the 40 pointers.
u64 HostRecord() {
    const u64 gdm = Slot(kSlotGameDataMan);
    if (!gdm) return 0;
    const u64 local = Field<u64>(gdm, 8);
    if (local && Field<int>(local, 0x10, -1) == 1) return local;
    const u64 four = Field<u64>(gdm, 0x10);
    for (int i = 0; four && i < 4; ++i)
        if (Field<int>(four + u64(i) * 0x6a0, 0x10, -1) == 1) return four + u64(i) * 0x6a0;
    const u64 table = Field<u64>(gdm, 0x20);
    for (int i = 0; table && i < 0x28; ++i) {
        const u64 rec = Field<u64>(table, u64(i) * 8);
        if (rec && Field<int>(rec, 0x10, -1) == 1) return rec;
    }
    return 0;
}

bool RemoveEffectNode(u64 chr, u64 node) {
    const u64 list = Field<u64>(chr, 0x1c8);
    if (!list || !node) return false;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u32)>(Guest(0x17e1d90))(list, node, 1);
    return true;
}

int CountEffect(u64 chr, int id, u64* first = nullptr) {
    int n = 0;
    u64 node = Field<u64>(Field<u64>(chr, 0x1c8), 8);
    for (int guard = 0; node && guard < 256; ++guard) {
        if (Field<int>(node, 0x40, -1) == id) {
            if (!n && first) *first = node;
            ++n;
        }
        node = Field<u64>(node, 0x58);
    }
    return n;
}

void LevelSyncTick(bool rule_on, bool phantom_hp_on, Clock::time_point now) {
    static Clock::time_point next{}, next_log{};
    static bool saved_range = false, wrote_range = false;
    static u32 orig_pct = 0, orig_add = 0;
    static const bool observe = [] { const char* v = std::getenv("BB_PARTY_LEVELSYNC_OBSERVE"); return v && *v && *v != '0'; }();
    static const bool logging = [] { const char* v = std::getenv("BB_PARTY_LEVELSYNC_LOG"); return v && *v && *v != '0'; }();
    if (now < next) return;
    next = now + std::chrono::milliseconds(500);
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm) return;
    const u64 host = HostRecord();
    const int host_level = host ? Field<int>(host, 0x90, -1) : -1;
    const u64 cm = Field<u64>(Slot(kSlotClientMan), 0x18);
    // The matching range (SprjNetworkClientMan +0x18: +0x8e4 percent, +0x8e0 levels) is an unlimited
    // placeholder here (no matching server): with the rule on a party runs the game's normal range, the
    // game's own step and its stat curves (0x1582940) read it; off, the placeholder is put back.
    if (cm && !observe) {
        const u32 pct = Field<u32>(cm, 0x8e4), add = Field<u32>(cm, 0x8e0);
        if (rule_on && (pct != levelsync::kPartyRangePercent || add != levelsync::kPartyRangeAdd)) {
            if (!saved_range) orig_pct = pct, orig_add = add, saved_range = true;
            const u32 p = levelsync::kPartyRangePercent, a = levelsync::kPartyRangeAdd;
            SafeWrite(cm + 0x8e4, &p, 4);
            SafeWrite(cm + 0x8e0, &a, 4);
            wrote_range = true;
            Log("level sync: matching range +%u %% +%u levels (was +%u %% +%u)", p, a, pct, add);
        } else if (!rule_on && wrote_range) {
            SafeWrite(cm + 0x8e4, &orig_pct, 4);
            SafeWrite(cm + 0x8e0, &orig_add, 4);
            wrote_range = false;
            Log("level sync: matching range back to +%u %% +%u levels", orig_pct, orig_add);
        }
    }
    const float mult = levelsync::RangeMultiplier(cm != 0, Field<u32>(cm, 0x8e4));
    const float add = levelsync::RangeAdd(cm != 0, Field<u32>(cm, 0x8e0));
    const bool do_log = logging && now >= next_log;
    if (do_log) next_log = now + std::chrono::seconds(5);
    u64 chrs[9];
    int n = 0;
    chrs[n++] = Field<u64>(wcm, 0x60);
    const int count = Field<int>(wcm, 0x30, 0);
    const u64 entries = Field<u64>(wcm, 0x38);
    for (int i = 0; entries && count > 0 && count <= 8 && i < count && n < 9; ++i) {
        const u64 c = Field<u64>(entries + u64(i) * 0x38, 0);
        if (c && c != chrs[0]) chrs[n++] = c;
    }
    for (int i = 0; i < n; ++i) {
        const u64 chr = chrs[i];
        if (!chr) continue;
        const u64 pgd = PlayerGameDataOf(chr);
        if (!pgd) continue;
        const int level = Field<int>(pgd, 0x90, -1);
        const int kind = Field<int>(pgd, 0x10, -1);
        const bool eligible = host && pgd != host && kind >= 2 && kind < 0x7fff;
        const bool above = eligible && levelsync::AboveRange(level, host_level, mult, add);
        u64 node = 0;
        const int has = CountEffect(chr, levelsync::kLevelSyncEffect, &node);
        const levelsync::Action act = levelsync::Decide(rule_on, eligible, above, has);
        const char* did = "";
        if (!observe) {
            if (act == levelsync::Action::Apply) {
                did = ApplyEffect(chr, levelsync::kLevelSyncEffect) ? ", applied" : ", apply failed";
            } else if (act == levelsync::Action::Remove) {
                did = RemoveEffectNode(chr, node) ? ", removed" : ", remove failed";
            }
        }
        if (do_log || *did) {
            const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
            // The defence rates the damage step multiplies in (0x17e76b0(list, out): physical, magic, fire,
            // thunder rate products; the level sync's CalcCorrectGraph 121 is in them).
            alignas(16) float def[8] = {};
            if (const u64 list = Field<u64>(chr, 0x1c8); list && i == 0)
                reinterpret_cast<void(BB_COOP_SYSV*)(u64, float*)>(Guest(0x17e76b0))(list, def);
            char b[440];
            std::snprintf(b, sizeof b,
                          "defence %.3f/%.3f, chr %x%s: level %d, record kind %d, host level %d, range x%.2f +%.0f, %s, 9200 x%d%s, "
                          "9006 %s 9026 %s, HP %d/%d, stamina max %d, rule %s",
                          double(def[0]), double(def[1]), Field<u32>(chr, 8, ~0u), i == 0 ? " (local)" : "", level, kind, host_level, double(mult),
                          double(add), above ? "ABOVE the range" : "within", has, did,
                          HasEffect(chr, 9006) ? "on" : "off", HasEffect(chr, 9026) ? "on" : "off",
                          Field<int>(dm, 0xf8, -1), Field<int>(dm, 0xfc, -1), Field<int>(dm, 0x138, -1),
                          rule_on ? "on" : "off");
            Log("level sync: %s (phantom health %s)", b, phantom_hp_on ? "on" : "off");
        }
    }
}

/// Cainhurst, Queen Annalise (entity 2500600; NW-a1c18ef, 0.7.7): her init event 12500010 sets her map hit
/// off and forces the throne animation 103010 once at the load. In a party the lamp's menu (the rest) resets
/// the chr and she drops through her throne into the void, on the host's and on every guest's game (solo
/// she stays; live anl run: y 130.5 -> 87 within 3 s, 2 s after the menu opened). This tick remembers her
/// height once the world has stood and, when she sinks, forces the animation again (Lua ForcePlayAnimation
/// 0x13CBFD0 as the event does; it holds her where she is, nothing moves her back). BB_PARTY_ANNALISE_SIT=0 switches it off.
struct SitState {
    Clock::time_point steady_since{}, next{}, last_fix{};
    std::uint32_t map = 0;
    float home[3] = {}, prev_y = 0;
    bool have_home = false;
    int fixes = 0;
};

void AnnaliseSitTick(const GameSnapshot& s, party::PartyLink* link) {
    static const bool enabled = [] {
        const char* v = std::getenv("BB_PARTY_ANNALISE_SIT");
        return !(v && v[0] == '0');
    }();
    static SitState st;
    constexpr int kAnnalise = 2500600;
    constexpr u32 kSitAnim = 103010;
    const auto now = Clock::now();
    if (!enabled || !link || !s.world_up || s.loading || !s.player || s.map_id != 0x19000000u) {
        st.steady_since = {};
        st.have_home = false;
        return;
    }
    if (st.map != s.map_id) {
        st.map = s.map_id;
        st.have_home = false;
    }
    if (st.steady_since == Clock::time_point{}) st.steady_since = now;
    if (now - st.steady_since < std::chrono::seconds(3) || now < st.next) return;
    st.next = now + std::chrono::milliseconds(200);
    const u64 wcm = Slot(kSlotWorldChrMan);
    const u64 chr = wcm ? ChrByEntity(kAnnalise) : 0;
    float p[3] = {};
    if (!chr || IsPlayer(wcm, chr) || ChrOff(chr) || !ChrPosition(chr, p)) return;
    if (!st.have_home) {
        std::memcpy(st.home, p, sizeof p);
        st.have_home = true;
        return;
    }
    const float prev = st.prev_y;
    st.prev_y = p[1];
    // sinking = below her spot and still going down (the animation settles her ~0.3 m lower and she stays), or far below it
    if (!(p[1] < st.home[1] - 1.0f || (p[1] < st.home[1] - 0.1f && p[1] < prev - 0.04f))) return;
    if (now - st.last_fix < std::chrono::milliseconds(400)) return;
    st.last_fix = now;
    ++st.fixes;
    reinterpret_cast<void(BB_COOP_SYSV*)(u32, u32, u32, u32)>(Guest(0x13cbfd0))(u32(kAnnalise), kSitAnim, 0, 0);
    if (st.fixes <= 6 || st.fixes % 50 == 0)
        Log("Annalise: sinking (y %.1f, spot %.1f): ForcePlayAnimation(%d, %u) #%d", double(p[1]), double(st.home[1]),
            kAnnalise, kSitAnim, st.fixes);
}

void Tick(const GameSnapshot& s, party::PartyLink* link, bool host) {
    ChrDumpTick(s);
    NpcMirrorTick(s, link, host);
    AnnaliseSitTick(s, link);
    KillEchoTick(s);
    EnemyHpTestTick(s);
    if (!On()) return;
    TickState& st = S();
    const auto now = Clock::now();
    const Rules r = host ? FromSettings() : InForce();
    party::status::SetShareLamps(r.share_lamps);
    party::status::SetHostRule(party::status::HostRule::GuestsBringLamps, r.guests_bring_lamps);
    party::status::SetHostRule(party::status::HostRule::Rot, r.rot);
    party::status::SetHostRule(party::status::HostRule::HostRevive, r.host_revive);
    party::status::SetHostRule(party::status::HostRule::HostLimboRevive, r.host_limbo_revive);
    party::status::SetHostRule(party::status::HostRule::KillRecord, r.kill_record);
    party::status::SetHostRule(party::status::HostRule::NpcMirror, r.npc_mirror);
    party::status::SetHostRule(party::status::HostRule::NpcFights, r.npc_fights);
    party::status::SetHostRule(party::status::HostRule::SkipPrologue, r.skip_prologue);
    party::status::SetHostRule(party::status::HostRule::AnyKidnap, r.any_kidnap);
    party::status::SetHostRule(party::status::HostRule::LevelSync, r.level_sync);
    party::status::SetHostRule(party::status::HostRule::PhantomHp, r.phantom_hp);
    party::status::SetHostRule(party::status::HostRule::PvpFullHp, r.pvp_full_hp);
    party::status::SetHostRule(party::status::HostRule::BossDrops, r.boss_drops);
    party::status::SetHostRule(party::status::HostRule::Gather, r.gather);
    party::status::SetHostRule(party::status::HostRule::GuestGather, r.guest_gather);
    party::status::SetHostRule(party::status::HostRule::LayerTravel, r.layer_travel);
    party::status::SetHostRule(party::status::HostRule::CovenantPerPlayer, r.covenant_per_player);
    party::status::SetHostRule(party::status::HostRule::TravelVote, r.travel_vote);
    party::status::SetHostRule(party::status::HostRule::RotCleanseBoss, r.rot_cleanse_boss);
    party::status::SetHostRule(party::status::HostRule::RotCleanseKiller, r.rot_cleanse_killer);
    party::status::SetHostRule(party::status::HostRule::RotCleanseEchoes, r.rot_cleanse_echoes);
    {   // 0.7.4 session rules: the host's on a guest (each module reads HostRuleOn).
        using HR = party::status::HostRule;
        party::status::SetHostRule(HR::OpenWorld, r.open_world);
        party::status::SetHostRule(HR::GuestReviveInplace, r.guest_revive_inplace);
        party::status::SetHostRule(HR::GuestRespawn, r.guest_respawn);
        party::status::SetHostRule(HR::GuestRefill, r.guest_refill);
        party::status::SetHostRule(HR::ChaliceRejoin, r.chalice_rejoin);
        party::status::SetHostRule(HR::StoryMirror, r.story_mirror);
        party::status::SetHostRule(HR::GuestLamps, r.guest_lamps);
        party::status::SetHostRule(HR::GuestMark, r.guest_mark);
        party::status::SetHostRule(HR::FullRewards, r.full_rewards);
    }
    {   // D7 for the menus' read-only lines on a guest (the host's values once they came).
        const int scaling[party::status::kScalingValues] = {r.boss_hp,  r.boss_damage,  r.boss_poise,
                                                            r.enemy_hp, r.enemy_damage, r.enemy_poise};
        party::status::SetHostScaling(scaling);
    }
    {   // The rot for System > Party and the party HUD (none while it is off).
        int kinds[party::status::kRotKinds] = {};
        for (int k = 0; k < kRotKinds && k < party::status::kRotKinds; ++k) kinds[k] = r.rot ? g_kind[k].load() : 0;
        party::status::SetRot(kinds);
    }
    if (host) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_have_host = false;  // a host's own settings count
    }
    // The host's rules to the party (on change, at once when a guest connects, and every 15 s):
    // a guest's game follows them, never its own bbport.ini values (guestrules, 0.7.3).
    if (host && link) {
        const std::string j = ToJson(r);
        int members = 0;
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot != 0 && e.connected) ++members;
        }
        const bool joined = members > st.last_members;
        st.last_members = members;
        if (j != st.last_sent || joined || now >= st.next_send) {
            link->post_event(party::kBroadcast, kEventName, j);
            if (j != st.last_sent) Log("rules sent to the party: %s", j.c_str());
            st.last_sent = j;
            st.next_send = now + std::chrono::seconds(15);
        }
    }

    CensusTick(s);
    AiProbeTick(s);
    const bool in_world = s.world_up && !s.loading && s.player;
    if (in_world) LevelSyncTick(r.level_sync, r.phantom_hp, now);
    // The rot belongs to the character: each save slot has its own (party_rot.txt), and a new game
    // (its own prologue) drops stacks a deleted character left in its slot.
    if (in_world) RotForSlot(party::flow::CurrentSaveSlot());
    {
        const bool own_prologue = in_world && coop::LocalPrologueHold();
        if (!own_prologue) {
            st.prologue_since = Clock::time_point{};
        } else if (st.prologue_since == Clock::time_point{}) {
            st.prologue_since = now;
        }
        const double prologue_s =
            own_prologue ? std::chrono::duration<double>(now - st.prologue_since).count() : 0.0;
        if (RotFromAnotherCharacter(g_stacks.load(), own_prologue, prologue_s)) {
            Log("rot: %d stack(s) from another character dropped (this save is in its prologue: a new game)",
                g_stacks.load());
            g_stacks = 0;
            for (auto& c : g_kind) c = 0;
            {
                std::lock_guard<std::mutex> lk(g_rot_mu);
                g_ledger.Clear();
            }
            SaveRot();
        }
    }
    // C4: the local player's death, the lamp rest, the rate factor.
    const u64 dm = in_world ? Field<u64>(Field<u64>(s.player, 0x3b0), 0x20) : 0;
    const int hp = dm ? Field<int>(dm, 0xf8, -1) : -1;
    const int max_hp = dm ? Field<int>(dm, 0xfc, -1) : -1;
    if (in_world && s.player == st.last_player && st.last_hp > 0 && hp == 0 && max_hp > 1) {
        ++g_deaths;
        st.last_death = now;
        st.up_since_death = false;
        int kind = -1;
        // Playtest 1 #19: the prologue is played alone (its first death is scripted): no rot.
        const bool prologue = coop::LocalPrologueHold();
        if (prologue && r.rot) Log("death %d in the prologue: no rot (#19)", g_deaths.load());
        if (r.rot && !prologue && g_stacks.load() < kRotMaxStacks) {
            // One kind picked at random per death (ERSC), among the hooked ones; BB_PARTY_ROT_KIND=N
            // forces kind N (tests).
            int hooked[kRotKinds], nh = 0;
            for (int k = 0; k < kRotKinds; ++k)
                if (g_kind_hooked[k]) hooked[nh++] = k;
            if (nh > 0) {
                const auto t = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                kind = hooked[(t >> 10) % static_cast<std::uint64_t>(nh)];
                if (const char* f = std::getenv("BB_PARTY_ROT_KIND"); f && *f) {
                    // "2,3": the kinds of this player's deaths in turn
                    std::vector<int> list;
                    for (const char* q = f; *q;) {
                        list.push_back(std::clamp(std::atoi(q), 0, kRotKinds - 1));
                        while (*q && *q != ',') ++q;
                        if (*q == ',') ++q;
                    }
                    kind = list[static_cast<std::size_t>(g_deaths.load() - 1) % list.size()];
                }
                ++g_stacks;
                g_kind[kind] = g_kind[kind].load() + 1;
            }
        }
        // 0.7.2: who killed this player (the lethal hit's attacker, a chr that is no player) and
        // the death's stack in the ledger (its killer's death or its bloodstain gives it back).
        rot::Killer killer;
        const char* killer_what = "nobody (a fall, the environment, a scripted death)";
        {
            const std::int64_t at = g_killer_at.exchange(0);
            const u64 attacker = g_killer_chr.exchange(0);
            const double seen_s =
                at ? std::chrono::duration<double>(now.time_since_epoch() - Clock::duration(at)).count() : -1.0;
            const u64 wcm = Slot(kSlotWorldChrMan);
            const bool self = attacker && attacker == s.player;
            const bool player = attacker && wcm && IsPlayer(wcm, attacker);
            if (rot::KillerCounts(seen_s, self, attacker && !player)) {
                killer.chr = attacker;
                killer.key = Field<u32>(attacker, 0x3b8, 0);
                killer.entity = Field<u32>(attacker, 0x1f0, 0);
                if (!rot::PartyEntity(killer.entity)) killer.entity = 0;
                killer_what = "an enemy";
            } else if (at) {
                killer_what = self ? "itself" : player ? "a player" : !attacker ? "nobody" : "a hit too long before";
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_rot_mu);
            g_ledger.OnDeath(kind, killer);
        }
        Log("death %d: killed by %s (chr %llx, entity %u, key %u); its stack (%s): %s", g_deaths.load(), killer_what,
            (unsigned long long)killer.chr, killer.entity, killer.key, kind >= 0 ? kRotKindName[kind] : "none",
            kind < 0         ? "-"
            : killer.Any()   ? "its killer's death or its bloodstain gives it back"
                             : "its bloodstain gives it back");
        if (kind >= 0) SaveRot();
        const int k = g_stacks.load();
        Log("death %d; rot %s, %d stack(s) (this one: %s): health %d, stamina %d, stamina cost %d, rally %d, "
            "discovery %d",
            g_deaths.load(), r.rot ? "on" : "off", k, kind >= 0 ? kRotKindName[kind] : "none", g_kind[0].load(),
            g_kind[1].load(), g_kind[2].load(), g_kind[3].load(), g_kind[4].load());
        if (r.rot) {
            static const char* const kWhat[kRotKinds] = {"max HP -10 %", "max stamina -10 %", "stamina costs +10 %",
                                                         "rally -15 %", "item discovery -10"};
            char b[200];
            std::snprintf(b, sizeof b, "Rot %d/%d: %s until you rest at a lamp", k, kRotMaxStacks,
                          kind >= 0 ? kWhat[kind] : "-");
            st.toast = b;  // shown once the player is back on its feet (the death's travel notice comes first)
            st.toast_at = Clock::time_point{};
        }
    }
    // Evidence for damage scaling: the local player's hits (first 40 per game).
    if (in_world && s.player == st.last_player && st.last_hp > 0 && hp >= 0 && hp < st.last_hp && st.hits < 40) {
        ++st.hits;
        Log("hit: HP %d -> %d (-%d), max %d", st.last_hp, hp, st.last_hp - hp, max_hp);
    }
    // Test aid (smoke group rot): BB_PARTY_ROT_TEST_FRAIL=1: any HP loss leaves this player at 1 HP,
    // so the next enemy hit kills it (a death with a killer, the game's own death path).
    static const bool frail = std::getenv("BB_PARTY_ROT_TEST_FRAIL") && std::getenv("BB_PARTY_ROT_TEST_FRAIL")[0] == '1';
    if (frail && dm && in_world && s.player == st.last_player && st.last_hp > 0 && hp > 1 && hp < st.last_hp) {
        const int one = 1;
        if (SafeWrite(dm + 0xf8, &one, 4)) Log("test: frail (BB_PARTY_ROT_TEST_FRAIL): HP %d -> 1", hp);
    }
    RotCleanseTick(s, r, host, in_world, now);
    if (!st.toast.empty()) {
        if (!(in_world && hp > 0)) {
            st.toast_at = Clock::time_point{};
        } else if (st.toast_at == Clock::time_point{}) {
            st.toast_at = now;
        } else if (std::chrono::duration<double>(now - st.toast_at).count() >= 2.0) {
            party::status::SetLastEvent(st.toast);
            Log("notice: %s", st.toast.c_str());
            st.toast.clear();
        }
    }
    st.last_player = in_world ? s.player : 0;
    st.last_hp = in_world ? hp : -1;
    g_local_chr.store(in_world ? s.player : 0, std::memory_order_relaxed);
    if (in_world && hp >= 0) g_hp_now = hp;
    if (in_world && hp > 0 && s.player == st.last_player) st.up_since_death = true;
    if (g_rest_pending.exchange(false)) {
        const double since_death = st.last_death != Clock::time_point{}
                                       ? std::chrono::duration<double>(now - st.last_death).count()
                                       : -1.0;
        if (LampWarpIsRespawn(g_rest_alive.load(), since_death, st.up_since_death)) {
            Log("lamp warp %.0f s after a death (%s): the respawn, not a rest", since_death,
                g_rest_alive.load() ? "not up again yet" : "dead at the warp");
        } else {
            CleanseRot(r, "lamp rest", now, st);
        }
    }
    // Playtest 1 #21: the lamp's menu opened (also when nobody travels) is a rest, as ERSC's grace.
    {
        const int dialog = in_world ? Field<int>(Slot(kSlotMenuManBal), 0x78, 0) : 0;
        float pos[3] = {}, d[3] = {};
        u32 lamp = 0;
        float spot_m = -1, obj_m = -1;
        const bool lamp_menu = g_lamp_menu_opened.exchange(false);
        if (((dialog && !st.dialog_was) || lamp_menu) && hp > 0 && LocalPos(s.player, pos)) {
            lamp = NearestLampSpot(s.map_id, pos, d);
            spot_m = lamp ? std::sqrt(d[0] * d[0] + d[2] * d[2]) : -1.0f;
            // The lamp itself stands a few m from its revive spot (Central Yharnam 2412951: 4.6 m,
            // live 2026-10-14: the box opened 5.7 m from the spot): also measured to the lamp's
            // object (map entity = the ReturnPoint id - 1000, or - 2000), the nearest one counts.
            for (const u32 off : {1000u, 2000u}) {
                float o[3];
                if (!lamp || !worldact::ObjectPoint(static_cast<std::int32_t>(lamp - off), 0.0f, 0.0f, o)) continue;
                const float od[3] = {pos[0] - o[0], pos[1] - o[1], pos[2] - o[2]};
                const float m = std::sqrt(od[0] * od[0] + od[2] * od[2]);
                if (obj_m < 0 || m < obj_m) obj_m = m;
                if (m < std::sqrt(d[0] * d[0] + d[2] * d[2])) d[0] = od[0], d[1] = od[1], d[2] = od[2];
            }
        }
        if (lamp && LampMenuRest(true, hp > 0, d[0], d[1], d[2])) {
            char why[128];
            std::snprintf(why, sizeof why, "lamp menu at lamp %u (spot %.1f m, lamp %.1f m, dialog %d)", lamp, spot_m,
                          obj_m, dialog);
            CleanseRot(r, why, now, st);
        } else if (lamp && spot_m >= 0 && spot_m < 15.0f) {
            Log("dialog %d opened %.1f m from lamp %u's spot (lamp %.1f m): no lamp menu, not a rest", dialog, spot_m,
                lamp, obj_m);
        }
        if (in_world) st.dialog_was = dialog != 0;
        // A level up (the Doll) is a rest too.
        if (in_world && s.level > 0) {
            if (st.last_level > 0 && s.level > st.last_level) CleanseRot(r, "level up", now, st);
            st.last_level = s.level;
        }
    }
    UpdateRotFactor(r);
    // Evidence: the local max HP once a rot change has settled (3 s, in the world).
    {
        const int f = static_cast<int>(std::lround(g_kind_value[kRotHealth].load() * 100.0f)) +
                      1000 * static_cast<int>(std::lround(g_kind_value[kRotStamina].load() * 100.0f)) +
                      1000000 * g_stacks.load();
        if (f != st.last_factor) {
            st.last_factor = f;
            st.factor_since = now;
        }
        if (in_world && max_hp > 0 && hp > 0 && (f != st.hp_logged_factor || max_hp != st.hp_logged_max) &&
            std::chrono::duration<double>(now - st.factor_since).count() >= 3.0) {
            st.hp_logged_factor = f;
            st.hp_logged_max = max_hp;
            const u64 list = Field<u64>(s.player, 0x1c8);
            Log("max HP now %d (HP %d), max stamina %d, rot factor HP %d %% stamina %d %% (%d stack(s)); "
                "stamina cost x%.2f, rally x%.2f, discovery sum %+.0f (with rot)",
                max_hp, hp, Field<int>(dm, 0x138, -1), f % 1000, f / 1000 % 1000, g_stacks.load(),
                double(g_kind_value[kRotStaminaCost].load()), double(g_kind_value[kRotRally].load()),
                double(g_kind_hooked[kRotDiscovery] && g_kind_orig[kRotDiscovery]
                           ? reinterpret_cast<float(BB_COOP_SYSV*)(u64)>(g_kind_orig[kRotDiscovery])(list) +
                                 g_kind_value[kRotDiscovery].load()
                           : 0.0f));
        }
    }
    g_rot_list.store(in_world ? Field<u64>(s.player, 0x1c8) : 0, std::memory_order_relaxed);
    // C4 evidence: one action's stamina use (a drop of 8+ in one tick from a full bar), with the
    // stamina-cost factor in force, at most one line per 5 s (a roll or a swing at full stamina
    // costs the same every time, so lines before and after a "stamina cost" stack compare).
    {
        static int last_st = -1;
        static std::chrono::steady_clock::time_point next_st{};
        int stn = in_world && hp > 0 ? Field<int>(dm, 0x134, -1) : -1;
        const int stmax = Field<int>(dm, 0x138, -1);
        // The "stamina cost" rot: what this tick's actions spent, times (factor - 1), taken too.
        {
            static int prev_st = -1, prev_max = -1;
            static float carry = 0;
            static std::uint64_t charged = 0, charged_n = 0;
            static std::chrono::steady_clock::time_point next_charge_log{};
            const float f = g_kind_hooked[kRotStaminaCost] ? g_kind_value[kRotStaminaCost].load() : 1.0f;
            const int extra = RotExtraStaminaDrain(prev_st, stn, prev_max, stmax, f, &carry);
            const int after = stn - extra;
            if (extra > 0 && SafeWrite(dm + 0x134, &after, sizeof after)) {
                stn = after;
                charged += std::uint64_t(extra);
                ++charged_n;
                Note(kRotStaminaCost, f);
                if (now >= next_charge_log) {
                    next_charge_log = now + std::chrono::seconds(5);
                    Log("rot stamina cost: %d -> %d (+%d for x%.2f; %llu extra over %llu drops)", prev_st, stn, extra,
                        double(f), (unsigned long long)charged, (unsigned long long)charged_n);
                }
            }
            if (f <= 1.0f) carry = 0;
            prev_st = stn;
            prev_max = stmax;
        }
        // The "rally" rot: part of each HP gain that is not a Blood Vial's is taken back.
        {
            static int prev_hp = -1, prev_mhp = -1, vials = -1;
            static float carry = 0;
            static std::uint64_t taken = 0, gains = 0, regained = 0;
            static std::chrono::steady_clock::time_point vial_at{}, next_vials{}, next_rally_log{}, damage_at{};
            static bool damaged = false;
            const float f = g_kind_hooked[kRotRally] ? g_kind_value[kRotRally].load() : 1.0f;
            const int hp_now = in_world ? hp : -1;
            // Damage (HP down, the max unchanged, still alive) opens the rally window; only a gain
            // inside it is a rally (a regeneration effect's slow ticks later are kept whole).
            if (prev_hp > 0 && hp_now > 0 && hp_now < prev_hp && prev_mhp == max_hp) {
                damage_at = now;
                damaged = true;
            }
            const double since_damage =
                damaged ? std::chrono::duration<double>(now - damage_at).count() : -1.0;
            const bool gain = f < 1.0f && prev_hp > 0 && hp_now > prev_hp && RotRallyGainInWindow(since_damage);
            // A Blood Vial (goods 1000) used in the last 3 s: its heal, not a rally. The count is
            // read twice a second while the rot is in force, and at every gain.
            if (f < 1.0f && in_world && (gain || now >= next_vials)) {
                next_vials = now + std::chrono::milliseconds(500);
                const int v = GoodsCount(1000);
                if (vials >= 0 && v >= 0 && v < vials) vial_at = now;
                vials = v;
            }
            int kept_hp = hp_now;
            if (gain) {
                const bool vial = now - vial_at < std::chrono::seconds(3);
                const int back = RotRallyTakeBack(prev_hp, hp_now, prev_mhp, max_hp, f, vial, &carry);
                if (!vial && prev_mhp == max_hp) {
                    regained += std::uint64_t(hp_now - prev_hp);
                    ++gains;
                }
                const int after = hp_now - back;
                if (back > 0 && SafeWrite(dm + 0xf8, &after, sizeof after)) {
                    kept_hp = after;
                    taken += std::uint64_t(back);
                    Note(kRotRally, f);
                }
                if (now >= next_rally_log) {
                    next_rally_log = now + std::chrono::seconds(5);
                    Log("rot rally: HP %d -> %d%s, %d kept (x%.2f; so far %llu HP regained in %llu gains, %llu taken back)",
                        prev_hp, hp_now, vial ? " after a Blood Vial" : " regained", kept_hp - prev_hp, double(f),
                        (unsigned long long)regained, (unsigned long long)gains, (unsigned long long)taken);
                }
            }
            if (f >= 1.0f) carry = 0;
            prev_hp = kept_hp;
            prev_mhp = max_hp;
        }
        if (stn >= 0 && last_st >= 0 && last_st == stmax && last_st - stn >= 8 && now >= next_st) {
            next_st = now + std::chrono::seconds(5);
            Log("stamina used from a full bar: %d (%d -> %d); stamina cost x%.2f (%d rot stack(s))", last_st - stn,
                last_st, stn, double(g_kind_value[kRotStaminaCost].load()), g_stacks.load());
        }
        last_st = stn;
    }
    if (g_stacks.load() > 0 && in_world && hp > 0 && now >= st.next_rot_log) {
        st.next_rot_log = now + std::chrono::seconds(20);
        std::string b;
        for (int k = 1; k < kRotKinds; ++k) {
            char e[96];
            std::snprintf(e, sizeof e, " %s: %u calls, last %.2f;", kRotKindName[k],
                          g_kind_calls[k].exchange(0), double(g_kind_last[k].load()));
            b += e;
        }
        Log("rot in effect (%d stack(s); the game's asks for this player since the last line):%s max HP %d, "
            "max stamina %d", g_stacks.load(), b.c_str(), max_hp, Field<int>(dm, 0x138, -1));
    }

    // D7: the rows (every second) and the enemies (every 2 s).
    if (s.cooperators >= 0) st.last_n = std::clamp(s.cooperators, 0, 3);
    if (now >= st.next_params) {
        st.next_params = now + std::chrono::seconds(1);
        WriteParams(r, st.last_n);
    }
    if (in_world && now >= st.next_enemies) {
        st.next_enemies = now + std::chrono::seconds(2);
        const bool scale = st.last_n > 0 && EnemyScalingActive(r);
        if (scale || st.log || (st.last_n > 0 && r.boss_hp > 0)) {
            if (EnemyPass(s, scale)) st.next_enemies = now;  // 0.7.8: the rest of a sliced pass, next frame
        }
    }
}

std::vector<EnemyKillCandidate> LoadedEnemies() {
    const u64 wcm = Slot(kSlotWorldChrMan);
    return wcm ? LoadedEnemyList(wcm) : std::vector<EnemyKillCandidate>{};
}

// H3: the kill-echo payer 0x18C5070 (docs/party/items.md 2) pays the LOCAL player (no attacker
// check) when the dead chr's +0x1E1 has neither 0x40000 (echoes paid; set by the payer) nor 0x200000,
// NpcParam soul (+0x28) > 0 and NpcParam byte +0x149 bit 4 clear. The per-frame death path 0x18BD9D0
// skips the payer for a remote-controlled chr ([[chr+0x18]+0x30]+0x18 bit 0).
std::string DescribeKillEchoGates(u64 chr) {
    u64 f = 0;
    SafeRead(chr + 0x1e1, &f, 5);
    const u64 npc = Field<u64>(Field<u64>(chr, 0x3c8), 8);
    const u64 rc = Field<u64>(Field<u64>(chr, 0x18), 0x30);
    char b[200];
    std::snprintf(b, sizeof b, "+0x1e1 %010llx (paid %d), remote %d, soul %d, npc+0x149 %02x", (unsigned long long)f,
                  (f & 0x40000) ? 1 : 0, rc ? (Field<u8>(rc, 0x18, 0) & 1) : 0, npc ? Field<int>(npc, 0x28, -1) : -1,
                  npc ? unsigned(Field<u8>(npc, 0x149, 0)) : 0u);
    return b;
}

bool ChrHasLevelSync(u64 chr) { return chr && HasEffect(chr, levelsync::kLevelSyncEffect); }

std::string DescribeBossHp(u64 chr, bool with_hp) {
    if (!chr) return "not loaded";
    const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
    char b[160];
    std::snprintf(b, sizeof b, "HP %d / max %d (base %d), doping 7500:%d 7501:%d 7502:%d",
                  with_hp ? (dm ? Field<int>(dm, 0xf8, -1) : -1) : 0,
                  dm ? Field<int>(dm, 0xfc, -1) : -1, dm ? Field<int>(dm, 0x100, -1) : -1, HasEffect(chr, 7500) ? 1 : 0,
                  HasEffect(chr, 7501) ? 1 : 0, HasEffect(chr, 7502) ? 1 : 0);
    return b;
}

int EnemyHp(u64 chr) {
    const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
    return dm ? Field<int>(dm, 0xf8, -1) : -1;
}

bool SetEnemyHpZero(u64 chr) {
    const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
    if (!dm || Field<int>(dm, 0xf8, -1) <= 0) return false;
    const int zero = 0;
    return SafeWrite(dm + 0xf8, &zero, 4);
}

bool SetEnemyDeadQuietly(u64 chr) {
    if (EnemyHp(chr) <= 0) return false;
    u8 b = 0;
    if (!SafeGet(chr + 0x1e3, &b)) return false;
    b |= 0x04;  // +0x1E1 bit 0x40000: echoes paid
    if (!SafeWrite(chr + 0x1e3, &b, 1)) return false;
    droproll::SkipDrop(chr);
    return SetEnemyHpZero(chr);
}

bool OnNpcOff(int slot, const std::string& json) {
    std::uint32_t map = 0;
    std::vector<std::uint32_t> off;
    if (slot != 0 || !npcmirror::FromJson(json, &map, &off)) return false;
    NpcMirrorState& st = NM();
    std::lock_guard<std::mutex> lk(st.mu);
    st.host_map = map;
    st.host_off = std::move(off);
    st.host_at = Clock::now();
    return true;
}

bool ApplySpEffectToEntity(int entity, int id, std::string* what) {
    const u64 chr = ChrByEntity(entity);
    if (!chr || !ApplyEffect(chr, id)) return false;
    if (what) {
        // Every doping node (a second one is what the dedupe keeps out) and the HP rate product.
        const u64 list = Field<u64>(chr, 0x1c8);
        std::string rows;
        for (u64 n = Field<u64>(list, 8); n && rows.size() < 160; n = Field<u64>(n, 0x58)) {
            const int nid = Field<int>(n, 0x40, -1);
            if (nid >= 7500 && nid <= 7503) {
                char e[32];
                std::snprintf(e, sizeof e, " %d/%x", nid, Field<u32>(n, 0x1c));
                rows += e;
            }
        }
        const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
        char b[256];
        std::snprintf(b, sizeof b, "chr %x: doping rows%s; HP %d / %d, rate %.3f", Field<u32>(chr, 8, ~0u),
                      rows.empty() ? " none" : rows.c_str(), Field<int>(dm, 0xf8, -1), Field<int>(dm, 0xfc, -1),
                      double(list && g_rate_orig ? reinterpret_cast<RateFn>(g_rate_orig)(list, 0) : -1.0f));
        *what = b;
    }
    return true;
}

bool ChrPosition(u64 chr, float out[3]) {
    const u64 phys = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
    return phys && SafeRead(phys + 0x1e0, out, 12);
}

}  // namespace coop::balance
