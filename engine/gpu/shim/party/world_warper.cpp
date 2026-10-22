// SPDX-License-Identifier: GPL-3.0-or-later
// The world warper (see world_warper.h; route, options and progress file in world_warper_rules.h).
#include "world_warper.h"

#include "bbport_compile_progress.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "lamp_travel.h"
#include "party_async_io.h"
#include "party_balance.h"
#include "party_director.h"
#include "party_enemy_census.h"
#include "party_ids.h"
#include "seamless_rules.h"
#include "world_warper_rules.h"
#include "../bbport_game_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" void runtime_pad_director_press(std::uint32_t buttons);        // src/runtime_pad.c
extern "C" void runtime_pad_director_sticks(int lx, int ly, int rx, int ry); // src/runtime_pad.c
[[noreturn]] void BbHardExit(int code);                                       // shim/bbgpu.cpp

namespace coop::warper {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
namespace cp = BbCompileProgress;

constexpr u64 kLampWarp = 0x13cdf30;      // void (u32 ReturnPointParam id): lamp / headstone warp
constexpr u64 kFlipperUpdate = 0x2034770; // main-thread tick (as party_director / lamp travel)
constexpr u64 kForceDead = 0x13320c0;     // Lua ForceDead(ctx unused, entity id)
constexpr u64 kChrDeath = 0x18bf9a0;      // the death path on a chr (what ForceDead calls)
constexpr u32 kPadCross = 0x4000;
constexpr u64 kGameDataMan = 0x553b130;   // *slot +8: the local player's PlayerGameData record
constexpr u64 kEventFlagMan = 0x553b100;  // singleton slot
constexpr u64 kGetFlagValue = 0x13cfd80;  // (man, first, bits) -> value
constexpr u64 kSetFlagValue = 0x13d0060;  // (man, first, bits, value)
constexpr u64 kTodWorld = 0x553b148;      // *(slot) +0x81 = 1: re-evaluate the time of day (party_story)
constexpr u64 kPlayRemo = 0x131a9f0;
constexpr u64 kMenuMan = 0x5562878;       // *slot: MenuMan; +0x2BC = the gesture asked for (param id 1..23)
constexpr u64 kMenuGesture = 0x2bc;      // (unused, remo id, mode, -1, -1, player entity) -> al

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("World warper: ") + line + "\n");
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <class T>
T Field(u64 base, u64 off, T fallback = T{}) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

/// The counters an area / action is measured with (all monotonic).
struct Counters {
    u64 created = 0, hitches = 0, hitch_us = 0, skipped = 0, waits = 0, compile_us = 0, flips = 0;
    u64 shader_hitches = 0, shader_hitch_us = 0;
    static Counters Now() {
        Counters c;
        c.created = cp::created_total.load();
        c.hitches = cp::hitch_frames.load();
        c.hitch_us = cp::hitch_us_total.load();
        c.skipped = cp::skipped_total.load();
        c.waits = cp::waits_total.load();
        c.compile_us = cp::gpu_thread_compile_us.load();
        c.flips = cp::flips_total.load();
        c.shader_hitches = cp::compile_hitch_frames.load();
        c.shader_hitch_us = cp::compile_hitch_us_total.load();
        return c;
    }
    Counters operator-(const Counters& o) const {
        Counters d;
        d.created = created - o.created, d.hitches = hitches - o.hitches, d.hitch_us = hitch_us - o.hitch_us;
        d.skipped = skipped - o.skipped, d.waits = waits - o.waits, d.compile_us = compile_us - o.compile_us;
        d.flips = flips - o.flips, d.shader_hitches = shader_hitches - o.shader_hitches;
        d.shader_hitch_us = shader_hitch_us - o.shader_hitch_us;
        return d;
    }
    Counters& operator+=(const Counters& o) {
        created += o.created, hitches += o.hitches, hitch_us += o.hitch_us, skipped += o.skipped;
        waits += o.waits, compile_us += o.compile_us, flips += o.flips;
        shader_hitches += o.shader_hitches, shader_hitch_us += o.shader_hitch_us;
        return *this;
    }
};

enum class Phase : std::uint8_t { ReplayBefore, Tour, Remo, Tod, ReplayAfter };
const char* PhaseName(Phase p) {
    return p == Phase::Tour ? "tour" : p == Phase::Remo ? "cutscenes" : p == Phase::Tod ? "time of day"
         : p == Phase::ReplayBefore ? "replay (before)" : "replay (after)";
}

/// What the area's routine does after its scripted steps.
enum class Sub : std::uint8_t { Steps, Effects, Gestures, Tools, Remo, Enemies, Done };

enum class Stage : std::uint8_t { Title, Settle, Next, WaitLoad, Arrive, Routine, Finish, Done };

struct EnemyTarget {
    u32 entity = 0;
    u64 chr = 0;
};

struct State {
    Options opt;
    bool on = false;
    std::string state_path;
    std::vector<Phase> phases;
    std::size_t phase_i = 0;
    std::vector<u32> route;       // the current phase's rows
    std::size_t idx = 0;          // next row to look at
    Progress progress;            // the tour's (from the file)
    Stage stage = Stage::Title;
    double at = 0, stage_at = 0, steady_since = 0, title_press_at = 0, start_at = 0;
    int title_presses = 0;
    int first_run_step = 0;       // the first-launch screens (brightness, controls): next press
    TitleScreen first_run_screen = TitleScreen::Other;
    double first_run_at = 0;
    u32 rp = 0;                   // the row being visited
    std::size_t visited = 0;      // rows done in this phase (this session)
    bool load_seen = false;
    bool interrupted = false;
    double arrive_at = 0, routine_at = 0, warp_at = 0;
    Counters c_warp, c_arrive, c_step, c_phase, c_session0;
    std::map<std::string, u64> action_new;  // the area's new pipelines per action
    std::map<std::string, u64> tour_action_new;
    std::vector<PadStep> steps;
    std::size_t step_i = 0;
    double step_end = 0;
    std::vector<u32> maps_seen;
    bool loadout_done = false;
    // enemies
    int enemy_round = 0;
    std::vector<u32> enemies_used;
    EnemyTarget target;
    double enemy_kill_at = 0, enemy_next_at = 0;
    bool enemy_pending = false, target_boss = false;
    // A boss's arena (`bosses`): candidate spots round the boss, each checked before the fight.
    int boss_try = -1;            // -1 none, else the candidate being checked
    int boss_check = 0;           // 0 placed, 1 settled (walking toward it), 2 checked
    double boss_check_at = 0;
    float boss_pos[3] = {}, boss_check_pos[3] = {}, boss_spot[3] = {};
    std::size_t bosses_safe = 0, bosses_unsafe = 0;
    u32 target_kind = 0;
    std::map<u64, u32> npc_ids;   // NpcParam row data -> row id (enemy kinds)
    Sub sub = Sub::Steps;
    // item effects (once per run)
    std::vector<int> effects;
    std::size_t effect_i = 0;
    double effect_at = 0;
    // cutscenes of the area
    std::vector<u32> remos;
    std::size_t remo_i = 0;
    int remo_stage = 0;           // 0 next, 1 asked (waiting for it to start), 2 playing
    double remo_at = 0;
    // gestures / hunter tools (once per run, on the first row outside the Hunter's Dream)
    std::vector<u32> pass;
    std::size_t pass_i = 0;
    int pass_stage = 0;
    double pass_at = 0;
    u32 quick0_before = 0;
    std::vector<int> anims_before;  // the gesture check: the player's animations before the request
    int tool_held = -1, tool_bullets = -1;  // the tool's count and the quicksilver bullets before its use
    std::size_t gestures_played = 0, gestures_ok = 0, tools_used = 0, pass_ok0 = 0;
    // time of day
    std::vector<int> tod_of_row;  // the Tod phase: each route row's value
    std::vector<u32> remo_of_row; // the Remo phase: each route row's cutscene
    int tod_now = -1;
    double curve_at = 0;
    double gear_wait = 0;
    // replay results: row -> counters
    std::map<u32, Counters> replay_before, replay_after;
    std::size_t done_rows = 0, skipped_rows = 0;
    u64 tour_created = 0;
    std::size_t kills = 0, bosses = 0, remos_played = 0, effects_applied = 0, tod_rows = 0;
} g;

