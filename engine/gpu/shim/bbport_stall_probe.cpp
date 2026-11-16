// SPDX-License-Identifier: GPL-3.0-or-later
// Stall probe (see bbport_stall_probe.h / bbport_stall_probe_rules.h). Diagnostic, off by default.
// The frame end / render thread "done" event this splits on was found with droogie/bbhost
// (GPL-3.0-or-later, src/engine/live_resolution.cpp at bdc5cdf: its quiet point after the frame
// end); ported by intent.
#include "bbport_stall_probe.h"
#include "bbport_stall_probe_rules.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "bbport_toggles.h"
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

// ---- raw stack dump (BB_STALL_PROBE_DUMP=<file>, hitch082) ----
// With the dump file set the sampler samples the main thread (M), the game's render thread (R), our GPU
// command thread (G), the draw pipe's recording thread (B, bb:DrawRec), the first Vulkan recorder thread
// (V, bb:VkRecorder) and the first guest copy thread (C, bb:Copy0) every ms all the time (not only once a frame runs long) into a ring, and writes the
// samples of every reported frame to the file: per sample the instruction pointer, the return
// addresses into our exe and into the game image found on the stack (values that follow a call
// instruction, in stack order: innermost first). tools/party/hitch_stacks.py symbolizes and sums them.
struct RawSample {
    u64 t = 0, rip = 0;
    std::uint8_t thread = 0, n_exe = 0, n_guest = 0;
    u64 exe[16] = {};
    u64 guest[8] = {};
};
constexpr int kRawRing = 6 * 4096;
RawSample* g_raw = nullptr;
u64 g_raw_n = 0;
std::FILE* g_dump = nullptr;

#ifdef _WIN32
u64 g_text_lo = 0, g_text_hi = 0;

void FindExeText() {
    const u64 base = reinterpret_cast<u64>(GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!std::memcmp(sec[i].Name, ".text", 6)) {
            g_text_lo = base + sec[i].VirtualAddress;
            g_text_hi = g_text_lo + sec[i].Misc.VirtualSize;
        }
    }
}

/// `a` follows a call instruction (e8 rel32, or an ff /2 form of 2, 3, 6 or 7 bytes).
bool AfterCall(u64 a) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(a);
    const auto reg2 = [](std::uint8_t modrm) { return ((modrm >> 3) & 7) == 2; };
    return p[-5] == 0xE8 || (p[-2] == 0xFF && reg2(p[-1])) || (p[-3] == 0xFF && reg2(p[-2])) ||
           (p[-6] == 0xFF && reg2(p[-5])) || (p[-7] == 0xFF && reg2(p[-6]));
}

void SampleRaw(HANDLE th, int thread) {
    static u64 stack[1024];
    if (SuspendThread(th) == static_cast<DWORD>(-1)) return;
    CONTEXT c;
    std::memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    SIZE_T got = 0;
    const BOOL ok = GetThreadContext(th, &c);
    if (ok && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), stack, sizeof stack, &got)) {
        // The stack ends before 8 KiB: the part up to the next page boundary.
        const SIZE_T part = 0x1000 - (c.Rsp & 0xFFF);
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), stack, part, &got)) got = 0;
    }
    ResumeThread(th);
    if (!ok) return;
    RawSample& r = g_raw[g_raw_n++ % kRawRing];
    r.t = NowNs();
    r.rip = c.Rip;
    r.thread = std::uint8_t(thread);
    r.n_exe = r.n_guest = 0;
    // Guest return addresses: the code segment only (it ends at 0x50d96dc). A stack value that points into
    // the game's data may sit on a page the GPU side read-protected: AfterCall reading it crashed the game.
    const u64 lo = reinterpret_cast<u64>(coop::Image()), hi = lo + std::min<u64>(coop::ImageSize(), 0x50d9000);
    for (SIZE_T i = 0; i < got / 8; ++i) {
        const u64 v = stack[i];
        if (v >= g_text_lo + 8 && v < g_text_hi) {
            if (r.n_exe < 16 && AfterCall(v)) r.exe[r.n_exe++] = v;
        } else if (v >= lo + 8 && v < hi) {
            if (r.n_guest < 8 && AfterCall(v)) r.guest[r.n_guest++] = v;
        }
    }
}

