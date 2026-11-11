// SPDX-License-Identifier: GPL-3.0-or-later
// Stall probe (see bbport_stall_probe.h / bbport_stall_probe_rules.h). Diagnostic, off by default.
// The frame end / render thread "done" event this splits on was found with droogie/bbhost
// (GPL-3.0-or-later, src/engine/live_resolution.cpp at bdc5cdf: its quiet point after the frame
// end); ported by intent.
#include "bbport_stall_probe.h"
#include "bbport_stall_probe_rules.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "party/coop_hooks.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace BbStallProbe {

std::atomic<int> g_zone{kZoneGame};
bool g_on = false;

namespace {

using u64 = std::uint64_t;

// Our offsets (bbhost - 0x400000), byte-checked by HookCallSite (each must be `call rel32` to its target).
constexpr u64 kPresentFrameEndCall = 0x1d97853;  // present step 0x1d976e0: call FrameEnd
constexpr u64 kFrameEnd = 0x2198fd0;             // GX device frame end (waits "done" at 0x21998db)
constexpr u64 kRenderDoneCall = 0x21b6a68;       // render thread 0x21b6910: call SetEvent(play +0x68, 1)
constexpr u64 kEventSet = 0x207e780;

int g_threshold_ms = 250;

u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

struct FrameRec {
    std::atomic<u64> frame{0}, prev_exit{0}, entry{0}, exit{0}, gx_done{0};
};
constexpr int kRing = 64;
FrameRec g_ring[kRing];
std::atomic<u64> g_frames{0};      // frame ends completed
std::atomic<u64> g_last_exit{0};   // the last frame end's exit
std::atomic<u64> g_entry{0};       // the current frame end's entry (while g_in_end)
std::atomic<bool> g_in_end{false};
std::atomic<u64> g_gx_done{0};     // the render thread's last "done"
std::atomic<unsigned long> g_main_tid{0}, g_gx_tid{0};
std::atomic<FrameEndFn> g_after_end{nullptr};  // the frame-end quiet point's user (party_seam.cpp)
bool g_end_hooked = false;

#ifdef _WIN32
unsigned long CurrentTid() { return GetCurrentThreadId(); }
#else
unsigned long CurrentTid() { return 0; }
#endif

BB_COOP_SYSV void FrameEndHook(u64 dev) {
    if (!g_on) {
        // Only the quiet point wants it (BB_STALL_PROBE off): the frame end, then its callback.
        const u64 entry = NowNs();
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kFrameEnd))(dev);
        const u64 exit = NowNs();
        const u64 prev = g_last_exit.exchange(exit, std::memory_order_relaxed);
        if (const FrameEndFn f = g_after_end.load(std::memory_order_acquire)) f(prev, entry, exit);
        return;
    }
    const u64 entry = NowNs();
    if (!g_main_tid.load(std::memory_order_relaxed)) {
        g_main_tid.store(CurrentTid(), std::memory_order_relaxed);
    }
    g_entry.store(entry, std::memory_order_relaxed);
    g_in_end.store(true, std::memory_order_release);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kFrameEnd))(dev);
    const u64 exit = NowNs();
    const u64 prev = g_last_exit.load(std::memory_order_relaxed);
    const u64 n = g_frames.load(std::memory_order_relaxed) + 1;
    FrameRec& r = g_ring[n % kRing];
    r.prev_exit.store(prev, std::memory_order_relaxed);
    r.entry.store(entry, std::memory_order_relaxed);
    r.exit.store(exit, std::memory_order_relaxed);
    r.gx_done.store(g_gx_done.load(std::memory_order_relaxed), std::memory_order_relaxed);
    r.frame.store(n, std::memory_order_release);
    g_last_exit.store(exit, std::memory_order_relaxed);
    g_in_end.store(false, std::memory_order_relaxed);
    g_frames.store(n, std::memory_order_release);
    if (const FrameEndFn f = g_after_end.load(std::memory_order_acquire)) f(prev, entry, exit);
}

BB_COOP_SYSV u64 RenderDoneHook(u64 ev, u64 value) {
    if (!g_gx_tid.load(std::memory_order_relaxed)) {
        g_gx_tid.store(CurrentTid(), std::memory_order_relaxed);
    }
    g_gx_done.store(NowNs(), std::memory_order_relaxed);
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kEventSet))(ev, value);
}

// ---- sampler (Windows: the thread is suspended, its context and stack read, then resumed) ----

