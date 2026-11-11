// SPDX-License-Identifier: GPL-3.0-or-later
// 0.8.1 seamless join and party travel: the held / settled loading screen drop and the frame-end quiet point
// (party_seam.h, seam_rules.h; research/reseam081.md 3.1 / 3.2). The frame end / render thread quiet point was
// found with droogie/bbhost (GPL-3.0-or-later, src/engine/live_resolution.cpp at bdc5cdf); ported by intent.
#include "party_seam.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "seam_rules.h"
#include "../bbport_settings.h"
#include "../bbport_stall_probe.h"
#include "../net/bbnet_internal.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace coop::seam {

namespace {

using u64 = std::uint64_t;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

// Our offsets (bbhost - 0x400000).
constexpr u64 kInGameStartClearCall = 0x193a755;  // InGameStart 0x193A3E0: call NowLoading clear (result unused)
constexpr u64 kNowLoadingClear = 0x176d3c0;       // request byte 0x556286B = 0, fade out (+0x804 = 3)
constexpr u64 kNowLoadingByte = 0x556286b;
constexpr u64 kWorldChrMan = 0x553e878;           // +0x60 the local PlayerIns (null while a map loads)
constexpr u64 kInGameStartStep = 0x193a3e0;       // MoveMapStep step 12 (the screen gate); rdi = the MoveMapStep
constexpr u64 kStepResolveCalls[2] = {0x19396eb, 0x193993f};  // step 7: call 0x19423A0 (spawn position -> +0xF0 / +0x100)
constexpr u64 kStepResolve = 0x19423a0;
constexpr u64 kStepApplyCall = 0x1939c02;         // step 9: call 0x1939C80 (spawn position -> the chr), again
constexpr u64 kStepApply = 0x1939c80;
constexpr u64 kStepHoldTimer = 0x278, kStepMap = 0x58, kStepPos = 0xf0, kStepRot = 0x100;  // MoveMapStep fields
constexpr double kHoldCapS = 60.0;                // a hold never outlives this (the B2 phases end sooner)
constexpr double kQuietBudgetMs = 4.0;

std::atomic<bool> g_hook{false}, g_frame{false};
std::atomic<bool> g_native_hook{false};   // step 12's prologue hook is in
std::atomic<bool> g_native_hold{false};   // the hold in force is the native one (g_hold too)
std::atomic<bool> g_native_seen{false};   // step 12 was held natively at least one frame (InGameStart has not run)
std::atomic<bool> g_forget_clear{false};  // the next InGameStart clear is swallowed (a native hold released "not dropping")
std::atomic<bool> g_after_native{false};  // the clear that follows a native release (for the log)
std::atomic<bool> g_place_hook{false};
struct Place {
    std::atomic<bool> armed{false}, applied{false}, refused{false};
    unsigned stage = 0;
    float pos[4] = {}, rot[4] = {};
    float shift_m = -1;
} g_place;
std::atomic<bool> g_hold{false};          // A
std::atomic<bool> g_pending{false};       // InGameStart's clear not run yet (held or settling)
double g_hold_at = 0, g_pending_at = 0;   // main thread (ms)
std::string g_hold_why;
SettleWatch g_watch;                      // B (main thread)
bool g_watch_for_hold = false;            // the settle watch started at a hold's release

struct Quiet {
    const char* name;
    std::function<void()> fn;
};
std::mutex g_qmu;
std::deque<Quiet> g_queue;

double NowMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("[coop] Party seam: ") + line + "\n");
}

bool PartyProcess() { return bbnet::settings().party; }

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0';
}

bool QuietPointOn() {
    static const bool on = !(std::getenv("BB_PARTY_QUIET_POINT") && std::getenv("BB_PARTY_QUIET_POINT")[0] == '0');
    return on && g_frame.load();
}

u64 Slot(u64 off) {
    u64 v = 0;
    return SafeGet(Guest(off), &v) ? v : 0;
}