void DumpFrame(u64 from, u64 to, double total_ms, double main_ms, double end_ms) {
    if (!g_dump) return;
    const u64 exe = reinterpret_cast<u64>(GetModuleHandleA(nullptr));
    const u64 lo = reinterpret_cast<u64>(coop::Image()), hi = lo + coop::ImageSize();
    std::fprintf(g_dump, "frame %.1f ms main %.1f end %.1f at %.3f\n", total_ms, main_ms, end_ms, double(to) / 1e9);
    const u64 n = g_raw_n, first = n > kRawRing ? n - kRawRing : 0;
    for (u64 i = first; i < n; ++i) {
        const RawSample& r = g_raw[i % kRawRing];
        if (r.t < from || r.t > to) continue;
        std::fprintf(g_dump, "%c %.2f ", "MRGBVC"[r.thread % 6], double(r.t - from) / 1e6);
        if (r.rip >= g_text_lo && r.rip < g_text_hi) {
            std::fprintf(g_dump, "e%llx", static_cast<unsigned long long>(r.rip - exe));
        } else if (r.rip >= lo && r.rip < hi) {
            std::fprintf(g_dump, "g%llx", static_cast<unsigned long long>(r.rip - lo));
        } else {
            u64 off = 0;
            const int m = ModuleIndex(r.rip, &off);
            std::fprintf(g_dump, "%s+%llx", m >= 0 ? g_module_names[m] : "?", static_cast<unsigned long long>(off));
        }
        for (int k = 0; k < r.n_exe; ++k) std::fprintf(g_dump, " e%llx", static_cast<unsigned long long>(r.exe[k] - exe));
        for (int k = 0; k < r.n_guest; ++k) std::fprintf(g_dump, " g%llx", static_cast<unsigned long long>(r.guest[k] - lo));
        std::fputc('\n', g_dump);
    }
    std::fflush(g_dump);
}
#else
void DumpFrame(u64, u64, double, double, double) {}
#endif

void Report(const FrameRec& r, const Sampled& s, RateLimit& limit) {
    const u64 prev = r.prev_exit.load(), entry = r.entry.load(), exit = r.exit.load(), gx = r.gx_done.load();
    const Split sp = SplitFrame(prev, entry, exit);
    if (sp.total_ms < g_threshold_ms) return;
    DumpFrame(prev ? prev : entry, exit, sp.total_ms, sp.main_ms, sp.end_ms);
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
    HANDLE main_th = nullptr, gx_th = nullptr, gpu_th = nullptr, rec_th = nullptr, vk_th = nullptr, copy_th = nullptr;
    // Unknown host modules land in the last name slot.
    g_module_names[kMaxModules - 1] = "?";
#endif
#ifdef _WIN32
    // The dump wants a sample every ms; a plain sleep wakes at the system timer's 15.6 ms unless something
    // raised its rate. A high-resolution waitable timer does it for this thread only (the game's own sleeps
    // keep their granularity, so the probe does not change what it measures).
    HANDLE timer = g_dump ? CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS)
                          : nullptr;
#endif
    for (;;) {
#ifdef _WIN32
        LARGE_INTEGER due;
        due.QuadPart = -10000;  // 1 ms
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, 50);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
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
        if (!main_th && g_main_tid.load()) {
            main_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid.load());
        }
        if (!gx_th && g_gx_tid.load()) {
            gx_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_gx_tid.load());
        }
        if (g_dump) {
            if (!gpu_th && BbStats::gpu_tid.load()) {
                gpu_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, BbStats::gpu_tid.load());
            }
            if (main_th) SampleRaw(main_th, 0);
            if (gx_th) SampleRaw(gx_th, 1);
            if (gpu_th) SampleRaw(gpu_th, 2);
            if (!rec_th && BbStats::drawrec_tid.load()) {
                rec_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, BbStats::drawrec_tid.load());
            }
            if (rec_th) SampleRaw(rec_th, 3);
            if (!vk_th && BbStats::recorder_tid.load()) {
                vk_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, BbStats::recorder_tid.load());
            }
            if (vk_th) SampleRaw(vk_th, 4);
            if (!copy_th && BbStats::copy_tid.load()) {
                copy_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, BbStats::copy_tid.load());
            }
            if (copy_th) SampleRaw(copy_th, 5);
        }
        const u64 last = g_last_exit.load(std::memory_order_relaxed);
        if (!last || NowNs() - last < sample_after_ns) continue;
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

