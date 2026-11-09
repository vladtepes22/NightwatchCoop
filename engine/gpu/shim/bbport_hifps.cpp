// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: high frame rate fixes ported from droogie/bbhost (GPL-3.0-or-later), by intent and into our structure.
// Their addresses minus 0x400000 are ours. Above 30 fps a few of the game's effects ran faster than at 30, and
// one menu box lost an early press. Each fix below only matters above 30, and each has a kill switch:
//
//  - the lights' blink (torches, braziers, candles; bbhost f24d18f): BB_BLINK_HOLD=0
//  - the water wakes characters leave (bbhost 57d2b9f): BB_WATER_GATE=0
//  - a message / Yes-No / quantity box takes OK as soon as it is up (bbhost 3b7668b, 602a958): BB_MENU_OK_AT_ONCE=0
//
// (The plants' wind, bbhost 5cee912, lives with the other frame-rate patches in bbport_frame_rate.cpp.)
//
// The "whole step" the blink and the water follow is BbEngine::LiveClock::WholeStep30(): 30 frames a game second at
// any rate, every frame at 30. Nightwatch: only under the Live clock (the "60 FPS++ (Deltatime)" patch); the native
// rate's fixed lists (engine/frame_rate.cpp) keep the game's own behaviour here, as visy's FPS++ mode does.
#include "bbport_hifps.h"

#include "engine/live_clock.h"
#include "party/coop_hooks.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" double runtime_pad_confirm_press_age(void);

namespace BbHiFps {
namespace {

using coop::u64;
using coop::u8;

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && (!std::strcmp(v, "0") || !std::strcmp(v, "off") || !std::strcmp(v, "false"));
}

template <class T>
T Load(u64 at) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), sizeof(T));
    return v;
}
template <class T>
void Store(u64 at, T v) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(at)), &v, sizeof(T));
}

// ---- The lights' blink (sub_0xea25f0; bbhost f24d18f) ----
// A GX light keeps a blink (+0x14/+0x18 the shortest and longest cycle, both 0: no blink; +0x20 the factor's high;
// +0x24 the cycle's length; +0x28 the time in it; +0x2c the factor the light's colour is scaled by; +0x4..+0x10 the
// light's own xorshift state). Once the time passes the length a new cycle begins - a length drawn from the light's
// generator, the time 0 - and the time past the length is dropped, half a frame on average: 1/60 s at 30 but
// 1/240 s at 120, so the lights blink 5-7% faster above 30 (4.95 cycles a game second at 30, 5.31 at 120).
// A cycle begins only on a whole 1/30 s step here; on the frames between, a light past its length holds the factor
// its next cycle begins with (the game's update run on a copy, so the light's generator is left for the whole step
// to draw from) while its time moves on.
constexpr u64 kBlinkUpdate = 0xea25f0;
constexpr u64 kShortest = 0x14, kLongest = 0x18, kHigh = 0x20, kLength = 0x24, kTime = 0x28, kFactor = 0x2c;
constexpr u64 kBlinkBytes = 0x30;

// Runtime switches of the sweep (BB_HIFPS_SWEEP): the fixes on (default) or the game's own behaviour.
std::atomic<bool> g_blink_on{true}, g_water_on{true};

using BlinkFn = void(BB_COOP_SYSV*)(u64 blink, float step);
BlinkFn g_blink_game = nullptr;
std::atomic<std::uint64_t> g_blink_begun{0}, g_blink_held{0};
std::atomic<std::uint64_t> g_blink_excess_us{0};  // the time past a cycle's length when it began, summed (diagnostics)

void BB_COOP_SYSV BlinkUpdate(u64 blink, float step) {
    if (Load<float>(blink + kShortest) == 0.0f && Load<float>(blink + kLongest) == 0.0f) return g_blink_game(blink, step);
    const float length = Load<float>(blink + kLength), time = Load<float>(blink + kTime);
    if (!(time > length)) return g_blink_game(blink, step);  // the game's test (a NaN is not past)
    if (!g_blink_on.load(std::memory_order_relaxed) || BbEngine::LiveClock::WholeStep30Exact()) {
        g_blink_begun.fetch_add(1, std::memory_order_relaxed);
        if (time - length < 1.0f) g_blink_excess_us.fetch_add(std::uint64_t((time - length) * 1e6f), std::memory_order_relaxed);
        return g_blink_game(blink, step);  // a whole step: the game's new cycle
    }
    alignas(16) u8 next[kBlinkBytes];
    std::memcpy(next, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(blink)), kBlinkBytes);
    g_blink_game(reinterpret_cast<u64>(next), 0.0f);
    Store<float>(blink + kFactor, Load<float>(reinterpret_cast<u64>(next) + kFactor));
    Store<float>(blink + kTime, time + step);
    g_blink_held.fetch_add(1, std::memory_order_relaxed);
}