bool MapLoading() {
    u64 player = 0;
    const u64 wcm = Slot(kWorldChrMan);
    return !wcm || !SafeGet(wcm + 0x60, &player) || !player;
}

void Clear(const char* why) {
    const double now = NowMs();
    const double after = g_pending.load() ? now - g_pending_at : 0.0;
    g_pending = false;
    using Fn = u64(BB_COOP_SYSV*)();
    reinterpret_cast<Fn>(Guest(kNowLoadingClear))();
    Log("loading screen dropped (%s), %.0f ms after InGameStart", why, after);
}

/// The settle watch's end: the drop, at the quiet point (we are in it).
void SettledDrop(const char* how) {
    char why[160];
    std::snprintf(why, sizeof why, "%s: %d frame(s) in %.0f ms, worst %.0f ms%s", how, g_watch.frames,
                  g_watch.HeldMs(NowMs()), g_watch.worst_ms, g_watch_for_hold ? ", after the hold" : "");
    Clear(why);
}

BB_COOP_SYSV u64 InGameStartClearHook() {
    const double now = NowMs();
    if (g_forget_clear.exchange(false)) {
        g_after_native = false;
        Log("InGameStart: the loading screen stays up for the next load (the native hold was given up)");
        return 0;
    }
    if (g_hold.load()) {
        g_pending = true;
        g_pending_at = now;
        Log("InGameStart: the loading screen stays up (held: %s)", g_hold_why.c_str());
        return 0;
    }
    if (SettledDropOn() && g_frame.load()) {
        g_pending = true;
        g_pending_at = now;
        g_watch_for_hold = g_after_native.exchange(false);
        g_watch.Start(now);
        Log("InGameStart: the loading screen stays up until frames run under %.0f ms for %.0f ms (at most %.0f ms)",
            kSettledFrameMs, kSettledRunMs, kSettleCapMs);
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)();
    const u64 r = reinterpret_cast<Fn>(Guest(kNowLoadingClear))();
    if (PartyProcess()) Log("InGameStart: loading screen dropped (vanilla)");
    return r;
}

/// MoveMapStep step 12 (the screen gate), before it runs: a native hold keeps its hold timer above zero, so the gate
/// stays shut and neither InGameStart nor the in-game update start (remp081 3.3). The timer is counted down per frame
/// by the step's update, so it is written every frame the step runs. Also: a placement that was armed but not written
/// (the C hold fallback) holds the screen here.
BB_COOP_SYSV void MoveMapGateHook(u64 step, u64, u64, u64, u64, u64) {
    if (g_place.armed.load() && !g_place.applied.load() && !g_hold.load()) {
        // The load did not take the beside-host spot (far from the lamp, another block, hook missed): hide the arrival
        // warp under the held screen as before this hook existed.
        g_hold_why = "load beside the host: the placement was not written; until the arrival warp";
        g_hold_at = NowMs();
        g_native_seen = false;
        g_native_hold = NativeHoldIn();
        g_hold = true;
        Log("hold: the loading screen stays up (%s)", g_hold_why.c_str());
    }
    if (!g_native_hold.load() || !g_hold.load()) return;
    if (NowMs() - g_hold_at > kHoldCapS * 1000.0) {
        g_native_hold = false;  // the InGameStart hook (g_hold) takes over: it drops at its cap
        Log("native hold (%s): over %.0f s; the InGameStart hook takes over", g_hold_why.c_str(), kHoldCapS);
        return;
    }
    float cur = 0;
    if (!SafeGet(step + kStepHoldTimer, &cur)) return;
    if (!(cur > kNativeHoldS * 0.5f)) {
        SafeWrite(step + kStepHoldTimer, &kNativeHoldS, 4);
    }
    if (!g_native_seen.exchange(true)) {
        Log("native hold: MoveMapStep's screen gate is held (hold timer +0x278 %.2f -> %.2f); InGameStart waits (%s)",
            double(cur), double(kNativeHoldS), g_hold_why.c_str());
    }
}