void NoteTsharp(const void* tsharp32, std::uint64_t address, unsigned width) {
    static const int want = [] {
        const char* e = std::getenv("BB_TSHARP_FIND");
        return e ? std::atoi(e) : 0;
    }();
    if (want <= 0) return;
#ifdef _WIN32
    static const u64 t0 = NowNs();
    static std::atomic<int> done{0};
    static std::atomic<u64> seen[64];
    if (width < 256 || NowNs() - t0 < 40'000'000'000ull || done.load() >= want) return;
    for (auto& a : seen) {
        u64 v = a.load(std::memory_order_relaxed);
        if (v == address) return;
        if (v == 0) {
            if (a.compare_exchange_strong(v, address)) break;
            if (v == address) return;
        }
    }
    if (done.fetch_add(1) >= want) return;
    unsigned char pat[32];
    std::memcpy(pat, tsharp32, 32);
    u64 q[4];
    std::memcpy(q, pat, 32);
    std::printf("T# find: texture %#llx width %u: %016llx %016llx %016llx %016llx\n", (unsigned long long)address,
                width, (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                (unsigned long long)q[3]);
    static unsigned char* buf = static_cast<unsigned char*>(std::malloc(1 << 20));
    int hits = 0;
    const HANDLE self = GetCurrentProcess();
    for (u64 a = 0x800000000ull; a < 0x2000000000ull && hits < 24;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi)) break;
        const u64 base = reinterpret_cast<u64>(mbi.BaseAddress), end = base + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY)) {
            for (u64 at = std::max(a, base); at < end && hits < 24; at += (1 << 20) - 64) {
                const SIZE_T n = SIZE_T(std::min<u64>(1 << 20, end - at));
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, reinterpret_cast<const void*>(at), buf, n, &got) || got < 16) continue;
                for (SIZE_T i = 0; i + 16 <= got; i += 4) {
                    if (std::memcmp(buf + i, pat, 16) != 0) continue;
                    const u64 hit = at + i;
                    std::printf("T# find:   hit at %#llx (full 32 bytes %s):", (unsigned long long)hit,
                                i + 32 <= got && !std::memcmp(buf + i, pat, 32) ? "equal" : "differ");
                    u64 w[20] = {};
                    SIZE_T g2 = 0;
                    ReadProcessMemory(self, reinterpret_cast<const void*>(hit - 96), w, sizeof w, &g2);
                    for (int k = 0; k < 20; ++k) std::printf(" %s%llx", k == 12 ? "| " : "", (unsigned long long)w[k]);
                    std::printf("\n");
                    ++hits;
                }
            }
        }
        a = end;
    }
    std::printf("T# find: %d hit(s)\n", hits);
    std::fflush(stdout);
#else
    (void)tsharp32;
    (void)address;
    (void)width;
#endif
}

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
#ifdef _WIN32
    if (const char* path = std::getenv("BB_STALL_PROBE_DUMP"); path && *path) {
        FindExeText();
        g_raw = new RawSample[kRawRing];
        g_dump = g_text_hi ? std::fopen(path, "w") : nullptr;
        std::printf("Stall probe: raw stacks of the main, render, GPU, draw-recording, Vulkan recorder and copy threads %s %s (BB_STALL_PROBE_DUMP)\n",
                    g_dump ? "go to" : "NOT written to", path);
    }
#endif
    std::printf("Stall probe: on (BB_STALL_PROBE): frames over %d ms split into main work / frame end%s\n",
                g_threshold_ms, b ? ", render thread done seen" : " (render thread done hook not installed)");
    std::thread(SamplerMain).detach();
}

} // namespace BbStallProbe
