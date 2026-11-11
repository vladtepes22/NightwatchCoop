// SPDX-License-Identifier: GPL-3.0-or-later
// Load trace (BB_LOAD_TRACE): see bbport_load_trace.h. Offsets are ours (bbhost - 0x400000).
#include "bbport_load_trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bbport_toggles.h"
#include "party/coop_hooks.h"
#include "party/game_state.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

extern "C" int runtime_win_thread_name(std::uint32_t thread_id, char* out, std::size_t size);
extern "C" void runtime_file_io_stats(std::uint64_t* opens, std::uint64_t* reads, std::uint64_t* bytes,
                                      std::uint64_t* read_ns);
namespace Vulkan {
extern std::atomic<std::uint64_t> g_bb_compile_ns_total;
extern std::atomic<std::uint32_t> g_bb_compiles_total;
} // namespace Vulkan

namespace BbLoadTrace {
namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;
inline unsigned long long ull(u64 v) { return static_cast<unsigned long long>(v); }

// MoveMapStep: update 0x19386e0 (rdi = the step; current step +0xC, map +0x58, hold timer +0x278, others
// gate +0x27C). InGameStep steps: 2 raises the screen, 4 asks for the load list, 6 builds the world and
// the MoveMapStep, 8 tears the old world down (research/remp081.md 3.1).
constexpr u64 kMoveMapUpdate = 0x19386e0, kStepCur = 0xc, kStepMap = 0x58, kHold = 0x278, kOthers = 0x27c;
constexpr u64 kInGame2 = 0x1928c90, kInGame4 = 0x1929180, kInGame6 = 0x1929220, kInGame8 = 0x1929c10;
// NowLoading (the loading screen, *0x5562878): +0x804 state (1 raising, 3 dropping), +0x808 style, +0x17C0 timer.
constexpr u64 kNowLoadingPtr = 0x5562878, kNlState = 0x804, kNlStyle = 0x808, kNlTimer = 0x17c0;

struct Counters {
    u64 ns = 0, opens = 0, reads = 0, bytes = 0, read_ns = 0, compiles = 0, compile_ns = 0;
    u64 images = 0, upload = 0, frames = 0, cpu_100ns = 0;
};

u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

u64 ProcessCpu100ns() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    const u64 kk = (u64(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    const u64 uu = (u64(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    return kk + uu;
#else
    return 0;
#endif
}

Counters Snapshot() {
    Counters c;
    c.ns = NowNs();
    runtime_file_io_stats(&c.opens, &c.reads, &c.bytes, &c.read_ns);
    c.compiles = Vulkan::g_bb_compiles_total.load(std::memory_order_relaxed);
    c.compile_ns = Vulkan::g_bb_compile_ns_total.load(std::memory_order_relaxed);
    c.images = BbStats::images_registered.load(std::memory_order_relaxed);
    c.upload = BbStats::image_upload_bytes.load(std::memory_order_relaxed);
    c.frames = BbStats::frame_number.load(std::memory_order_relaxed);
    c.cpu_100ns = ProcessCpu100ns();
    return c;
}

// Per-thread CPU time (user + kernel, 100 ns units) of this process, keyed by thread id.
std::unordered_map<u32, u64> ThreadCpu() {
    std::unordered_map<u32, u64> out;
#ifdef _WIN32
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(th, &c, &e, &k, &u)) {
                out[te.th32ThreadID] = ((u64(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                                       ((u64(u.dwHighDateTime) << 32) | u.dwLowDateTime);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
#endif
    return out;
}

bool g_on = false;
bool g_threads = false;  // BB_LOAD_TRACE=2: the busiest threads per phase too (a Toolhelp snapshot per step change)
std::mutex g_lock;
u64 g_t0 = 0;                 // process-relative clock origin
Counters g_last;              // at the previous step change
Counters g_load_start;        // at MoveMapStep step 0 of the current load
std::unordered_map<u32, u64> g_last_threads;
int g_move_step = -1;         // last seen MoveMapStep step (-1: none)
u64 g_move_obj = 0;
int g_ingame_step = -1;
u32 g_main_tid = 0;
bool g_in_load = false;
std::atomic<bool> g_sample_load{false};  // a MoveMapStep is between steps 0 and 12 (the sampler runs)

double Ms(u64 a, u64 b) { return b >= a ? double(b - a) / 1e6 : 0.0; }

void Emit(const char* what, int from, int to, u64 obj) {
    const Counters now = Snapshot();
    const Counters& p = g_last;
    char threads[256] = "";
    if (g_threads) {
        auto cur = ThreadCpu();
        std::vector<std::pair<u64, u32>> busy;
        for (const auto& [tid, t] : cur) {
            const auto it = g_last_threads.find(tid);
            const u64 d = t - (it == g_last_threads.end() ? 0 : it->second);
            if (d) busy.emplace_back(d, tid);
        }
        std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int n = 0;
        for (const auto& [d, tid] : busy) {
            if (n++ >= 4) break;
            const std::size_t len = std::strlen(threads);
            std::snprintf(threads + len, sizeof(threads) - len, "%s%s%u %.0f ms", n > 1 ? ", " : "; threads: ",
                          tid == g_main_tid ? "main " : "", unsigned(tid), double(d) / 1e4);
        }
        g_last_threads = std::move(cur);
    }
    float hold = 0, others = 0;
    u32 map = 0;
    if (obj) {
        coop::SafeGet(obj + kHold, &hold);
        coop::SafeGet(obj + kOthers, &others);
        coop::SafeGet(obj + kStepMap, &map);
    }
    std::printf("Load trace: [%8.3f s] %s %d -> %d after %.0f ms (%llu frames): io %llu opens, %llu reads, %.1f MB, "
                "%.0f ms in reads; gpu %llu compiles %.0f ms, %llu images, %.1f MB uploaded; cpu %.2f s%s\n",
                Ms(g_t0, now.ns) / 1e3, what, from, to, Ms(p.ns, now.ns), ull(now.frames - p.frames),
                ull(now.opens - p.opens), ull(now.reads - p.reads), double(now.bytes - p.bytes) / 1e6,
                double(now.read_ns - p.read_ns) / 1e6, ull(now.compiles - p.compiles),
                double(now.compile_ns - p.compile_ns) / 1e6, ull(now.images - p.images),
                double(now.upload - p.upload) / 1e6, double(now.cpu_100ns - p.cpu_100ns) / 1e7, threads);
    u64 nl = 0;
    int nl_state = -1, nl_style = -1;
    float nl_timer = 0;
    if (coop::SafeGet(coop::Guest(kNowLoadingPtr), &nl) && nl) {
        coop::SafeGet(nl + kNlState, &nl_state);
        coop::SafeGet(nl + kNlStyle, &nl_style);
        coop::SafeGet(nl + kNlTimer, &nl_timer);
    }
    if (obj) {
        std::printf("Load trace:   MoveMapStep %llx map 0x%08x hold %.2f s others-gate %.2f s; NowLoading state %d "
                    "style %d timer %.2f\n",
                    ull(obj), map, double(hold), double(others), nl_state, nl_style, double(nl_timer));
    } else {
        std::printf("Load trace:   NowLoading state %d style %d timer %.2f\n", nl_state, nl_style, double(nl_timer));
    }
    g_last = now;
}

void EmitLoadTotal(const Counters& now) {
    const Counters& p = g_load_start;
    std::printf("Load trace: [%8.3f s] LOAD TOTAL (MoveMapStep step 0 -> 13) %.0f ms, %llu frames: io %llu opens, "
                "%llu reads, %.1f MB, %.0f ms in reads; gpu %llu compiles %.0f ms, %llu images, %.1f MB uploaded; "
                "cpu %.2f s\n",
                Ms(g_t0, now.ns) / 1e3, Ms(p.ns, now.ns), ull(now.frames - p.frames), ull(now.opens - p.opens),
                ull(now.reads - p.reads), double(now.bytes - p.bytes) / 1e6, double(now.read_ns - p.read_ns) / 1e6,
                ull(now.compiles - p.compiles), double(now.compile_ns - p.compile_ns) / 1e6,
                ull(now.images - p.images), double(now.upload - p.upload) / 1e6,
                double(now.cpu_100ns - p.cpu_100ns) / 1e7);
}

// Hang guard: a MoveMapStep step that has not changed for kStallS seconds is reported once (the step, its map and
// timers, the file IO and CPU since the step began, and the busiest threads with their names: the game's resource
// threads "Core.Res.*" are the load's workers, all idle while a job is lost).
constexpr double kStallS = 30.0;
u64 g_step_since_ns = 0;
Counters g_step_since;
std::unordered_map<u32, u64> g_step_threads;
bool g_stall_reported = false;

void ReportStall(u64 step, int cur) {
    const Counters now = Snapshot();
    char names[512] = "";
#ifdef _WIN32
    const auto cpu = ThreadCpu();
    std::vector<std::pair<u64, u32>> busy;
    for (const auto& [tid, t] : cpu) {
        const auto it = g_step_threads.find(tid);
        busy.emplace_back(t - (it == g_step_threads.end() ? 0 : it->second), tid);
    }
    std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    int n = 0, res = 0;
    for (const auto& [d, tid] : busy) {
        char nm[64] = "";
        runtime_win_thread_name(tid, nm, sizeof(nm));
        const bool core = std::strncmp(nm, "Core.Res", 8) == 0;
        res += core && d > 0;
        if (n < 5 || (core && n < 12)) {
            const std::size_t len = std::strlen(names);
            std::snprintf(names + len, sizeof(names) - len, "%s%s%u %.0f ms", n ? ", " : "", nm[0] ? nm : "thread", unsigned(tid),
                          double(d) / 1e4);
            ++n;
        }
    }
    std::snprintf(names + std::strlen(names), sizeof(names) - std::strlen(names), "; resource threads busy: %d", res);
#endif
    float hold = 0, others = 0;
    u32 map = 0;
    coop::SafeGet(step + kHold, &hold);
    coop::SafeGet(step + kOthers, &others);
    coop::SafeGet(step + kStepMap, &map);
    std::printf("Load trace: STALL: MoveMapStep step %d has not advanced for %.0f s (map 0x%08x, hold %.2f s, others-gate %.2f s): "
                "io %llu reads, %.1f MB, cpu %.2f s since it began; threads: %s\n",
                cur, Ms(g_step_since_ns, now.ns) / 1e3, map, double(hold), double(others), ull(now.reads - g_step_since.reads),
                double(now.bytes - g_step_since.bytes) / 1e6, double(now.cpu_100ns - g_step_since.cpu_100ns) / 1e7, names);
    std::fflush(stdout);
}

// World fade-in diagnostic: for 3 s after a load ends (step 13) the two fade objects of the SprjFade singleton
// (*0x5540068; +0x20 and +0x38, the pair MoveMapStep step 0 clears to black: floats at +0xc .. +0x28, int at +0x38) are
// printed when a value changes (sampled every 16 ms), with the NowLoading state: whether the world's fade from black runs and ends.
constexpr u64 kSprjFadePtr = 0x5540068;
void FadeWatch(u64 load_end_ns) {
    std::string last;
    for (int i = 0; i < 190; ++i) {  // 3 s at 16 ms; a line only when a value changed
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        u64 fade = 0;
        if (!coop::SafeGet(coop::Guest(kSprjFadePtr), &fade) || !fade) continue;
        char line[512];
        int n = std::snprintf(line, sizeof(line), "Load trace: fade +%.2f s:", Ms(load_end_ns, NowNs()) / 1e3);
        for (const u64 slot : {u64(0x20), u64(0x38)}) {
            u64 obj = 0;
            if (!coop::SafeGet(fade + slot, &obj) || !obj) {
                n += std::snprintf(line + n, sizeof(line) - n, " [+0x%llx none]", ull(slot));
                continue;
            }
            float f[9] = {};
            for (int k = 0; k < 9; ++k) coop::SafeGet(obj + 0xc + 4 * k, &f[k]);
            int st = 0;
            coop::SafeGet(obj + 0x38, &st);
            n += std::snprintf(line + n, sizeof(line) - n, " [+0x%llx %llx: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f st %d]",
                               ull(slot), ull(obj), double(f[0]), double(f[1]), double(f[2]), double(f[3]), double(f[4]),
                               double(f[5]), double(f[6]), double(f[7]), double(f[8]), st);
        }
        u64 nlp = 0;
        int nl_state = -1;
        if (coop::SafeGet(coop::Guest(kNowLoadingPtr), &nlp) && nlp) coop::SafeGet(nlp + kNlState, &nl_state);
        const char* sep = std::strstr(line, " s:");
        const std::string body = std::string(sep ? sep + 3 : line) + std::to_string(nl_state);
        if (body == last) continue;
        last = body;
        std::printf("%s NowLoading %d\n", line, nl_state);
        std::fflush(stdout);
    }
}

/// MoveMapStep update, every frame (rdi = the step).
BB_COOP_SYSV void MoveMapUpdateHook(u64 step, u64, u64, u64, u64, u64) {
    int cur = -1;
    if (!coop::SafeGet(step + kStepCur, &cur)) return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (step != g_move_obj) {
        g_move_obj = step;
        g_move_step = -1;
    }
    if (cur == g_move_step) {
        if (g_in_load && !g_stall_reported && g_step_since_ns && Ms(g_step_since_ns, NowNs()) >= kStallS * 1e3) {
            g_stall_reported = true;
            ReportStall(step, cur);
        }
        return;
    }
    g_step_since_ns = NowNs();
    g_step_since = Snapshot();
    g_stall_reported = false;
#ifdef _WIN32
    g_step_threads = ThreadCpu();
#endif
    if (cur == 0 || g_move_step < 0) {
        g_load_start = Snapshot();
        g_in_load = true;
    }
    Emit("MoveMapStep", g_move_step, cur, step);
    if (cur == 13 && g_in_load) {
        EmitLoadTotal(g_last);
        std::thread(FadeWatch, NowNs()).detach();
        g_in_load = false;
        g_sample_load = false;
    } else if (cur >= 0 && cur < 13) {
        g_sample_load = true;
    }
    g_move_step = cur;
}

#ifdef _WIN32
// ---- sampler (BB_LOAD_TRACE=2): while a MoveMapStep runs (steps 0..12), the four busiest threads of the last
// 250 ms are sampled every 4 ms (suspend, context, stack, resume: nothing that takes a lock while a thread is
// suspended); at step 13 the top program counters per thread are printed as guest offsets (host-module samples
// are keyed by the first guest return address on the stack, marked "via host"). functions.tsv maps them offline.
struct ThreadSamples {
    u32 tid = 0;
    HANDLE handle = nullptr;
    std::unordered_map<u64, u32> pcs;  // guest function entry (bit 63: a host-module sample, keyed by its guest caller's entry)
    std::unordered_map<u64, u32> exe;  // host samples inside bbport.exe, by 64-byte bucket of the exe offset (addr2line offline)
    u32 total = 0, host = 0;
};

constexpr u64 kHostBit = u64(1) << 63;

/// The entry of the guest function holding `off`: the nearest `push rbp; mov rbp, rsp` (55 48 89 e5) before it (the
/// game keeps frame pointers), at most 64 KiB back; the offset itself when none is found. Cached per 64-byte bucket.
u64 FunctionEntry(u64 off) {
    static std::unordered_map<u64, u64> cache;
    const u64 key = off >> 6;
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const unsigned char* img = coop::Image();
    u64 entry = off;
    const u64 floor = off > 0x10000 ? off - 0x10000 : 0;
    for (u64 p = off; p > floor + 3; --p) {
        // push rbp, then: mov rbp,rsp | push r12..r15 | push rbx | sub rsp,imm (leaf functions keep no frame pointer)
        if (img[p] == 0x55 && ((img[p + 1] == 0x48 && img[p + 2] == 0x89 && img[p + 3] == 0xe5) ||
                               (img[p + 1] == 0x41 && (img[p + 2] & 0xfc) == 0x54) || img[p + 1] == 0x53 ||
                               (img[p + 1] == 0x48 && (img[p + 2] == 0x83 || img[p + 2] == 0x81) && img[p + 3] == 0xec))) {
            entry = p;
            break;
        }
    }
    cache[key] = entry;
    return entry;
}

// The game's code segment ends at 0x50d96dc (the ELF's first PT_LOAD); the data / bss after it are not callers.
constexpr u64 kTextEnd = 0x50da000;

void SampleOne(ThreadSamples& ts) {
    const u64 lo = reinterpret_cast<u64>(coop::Image()), hi = lo + std::min<u64>(coop::ImageSize(), kTextEnd);
    static const u64 exe_lo = reinterpret_cast<u64>(GetModuleHandleA(nullptr));
    static const u64 exe_hi = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_lo);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe_lo + dos->e_lfanew);
        return exe_lo + nt->OptionalHeader.SizeOfImage;
    }();
    if (SuspendThread(ts.handle) == static_cast<DWORD>(-1)) return;
    CONTEXT c;
    std::memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    u64 stack[128];
    SIZE_T got = 0;
    const BOOL ok = GetThreadContext(ts.handle, &c);
    if (ok && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), stack, sizeof stack, &got)) {
        got = 0;
    }
    ResumeThread(ts.handle);
    if (!ok) return;
    ++ts.total;
    if (c.Rip >= lo && c.Rip < hi) {
        ++ts.pcs[FunctionEntry(c.Rip - lo)];
        return;
    }
    ++ts.host;
    u64 caller = 0;
    for (SIZE_T i = 0; i < got / 8; ++i) {
        if (stack[i] >= lo && stack[i] < hi) {
            caller = FunctionEntry(stack[i] - lo);
            break;
        }
    }
    ++ts.pcs[caller | kHostBit];
    // Where in our exe (the sample itself, or the first exe return address on the stack from a DLL / kernel wait).
    u64 e = 0;
    if (c.Rip >= exe_lo && c.Rip < exe_hi) {
        e = c.Rip - exe_lo;
    } else {
        for (SIZE_T i = 0; i < got / 8; ++i) {
            if (stack[i] >= exe_lo && stack[i] < exe_hi) {
                e = (stack[i] - exe_lo) | kHostBit;  // bit 63: a return address, not the sample itself
                break;
            }
        }
    }
    ++ts.exe[e & kHostBit ? e : (e & ~u64(63))];
}

void PrintSamples(std::vector<ThreadSamples>& threads) {
    for (ThreadSamples& ts : threads) {
        if (!ts.total) continue;
        std::vector<std::pair<u32, u64>> rows;
        rows.reserve(ts.pcs.size());
        for (const auto& [pc, n] : ts.pcs) rows.emplace_back(n, pc);
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string line;
        char b[64];
        int n = 0;
        for (const auto& [count, pc] : rows) {
            if (n++ >= 24) break;
            const bool host = pc >> 63;
            std::snprintf(b, sizeof b, "%s%s+0x%llx %d%%", n > 1 ? "; " : "", host ? "via host " : "",
                          ull(pc & ~kHostBit), int(100.0 * count / ts.total + 0.5));
            line += b;
        }
        char name[64] = "";
        if (!runtime_win_thread_name(ts.tid, name, sizeof name)) name[0] = 0;
        std::printf("Load trace: samples %sthread %u%s%s%s: %u (%u in host modules): %s\n",
                    ts.tid == g_main_tid ? "main " : "", unsigned(ts.tid), name[0] ? " (" : "", name, name[0] ? ")" : "",
                    ts.total, ts.host, line.c_str());
        if (ts.host * 5 >= ts.total) {  // a fifth or more in host code: where in our exe
            rows.clear();
            for (const auto& [e, cnt] : ts.exe) rows.emplace_back(cnt, e);
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            line.clear();
            n = 0;
            for (const auto& [count, e] : rows) {
                if (n++ >= 12) break;
                std::snprintf(b, sizeof b, "%s%sexe+0x%llx %d%%", n > 1 ? "; " : "", e >> 63 ? "ret " : "",
                              ull(e & ~kHostBit), int(100.0 * count / ts.total + 0.5));
                line += b;
            }
            std::printf("Load trace:   thread %u in our exe: %s\n", unsigned(ts.tid), line.c_str());
        }
    }
}

void SamplerMain() {
    std::vector<ThreadSamples> threads;
    std::unordered_map<u32, u64> prev = ThreadCpu();
    u64 last_pick = NowNs();
    const u32 self = GetCurrentThreadId();
    bool was_sampling = false;
    for (;;) {
        const bool sampling = g_sample_load.load(std::memory_order_relaxed);
        if (!sampling) {
            if (was_sampling) {
                PrintSamples(threads);
                for (ThreadSamples& ts : threads) CloseHandle(ts.handle);
                threads.clear();
                was_sampling = false;
            }
            Sleep(20);
            prev = ThreadCpu();
            last_pick = NowNs();
            continue;
        }
        was_sampling = true;
        const u64 now = NowNs();
        if (now - last_pick > 250'000'000ULL) {
            last_pick = now;
            std::unordered_map<u32, u64> cur = ThreadCpu();
            std::vector<std::pair<u64, u32>> busy;
            for (const auto& [tid, t] : cur) {
                if (tid == self) continue;
                const auto it = prev.find(tid);
                const u64 d = t - (it == prev.end() ? 0 : it->second);
                if (d > 200'000) busy.emplace_back(d, tid);  // over 20 ms of the last 250 ms
            }
            prev = std::move(cur);
            std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (busy.size() > 4) busy.resize(4);
            for (const auto& [d, tid] : busy) {
                bool have = false;
                for (const ThreadSamples& ts : threads) have = have || ts.tid == tid;
                if (have) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                if (!h) continue;
                ThreadSamples ts;
                ts.tid = tid;
                ts.handle = h;
                threads.push_back(std::move(ts));
            }
            if (threads.size() > 8) {  // keep the eight busiest-ever threads
                CloseHandle(threads.back().handle);
                threads.pop_back();
            }
        }
        for (ThreadSamples& ts : threads) SampleOne(ts);
        Sleep(4);
    }
}
#endif

void InGameEntered(int step) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (step == g_ingame_step) return;
    Emit("InGameStep", g_ingame_step, step, 0);
    g_ingame_step = step;
    if (step == 2) g_move_step = -1;  // a new load: the next MoveMapStep is logged from its step 0
}
BB_COOP_SYSV void InGame2Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(2); }
BB_COOP_SYSV void InGame4Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(4); }
BB_COOP_SYSV void InGame6Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(6); }
BB_COOP_SYSV void InGame8Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(8); }

} // namespace