// ---- The water wakes (sub_0x2277780 the manager's update, sub_0x2277e50 its render task; bbhost 57d2b9f) ----
// GX's water interaction keeps a grid of heights with three height maps (+0x10 the one a step writes and the water
// surfaces read, +0xd0 the last step's, +0xd8 the one before). Each frame the manager's update turns the maps round
// and hands each water surface the impulses queued for it; the render task splats them and runs one wave step over a
// fixed time. So the wakes spread at the frame rate: twice as fast at 60, four times at 120. Ours steps on the
// whole-1/30-s frames only: on the others the update's turn of the maps is undone, the task returns at once, and
// the impulses the update handed on wait for the next update to put them back in their queue. In screen-space mode
// (the default) the grid follows the camera in whole units, so a frame on which the camera's truncated x or z moved
// steps too and takes its 1/30 s from the next whole step.
constexpr u64 kWaterUpdate = 0x2277780, kWaterTask = 0x2277e50;
constexpr u64 kContextCamera = 0x3f8;
constexpr u64 kManagerHolders = 0x18, kManagerSurfaces = 0x40, kManagerScreenHolder = 0x60, kManagerNoHolder = 0x68;
constexpr u64 kManagerOn = 0x78, kManagerScreenSpace = 0x79;
constexpr u64 kHolderInUse = 0x18;
constexpr u64 kGridMap = 0x10, kGridLast = 0xd0, kGridBefore = 0xd8, kGridKind = 0x30;
constexpr u64 kSurfaceTaken = 0x98, kSurfaceQueued = 0xa0;
constexpr u64 kImpulseBytes = 0x20;

using WaterUpdateFn = u64(BB_COOP_SYSV*)(u64 manager);
using WaterTaskFn = u64(BB_COOP_SYSV*)(u64 self, u64 context, u64 param);
WaterUpdateFn g_water_update_game = nullptr;
WaterTaskFn g_water_task_game = nullptr;
// Our task has run. Before that the update holds no frame: the game's task would step the maps the update turned back.
std::atomic<bool> g_task_ours{false};

constexpr int kManagers = 4, kGrids = 32;
struct Gate {
    std::atomic<u64> manager{0};
    bool started = false;
    bool have_camera = false;
    float camera_x = 0.0f, camera_z = 0.0f;  // trunc(camera) at the last step
    int ahead = 0;                           // a camera step took the next whole step's place
    bool decided = false;                    // its update ran since its task last did
    bool waiting = false;                    // a held frame's update handed impulses on that no task took
    bool held = false;                       // this frame's update held the step
    int turned = 0;                          // the grids that update turned round, in its order
    u64 grids[kGrids];
};
Gate g_gates[kManagers];
std::atomic<std::uint64_t> g_frames{0}, g_camera_steps{0}, g_held{0}, g_kept{0}, g_lost{0};

Gate* GateOf(u64 manager, bool claim) {
    for (Gate& g : g_gates) {
        u64 have = g.manager.load(std::memory_order_acquire);
        if (have == manager) return &g;
        if (!have && claim && (g.manager.compare_exchange_strong(have, manager, std::memory_order_acq_rel) || have == manager)) return &g;
    }
    return nullptr;
}

// sub_12bbda0's turn: the oldest map becomes the one to write.
void TurnRound(u64 grid) {
    const u64 map = Load<u64>(grid + kGridMap), last = Load<u64>(grid + kGridLast), before = Load<u64>(grid + kGridBefore);
    Store<u64>(grid + kGridMap, before);
    Store<u64>(grid + kGridLast, map);
    Store<u64>(grid + kGridBefore, last);
}
// And back: the map a step wrote is +0x10 again.
void TurnBack(u64 grid) {
    const u64 map = Load<u64>(grid + kGridMap), last = Load<u64>(grid + kGridLast), before = Load<u64>(grid + kGridBefore);
    Store<u64>(grid + kGridMap, last);
    Store<u64>(grid + kGridLast, before);
    Store<u64>(grid + kGridBefore, map);
}