constexpr int kMaxModules = 24;
const char* g_module_names[kMaxModules];
u64 g_module_bases[kMaxModules];
int g_module_count = 0;

struct Sampled {
    SampleTable main, gx;
    unsigned zone[kZoneCount] = {};
    unsigned main_in_end = 0;  // main-thread samples taken inside the frame end (render wait)
    void Clear() {
        main.Clear();
        gx.Clear();
        for (unsigned& z : zone) z = 0;
        main_in_end = 0;
    }
};

#ifdef _WIN32
int ModuleIndex(u64 addr, u64* off) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) ||
        !m) {
        *off = addr;
        return -2;
    }
    const u64 base = reinterpret_cast<u64>(m);
    *off = addr - base;
    for (int i = 0; i < g_module_count; ++i) {
        if (g_module_bases[i] == base) return i;
    }
    if (g_module_count >= kMaxModules - 1) return -2;  // the last slot stays "?"
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, sizeof path);
    const char* slash = std::strrchr(path, '\\');
    const char* name = slash ? slash + 1 : path;
    char* copy = static_cast<char*>(std::malloc(std::strlen(name) + 1));
    if (!copy) return -2;
    std::strcpy(copy, name);
    g_module_bases[g_module_count] = base;
    g_module_names[g_module_count] = copy;
    return g_module_count++;
}

/// One sample of `tid` into `t`. Nothing that may take a lock runs while it is suspended.
bool SampleThread(HANDLE th, SampleTable& t) {
    const u64 lo = reinterpret_cast<u64>(coop::Image()), hi = lo + coop::ImageSize();
    if (SuspendThread(th) == static_cast<DWORD>(-1)) return false;
    CONTEXT c;
    std::memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    u64 stack[192];
    SIZE_T got = 0;
    const BOOL ok = GetThreadContext(th, &c);
    if (ok && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), stack, sizeof stack, &got)) {
        got = 0;
    }
    ResumeThread(th);
    if (!ok) return false;
    const u64 rip = c.Rip;
    if (rip >= lo && rip < hi) {
        t.Add(-1, rip - lo, 0);
        return true;
    }
    u64 caller = 0;
    for (SIZE_T i = 0; i < got / 8; ++i) {
        if (stack[i] >= lo && stack[i] < hi) {
            caller = stack[i] - lo;
            break;
        }
    }
    // Outside our exe (a kernel wait in ntdll ...): which of our functions waits there.
    static const u64 exe_lo = reinterpret_cast<u64>(GetModuleHandleA(nullptr));
    static const u64 exe_hi = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_lo);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe_lo + dos->e_lfanew);
        return exe_lo + nt->OptionalHeader.SizeOfImage;
    }();
    u64 via = 0;
    if (rip < exe_lo || rip >= exe_hi) {
        for (SIZE_T i = 0; i < got / 8; ++i) {
            if (stack[i] >= exe_lo && stack[i] < exe_hi) {
                via = stack[i] - exe_lo + 1;
                break;
            }
        }
    }
    u64 off = 0;
    const int m = ModuleIndex(rip, &off);
    t.Add(m < 0 ? kMaxModules - 1 : m, off, caller, via);  // unknown module: the last slot's name ("?")
    return true;
}
#endif