bool FiniteVec(const float* v, int n) {
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1e5f) return false;
    }
    return true;
}

/// The armed spot into the MoveMapStep `step` (after the game resolved its own spawn position into +0xF0 / +0x100, or
/// again before the chr gets it). Once per load; later calls only repeat it.
void PlaceInto(u64 step, const char* where) {
    if (!g_place.armed.load()) return;
    u32 map = 0;
    float own[4] = {}, own_rot[4] = {};
    if (!SafeGet(step + kStepMap, &map) || !SafeRead(step + kStepPos, own, 16) || !SafeRead(step + kStepRot, own_rot, 16)) {
        return;
    }
    if (g_place.applied.load()) {
        // Again (step 9): the spot must still be there; whoever overwrote it, the chr gets this one.
        if (std::memcmp(own, g_place.pos, 12) != 0) {
            SafeWrite(step + kStepPos, g_place.pos, 16);
            SafeWrite(step + kStepRot, g_place.rot, 16);
            Log("load placement (%s): the spot was overwritten meanwhile; written again", where);
        }
        return;
    }
    if (g_place.refused.load()) return;
    const bool stage_ok = (map >> 16) == (g_place.stage >> 16);
    const double dx = double(own[0]) - g_place.pos[0], dy = double(own[1]) - g_place.pos[1],
                 dz = double(own[2]) - g_place.pos[2];
    const bool own_ok = FiniteVec(own, 3);
    const double shift = own_ok ? std::sqrt(dx * dx + dy * dy + dz * dz) : -1.0;
    if (const char* why = PlacementRefusal(stage_ok, FiniteVec(g_place.pos, 3) && FiniteVec(g_place.rot, 4), shift)) {
        g_place.refused = true;
        Log("load placement (%s): not written: %s (load map %#x, spot's %#x, %.1f m)", where, why, map, g_place.stage, shift);
        return;
    }
    // The game's own w stays (the position is a 3-vector in a 4-vector).
    g_place.pos[3] = own[3];
    SafeWrite(step + kStepPos, g_place.pos, 16);
    SafeWrite(step + kStepRot, g_place.rot, 16);
    g_place.shift_m = float(shift);
    g_place.applied = true;
    Log("load placement (%s): the load's spawn position (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) beside the host, %.2f m, "
        "inside the load (no arrival warp)", where, double(own[0]), double(own[1]), double(own[2]), double(g_place.pos[0]),
        double(g_place.pos[1]), double(g_place.pos[2]), shift);
}

BB_COOP_SYSV u64 ResolveHook(u64 a0, u64 a1, u64 a2, u64 a3) {
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64);
    const u64 r = reinterpret_cast<Fn>(Guest(kStepResolve))(a0, a1, a2, a3);
    PlaceInto(a0, "step 7");
    return r;
}

BB_COOP_SYSV u64 ApplyHook(u64 step) {
    PlaceInto(step, "step 9");
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kStepApply))(step);
}

void RunQueue(double budget_ms) {
    const double start = NowMs();
    for (;;) {
        Quiet q;
        {
            std::lock_guard<std::mutex> lk(g_qmu);
            if (g_queue.empty()) return;
            q = std::move(g_queue.front());
            g_queue.pop_front();
        }
        const double t0 = NowMs();
        q.fn();
        const double dt = NowMs() - t0;
        if (dt > 2.0) Log("quiet point: %s took %.1f ms", q.name, dt);
        if (NowMs() - start >= budget_ms) return;
    }
}