// Whether the manager's update does anything: without these it returns at once.
bool UpdateRuns(u64 manager) { return Load<u8>(manager + kManagerOn) != 0 && Load<u64>(manager + kManagerNoHolder) != 0; }

// The grids the update is about to turn round, in its order: the grid of each holder in use (but the one for none),
// then screen-space mode's - each one with its maps and of the kind that steps (kind 3: the waves characters make).
// False when there are more than kGrids.
bool GridsToTurn(u64 manager, Gate& g) {
    g.turned = 0;
    if (!UpdateRuns(manager)) return true;
    auto add = [&](u64 grid) {
        if (!grid || !Load<u64>(grid + kGridMap) || Load<u8>(grid + kGridKind) != 3) return true;
        if (g.turned == kGrids) return false;
        g.grids[g.turned++] = grid;
        return true;
    };
    const u64 none = Load<u64>(manager + kManagerNoHolder);
    const u64 first = Load<u64>(manager + kManagerHolders), end = Load<u64>(manager + kManagerHolders + 8);
    for (u64 at = first; at < end; at += 8) {
        const u64 holder = Load<u64>(at);
        if (!holder || !Load<u8>(holder + kHolderInUse) || holder == none) continue;
        if (!add(Load<u64>(holder))) return false;
    }
    const u64 screen = Load<u64>(manager + kManagerScreenHolder);
    return !screen || add(Load<u64>(screen));
}

// A held frame's impulses back in their queues, before the update hands the queues on: each water surface's +0x98
// vector still holds what the held frame's update handed it; they go to the end of its +0xa0 queue in the room the
// queue has, or (the queue empty and without the room) the two vectors swap places as the update's own swap does,
// when they come from one heap. Else they are dropped. The +0x98 vector is left empty either way.
void RequeueImpulses(u64 manager) {
    const u64 first = Load<u64>(manager + kManagerSurfaces), end = Load<u64>(manager + kManagerSurfaces + 8);
    for (u64 at = first; at < end; at += 8) {
        const u64 surface = Load<u64>(at);
        if (!surface) continue;
        const u64 taken = Load<u64>(surface + kSurfaceTaken), queued = Load<u64>(surface + kSurfaceQueued);
        if (!taken || !queued || taken == queued) continue;
        const u64 t_first = Load<u64>(taken + 8), t_end = Load<u64>(taken + 0x10);
        if (t_end <= t_first) continue;
        const u64 bytes = t_end - t_first;
        const u64 q_first = Load<u64>(queued + 8), q_end = Load<u64>(queued + 0x10), q_room = Load<u64>(queued + 0x18);
        if (q_end >= q_first && q_room >= q_end && q_room - q_end >= bytes) {
            std::memmove(reinterpret_cast<void*>(static_cast<std::uintptr_t>(q_end)),
                         reinterpret_cast<const void*>(static_cast<std::uintptr_t>(t_first)), bytes);
            Store<u64>(queued + 0x10, q_end + bytes);
            Store<u64>(taken + 0x10, t_first);
            g_kept.fetch_add(bytes / kImpulseBytes, std::memory_order_relaxed);
        } else if (q_end == q_first && Load<u64>(taken + 0x20) == Load<u64>(queued + 0x20)) {
            for (u64 f = 8; f <= 0x18; f += 8) {
                const u64 a = Load<u64>(taken + f);
                Store<u64>(taken + f, Load<u64>(queued + f));
                Store<u64>(queued + f, a);
            }
            g_kept.fetch_add(bytes / kImpulseBytes, std::memory_order_relaxed);
        } else {
            Store<u64>(taken + 0x10, t_first);
            g_lost.fetch_add(bytes / kImpulseBytes, std::memory_order_relaxed);
        }
    }
}

float Trunc(float x) { return std::fabs(x) < 1e7f ? static_cast<float>(static_cast<long long>(x)) : x; }