void Report(const FrameRec& r, const Sampled& s, RateLimit& limit) {
    const u64 prev = r.prev_exit.load(), entry = r.entry.load(), exit = r.exit.load(), gx = r.gx_done.load();
    const Split sp = SplitFrame(prev, entry, exit);
    if (sp.total_ms < g_threshold_ms) return;
    unsigned dropped = 0;
    if (!limit.Allow(exit, &dropped)) return;
    // The render thread's last "done" before this frame end returned: after the main work ended
    // (it was still playing the previous frame) or before (idle, waiting for this frame).
    char gxs[96] = "render thread done: not seen";
    if (gx && gx >= entry) {
        std::snprintf(gxs, sizeof gxs, "render thread done %.1f ms into the frame end", double(gx - entry) / 1e6);
    } else if (gx && gx >= prev) {
        std::snprintf(gxs, sizeof gxs, "render thread done %.1f ms into the main work", double(gx - prev) / 1e6);
    } else if (gx) {
        std::snprintf(gxs, sizeof gxs, "render thread idle the whole frame");
    }
    const unsigned main_n = s.main.Total();
    std::string line;
    char b[256];
    std::snprintf(b, sizeof b, "Stall probe: %.0f ms frame: main thread %.0f ms, frame end %.0f ms (%s)", sp.total_ms,
                  sp.main_ms, sp.end_ms, gxs);
    line += b;
    if (main_n) {
        std::snprintf(b, sizeof b, "; main samples %u (party tick %u, in the frame end %u): ", main_n,
                      s.zone[kZonePartyTick], s.main_in_end);
        line += b;
        line += FormatTop(s.main, g_module_names, g_module_count, 5);
    }
    if (s.gx.Total()) {
        std::snprintf(b, sizeof b, "; render thread samples %u: ", s.gx.Total());
        line += b;
        line += FormatTop(s.gx, g_module_names, g_module_count, 4);
    }
    if (dropped) {
        std::snprintf(b, sizeof b, " (%u more stall lines dropped)", dropped);
        line += b;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

void SamplerMain() {
    static Sampled s;  // large: not on this thread's stack
    RateLimit limit(20, 10'000'000'000ull);
    const u64 sample_after_ns = u64(std::min(50, g_threshold_ms / 4)) * 1'000'000ull;
    u64 seen = g_frames.load(std::memory_order_acquire);
#ifdef _WIN32
    HANDLE main_th = nullptr, gx_th = nullptr;
    // Unknown host modules land in the last name slot.
    g_module_names[kMaxModules - 1] = "?";
#endif
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const u64 n = g_frames.load(std::memory_order_acquire);
        if (n != seen) {
            // Frames ended since the last look: the one sampled is the last of them (only a long
            // frame gets samples, and a long frame is the only one in its 1 ms poll).
            for (u64 f = (n - seen > kRing ? n - kRing + 1 : seen + 1); f <= n; ++f) {
                const FrameRec& r = g_ring[f % kRing];
                if (r.frame.load(std::memory_order_acquire) != f) continue;
                static const Sampled empty;
                Report(r, f == n ? s : empty, limit);
            }
            seen = n;
            s.Clear();
            continue;
        }
#ifdef _WIN32
        const u64 last = g_last_exit.load(std::memory_order_relaxed);
        if (!last || NowNs() - last < sample_after_ns) continue;
        if (!main_th && g_main_tid.load()) {
            main_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid.load());
        }
        if (!gx_th && g_gx_tid.load()) {
            gx_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_gx_tid.load());
        }
        const bool in_end = g_in_end.load(std::memory_order_acquire);
        const int zone = g_zone.load(std::memory_order_relaxed);
        if (main_th && SampleThread(main_th, s.main)) {
            if (zone >= 0 && zone < kZoneCount) ++s.zone[zone];
            if (in_end) ++s.main_in_end;
        }
        if (in_end && gx_th) {
            SampleThread(gx_th, s.gx);  // the main thread waits for it: where is it?
        }
#endif
    }
}

} // namespace

bool InstallFrameEnd() {
    if (g_end_hooked) return true;
    if (!coop::Image()) return false;
    // "probe" in the names: left out of the party's hook-set check (party_link.cpp). The frame-end hook
    // only times the call and runs the quiet point's callback after it (both games may differ there).
    g_end_hooked = coop::HookCallSite(kPresentFrameEndCall, kFrameEnd, reinterpret_cast<const void*>(&FrameEndHook),
                                      "stall probe: frame end");
    return g_end_hooked;
}

bool SetFrameEndCallback(FrameEndFn fn) {
    if (!InstallFrameEnd()) return false;
    g_after_end.store(fn, std::memory_order_release);
    return true;
}

void Install() {
    g_threshold_ms = ThresholdMsFromEnv(std::getenv("BB_STALL_PROBE"));
    if (!g_threshold_ms) return;
    if (!coop::Image()) {
        std::printf("Stall probe: no image; off\n");
        return;
    }
    const bool a = InstallFrameEnd();
    const bool b = coop::HookCallSite(kRenderDoneCall, kEventSet, reinterpret_cast<const void*>(&RenderDoneHook),
                                      "stall probe: render thread done");
    if (!a) {
        std::printf("Stall probe: the frame end call is not the expected code; off\n");
        return;
    }
    g_on = true;
    std::printf("Stall probe: on (BB_STALL_PROBE): frames over %d ms split into main work / frame end%s\n",
                g_threshold_ms, b ? ", render thread done seen" : " (render thread done hook not installed)");
    std::thread(SamplerMain).detach();
}

} // namespace BbStallProbe