void OnFrameEnd(u64 prev_exit, u64 /*entry*/, u64 exit) {
    RunQueue(kQuietBudgetMs);
    if (!g_pending.load()) return;
    const double now = NowMs();
    u8 req = 0;
    if (SafeGet(Guest(kNowLoadingByte), &req) && !req) {
        g_pending = false;  // cleared by someone else meanwhile (FallbackToB1's drop, a vanilla path)
        g_watch.active = false;
        Log("held drop: the screen was cleared meanwhile; nothing to do");
        return;
    }
    if (MapLoading()) {
        // A new map load started under the held screen: its own InGameStart drops it.
        g_pending = false;
        g_watch.active = false;
        Log("held drop: a map load started under the screen; its InGameStart clears it");
        return;
    }
    if (g_hold.load()) {
        if (now - g_hold_at > kHoldCapS * 1000.0) {
            g_hold = false;
            Log("hold (%s): over %.0f s; the screen goes", g_hold_why.c_str(), kHoldCapS);
            Clear("hold cap");
        }
        return;
    }
    if (g_watch.active) {
        const double frame_ms = prev_exit ? double(exit - prev_exit) / 1e6 : -1.0;
        if (g_watch.Frame(frame_ms, now)) SettledDrop(g_watch.capped ? "cap" : "settled");
        return;
    }
    Clear("released at the quiet point");
}

}  // namespace

bool ParallelFollowOn() {
    return PartyProcess() && BbSettings::Get().party_parallel_follow.load(std::memory_order_relaxed);
}
bool SettledDropOn() {
    return PartyProcess() && BbSettings::Get().party_settled_drop.load(std::memory_order_relaxed);
}
bool HoldHookIn() { return g_hook.load(); }
bool NativeHoldIn() { return g_native_hook.load(); }
bool NativeHeld() { return g_hold.load() && g_native_hold.load() && g_native_seen.load(); }
bool HoldIsNative() { return g_hold.load() && g_native_hold.load(); }
bool PlacementHookIn() { return g_place_hook.load() && !EnvOff("BB_PARTY_LOAD_PLACE"); }

void ArmPlacement(unsigned stage, const float pos[4], const float rot[4]) {
    if (!PlacementHookIn()) return;
    g_place.armed = false;
    g_place.applied = false;
    g_place.refused = false;
    g_place.shift_m = -1;
    g_place.stage = stage;
    std::memcpy(g_place.pos, pos, 16);
    std::memcpy(g_place.rot, rot, 16);
    g_place.armed = true;
    Log("load placement armed: map %#x beside the host at (%.2f, %.2f, %.2f)", stage, double(pos[0]), double(pos[1]),
        double(pos[2]));
}

void DisarmPlacement(const char* why) {
    if (!g_place.armed.exchange(false)) return;
    Log("load placement disarmed (%s): %s", why, g_place.applied.load() ? "it was written into the load" : "it was not written");
    g_place.applied = false;
    g_place.refused = false;
}

PlacementState Placement() {
    PlacementState p;
    p.armed = g_place.armed.load();
    p.applied = g_place.applied.load();
    p.shift_m = g_place.shift_m;
    return p;
}
bool Holding() { return g_hold.load(); }
bool ScreenHeld() { return g_hold.load() || g_pending.load(); }

void Hold(const char* why) {
    if (!g_hook.load()) return;
    g_hold_why = why ? why : "";
    g_hold_at = NowMs();
    g_native_seen = false;
    g_native_hold = NativeHoldIn();
    g_hold = true;
    Log("hold: the loading screen stays up past InGameStart (%s)%s", g_hold_why.c_str(),
        g_native_hold.load() ? ", natively: the screen gate is held, InGameStart waits" : "");
}

bool ReleaseHold(const char* why, bool drop) {
    if (!g_hold.exchange(false)) return false;
    const bool native = g_native_hold.exchange(false) && g_native_seen.exchange(false);
    const bool was = g_pending.load() || native;
    const double now = NowMs();
    if (native) {
        // InGameStart has not run under this hold: the gate opens at its next frame and its clear goes through the hook
        // (B's settle watch, else the vanilla drop); "not dropping" swallows that one clear.
        Log("hold released (%s) after %.0f ms; %s", why, now - g_hold_at,
            drop ? "the gate opens, InGameStart runs and the screen goes" : "the screen stays for the next load");
        if (drop) {
            g_after_native = true;
        } else {
            g_forget_clear = true;
        }
        return true;
    }
    Log("hold released (%s) after %.0f ms; %s", why, now - g_hold_at,
        !was ? "InGameStart had not run under it" : drop ? "the screen goes" : "the screen stays for the next load");
    if (!was) return false;
    if (!drop) {
        g_pending = false;
        return true;
    }
    if (SettledDropOn() && g_frame.load()) {
        g_watch_for_hold = true;
        g_watch.Start(now);
        return true;
    }
    if (g_frame.load()) return true;  // OnFrameEnd drops it at the next quiet point
    Clear(why);
    return true;
}