u64 BB_COOP_SYSV WaterUpdate(u64 manager) {
    Gate* g = manager && g_task_ours.load(std::memory_order_relaxed) ? GateOf(manager, true) : nullptr;
    if (!g) return g_water_update_game(manager);
    if (g->waiting) RequeueImpulses(manager);
    g->waiting = false;
    g->decided = true;
    g->turned = 0;
    g->held = false;
    bool step = true;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (!g_water_on.load(std::memory_order_relaxed)) {
        if (g->waiting) RequeueImpulses(manager);  // switched off after a held frame: its impulses go back first
        g->waiting = false;
        g->started = true;
        g->ahead = 0;
        return g_water_update_game(manager);  // the game's: a step every frame
    }
    if (!g->started) {
        g->started = true;  // the first frame steps
    } else if (g->ahead > 0 && BbEngine::LiveClock::WholeStep30()) {
        g->ahead = 0;  // a camera step already took this step's place
        step = false;
    } else if (!BbEngine::LiveClock::WholeStep30()) {
        step = false;
    }
    if (step) return g_water_update_game(manager);
    if (!GridsToTurn(manager, *g)) {
        g->turned = 0;  // more grids than kept: this frame steps
        return g_water_update_game(manager);
    }
    // The impulses it hands on wait where the task reads them, for this frame's camera step or the next update.
    g->waiting = UpdateRuns(manager);
    g->held = true;
    const u64 r = g_water_update_game(manager);
    for (int i = g->turned - 1; i >= 0; --i) TurnBack(g->grids[i]);
    g_held.fetch_add(1, std::memory_order_relaxed);
    return r;
}

u64 BB_COOP_SYSV WaterTask(u64 self, u64 context, u64 param) {
    g_task_ours.store(true, std::memory_order_relaxed);
    const u64 manager = param ? Load<u64>(param) : 0;
    Gate* g = manager ? GateOf(manager, false) : nullptr;
    if (!g || !g->decided) return g_water_task_game(self, context, param);
    g->decided = false;
    bool scroll = Load<u8>(manager + kManagerScreenSpace) != 0;
    float cx = 0.0f, cz = 0.0f;
    const u64 camera = context ? Load<u64>(context + kContextCamera) : 0;
    if (scroll && camera) {
        cx = Trunc(Load<float>(camera + 0x30));
        cz = Trunc(Load<float>(camera + 0x38));
    } else {
        scroll = false;
    }
    bool camera_step = false;
    if (g->held) {
        if (!scroll || (g->have_camera && cx == g->camera_x && cz == g->camera_z)) return 0;  // held: nothing to step
        camera_step = true;  // the camera crossed a unit on a held frame: step after all, one step ahead
        g->ahead = 1;
        for (int i = 0; i < g->turned; ++i) TurnRound(g->grids[i]);  // its grids as the update left them
        g->waiting = false;                                           // and its impulses this step's
    }
    if (scroll) {
        g->have_camera = true;
        g->camera_x = cx;
        g->camera_z = cz;
    }
    g->turned = 0;
    if (camera_step) g_camera_steps.fetch_add(1, std::memory_order_relaxed);
    return g_water_task_game(self, context, param);
}

// ---- The boxes' OK (0x1beaad0; bbhost 3b7668b, 602a958) ----
// A message, Yes/No or quantity box (Hunter's Mark, quitting, buying, selling, fortifying, discarding) takes OK only once it has been up its minimum (+0xde0, 0.5 s; +0x1f10 the seconds up, which the box's
// update adds the frame's seconds to): an early press was lost. OK raises the time up to the minimum, but only for
// a Cross or Circle press that went down after the box opened: the press that confirms the level-up window (and
// opens its Yes/No box) is still held when the box comes up, and answering at once would answer its own box with No.
constexpr u64 kBoxConfirm = 0x1beaad0;
constexpr u64 kBoxUpMinimum = 0xde0, kBoxUpTime = 0x1f10;
std::atomic<std::uint64_t> g_box_early{0}, g_box_held{0};
bool g_box_on = true, g_box_log = false;  // BB_MENU_OK_AT_ONCE=0: the game's own wait (still logged under BB_HIFPS_LOG)