void Install() {
    const char* e = std::getenv("BB_LOAD_TRACE");
    if (!e || !e[0] || e[0] == '0') return;
    if (!coop::Image()) {
        std::printf("Load trace: no image; off\n");
        return;
    }
    g_threads = e[0] == '2';
    g_t0 = NowNs();
#ifdef _WIN32
    g_main_tid = GetCurrentThreadId();
#endif
    g_last = Snapshot();
    if (g_threads) g_last_threads = ThreadCpu();
    // Prologues (whole, position-independent instructions): push rbp; mov rbp,rsp; push r15; push r14; [push r13;
    // push r12;] push rbx; push rax.
    const bool a = coop::HookPrologue(kMoveMapUpdate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50},
                                      &MoveMapUpdateHook, "load trace probe: MoveMapStep update");
    const bool b = coop::HookPrologue(kInGame2, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50}, &InGame2Hook,
                                      "load trace probe: InGameStep step 2");
    const bool c = coop::HookPrologue(kInGame4, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50}, &InGame4Hook,
                                      "load trace probe: InGameStep step 4");
    const bool d = coop::HookPrologue(kInGame6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
                                      &InGame6Hook, "load trace probe: InGameStep step 6");
    const bool f = coop::HookPrologue(kInGame8, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
                                      &InGame8Hook, "load trace probe: InGameStep step 8");
    g_on = a;
#ifdef _WIN32
    if (a && g_threads) std::thread(SamplerMain).detach();
#endif
    std::printf("Load trace: %s (BB_LOAD_TRACE): MoveMapStep update %s, InGameStep 2/4/6/8 %d%d%d%d%s\n",
                a ? "on" : "OFF (the MoveMapStep update's prologue differs)", a ? "hooked" : "not hooked", b, c, d, f,
                g_threads ? "; busiest threads per step, sampled during the load" : "");
}

} // namespace BbLoadTrace