std::mutex g_status_mu;
std::string g_status;  // under g_status_mu
std::atomic<bool> g_active{false};

void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_status_mu);
    g_status = s;
}

void AppendState(const std::string& line) {
    if (g.state_path.empty()) return;
    if (FILE* f = std::fopen(g.state_path.c_str(), "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fflush(f);
        std::fclose(f);
    }
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string RowName(u32 rp) {
    const char* area = AreaName(rp);
    char b[128];
    std::snprintf(b, sizeof b, "%s%s%u", area, area[0] ? ", row " : "row ", rp);
    return b;
}

void Release() {
    runtime_pad_director_press(0);
    runtime_pad_director_sticks(-1, -1, -1, -1);
}

void Apply(const PadStep& s) {
    runtime_pad_director_press(s.buttons);
    runtime_pad_director_sticks(s.lx, s.ly, s.rx, s.ry);
}

bool PlayerPos(u64 player, float out[3]) {
    float p[4] = {};
    if (!player || !SafeRead(player + 0xc0, p, sizeof p)) return false;
    out[0] = p[0], out[1] = p[1], out[2] = p[2];
    return true;
}

/// Places the player (the chr's own warp, vtable +0x5F8, as the lamp travel test's WalkTo).
bool PlacePlayer(u64 player, const float pos3[3], float yaw) {
    alignas(16) float pos[4] = {pos3[0], pos3[1], pos3[2], 1.0f};
    alignas(16) float rot[4] = {0, yaw, 0, 0};
    u64 vtbl = 0, fn = 0;
    if (!player || !SafeGet(player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) return false;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, const float*, const float*, u64)>(fn)(player, pos, rot, 0);
    return true;
}

/// Back to the row's spot (its revive point), facing its yaw.
void BackToSpot(u64 player, u32 rp) {
    float sp[4];
    if (ids::TemplateReturnSpot(rp, sp)) PlacePlayer(player, sp, sp[3]);
}

/// Keeps the player alive: HP back to its maximum (PlayerIns +0x3B0 -> +0x20: HP +0xF8, max +0xFC).
void KeepAlive(u64 player) {
    const u64 dm = Field<u64>(Field<u64>(player, 0x3b0), 0x20);
    if (!dm) return;
    const int hp = Field<int>(dm, 0xf8, -1), max = Field<int>(dm, 0xfc, -1);
    if (hp > 0 && max > 0 && hp < max) SafeWrite(dm + 0xf8, &max, 4);
}

std::string Counts(const Counters& c) {
    char b[300];
    std::snprintf(b, sizeof b, "%llu new pipelines, %llu hitches over 50 ms (%.0f ms; %llu of them, %.0f ms, with "
                  "a pipeline made in the frame), %llu draws skipped, %llu GPU-thread waits, %.0f ms GPU-thread compiles",
                  static_cast<unsigned long long>(c.created), static_cast<unsigned long long>(c.hitches),
                  c.hitch_us / 1e3, static_cast<unsigned long long>(c.shader_hitches), c.shader_hitch_us / 1e3,
                  static_cast<unsigned long long>(c.skipped),
                  static_cast<unsigned long long>(c.waits), c.compile_us / 1e3);
    return b;
}

void UpdateStatus(const char* doing) {
    const Phase ph = g.phases.empty() ? Phase::Tour : g.phases[g.phase_i];
    const std::size_t total = g.route.size();
    std::size_t n = ph == Phase::Tour ? HandledCount(g.route, g.progress) : g.visited;
    if (g.rp && n < total) ++n;  // the row in progress
    const Counters here = Counters::Now() - g.c_warp;
    char b[600];
    std::snprintf(b, sizeof b,
                  "World warper: %s %zu / %zu\n%s\n%s - %llu new pipelines here, %llu this session",
                  PhaseName(ph), n, total, g.rp ? RowName(g.rp).c_str() : "-", doing,
                  static_cast<unsigned long long>(g.rp ? here.created : 0),
                  static_cast<unsigned long long>(cp::created_total.load() - g.c_session0.created));
    SetStatus(b);
}

bool Steady(const GameSnapshot& s) {
    return s.world_up && !s.loading;
}

std::vector<u32> AllRows() {
    return std::vector<u32>(std::begin(ids::kPartyReturnPoints), std::end(ids::kPartyReturnPoints));
}

/// `dump`: the player record's words (equipment research), 8 to a line, zero lines left out.
void DumpRecord() {
    u64 gdm = 0;
    if (!SafeGet(Guest(kGameDataMan), &gdm) || !gdm) return;
    const u64 rec = Field<u64>(gdm, 8);
    if (!rec) return;
    Log("dump: player record %#llx", static_cast<unsigned long long>(rec));
    for (u64 off = 0; off < 0x1000; off += 32) {
        u32 w[8] = {};
        if (!SafeRead(rec + off, w, sizeof w)) break;
        bool any = false;
        for (u32 v : w) any = any || v;
        if (!any) continue;
        Log("dump +%03llx: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<unsigned long long>(off), w[0], w[1],
            w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

int TodValue() {
    u64 man = 0;
    if (!SafeGet(Guest(kEventFlagMan), &man) || !man) return -1;
    return static_cast<int>(reinterpret_cast<u32(BB_COOP_SYSV*)(u64, u32, u32)>(Guest(kGetFlagValue))(man, 9800, 3));
}

/// Day / evening / night / Blood Moon now (the flags and the world's re-evaluation, as party_story).
bool SetTod(int v) {
    u64 man = 0, w = 0;
    if (v < 0 || !SafeGet(Guest(kEventFlagMan), &man) || !man) return false;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32, u32, u32)>(Guest(kSetFlagValue))(man, 9800, 3, static_cast<u32>(v));
    if (SafeGet(Guest(kTodWorld), &w) && w) {
        const std::uint8_t one = 1;
        SafeWrite(w + 0x81, &one, 1);
    }
    const int now = TodValue();
    Log("time of day -> %s (9800..9802 = %d)", TodName(v), now);
    g.tod_now = now;
    return now == v;
}

bool ApplyEffect(u64 player, int id) {
    u64 vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) return false;
    using AddSpEffectFn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, float, float, float, float, float);
    reinterpret_cast<AddSpEffectFn>(fn)(player, static_cast<u64>(id), player, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    return true;
}

/// The item effects to show once per run: the SpEffect of every usable goods (EquipParamGoods
/// refId with an SpEffectParam row), less the ones that warp, summon or end the run.
std::vector<int> ItemEffects() {
    std::set<int> out;
    for (const auto& [id, row] : GameParamRows("EquipParamGoods", "EQUIP_PARAM_GOODS_ST")) {
        std::int32_t ref = -1;
        if (!SafeGet(row + 0x0, &ref) || ref <= 0 || !ItemEffectAllowed(id, static_cast<u32>(ref))) continue;
        if (g.progress.fx.count(static_cast<u32>(ref))) continue;  // shown (or crashed in) before
        if (GameParamRow("SpEffectParam", "SP_EFFECT_PARAM_ST", static_cast<u32>(ref))) out.insert(ref);
    }
    return std::vector<int>(out.begin(), out.end());
}

/// The NpcParam row id of an enemy chr (ChrIns +0x3C8 -> +8: its row data), 0 unknown.
u32 NpcParamId(u64 chr) {
    const u64 row = Field<u64>(Field<u64>(chr, 0x3c8), 8);
    if (!row) return 0;
    if (g.npc_ids.empty()) {
        for (const auto& [id, data] : GameParamRows("NpcParam", "NPC_PARAM_ST")) g.npc_ids[data] = id;
    }
    const auto it = g.npc_ids.find(row);
    return it == g.npc_ids.end() ? 0 : it->second;
}

/// Lines of the coverage summary (the log and the progress file's last lines).
std::string Coverage() {
    char b[400];
    std::snprintf(b, sizeof b, "%zu places, %zu enemy kinds, %zu cutscenes played (%zu tried), %zu item effects, "
                  "%zu time-of-day rows (this session: %zu kills, %zu bosses fought, %zu without a safe spot)",
                  g.progress.done.size(), g.progress.kinds.size(), g.progress.remos_ok.size(), g.progress.remos.size(),
                  g.progress.fx.size(), g.progress.tod_done.size(), g.kills, g.bosses, g.bosses_unsafe);
    return b;
}

void BeginPhase(double now) {
    const Phase ph = g.phases[g.phase_i];
    g.idx = 0;
    g.visited = 0;
    g.c_phase = Counters::Now();
    auto spot = [](u32 rp) {
        float sp[4];
        return ids::TemplateReturnSpot(rp, sp);
    };
    if (ph == Phase::Tour) {
        g.route = BuildRoute(AllRows(), spot, g.opt);
        g.progress = ParseProgress(ReadFile(g.state_path));
        const std::size_t handled = HandledCount(g.route, g.progress);
        Log("tour: %zu rows (every ReturnPointParam row with a spot, Chalice Dungeons left out)%s; %zu already "
            "handled (progress file %s)", g.route.size(), g.opt.lamps.empty() ? "" : " from lamps=", handled,
            g.state_path.empty() ? "(none)" : g.state_path.c_str());
        for (u32 rp : g.route) {
            if (const char* why = SkipReason(g.progress, rp)) {
                if (std::strcmp(why, "done") != 0) Log("tour: row %u not visited again (%s)", rp, why);
            }
        }
        if (g.progress.tod_orig < 0) {
            g.progress.tod_orig = TodValue();
            AppendState("todorig " + std::to_string(g.progress.tod_orig));
        }
        g.effects = g.opt.items && g.opt.act && !g.progress.effects_done ? ItemEffects() : std::vector<int>{};
    } else if (ph == Phase::Remo) {
        if (g.state_path.size()) g.progress = ParseProgress(ReadFile(g.state_path));
        const std::vector<u32> all = g.opt.remos.empty() ? std::vector<u32>(std::begin(ids::kPartyRemos),
                                                                            std::end(ids::kPartyRemos))
                                                         : g.opt.remos;
        Options ro = g.opt;
        ro.lamps.clear();
        ro.max = 0;
        g.route.clear();
        g.remo_of_row.clear();
        for (const auto& [rp, id] : CutsceneRoute(BuildRoute(AllRows(), spot, ro), all, g.progress)) {
            g.route.push_back(rp);
            g.remo_of_row.push_back(id);
        }
        Log("cutscenes: %zu of %zu not played yet (%zu played in the run), one warp each", g.route.size(), all.size(),
            g.progress.remos_ok.size());
    } else if (ph == Phase::Tod) {
        const std::vector<u32> rows = TodRoute(BuildRoute(AllRows(), spot, g.opt));
        g.route.clear();
        g.tod_of_row.clear();
        for (int v : TodPasses(g.progress.tod_orig)) {
            for (u32 rp : rows) {
                g.route.push_back(rp);
                g.tod_of_row.push_back(v);
            }
        }
        Log("time of day: %zu rows (%zu maps x %zu values; the save's own is %s)", g.route.size(), rows.size(),
            TodPasses(g.progress.tod_orig).size(), TodName(g.progress.tod_orig));
    } else {
        Options ro;
        ro.lamps = g.opt.replay.empty() ? std::vector<u32>(std::begin(kDefaultReplay), std::end(kDefaultReplay))
                                        : g.opt.replay;
        g.route = BuildRoute(AllRows(), spot, ro);
        Log("%s: %zu rows (look + walk each, the stutter measurement)", PhaseName(ph), g.route.size());
    }
    g.stage = Stage::Next;
    g.at = now;
}

void StartSteps(std::vector<PadStep> steps, double now) {
    g.steps = std::move(steps);
    g.step_i = 0;
    g.step_end = 0;
    g.c_step = Counters::Now();
    (void)now;
}

/// Runs the scripted steps; true once all ran.
bool RunSteps(double now) {
    if (g.step_i >= g.steps.size()) return true;
    if (g.step_end == 0) {
        Apply(g.steps[g.step_i]);
        g.step_end = now + g.steps[g.step_i].seconds;
        return false;
    }
    if (now < g.step_end) return false;
    const Counters c = Counters::Now();
    g.action_new[g.steps[g.step_i].what] += c.created - g.c_step.created;
    g.c_step = c;
    ++g.step_i;
    g.step_end = 0;
    if (g.step_i >= g.steps.size()) {
        Release();
        return true;
    }
    Apply(g.steps[g.step_i]);
    g.step_end = now + g.steps[g.step_i].seconds;
    return false;
}

/// The tour row's gear (GearFor): before its warp, so the load builds the chr with it.
void PutOnGear(std::size_t n) {
    const Gear gear = GearFor(n);
    int ok = 0, all = 0;
    ok += EquipSlotItem(kSlotRight1, kItemWeapon, gear.right) ? 1 : 0;
    ok += EquipSlotItem(kSlotLeft1, kItemWeapon, gear.left) ? 1 : 0;
    all += 2;
    for (int k = 0; k < 4; ++k) {
        const u32 piece = gear.armor + 1000u * static_cast<u32>(k);
        if (!GameParamRow("EquipParamProtector", "EQUIP_PARAM_PROTECTOR_ST", piece)) continue;
        ++all;
        ok += EquipSlotItem(kSlotHead + k, kItemProtector, piece) ? 1 : 0;
    }
    Log("gear %zu: armor set %u, right %u, left %u: %d of %d slots set", n, gear.armor, gear.right, gear.left, ok,
        all);
}

/// The next enemy to fight: a living one of a kind (model) not seen in the run yet, nearest the
/// row's spot first (bosses only when `bosses`); when the area has no new kind, the nearest
/// regular enemy within 60 m once (its death in this area's light).
bool PickEnemy(u32 rp, EnemyTarget* out, float* dist, bool* boss, u32* kind) {
    float sp[4];
    if (!ids::TemplateReturnSpot(rp, sp)) return false;
    const auto list = balance::LoadedEnemies();
    float best = kEnemyRange * kEnemyRange, near_best = kEnemyRange * kEnemyRange;
    const EnemyKillCandidate* pick = nullptr;
    const EnemyKillCandidate* near = nullptr;
    u32 pick_kind = 0;
    std::set<u32> kinds_here;
    for (const auto& e : list) {
        if (!e.alive || !e.have_pos || !e.chr) continue;
        if (std::find(g.enemies_used.begin(), g.enemies_used.end(), e.id) != g.enemies_used.end()) continue;
        const bool is_boss = EnemyIsBossEntity(e.entity);
        if (is_boss && !g.opt.bosses) continue;
        const float dx = e.pos[0] - sp[0], dy = e.pos[1] - sp[1], dz = e.pos[2] - sp[2];
        const float d = dx * dx + dy * dy + dz * dz;
        const u32 npc = NpcParamId(e.chr);
        if (!EnemyRowFightable(npc)) continue;  // the Dream's messengers and other set pieces
        const u32 k = EnemyKind(npc);
        if (k && !g.progress.kinds.count(k) && !kinds_here.count(k) && d < best) {
            best = d;
            pick = &e;
            pick_kind = k;
        }
        if (k) kinds_here.insert(k);
        if (!is_boss && d < near_best) {
            near_best = d;
            near = &e;
        }
    }
    if (!pick && g.enemy_round == 0 && near) {
        pick = near;
        best = near_best;
        const u32 npc = NpcParamId(near->chr);
        pick_kind = npc ? EnemyKind(npc) : 0;
    }
    if (!pick) return false;
    g.enemies_used.push_back(pick->id);
    out->entity = pick->entity;
    out->chr = pick->chr;
    *dist = std::sqrt(best);
    *boss = EnemyIsBossEntity(pick->entity);
    *kind = pick_kind;
    return true;
}

/// The cutscenes of the map the player is in that the run has not played yet.
std::vector<u32> RemosFor(u32 map_id) {
    std::vector<u32> v;
    for (u32 id : ids::kPartyRemos) {
        if (RemoMap(id) == (map_id & 0xffff0000u) && !g.progress.remos.count(id)) v.push_back(id);
    }
    return v;
}

/// Stands the player 2.5 m from the enemy, facing it.
bool GoToEnemy(u64 player, const EnemyTarget& t) {
    float e[3], me[3];
    if (!balance::ChrPosition(t.chr, e) || !PlayerPos(player, me)) return false;
    float dx = me[0] - e[0], dz = me[2] - e[2];
    const float h = std::sqrt(dx * dx + dz * dz);
    if (h < 0.01f) dx = 0, dz = 1;
    else dx /= h, dz /= h;
    const float pos[3] = {e[0] + dx * 2.5f, e[1] + 0.2f, e[2] + dz * 2.5f};
    const float yaw = std::atan2(e[0] - pos[0], e[2] - pos[2]);
    return PlacePlayer(player, pos, yaw);
}

/// The animations the local player played last (its TimeAct module [[chr+0x3B0]+0x18]: a ring of
/// 10 {anim id, start, end, length} at +0x20, the next write index at +0xC0); 0 unknown.
std::vector<int> RecentAnims(u64 chr) {
    std::vector<int> v;
    const u64 ta = Field<u64>(Field<u64>(chr, 0x3b0), 0x18);
    if (!ta) return v;
    for (int i = 0; i < 10; ++i) v.push_back(Field<int>(ta, 0x20 + 0x10 * static_cast<u64>(i), 0));
    return v;
}

/// The yaw a chr faces (its physics rotation +0x1D0, forward = (-sin, -cos)); false unknown.
bool ChrYaw(u64 chr, float* yaw) {
    const u64 phys = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
    float r[4] = {};
    if (!phys || !SafeRead(phys + 0x1d0, r, sizeof r) || !std::isfinite(r[1])) return false;
    *yaw = r[1];
    return true;
}

/// Stands the player at boss candidate spot `i` (0: the arena's entry floor, kBossArenas; 1..4:
/// BossSpot round the boss, in front first, where an idle boss looks into its arena), facing the
/// boss. `spot`: where it was put. False: no such spot (no arena entry known for spot 0).
bool PlaceAtBoss(u64 player, const EnemyTarget& t, int i, float boss_pos[3], float spot[3]) {
    float b[3], yaw = 0;
    if (!balance::ChrPosition(t.chr, b)) return false;
    if (!ChrYaw(t.chr, &yaw)) yaw = 0;
    if (i == 0) {
        if (!BossArenaSpot(t.entity, spot)) return false;
    } else {
        BossSpot(b, yaw, i - 1, spot);
    }
    boss_pos[0] = b[0], boss_pos[1] = b[1], boss_pos[2] = b[2];
    return PlacePlayer(player, spot, FaceYaw(spot, b));
}

/// The kill of the director's host_kill test step: ForceDead by id, the chr's death path, HP 0.
void KillEnemy(const EnemyTarget& t) {
    if (balance::EnemyHp(t.chr) <= 0) return;
    if (t.entity) reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(Guest(kForceDead))(0, t.entity);
    if (balance::EnemyHp(t.chr) > 0) {
        std::uint8_t b = 0;
        if (SafeGet(t.chr + 0x1e3, &b)) {
            b = std::uint8_t(b & 0xdf);
            SafeWrite(t.chr + 0x1e3, &b, 1);
        }
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(Guest(kChrDeath))(t.chr);
    }
    if (balance::EnemyHp(t.chr) > 0) {
        const int zero = 0;
        const u64 dm = Field<u64>(Field<u64>(t.chr, 0x3b0), 0x20);
        if (dm) SafeWrite(dm + 0xf8, &zero, 4);
    }
}

std::string ActionList(const std::map<std::string, u64>& m) {
    std::vector<std::pair<std::string, u64>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    for (const auto& [k, n] : v) {
        if (!n) continue;
        char b[96];
        std::snprintf(b, sizeof b, "%s%s %llu", out.empty() ? "" : ", ", k.c_str(), static_cast<unsigned long long>(n));
        out += b;
    }
    return out.empty() ? "none" : out;
}

void FinishRow(const GameSnapshot& s, double now, const char* note) {
    Release();
    const Phase ph = g.phases[g.phase_i];
    const Counters all = Counters::Now() - g.c_warp;
    const Counters here = Counters::Now() - g.c_arrive;  // in the world (no loading screen)
    const double worst = cp::TakeWorstFrameUs() / 1e3;
    ++g.visited;
    if (ph == Phase::Tour) {
        g.tour_created += all.created;
        for (const auto& [k, n] : g.action_new) g.tour_action_new[k] += n;
        AppendState(DoneLine(g.rp, all.created, here.hitches, worst, now - g.warp_at, MapName(s.map_id)));
        g.progress.done.insert(g.rp);
        ++g.done_rows;
        const std::size_t n = HandledCount(g.route, g.progress);
        Log("[%zu/%zu] %s (%s)%s: +%llu new pipelines (in the world %llu: %s); %llu hitches (%llu with a pipeline made), worst frame %.0f ms; "
            "%.0f s; %llu new this session", n, g.route.size(), RowName(g.rp).c_str(), MapName(s.map_id).c_str(), note,
            static_cast<unsigned long long>(all.created), static_cast<unsigned long long>(here.created),
            ActionList(g.action_new).c_str(), static_cast<unsigned long long>(here.hitches),
            static_cast<unsigned long long>(here.shader_hitches), worst, now - g.arrive_at,
            static_cast<unsigned long long>(cp::created_total.load() - g.c_session0.created));
    } else if (ph == Phase::Remo) {
        Log("cutscenes %zu/%zu %s: cutscene %u %s; +%llu new pipelines; %llu new this session", g.visited,
            g.route.size(), RowName(g.rp).c_str(), g.remo_of_row[g.idx - 1],
            g.progress.remos_ok.count(g.remo_of_row[g.idx - 1]) ? "played" : "NOT played",
            static_cast<unsigned long long>(all.created),
            static_cast<unsigned long long>(cp::created_total.load() - g.c_session0.created));
    } else if (ph == Phase::Tod) {
        const int v = g.tod_of_row[g.idx - 1];
        AppendState("tod " + std::to_string(v) + " " + std::to_string(g.rp) + " " + std::to_string(all.created));
        g.progress.tod_done.insert(std::uint64_t(static_cast<u32>(v)) << 32 | g.rp);
        ++g.tod_rows;
        Log("time of day %s %zu/%zu %s: +%llu new pipelines; %llu new this session", TodName(v), g.visited,
            g.route.size(), RowName(g.rp).c_str(), static_cast<unsigned long long>(all.created),
            static_cast<unsigned long long>(cp::created_total.load() - g.c_session0.created));
    } else {
        (ph == Phase::ReplayBefore ? g.replay_before : g.replay_after)[g.rp] = here;
        Log("%s %zu/%zu %s: %s; worst frame %.0f ms%s", PhaseName(ph), g.visited, g.route.size(),
            RowName(g.rp).c_str(), Counts(here).c_str(), worst, note);
    }
    g.rp = 0;
    g.stage = Stage::Next;
    g.at = now + 0.5;
}

std::vector<PadStep> RoutineSteps(const GameSnapshot& s) {
    const Phase ph = g.phases[g.phase_i];
    std::vector<PadStep> v = LookSteps();
    if (ph == Phase::Tod || ph == Phase::Remo) return v;
    const auto walk = WalkSteps();
    v.insert(v.end(), walk.begin(), walk.end());
    if (ph != Phase::Tour || !g.opt.act) return v;
    const u32 map = s.map_id & 0xffff0000u;
    const bool first_in_map = std::find(g.maps_seen.begin(), g.maps_seen.end(), map) == g.maps_seen.end();
    if (first_in_map) {
        g.maps_seen.push_back(map);
        const auto act = ActionSteps();
        v.insert(v.end(), act.begin(), act.end());
    }
    if (!g.loadout_done) {
        g.loadout_done = true;
        const auto lo = LoadoutSteps(!g.opt.gear);  // no switch into a changed slot (Gear)
        v.insert(v.end(), lo.begin(), lo.end());
    }
    if (g.opt.gear) {
        const auto gs = GearSteps();
        v.insert(v.end(), gs.begin(), gs.end());
    }
    return v;
}

void Finish(double now) {
    Release();
    const Counters total = Counters::Now() - g.c_session0;
    Log("done: %zu areas visited, %zu skipped this session; %s; %.1f min", g.done_rows, g.skipped_rows,
        Counts(total).c_str(), (now - g.start_at) / 60.0);
    Log("coverage (the run, all sessions): %s", Coverage().c_str());
    if (!g.tour_action_new.empty()) Log("new pipelines per action (tour): %s", ActionList(g.tour_action_new).c_str());
    if (!g.replay_before.empty() && !g.replay_after.empty()) {
        Counters b, a;
        for (const auto& [rp, c] : g.replay_before) b += c;
        for (const auto& [rp, c] : g.replay_after) a += c;
        Log("stutter check: replay before the tour: %s", Counts(b).c_str());
        Log("stutter check: replay after the tour:  %s", Counts(a).c_str());
    }
    const bool tour = std::find(g.phases.begin(), g.phases.end(), Phase::Tour) != g.phases.end();
    if (tour) AppendState("finished");
    SetStatus("World warper: finished\nSaving the shader cache...\n");
    g.stage = Stage::Done;
    g.at = now + 2.0;
}

void TickImpl(double now) {
    const GameSnapshot s = ReadGameState(false);
    if (g.stage > Stage::Settle && g.stage < Stage::Done && now - g.curve_at >= 60.0) {
        // The pipeline curve: new pipelines over time (the plateau check).
        g.curve_at = now;
        Log("curve: %.0f s, %llu new pipelines this session, %zu areas done, %zu enemy kinds", now - g.start_at,
            static_cast<unsigned long long>(cp::created_total.load() - g.c_session0.created), g.progress.done.size(),
            g.progress.kinds.size());
    }
    if (s.player && Steady(s) && g.stage >= Stage::Next && g.stage <= Stage::Routine) KeepAlive(s.player);
    switch (g.stage) {
    case Stage::Title:
        if (Steady(s)) {
            g.stage = Stage::Settle;
            g.steady_since = now;
            Log("in the world; starting in a few seconds");
            SetStatus("World warper\nIn the world\nStarting...");
            return;
        }
        if (g.title_press_at > 0 && now - g.title_press_at >= 0.12) {
            runtime_pad_director_press(0);
            g.title_press_at = 0;
        }
        if (!s.world_up) {
            // The title screens (TitleScreenStep): the first-launch brightness and controls screens
            // (a save folder without the game's system data) are confirmed with their defaults;
            // a title without Continue (no character to continue) ends the run - a Cross there
            // would start a New Game.
            // Text lookup times (steady clock ms; a screen looks its texts up when it builds).
            const std::int64_t ms = static_cast<std::int64_t>(now * 1000.0);
            const TitleScreen screen = ClassifyTitle(BbGameMenu::TextSeenMs("brightness"),
                                                     BbGameMenu::TextSeenMs("Camera X-Axis"),
                                                     BbGameMenu::TextSeenMs("Continue"),
                                                     BbGameMenu::TextSeenMs("New Game"), ms);
            // The brightness and controls screens are the New Game's (live 2026-10-16: a save
            // folder without a character went title -> brightness -> controls -> the opening):
            // the warper never plays through them; the run ends as for a title without Continue.
            if (screen == TitleScreen::Brightness || screen == TitleScreen::Controls ||
                screen == TitleScreen::NoCharacter) {
                Log("FAILED: the title has no Continue (no character in this save folder to continue); "
                    "the warper needs a save with a character");
                SetStatus("World warper: FAILED\nNo character to continue in this save folder\n");
                g.stage = Stage::Done;
                g.at = now + 3.0;
                return;
            }
        }
        // The title menu built with New Game and no Continue yet: no Cross until it is clear
        // (a Cross there would start a New Game; ClassifyTitle ends the run 1 s later).
        if (!s.world_up && TitleUndecided(BbGameMenu::TextSeenMs("Continue"), BbGameMenu::TextSeenMs("New Game"))) {
            return;
        }
        if (!s.world_up && now - g.at >= 2.5 && now - g.start_at > 4.0) {
            // The title: Cross until the save is continued (as the lamp travel test).
            g.at = now;
            runtime_pad_director_press(kPadCross);
            g.title_press_at = now;
            if (++g.title_presses <= 3 || g.title_presses % 20 == 0) Log("title: Cross #%d", g.title_presses);
            SetStatus("World warper\nContinuing the save...\n");
        }
        if (now - g.start_at > 300.0) {
            Log("FAILED: no world within 300 s of the start (no save to continue?)");
            SetStatus("World warper: FAILED\nNo world: is there a save to continue?\n");
            g.stage = Stage::Done;
            g.at = now + 3.0;
        }
        return;
    case Stage::Settle:
        if (!Steady(s)) {
            g.steady_since = now;
            return;
        }
        if (now - g.steady_since < 4.0) return;
        if (g.opt.dump) DumpRecord();
        BeginPhase(now);
        return;
    case Stage::Next: {
        if (now < g.at) return;
        if (!Steady(s)) return;  // a load (a death) first
        const Phase ph = g.phases[g.phase_i];
        g.idx = ph == Phase::Tour ? NextIndex(g.route, g.progress, g.idx) : g.idx;
        while (ph == Phase::Tod && g.idx < g.route.size() &&
               g.progress.tod_done.count(std::uint64_t(static_cast<u32>(g.tod_of_row[g.idx])) << 32 | g.route[g.idx])) {
            ++g.idx;
        }
        if (g.idx >= g.route.size()) {
            Log("%s finished: %s", PhaseName(ph), Counts(Counters::Now() - g.c_phase).c_str());
            if (ph == Phase::Tod && g.progress.tod_orig >= 0 && TodValue() != g.progress.tod_orig) {
                SetTod(g.progress.tod_orig);  // the save's own time of day back
            }
            if (++g.phase_i < g.phases.size()) {
                BeginPhase(now);
            } else {
                Finish(now);
            }
            return;
        }
        g.rp = g.route[g.idx++];
        if (ph == Phase::Tour) {
            AppendState("start " + std::to_string(g.rp));
            ++g.progress.starts[g.rp];
        } else if (ph == Phase::Tod) {
            const int v = g.tod_of_row[g.idx - 1];
            if (TodValue() != v) SetTod(v);
        }
        g.c_warp = Counters::Now();
        g.warp_at = now;
        cp::TakeWorstFrameUs();
        g.action_new.clear();
        g.enemies_used.clear();
        g.enemy_round = 0;
        g.load_seen = false;
        g.interrupted = false;
        Log("%s: warping to %s (%s)", PhaseName(ph), RowName(g.rp).c_str(), MapName(MapOfRow(g.rp)).c_str());
        reinterpret_cast<void(BB_COOP_SYSV*)(u32)>(Guest(kLampWarp))(g.rp);
        g.stage = Stage::WaitLoad;
        g.stage_at = now;
        UpdateStatus("loading");
        return;
    }
    case Stage::WaitLoad:
        if (!Steady(s)) {
            g.load_seen = true;
            g.stage = Stage::Arrive;
            g.stage_at = now;
            g.steady_since = 0;
            return;
        }
        if (now - g.stage_at > 25.0) {
            Log("row %u: no loading screen within 25 s of the warp: skipped", g.rp);
            if (g.phases[g.phase_i] == Phase::Tour) {
                AppendState("skip " + std::to_string(g.rp) + " no load");
                g.progress.skipped.insert(g.rp);
            }
            ++g.skipped_rows;
            g.rp = 0;
            g.stage = Stage::Next;
            g.at = now;
        }
        return;
    case Stage::Arrive:
        if (!Steady(s)) {
            g.steady_since = 0;
            if (now - g.stage_at > 240.0) {
                // Stuck in the load: end the game; the launcher starts it again and the progress
                // file's "start" lines skip this row after kMaxAttempts.
                Log("FAILED: row %u: still loading 240 s after the warp; ending the game (the next start resumes)",
                    g.rp);
                Release();
                BbHardExit(3);
            }
            return;
        }
        if (g.steady_since == 0) {
            g.steady_since = now;
            g.arrive_at = now;
            g.c_arrive = Counters::Now();
            UpdateStatus("arrived");
            // The row's gear, in the world as the equipment menu changes it (the chr loads the new
            // parts at once and the next load keeps them).
            g.gear_wait = 0;
            if (!g.interrupted && g.phases[g.phase_i] == Phase::Tour && g.opt.gear && g.opt.act) {
                PutOnGear(g.idx - 1);
                g.gear_wait = 4.0;
            }
        }
        if (now - g.steady_since < 2.5 + g.gear_wait) return;
        if (g.interrupted) {
            // Back after a load in the middle of the routine (a death, a fall, an event warp): the
            // row is done; running it again from the respawn could repeat the same fall.
            FinishRow(s, now, " (interrupted by a load)");
            return;
        }
        StartSteps(RoutineSteps(s), now);
        g.sub = Sub::Steps;
        g.stage = Stage::Routine;
        g.routine_at = now;
        g.enemy_pending = false;
        g.enemy_next_at = 0;
        UpdateStatus("looking round");
        return;
    case Stage::Routine: {
        if (!Steady(s)) {
            // A load in the middle (the player died, or an event moved it): wait, then go on.
            Release();
            g.interrupted = true;
            g.stage = Stage::Arrive;
            g.stage_at = now;
            g.steady_since = 0;
            Log("row %u: a load during the routine (step %zu of %zu); finishing the row after it", g.rp, g.step_i,
                g.steps.size());
            g.steps.clear();
            return;
        }
        if (g.interrupted && g.steps.empty()) {
            FinishRow(s, now, " (interrupted by a load)");
            return;
        }
        if (!RunSteps(now)) {
            static double last = 0;
            if (now - last > 1.0 && g.step_i < g.steps.size()) {
                last = now;
                UpdateStatus(g.steps[g.step_i].what);
            }
            return;
        }
        const Phase ph = g.phases[g.phase_i];
        if (g.sub == Sub::Steps) g.sub = Sub::Effects;
        if (g.sub == Sub::Effects) {
            // Once per run (the first row): every item's effect on the player, 1.2 s apart.
            if (ph == Phase::Tour && g.effect_i < g.effects.size()) {
                if (now < g.effect_at) return;
                if (g.effect_i == 0) {
                    g.c_step = Counters::Now();
                    Log("item effects: %zu SpEffects of the goods (%zu shown in earlier sessions)", g.effects.size(),
                        g.progress.fx.size());
                }
                const int fx = g.effects[g.effect_i];
                AppendState("fx " + std::to_string(fx));  // first: a crash in it is not repeated
                g.progress.fx.insert(static_cast<u32>(fx));
                if (ApplyEffect(s.player, fx)) ++g.effects_applied;
                ++g.effect_i;
                g.effect_at = now + 1.2;
                UpdateStatus("item effects");
                if (g.effect_i == g.effects.size()) {
                    const Counters c = Counters::Now();
                    g.action_new["item effects"] += c.created - g.c_step.created;
                    g.c_step = c;
                    AppendState("effects");
                    g.progress.effects_done = true;
                    Log("item effects: %zu applied, +%llu new pipelines", g.effects_applied,
                        static_cast<unsigned long long>(g.action_new["item effects"]));
                }
                return;
            }
            g.sub = Sub::Gestures;
            g.pass = ph == Phase::Tour && g.opt.act && g.opt.gestures && (s.map_id >> 24) != 21 ? GesturesLeft(g.progress)
                                                                                               : std::vector<u32>{};
            g.pass_i = 0;
            g.pass_stage = 0;
            g.pass_at = now;
            if (g.opt.gestures_max > 0 && g.pass.size() > static_cast<std::size_t>(g.opt.gestures_max))
                g.pass.resize(static_cast<std::size_t>(g.opt.gestures_max));
            if (!g.pass.empty()) {
                g.c_step = Counters::Now();
                Log("gestures: %zu to play (%zu in earlier sessions)", g.pass.size(), g.progress.gestures.size());
                g.pass_ok0 = g.gestures_ok;
            }
        }
        if (g.sub == Sub::Gestures) {
            // Each gesture as the gesture menu asks for it (MenuMan +0x2BC = its param id), held
            // kGestureSeconds, then a dodge ends a looping one (Pray, Sit Down, ...).
            if (g.pass_i < g.pass.size()) {
                if (now < g.pass_at) return;
                const u32 id = g.pass[g.pass_i];
                if (g.pass_stage == 0) {
                    AppendState("gesture " + std::to_string(id));
                    g.progress.gestures.insert(id);
                    const u64 menu = Field<u64>(Guest(kMenuMan), 0);
                    const int v = static_cast<int>(id);
                    g.anims_before = RecentAnims(s.player);
                    const bool ok = menu && SafeWrite(menu + kMenuGesture, &v, 4);
                    if (ok) ++g.gestures_played;
                    if (!ok) Log("gesture %u (%s): NOT asked (no menu manager)", id, GestureName(id));
                    UpdateStatus("gestures");
                    g.pass_stage = 1;
                    g.pass_at = now + kGestureSeconds;
                    return;
                }
                if (g.pass_stage == 1) {
                    // Verified only when the player's animations show a gesture (a000_104xxx) that
                    // was not there before the request; else logged as not played (unverified).
                    const bool seen = GestureSeen(g.anims_before, RecentAnims(s.player));
                    if (seen) {
                        ++g.gestures_ok;
                        AppendState("gestureok " + std::to_string(id));
                        g.progress.gestures_ok.insert(id);
                    }
                    Log("gesture %u (%s): %s", id, GestureName(id),
                        seen ? "played" : "NOT played (skipped, unverified)");
                    if (!seen) {  // nothing to end: no dodge; a request left unread is withdrawn
                        const u64 menu = Field<u64>(Guest(kMenuMan), 0);
                        const int zero = 0;
                        if (menu) SafeWrite(menu + kMenuGesture, &zero, 4);
                        g.pass_stage = 0;
                        g.pass_at = now + 0.5;
                        if (++g.pass_i == g.pass.size()) {
                            Log("gestures: %zu of %zu played", g.gestures_ok - g.pass_ok0, g.pass.size());
                        }
                        return;
                    }
                    runtime_pad_director_press(kCircle);  // ends a looping gesture
                    g.pass_stage = 2;
                    g.pass_at = now + 0.15;
                    return;
                }
                runtime_pad_director_press(0);
                g.pass_stage = 0;
                g.pass_at = now + 1.2;
                if (g.pass_i + 1 == g.pass.size()) Log("gestures: %zu of %zu played", g.gestures_ok - g.pass_ok0, g.pass.size());
                if (++g.pass_i == g.pass.size()) {
                    const Counters c = Counters::Now();
                    g.action_new["gestures"] += c.created - g.c_step.created;
                    g.c_step = c;
                }
                return;
            }
            g.sub = Sub::Tools;
            g.pass = ph == Phase::Tour && g.opt.act && g.opt.tools && (s.map_id >> 24) != 21 ? ToolsLeft(g.progress)
                                                                                            : std::vector<u32>{};
            g.pass_i = 0;
            g.pass_stage = 0;
            g.pass_at = now;
            if (g.opt.tools_max > 0 && g.pass.size() > static_cast<std::size_t>(g.opt.tools_max))
                g.pass.resize(static_cast<std::size_t>(g.opt.tools_max));
            if (!g.pass.empty()) {
                g.c_step = Counters::Now();
                g.quick0_before = QuickItemId(0);
                g.pass_ok0 = g.tools_used;
                Log("hunter tools: %zu to use (%zu in earlier sessions); quick slot 0 held %u", g.pass.size(),
                    g.progress.tools.size(), g.quick0_before);
            }
        }
        if (g.sub == Sub::Tools) {
            // Each tool / throwable from quick slot 0 (selected), Square, kToolSeconds; quicksilver
            // bullets topped up first (the tools' cost), HP kept full as everywhere.
            if (g.pass_i < g.pass.size()) {
                if (now < g.pass_at) return;
                const u32 id = g.pass[g.pass_i];
                if (g.pass_stage == 0) {
                    AppendState("tool " + std::to_string(id));
                    g.progress.tools.insert(id);
                    if (ItemCount(kItemGoods, kQuicksilverBullets) < 20) GiveItem(kItemGoods, kQuicksilverBullets, 20);
                    if (ItemCount(kItemGoods, id) < 2) GiveItem(kItemGoods, id, 3);
                    const bool ok = SetQuickItem(0, id, true);
                    bool sel = false;
                    const u32 now_id = QuickItemId(0, &sel);
                    Log("tool %u: quick slot 0 = %u%s%s", id, now_id, sel ? " (selected)" : "", ok ? "" : ": NOT set");
                    UpdateStatus("hunter tools");
                    g.pass_stage = ok ? 1 : 3;
                    g.pass_at = now + 0.5;
                    return;
                }
                if (g.pass_stage == 1) {
                    g.tool_held = ItemCount(kItemGoods, id);
                    g.tool_bullets = ItemCount(kItemGoods, kQuicksilverBullets);
                    runtime_pad_director_press(kSquare);
                    g.pass_stage = 2;
                    g.pass_at = now + 0.15;
                    return;
                }
                if (g.pass_stage == 2) {  // Square released; the use is checked after the action
                    runtime_pad_director_press(0);
                    g.pass_stage = 4;
                    g.pass_at = now + kToolSeconds;
                    return;
                }
                if (g.pass_stage == 4) {
                    // Verified by what the use costs: one of the item (throwables) or quicksilver
                    // bullets (tools). Neither changed: not counted, logged as unverified.
                    const int held = ItemCount(kItemGoods, id), bullets = ItemCount(kItemGoods, kQuicksilverBullets);
                    const bool used = ToolUseSeen(g.tool_held, held, g.tool_bullets, bullets);
                    if (used) {
                        ++g.tools_used;
                        AppendState("toolok " + std::to_string(id));
                        g.progress.tools_ok.insert(id);
                    }
                    Log("tool %u: %s (held %d -> %d, quicksilver bullets %d -> %d)", id,
                        used ? "used" : "NOT used (skipped, unverified)", g.tool_held, held, g.tool_bullets, bullets);
                }
                runtime_pad_director_press(0);
                g.pass_at = now + 0.3;
                g.pass_stage = 0;
                if (++g.pass_i == g.pass.size()) {
                    const Counters c = Counters::Now();
                    g.action_new["hunter tools"] += c.created - g.c_step.created;
                    g.c_step = c;
                    if (g.quick0_before) SetQuickItem(0, g.quick0_before, true);
                    Log("hunter tools: %zu of %zu used (verified by their cost), +%llu new pipelines", g.tools_used - g.pass_ok0, g.pass.size(),
                        static_cast<unsigned long long>(g.action_new["hunter tools"]));
                }
                return;
            }
            g.sub = Sub::Remo;
            g.remos = ph == Phase::Tour && g.opt.remo && !g.opt.remo_only ? RemosFor(s.map_id) : std::vector<u32>{};
            if (ph == Phase::Remo) g.remos = {g.remo_of_row[g.idx - 1]};
            // One cutscene a row: a second one asked for right after the first ended the game
            // (live 2026-10-15, twice, exit 1); the map's next one waits for its next row.
            if (g.remos.size() > 1) g.remos.resize(1);
            g.remo_i = 0;
            g.remo_stage = 0;
            g.remo_at = now;
        }
        if (g.sub == Sub::Remo) {
            // The map's cutscenes (each once per run; marked before it plays, so a crash in one
            // does not repeat it).
            if (g.remo_i < g.remos.size()) {
                const u32 id = g.remos[g.remo_i];
                const bool playing = CutscenePlaying();
                if (g.remo_stage == 0) {
                    if (now - g.remo_at < 1.0) return;
                    AppendState("remo " + std::to_string(id));
                    g.progress.remos.insert(id);
                    ++g.progress.remo_tries[id];
                    g.c_step = Counters::Now();
                    const u64 ok = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64)>(Guest(kPlayRemo))(
                        0, id, 0, 0xffffffffu, 0xffffffffu, 10000);
                    Log("row %u: cutscene %u %s", g.rp, id, (ok & 0xff) ? "queued" : "NOT queued");
                    UpdateStatus("cutscene");
                    g.remo_stage = (ok & 0xff) ? 1 : 3;
                    g.remo_at = now;
                    return;
                }
                if (g.remo_stage == 1) {  // waiting for it to start
                    if (playing) {
                        g.remo_stage = 2;
                        g.remo_at = now;
                        ++g.remos_played;
                        return;
                    }
                    if (now - g.remo_at < 8.0) return;
                    Log("row %u: cutscene %u did not start", g.rp, id);
                } else if (g.remo_stage == 2) {  // playing: to its end (a skip after 150 s)
                    if (playing && now - g.remo_at < g.opt.remo_skip) return;
                    if (playing && now - g.remo_at < g.opt.remo_skip + 20.0) {
                        RequestRemoSkip();
                        return;
                    }
                    if (!g.progress.remos_ok.count(id)) {
                        AppendState("remook " + std::to_string(id));
                        g.progress.remos_ok.insert(id);
                    }
                    Log("row %u: cutscene %u played (%.0f s)", g.rp, id, now - g.remo_at);
                }
                const Counters c = Counters::Now();
                g.action_new["cutscene"] += c.created - g.c_step.created;
                g.c_step = c;
                ++g.remo_i;
                g.remo_stage = 0;
                g.remo_at = now;
                return;
            }
            if (g.remo_i > 0 && now - g.remo_at < 4.0) return;  // the scene's fade back
            g.sub = Sub::Enemies;
            g.c_step = Counters::Now();
        }
        // Enemies (the tour only): every kind not seen yet in the run (a boss is fought but left
        // alive), else the nearest regular one; each fought briefly, then killed.
        if (ph == Phase::Tour && g.opt.enemies && g.enemy_round < std::max(1, g.opt.enemies_per_row)) {
            if (!g.enemy_pending) {
                float d = 0;
                EnemyTarget t;
                bool boss = false;
                u32 kind = 0;
                if (!PickEnemy(g.rp, &t, &d, &boss, &kind)) {
                    if (g.enemy_round == 0) Log("row %u: no living enemy here", g.rp);
                    g.enemy_round = 1 << 20;
                } else {
                    g.target = t;
                    g.target_boss = boss;
                    g.target_kind = kind;
                    g.enemy_pending = true;
                    ++g.enemy_round;
                    g.boss_try = -1;
                    if (boss) {
                        // Into its arena: candidate spots round it, each checked first (below).
                        g.boss_try = 0;
                        g.boss_check = 0;
                        g.boss_check_at = now + 1.5;
                        while (g.boss_try < kBossSpots &&
                               !PlaceAtBoss(s.player, t, g.boss_try, g.boss_pos, g.boss_spot))
                            ++g.boss_try;
                    } else {
                        GoToEnemy(s.player, t);
                    }
                    if (kind && !g.progress.kinds.count(kind)) {
                        g.progress.kinds.insert(kind);
                        AppendState("kind " + std::to_string(kind));
                    }
                    Log("row %u: %s %u (kind %u), %.0f m from the spot (HP %d); %zu kinds in the run", g.rp,
                        boss ? "boss" : "enemy", t.entity, kind, d, balance::EnemyHp(t.chr), g.progress.kinds.size());
                    UpdateStatus(boss ? "fighting a boss" : "fighting");
                    std::vector<PadStep> fight = {
                        {0.6f, 0, -1, -1, -1, -1, "enemy"},     {0.15f, kR1, -1, -1, -1, -1, "enemy"},
                        {0.9f, 0, -1, -1, -1, -1, "enemy"},     {0.15f, kR1, -1, -1, -1, -1, "enemy"},
                        {1.2f, 0, -1, -1, -1, -1, "enemy"},     {0.15f, kL2, -1, -1, -1, -1, "enemy"},
                        {1.0f, 0, -1, -1, -1, -1, "enemy"},
                    };
                    if (boss) {  // its attacks and effects: 20 s more, dodging (it comes to the arena entry)
                        for (int i = 0; i < 10; ++i) {
                            fight.push_back({0.15f, kR1, -1, -1, -1, -1, "boss"});
                            fight.push_back({1.0f, 0, -1, -1, -1, -1, "boss"});
                            fight.push_back({0.2f, kCircle, 128, 255, -1, -1, "boss"});
                            fight.push_back({0.7f, 0, -1, -1, -1, -1, "boss"});
                        }
                    }
                    StartSteps(fight, now);
                    g.enemy_kill_at = 0;
                }
                return;
            }
            if (g.boss_try >= 0 && g.boss_check < 2) {
                // The boss spot check: settled on the boss's floor, then a short walk toward it.
                if (now < g.boss_check_at) return;
                float me[3];
                if (!PlayerPos(s.player, me)) return;
                if (g.boss_check == 0) {
                    std::copy(me, me + 3, g.boss_check_pos);
                    runtime_pad_director_sticks(128, 0, -1, -1);  // forward: the camera follows the facing
                    g.boss_check = 1;
                    g.boss_check_at = now + 0.8;
                    return;
                }
                Release();
                const bool spot_ok = g.boss_try == 0 ? ArenaSpotOk(g.boss_spot, g.boss_check_pos, me)
                                                     : BossSpotOk(g.boss_pos, g.boss_check_pos, me);
                if (spot_ok) {
                    ++g.bosses_safe;
                    Log("row %u: boss %u: spot %d (%s) is safe (%.1f m from it, height %+.1f m); fighting", g.rp,
                        g.target.entity, g.boss_try, g.boss_try == 0 ? "the arena's entry" : "round the boss",
                        std::sqrt((me[0] - g.boss_pos[0]) * (me[0] - g.boss_pos[0]) +
                                  (me[2] - g.boss_pos[2]) * (me[2] - g.boss_pos[2])),
                        me[1] - g.boss_pos[1]);
                    g.boss_check = 2;
                    return;
                }
                Log("row %u: boss %u: spot %d not usable (settled %.1f/%.1f/%.1f, walked %.1f/%.1f/%.1f, boss "
                    "%.1f/%.1f/%.1f)", g.rp, g.target.entity, g.boss_try, g.boss_check_pos[0], g.boss_check_pos[1],
                    g.boss_check_pos[2], me[0], me[1], me[2], g.boss_pos[0], g.boss_pos[1], g.boss_pos[2]);
                while (++g.boss_try < kBossSpots) {
                    if (!PlaceAtBoss(s.player, g.target, g.boss_try, g.boss_pos, g.boss_spot)) continue;
                    g.boss_check = 0;
                    g.boss_check_at = now + 1.5;
                    return;
                }
                ++g.bosses_unsafe;
                Log("row %u: boss %u: no safe spot in its arena; left alone", g.rp, g.target.entity);
                BackToSpot(s.player, g.rp);
                g.steps.clear();
                g.boss_try = -1;
                g.enemy_pending = false;
                return;
            }
            if (!RunSteps(now)) return;
            if (g.enemy_kill_at == 0) {
                if (g.target_boss) {
                    ++g.bosses;  // left alive: its defeat flags and scenes stay the game's
                } else {
                    if (g.opt.kill) KillEnemy(g.target);
                    ++g.kills;
                }
                g.enemy_kill_at = now;
                UpdateStatus(g.target_boss ? "boss fought" : "enemy killed");
                return;
            }
            if (now - g.enemy_kill_at < (g.target_boss ? 0.5 : 3.0)) return;  // the death animation
            const Counters c = Counters::Now();
            g.action_new[g.target_boss ? "boss" : "enemy"] += c.created - g.c_step.created;
            g.c_step = c;
            g.enemy_pending = false;
            return;
        }
        BackToSpot(s.player, g.rp);
        FinishRow(s, now, g.interrupted ? " (interrupted by a load)" : "");
        return;
    }
    case Stage::Finish:
        return;
    case Stage::Done:
        if (now < g.at) return;
        g.on = false;
        g_active = false;
        Log("ending the game (shader cache saved)");
        BbHardExit(0);
    }
}

BB_COOP_SYSV void OwnTickEntry(u64, u64, u64, u64, u64, u64) {
    Tick();
}

} // namespace

void Install(unsigned char* image, std::uint64_t size) {
    const char* env = std::getenv("BB_WORLD_WARPER");
    g.opt = ParseOptions(env);
    if (g.opt.mode == Mode::Off) return;
    if (PartyRequested()) {
        std::printf("World warper: off: party mode is on (the warper runs solo only)\n");
        return;
    }
    if (!HooksInit(image, size)) return;
    if (!MatchesOriginal(kLampWarp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53}) ||
        !MatchesOriginal(kForceDead, {0x55, 0x48, 0x89, 0xe5})) {
        std::printf("World warper: NOT installed: the code differs from 1.09\n");
        return;
    }
    if (const char* p = std::getenv("BB_WORLD_WARPER_STATE"); p && p[0]) {
        g.state_path = p;
    } else if (const char* u = std::getenv("BB_GPU_USER_DIR"); u && u[0]) {
        g.state_path = std::string(u) + "/world_warper_state.txt";
    }
    switch (g.opt.mode) {
    case Mode::Replay: g.phases = {Phase::ReplayBefore}; break;
    case Mode::Measure: g.phases = {Phase::ReplayBefore, Phase::Tour, Phase::ReplayAfter}; break;
    default:
        g.phases = {Phase::Tour};
        if (g.opt.remo && g.opt.remo_pass) g.phases.push_back(Phase::Remo);  // the cutscenes the tour's rows could not play
        if (g.opt.tod) g.phases.push_back(Phase::Tod);
        if (g.opt.remo_only) g.phases = {Phase::Remo};
        break;
    }
    if (!lamptravel::OwnsTick()) {
        HookPrologue(kFlipperUpdate,
                     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
                      0x38},
                     &OwnTickEntry, "world warper main-thread tick");
    }
    g.on = true;
    g_active = true;
    SetStatus("World warper\nStarting...\n");
    std::printf("World warper: on (%s; actions %s, enemies %s, gear %s, cutscenes %s, time of day %s, chalice %s; "
                "progress file %s)\n", env, g.opt.act ? "on" : "off", g.opt.enemies ? "on" : "off",
                g.opt.gear ? "on" : "off", g.opt.remo ? "on" : "off", g.opt.tod ? "on" : "off",
                g.opt.chalice ? "on" : "off",
                g.state_path.empty() ? "(none)" : g.state_path.c_str());
}

void Tick() {
    if (!g.on) return;
    const double now = Now();
    if (g.start_at == 0) {
        g.start_at = now;
        g.c_session0 = Counters::Now();
    }
    TickImpl(now);
}

bool Active() {
    return g_active.load(std::memory_order_relaxed);
}

std::string StatusText() {
    std::lock_guard<std::mutex> lk(g_status_mu);
    return g_status;
}

} // namespace coop::warper