void AtQuietPoint(const char* name, std::function<void()> fn) {
    if (!QuietPointOn() || !SettledDropOn()) {
        fn();
        return;
    }
    std::lock_guard<std::mutex> lk(g_qmu);
    g_queue.push_back({name, std::move(fn)});
}

void Install() {
    if (std::getenv("BB_PARTY_SEAM_HOOKS") && std::getenv("BB_PARTY_SEAM_HOOKS")[0] == '0') {
        Log("hooks off (BB_PARTY_SEAM_HOOKS=0): no held / settled screen drop, no quiet point");
        return;
    }
    // "probe" in the names keeps both out of the party's hook-set check: the hooks are pass-through unless
    // this game's own (local) toggles use them, so two games may differ there.
    g_hook = HookCallSite(kInGameStartClearCall, kNowLoadingClear, reinterpret_cast<const void*>(&InGameStartClearHook),
                          "seam probe: InGameStart screen clear");
    const char* nh = std::getenv("BB_PARTY_NATIVE_HOLD");
    if (!(nh && nh[0] == '1')) {
        // Measured (seam2): +0.7 s per guest screen and more long frames after the drop than the InGameStart clear hook, and
        // an item description box on the guest at the drop; so opt-in.
        Log("native hold off (BB_PARTY_NATIVE_HOLD=1 enables): the InGameStart clear hook alone holds");
    } else {
        // 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58: push rbp; mov rbp,rsp; push r15..r12, rbx; sub rsp,0x58
        g_native_hook = HookPrologue(kInGameStartStep, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                        0x53, 0x48, 0x83, 0xec, 0x58},
                                     &MoveMapGateHook, "seam probe: MoveMapStep screen gate (step 12)");
        if (!g_native_hook.load()) Log("native hold: step 12's prologue differs (the code differs); the InGameStart hook holds");
    }
    if (EnvOff("BB_PARTY_LOAD_PLACE")) {
        Log("load placement off (BB_PARTY_LOAD_PLACE=0)");
    } else {
        const bool a = HookCallSite(kStepResolveCalls[0], kStepResolve, reinterpret_cast<const void*>(&ResolveHook),
                                    "seam probe: MoveMapStep spawn position (step 7 a)");
        const bool b = HookCallSite(kStepResolveCalls[1], kStepResolve, reinterpret_cast<const void*>(&ResolveHook),
                                    "seam probe: MoveMapStep spawn position (step 7 b)");
        const bool c = HookCallSite(kStepApplyCall, kStepApply, reinterpret_cast<const void*>(&ApplyHook),
                                    "seam probe: MoveMapStep spawn position (step 9)");
        g_place_hook = a && b && c;
        if (!g_place_hook.load()) Log("load placement: a call site differs (%d %d %d); no in-load placement", a, b, c);
    }
    g_frame = BbStallProbe::SetFrameEndCallback(&OnFrameEnd);
    Log("InGameStart clear hook %s, native hold %s, load placement %s, frame-end quiet point %s (party_parallel_follow / "
        "party_settled_drop / party_load_beside)",
        g_hook.load() ? "in" : "NOT in (the code differs)", NativeHoldIn() ? "in" : "off", PlacementHookIn() ? "in" : "off",
        g_frame.load() ? "in" : "NOT in");
}

}  // namespace coop::seam