void BB_COOP_SYSV BoxConfirm(u64 box, u64, u64, u64, u64, u64) {
    if (!box) return;
    const float least = Load<float>(box + kBoxUpMinimum), shown = Load<float>(box + kBoxUpTime);
    if (!(shown < least)) return;
    const double age = runtime_pad_confirm_press_age();
    const bool fresh = age < static_cast<double>(shown);
    if (g_box_log) {
        std::printf("High-FPS fixes: box %llx OK after %.3f s of its %.3f s, the confirm went down %.3f s ago: %s\n",
                    (unsigned long long)box, double(shown), double(least), age,
                    !g_box_on ? "the game's wait (switch off)" : fresh ? "OK at once" : "a held press waits");
        std::fflush(stdout);
    }
    if (!g_box_on) return;
    if (fresh) {
        Store<float>(box + kBoxUpTime, least);  // up long enough: the OK goes through
        g_box_early.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_box_held.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

void Install() {
    // Only above 30 fps: with the frame-rate hooks off the game's own behaviour is the right one for all three
    // (the boxes' OK is independent of the frame rate, but is kept with the rest of the fixes' install).
    const bool fast = BbEngine::LiveClock::Active();
    const bool blink = fast && !EnvOff("BB_BLINK_HOLD") &&
                       coop::ReplacePrologue(kBlinkUpdate, {0xc5, 0xfa, 0x10, 0x57, 0x14, 0xc5, 0xf0, 0x57, 0xc9, 0xc5, 0xf8, 0x2e, 0xd1},
                                             reinterpret_cast<const void*>(&BlinkUpdate), reinterpret_cast<void**>(&g_blink_game),
                                             "hifps: lights blink at 30's pace");
    bool water = false;
    if (fast && !EnvOff("BB_WATER_GATE")) {
        const bool a = coop::ReplacePrologue(kWaterUpdate,
                                             {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
                                              0xec, 0x68, 0x49, 0x89, 0xfd},
                                             reinterpret_cast<const void*>(&WaterUpdate), reinterpret_cast<void**>(&g_water_update_game),
                                             "hifps: water wakes update");
        // Both or neither: an update that holds frames needs the task that skips them.
        water = a && coop::ReplacePrologue(kWaterTask,
                                           {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                            0xec, 0x28, 0x03, 0x00, 0x00},
                                           reinterpret_cast<const void*>(&WaterTask), reinterpret_cast<void**>(&g_water_task_game),
                                           "hifps: water wakes task");
    }
    bool menu = false;
    g_box_on = !EnvOff("BB_MENU_OK_AT_ONCE");
    g_box_log = std::getenv("BB_HIFPS_LOG") != nullptr;
    if (g_box_on || g_box_log) {
        menu = coop::HookPrologue(kBoxConfirm, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x88, 0x01, 0x00, 0x00},
                                  &BoxConfirm, "hifps: box OK at once");
    }
    if (EnvOff("BB_HIFPS_LOG") == false && std::getenv("BB_HIFPS_LOG")) {
        std::thread([] {
            char line[512];
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                Report(line, sizeof line);
                std::printf("High-FPS fixes: %s\n", line);
                std::fflush(stdout);
            }
        }).detach();
    }
    std::printf("High-FPS fixes (droogie/bbhost): lights' blink at 30's pace %s, water wakes at 30 steps a second %s, "
                "box OK at once %s (Live clock %s)\n",
                blink ? "on" : "off", water ? "on" : "off", menu ? "on" : "off", fast ? "on" : "off");
    std::fflush(stdout);
}

void Report(char* out, std::size_t size) {
    std::snprintf(out, size,
                  "blink cycles begun %llu held %llu; water frames %llu stepped %llu (camera %llu) held %llu impulses kept %llu lost %llu; "
                  "box OK early %llu waited %llu",
                  (unsigned long long)g_blink_begun.load(), (unsigned long long)g_blink_held.load(),
                  (unsigned long long)g_frames.load(), (unsigned long long)(g_frames.load() - g_held.load() + g_camera_steps.load()),
                  (unsigned long long)g_camera_steps.load(),
                  (unsigned long long)g_held.load(), (unsigned long long)g_kept.load(), (unsigned long long)g_lost.load(),
                  (unsigned long long)g_box_early.load(), (unsigned long long)g_box_held.load());
}

}  // namespace BbHiFps
