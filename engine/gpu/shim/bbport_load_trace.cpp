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
#include <string>
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
extern "C" void runtime_win_set_thread_name(const char* name);
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
using u8 = std::uint8_t;
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
bool g_profile = false;  // BB_LOAD_PROFILE=1: the per-load profile (threads by name, block states, fades, the screen's clock)
bool g_profile_samples = false;  // BB_LOAD_PROFILE=2: the sampler too, per step (it costs ~0.5 core and slows a load by 1-3 s)
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
// The sampler's bucket (BB_LOAD_PROFILE): the MoveMapStep step 0..13, 100 + the InGameStep step before it, -1 none.
std::atomic<int> g_cur_step{-1};

double Ms(u64 a, u64 b) { return b >= a ? double(b - a) / 1e6 : 0.0; }

/// A thread's description (the game names its threads: "Core.Res.PostProcessor_0", "CSChrThread0", ...; ours
/// through runtime_win_set_thread_name), cached; "main" for the game's main thread, "thread" when it has none.
const char* ThreadName(u32 tid) {
    static std::unordered_map<u32, std::string> cache;
    static std::mutex lock;
    std::lock_guard<std::mutex> lk(lock);
    auto it = cache.find(tid);
    if (it != cache.end()) return it->second.c_str();
    char nm[64] = "";
    if (tid == g_main_tid) {
        std::snprintf(nm, sizeof nm, "main");
    } else if (!runtime_win_thread_name(tid, nm, sizeof nm) || !nm[0]) {
        std::snprintf(nm, sizeof nm, "thread");
    }
    return cache.emplace(tid, nm).first->second.c_str();
}

/// The busiest threads between two Toolhelp CPU snapshots: "name tid ms" for the top `max_n`, and with the profile
/// the main thread's busy share of `wall_ms` (the rest of the wall time the main thread was blocked: a wait on the
/// resource tasks, an event or the frame end).
std::string BusyThreads(const std::unordered_map<u32, u64>& prev, const std::unordered_map<u32, u64>& cur, int max_n,
                        double wall_ms) {
    std::vector<std::pair<u64, u32>> busy;
    u64 main_cpu = 0;
    for (const auto& [tid, t] : cur) {
        const auto it = prev.find(tid);
        const u64 d = t - (it == prev.end() ? 0 : it->second);
        if (tid == g_main_tid) main_cpu = d;
        if (d) busy.emplace_back(d, tid);
    }
    std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    char b[96];
    int n = 0;
    for (const auto& [d, tid] : busy) {
        if (n++ >= max_n) break;
        if (g_profile) {
            std::snprintf(b, sizeof b, "%s%s %u %.0f ms", n > 1 ? ", " : "; threads: ", ThreadName(tid), unsigned(tid),
                          double(d) / 1e4);
        } else {
            std::snprintf(b, sizeof b, "%s%s%u %.0f ms", n > 1 ? ", " : "; threads: ", tid == g_main_tid ? "main " : "",
                          unsigned(tid), double(d) / 1e4);
        }
        out += b;
    }
    if (g_profile && wall_ms > 0) {
        const double busy_ms = double(main_cpu) / 1e4;
        std::snprintf(b, sizeof b, "; main busy %.0f ms (%.0f%%), blocked %.0f ms", busy_ms, 100.0 * busy_ms / wall_ms,
                      std::max(0.0, wall_ms - busy_ms));
        out += b;
    }
    return out;
}

void Emit(const char* what, int from, int to, u64 obj) {
    const Counters now = Snapshot();
    const Counters& p = g_last;
    std::string threads;
    if (g_threads) {
        auto cur = ThreadCpu();
        threads = BusyThreads(g_last_threads, cur, g_profile ? 8 : 4, Ms(p.ns, now.ns));
        g_last_threads = std::move(cur);
    }
    if (g_profile) {
        const double wall = Ms(p.ns, now.ns);
        char fps[48];
        std::snprintf(fps, sizeof fps, "; %.0f fps", wall > 0 ? 1e3 * double(now.frames - p.frames) / wall : 0.0);
        threads += fps;
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
                double(now.upload - p.upload) / 1e6, double(now.cpu_100ns - p.cpu_100ns) / 1e7, threads.c_str());
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

// ---- BB_LOAD_PROFILE: what the world-ready wait (MoveMapStep 9 -> 10, 0x19E8190) waits on ---------------------
// 0x19E8190 (called by steps 9 / 10 / 11 / 12 / 13) is true when every WorldBlockRes the load selected is Resident
// (load_state +0xC8 == 7) and every requested entry of the map collision manager is at its state 6. Layouts from
// bbhost (research/bbhost/include/bbhost/engine/sprj/world_info.hpp, map_ins.hpp): FieldArea *0x553B148 -> +8
// WorldRes (its WorldInfo prefix: +0x18 block count, +0x20 WorldBlockInfo[0xA0] with the block id at +8);
// WorldRes +0x2B18 / +0x2B20 WorldAreaRes[0x110] (load_state +0x2C, 6 = active), +0x2B30 / +0x2B38
// WorldBlockRes[0x420] (+0xC2 selected, +0xC5 desired resident, +0xC8 load_state, +0xD0 the loading record,
// +0x160 three base file caps, +0x180 3x5x5 partition file caps; a file cap's state byte +0x78, 4 = loaded).
// MapCollisionManager *0x553E850: +0x20 / +0x28 groups[0xB8] (+0x78 count, +0x80 entries[0x138]: +0xD8 and
// +0x100 the two state bytes the gate wants at 6, +0x118 flags, +0x11A desired state 0 / 2 / 4).
constexpr u64 kFieldAreaPtr = 0x553b148, kCollisionManPtr = 0x553e850;
const char* const kBlockState[] = {"Unloaded", "BeginLoad", "Loading", "Preparing", "ActivationDecision",
                                   "FinalizingUnload", "Activating", "Resident", "DeleteWait", "BeginUnload"};
const char* BlockStateName(int s) { return s >= 0 && s < 10 ? kBlockState[s] : "?"; }

struct BlockSeen {
    u32 id = 0;
    int state = -1;
    u8 sel = 0, des = 0;
    int files_pending = -1;  // base + partition file caps not at state 4
    u64 since = 0, files_at = 0;
};
std::vector<BlockSeen> g_blocks;
std::vector<int> g_areas;
std::string g_coll_last;
u64 g_coll_at = 0;

/// A block's name from its id (0x18010000 = m24_01).
void BlockName(u32 id, char* out, std::size_t n) { std::snprintf(out, n, "m%02u_%02u", id >> 24, (id >> 16) & 0xff); }

/// File caps of a WorldBlockRes not yet at state 4 (loaded): the loading record's six (+0xD8 .. +0x100: the msb /
/// context files state 2 waits on), the three base files (+0x160), the 75 partition files (+0x180) and the extra
/// list (+0x3EC count, +0x3F0 pointers): what state 3 -> 4 checks (0x155AAE0 case 3) besides the nvm prepare.
int FilesPending(u64 block) {
    int pending = 0;
    auto count = [&](u64 at) {
        u64 cap = 0;
        u8 st = 0;
        if (coop::SafeGet(at, &cap) && cap && coop::SafeGet(cap + 0x78, &st) && st != 4) ++pending;
    };
    for (u64 off = 0xd8; off <= 0x100; off += 8) count(block + off);
    for (u64 off = 0x160; off < 0x160 + 3 * 8; off += 8) count(block + off);
    for (u64 off = 0x180; off < 0x180 + 75 * 8; off += 8) count(block + off);
    int extra = 0;
    u64 extra_list = 0;
    if (coop::SafeGet(block + 0x3ec, &extra) && extra > 0 && extra < 256 && coop::SafeGet(block + 0x3f0, &extra_list) && extra_list) {
        for (int i = 0; i < extra; ++i) count(extra_list + u64(i) * 8);
    }
    return pending;
}

/// Every frame of a load (from the MoveMapStep update): one line per block / area state change, the count of its
/// file caps still loading (at most one line per 100 ms per block), and the collision entries' state summary when
/// it changes (at most every 100 ms).
void ProbeSubGates(u64 now);

void ProbeWorld(u64 now) {
    u64 fa = 0, wr = 0;
    if (!coop::SafeGet(coop::Guest(kFieldAreaPtr), &fa) || !fa || !coop::SafeGet(fa + 8, &wr) || !wr) return;
    u32 n = 0;
    u64 list = 0;
    if (!coop::SafeGet(wr + 0x2b30, &n) || !coop::SafeGet(wr + 0x2b38, &list) || n > 60 || !list) return;
    if (g_blocks.size() != n) g_blocks.assign(n, BlockSeen{});
    for (u32 i = 0; i < n; ++i) {
        const u64 b = list + u64(i) * 0x420;
        u64 info = 0;
        u32 id = 0;
        int st = -1;
        u8 sel = 0, des = 0;
        coop::SafeGet(b + 8, &info);
        if (info) coop::SafeGet(info + 8, &id);
        coop::SafeGet(b + 0xc8, &st);
        coop::SafeGet(b + 0xc2, &sel);
        coop::SafeGet(b + 0xc5, &des);
        BlockSeen& s = g_blocks[i];
        char name[16];
        BlockName(id, name, sizeof name);
        if (s.id != id || s.state != st || s.sel != sel || s.des != des) {
            const int files = (st >= 1 && st <= 3) ? FilesPending(b) : -1;
            char suffix[48] = "";
            if (files >= 0) std::snprintf(suffix, sizeof suffix, "; file caps loading %d", files);
            std::printf("Load profile: [%8.3f s] block %s (res %u) state %d %s -> %d %s after %.0f ms; selected %d desired %d%s\n",
                        Ms(g_t0, now) / 1e3, name, i, s.state, BlockStateName(s.state), st, BlockStateName(st),
                        s.since ? Ms(s.since, now) : 0.0, sel, des, suffix);
            s.id = id;
            s.state = st;
            s.sel = sel;
            s.des = des;
            s.since = now;
            s.files_pending = files;
            s.files_at = now;
        } else if (st >= 1 && st <= 3 && Ms(s.files_at, now) >= 100.0) {
            const int files = FilesPending(b);
            if (files != s.files_pending) {
                std::printf("Load profile: [%8.3f s] block %s (res %u) %s: %d file caps still loading\n", Ms(g_t0, now) / 1e3,
                            name, i, BlockStateName(st), files);
                s.files_pending = files;
            }
            s.files_at = now;
        }
    }
    u32 na = 0;
    u64 alist = 0;
    if (coop::SafeGet(wr + 0x2b18, &na) && coop::SafeGet(wr + 0x2b20, &alist) && na <= 20 && alist) {
        if (g_areas.size() != na) g_areas.assign(na, -1);
        for (u32 i = 0; i < na; ++i) {
            int st = -1;
            u8 area_no = 0;
            u64 info = 0;
            coop::SafeGet(alist + u64(i) * 0x110 + 0x2c, &st);
            if (coop::SafeGet(alist + u64(i) * 0x110 + 8, &info) && info) coop::SafeGet(info + 0xb, &area_no);
            if (st != g_areas[i]) {
                std::printf("Load profile: [%8.3f s] area m%02u (res %u) state %d -> %d\n", Ms(g_t0, now) / 1e3, area_no, i,
                            g_areas[i], st);
                g_areas[i] = st;
            }
        }
    }
    if (Ms(g_coll_at, now) < 100.0) return;
    g_coll_at = now;
    ProbeSubGates(now);
}

// The other three parts of 0x19E8190's answer, read the way the function reads them:
// - collision entries (0x1886B50): an entry in the load groups is ready when +0xD8 == 6 and (+0x118 bit 3 or
//   +0x100 == 6); one only in the draw groups (+0x11A == 2 from that branch) when +0xD8 == 6;
// - MapIns blocks (*0x553B158: +0x10 / +0x18 areas[0x20] -> +0x10 / +0x18 blocks[0x198]; 0x15934D0): ready when
//   +0x180 (every part in the masks loaded: MapIns +0x190 bits 0 and 1-2, its +0x1A8 file list at state 4) and
//   +0x184 (both passes over the +0x150 parts done; +0xB8 / +0xBC are the passes' cursors; the function issues at
//   most 20 backread requests (0x1587ED0) per call, so a pass over N parts needs N / 20 frames);
// - obj blocks (*0x553B168, blocks[0x1F8]; 0x15B2B20): ready when +0x120;
// - chr (0x19192A0, WorldChrMan *0x553E878): every chr's vfunc +0x78 true; +0x5F74 of +0x5F70 chr loads used
//   this frame (a per-frame budget), +0x5F88 chrs seen.
constexpr u64 kMapInsManPtr = 0x553b158, kObjManPtr = 0x553b168, kWorldChrManPtr = 0x553e878;
struct SubSeen {
    int ready = -1, parts = -1, pending = -1, cur1 = -1, cur2 = -1, qmax = -1;
};
std::vector<SubSeen> g_mapins, g_objs;
int g_chr_last[3] = {-1, -1, -1};

template <class Fn>
void WalkBlocks(u64 man_ptr, u64 stride, Fn fn) {
    u64 man = 0;
    int na = 0;
    u64 areas = 0;
    if (!coop::SafeGet(coop::Guest(man_ptr), &man) || !man || !coop::SafeGet(man + 0x10, &na) || na <= 0 || na > 20 ||
        !coop::SafeGet(man + 0x18, &areas) || !areas) {
        return;
    }
    int flat = 0;
    for (int a = 0; a < na; ++a) {
        int nb = 0;
        u64 blocks = 0;
        coop::SafeGet(areas + u64(a) * 0x20 + 0x10, &nb);
        coop::SafeGet(areas + u64(a) * 0x20 + 0x18, &blocks);
        if (nb <= 0 || nb > 60 || !blocks) continue;
        for (int b = 0; b < nb; ++b) fn(a, b, flat++, blocks + u64(b) * stride);
    }
}

void ProbeSubGates(u64 now) {
    // collision
    u64 cm = 0;
    u32 groups = 0;
    u64 glist = 0;
    if (coop::SafeGet(coop::Guest(kCollisionManPtr), &cm) && cm && coop::SafeGet(cm + 0x20, &groups) &&
        coop::SafeGet(cm + 0x28, &glist) && groups <= 60 && glist) {
        u32 total = 0, wanted = 0, not_ready = 0, by_state[8] = {};
        char last[160] = "";
        for (u32 g = 0; g < groups; ++g) {
            const u64 grp = glist + u64(g) * 0xb8;
            u32 count = 0;
            u64 entries = 0;
            if (!coop::SafeGet(grp + 0x78, &count) || !coop::SafeGet(grp + 0x80, &entries) || !entries || count > 4096) continue;
            for (u32 e = 0; e < count; ++e) {
                const u64 ent = entries + u64(e) * 0x138;
                u8 s1 = 0, s2 = 0, flags = 0, want = 0;
                coop::SafeGet(ent + 0xd8, &s1);
                coop::SafeGet(ent + 0x100, &s2);
                coop::SafeGet(ent + 0x118, &flags);
                coop::SafeGet(ent + 0x11a, &want);
                ++total;
                if (!want) continue;
                ++wanted;
                const bool ready = s1 == 6 && (want == 2 || (flags & 8) || s2 == 6);
                if (!ready) {
                    ++not_ready;
                    ++by_state[s1 & 7];
                    std::snprintf(last, sizeof last, "group %u entry %u: +0xd8 %u +0x100 %u flags 0x%02x want %u", g, e, s1, s2,
                                  flags, want);
                }
            }
        }
        char line[320];
        int k = std::snprintf(line, sizeof line, "collision: %u groups, %u entries, %u requested, %u not ready", groups, total,
                              wanted, not_ready);
        if (not_ready) {
            k += std::snprintf(line + k, sizeof line - k, " (+0xd8 states");
            for (int st = 0; st < 8; ++st) {
                if (by_state[st]) k += std::snprintf(line + k, sizeof line - k, " %d:%u", st, by_state[st]);
            }
            std::snprintf(line + k, sizeof line - k, "; last: %s)", last);
        }
        if (g_coll_last != line) {
            g_coll_last = line;
            std::printf("Load profile: [%8.3f s] %s\n", Ms(g_t0, now) / 1e3, line);
        }
    }
    // MapIns blocks (map parts)
    WalkBlocks(kMapInsManPtr, 0x198, [&](int a, int b, int flat, u64 blk) {
        if (g_mapins.size() <= std::size_t(flat)) g_mapins.resize(flat + 1);
        SubSeen& seen = g_mapins[flat];
        int loaded = 0, done = 0, parts = 0, cur1 = 0, cur2 = 0;
        coop::SafeGet(blk + 0x180, &loaded);
        coop::SafeGet(blk + 0x184, &done);
        coop::SafeGet(blk + 0x150, &parts);
        coop::SafeGet(blk + 0xb8, &cur1);
        coop::SafeGet(blk + 0xbc, &cur2);
        const int ready = (loaded && done) ? 1 : 0;
        int pending = -1;
        if (!ready && parts > 0 && parts < 8192) {  // parts whose backread was requested (+0x190 bit 0) but are not loaded
            u64 list = 0;
            pending = 0;
            if (coop::SafeGet(blk + 0x158, &list) && list) {
                for (int i = 0; i < parts; ++i) {
                    u64 ins = 0;
                    if (!coop::SafeGet(list + u64(i) * 0x48 + 0x40, &ins) || !ins) continue;
                    u8 f = 0;
                    coop::SafeGet(ins + 0x190, &f);
                    if ((f & 1) == 0) continue;
                    bool ok = (f & 6) != 0;
                    u64 res = 0, files = 0;
                    u8 skip = 0;
                    if (ok && coop::SafeGet(ins + 0x198, &res) && res && coop::SafeGet(ins + 0x1b0, &skip) && !skip &&
                        coop::SafeGet(ins + 0x1a8, &files) && files) {
                        u32 n = 0;
                        coop::SafeGet(files, &n);
                        for (u32 j = 0; ok && j < n && j < 64; ++j) {
                            u64 cap = 0;
                            u8 st = 0;
                            if (coop::SafeGet(files + 8 + u64(j) * 16, &cap) && cap && coop::SafeGet(cap + 0x78, &st) && st != 4) ok = false;
                        }
                    }
                    if (!ok) ++pending;
                }
            }
        }
        // The block's MapInsUpdateHelper request queue (MapIns +0 -> helper +0 -> queue: [0] capacity, +0x10 count; the
        // assert MapInsUpdateHelper.cpp(0xcc) fires on a push at capacity): its high-water mark bounds the per-call
        // part budget (BB_LOAD_PARTS_BUDGET).
        u32 qcap = 0, qcount = 0;
        {
            u64 list = 0, ins = 0, helper = 0, queue = 0;
            if (parts > 0 && coop::SafeGet(blk + 0x158, &list) && list && coop::SafeGet(list + 0x40, &ins) && ins &&
                coop::SafeGet(ins, &helper) && helper && coop::SafeGet(helper, &queue) && queue) {
                coop::SafeGet(queue, &qcap);
                coop::SafeGet(queue + 0x10, &qcount);
            }
        }
        if (ready != seen.ready || parts != seen.parts || pending != seen.pending || cur1 != seen.cur1 || cur2 != seen.cur2 ||
            int(qcount) > seen.qmax) {
            std::printf("Load profile: [%8.3f s] map parts area %d block %d: %s, %d parts, pass cursors %d / %d, loaded-flag %d, "
                        "requested parts not loaded %d; helper queue %u of %u\n",
                        Ms(g_t0, now) / 1e3, a, b, ready ? "READY" : "not ready", parts, cur1, cur2, loaded, pending, qcount, qcap);
            const int qmax = std::max(seen.qmax, int(qcount));
            seen = SubSeen{ready, parts, pending, cur1, cur2, qmax};
        }
    });
    // obj blocks
    WalkBlocks(kObjManPtr, 0x1f8, [&](int a, int b, int flat, u64 blk) {
        if (g_objs.size() <= std::size_t(flat)) g_objs.resize(flat + 1);
        int ready = 0;
        coop::SafeGet(blk + 0x120, &ready);
        ready = ready ? 1 : 0;
        if (ready != g_objs[flat].ready) {
            std::printf("Load profile: [%8.3f s] objs area %d block %d: %s\n", Ms(g_t0, now) / 1e3, a, b, ready ? "READY" : "not ready");
            g_objs[flat].ready = ready;
        }
    });
    // chr
    u64 wcm = 0;
    if (coop::SafeGet(coop::Guest(kWorldChrManPtr), &wcm) && wcm) {
        int budget = 0, used = 0, seen = 0;
        coop::SafeGet(wcm + 0x5f70, &budget);
        coop::SafeGet(wcm + 0x5f74, &used);
        coop::SafeGet(wcm + 0x5f88, &seen);
        if (budget != g_chr_last[0] || used != g_chr_last[1] || seen != g_chr_last[2]) {
            std::printf("Load profile: [%8.3f s] chr: %d chrs seen by the ready check, %d of %d chr loads used this frame\n",
                        Ms(g_t0, now) / 1e3, seen, used, budget);
            g_chr_last[0] = budget;
            g_chr_last[1] = used;
            g_chr_last[2] = seen;
        }
    }
}

// ---- BB_LOAD_PROFILE: the loading screen's own clock -------------------------------------------------------------
// A 4 ms poller of the party director's "loading screen" condition (NowLoading requested *0x556286B and no in-game
// frame: the helper *0x553E8B8 +0x50 / +0x51) and of the NowLoading state (+0x804: 1 raising, 2 up, 3 dropping):
// the screen's up / gone times (what tools/mp/loadab.py measures), the fade-out (lamp press -> up: the state-1
// time) and the fade-in (state 3 -> gone), and at "gone" every thread's CPU time over the screen, by name.
constexpr u64 kNowLoadingRequested = 0x556286b, kNowLoadingHelper = 0x553e8b8;

#ifdef _WIN32
void PrintLoadThreads(const std::unordered_map<u32, u64>& prev, const std::unordered_map<u32, u64>& cur, double wall_ms) {
    std::vector<std::pair<u64, u32>> busy;
    u64 sum = 0;
    for (const auto& [tid, t] : cur) {
        const auto it = prev.find(tid);
        const u64 d = t - (it == prev.end() ? 0 : it->second);
        if (d >= 50'000) busy.emplace_back(d, tid);  // 5 ms or more
        sum += d;
    }
    std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string line;
    char b[96];
    for (const auto& [d, tid] : busy) {
        std::snprintf(b, sizeof b, "%s%s %u %.0f", line.empty() ? "" : ", ", ThreadName(tid), unsigned(tid), double(d) / 1e4);
        line += b;
    }
    std::printf("Load profile: threads over the screen (ms cpu; %.0f ms wall, %.2f cores busy): %s\n", wall_ms,
                wall_ms > 0 ? double(sum) / 1e4 / wall_ms : 0.0, line.c_str());
}

void ScreenPoller() {
    runtime_win_set_thread_name("bb:LoadPoller");
    bool up = false;
    int nl_state = -1;
    u64 up_ns = 0, raise_ns = 0, drop_ns = 0;
    Counters at_up;
    std::unordered_map<u32, u64> threads_up;
    for (;;) {
        Sleep(4);
        u8 requested = 0;
        u64 helper = 0;
        u8 f50 = 0, f51 = 0;
        coop::SafeGet(coop::Guest(kNowLoadingRequested), &requested);
        if (coop::SafeGet(coop::Guest(kNowLoadingHelper), &helper) && helper) {
            coop::SafeGet(helper + 0x50, &f50);
            coop::SafeGet(helper + 0x51, &f51);
        }
        const bool loading = requested && !(f50 || f51);
        u64 nl = 0;
        int st = -1;
        if (coop::SafeGet(coop::Guest(kNowLoadingPtr), &nl) && nl) coop::SafeGet(nl + kNlState, &st);
        const u64 now = NowNs();
        if (st != nl_state) {
            if (st == 1) raise_ns = now;
            if (st == 3) drop_ns = now;
            std::printf("Load profile: [%8.3f s] NowLoading state %d -> %d%s\n", Ms(g_t0, now) / 1e3, nl_state, st,
                        up ? "" : " (screen not up)");
            nl_state = st;
        }
        if (loading && !up) {
            up = true;
            up_ns = now;
            at_up = Snapshot();
            threads_up = ThreadCpu();
            g_sample_load = true;
            std::printf("Load profile: [%8.3f s] SCREEN UP (NowLoading state %d; raise -> up %.0f ms)\n", Ms(g_t0, now) / 1e3, st,
                        raise_ns ? Ms(raise_ns, now) : 0.0);
        } else if (!loading && up) {
            up = false;
            g_sample_load = false;
            const Counters c = Snapshot();
            const double wall = Ms(up_ns, now);
            std::printf("Load profile: [%8.3f s] SCREEN GONE after %.0f ms, %llu frames (%.0f fps); drop -> gone %.0f ms; io %llu "
                        "reads, %.1f MB, %.0f ms in reads; gpu %llu compiles %.0f ms, %llu images, %.1f MB uploaded; process cpu %.2f s\n",
                        Ms(g_t0, now) / 1e3, wall, ull(c.frames - at_up.frames), wall > 0 ? 1e3 * double(c.frames - at_up.frames) / wall : 0.0,
                        drop_ns > up_ns ? Ms(drop_ns, now) : 0.0, ull(c.reads - at_up.reads), double(c.bytes - at_up.bytes) / 1e6,
                        double(c.read_ns - at_up.read_ns) / 1e6, ull(c.compiles - at_up.compiles),
                        double(c.compile_ns - at_up.compile_ns) / 1e6, ull(c.images - at_up.images),
                        double(c.upload - at_up.upload) / 1e6, double(c.cpu_100ns - at_up.cpu_100ns) / 1e7);
            PrintLoadThreads(threads_up, ThreadCpu(), wall);
            std::fflush(stdout);
        }
    }
}
#endif

/// MoveMapStep update, every frame (rdi = the step).
BB_COOP_SYSV void MoveMapUpdateHook(u64 step, u64, u64, u64, u64, u64) {
    int cur = -1;
    if (!coop::SafeGet(step + kStepCur, &cur)) return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (step != g_move_obj) {
        g_move_obj = step;
        g_move_step = -1;
    }
    if (g_profile && g_in_load) ProbeWorld(NowNs());
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
    g_cur_step = cur;
    if (cur == 13 && g_in_load) {
        EmitLoadTotal(g_last);
        std::thread(FadeWatch, NowNs()).detach();
        g_in_load = false;
        if (!g_profile) g_sample_load = false;  // the profile samples until the screen is gone (the poller)
    } else if (cur >= 0 && cur < 13 && !g_profile) {
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
    u64 stack[512];  // 4 KB: the driver's and the heap's deep host frames still reach an exe / guest return address
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

/// The profile's step label for the sampler's buckets.
std::string StepLabel(int step) {
    char b[48];
    if (step < 0) std::snprintf(b, sizeof b, "no step");
    else if (step >= 100) std::snprintf(b, sizeof b, "InGameStep %d", step - 100);
    else std::snprintf(b, sizeof b, "MoveMapStep %d", step);
    return b;
}

void SamplerMain() {
    runtime_win_set_thread_name("bb:LoadSampler");
    std::vector<ThreadSamples> threads;
    std::unordered_map<u32, u64> prev = ThreadCpu();
    u64 last_pick = NowNs(), last_idle_snapshot = NowNs();
    const u32 self = GetCurrentThreadId();
    bool was_sampling = false;
    int bucket = -1;  // the profile: samples are printed per step (the step the samples were taken in)
    u64 bucket_since = 0;
    auto flush = [&](int step, u64 now) {
        u32 total = 0;
        for (const ThreadSamples& ts : threads) total += ts.total;
        if (total) {
            if (g_profile) {
                std::printf("Load profile: samples of %s (%.0f ms):\n", StepLabel(step).c_str(), Ms(bucket_since, now));
            }
            PrintSamples(threads);
        }
        for (ThreadSamples& ts : threads) {
            ts.pcs.clear();
            ts.exe.clear();
            ts.total = ts.host = 0;
        }
    };
    for (;;) {
        const bool sampling = g_sample_load.load(std::memory_order_relaxed);
        if (!sampling) {
            if (was_sampling) {
                flush(bucket, NowNs());
                for (ThreadSamples& ts : threads) CloseHandle(ts.handle);
                threads.clear();
                was_sampling = false;
                bucket = -1;
            }
            Sleep(g_profile ? 100 : 20);
            if (!g_profile || NowNs() - last_idle_snapshot > 500'000'000ULL) {  // a Toolhelp snapshot costs ms
                prev = ThreadCpu();
                last_idle_snapshot = NowNs();
            }
            last_pick = NowNs();
            continue;
        }
        const u64 now = NowNs();
        if (!was_sampling) {
            bucket_since = now;
            bucket = g_profile ? g_cur_step.load(std::memory_order_relaxed) : -1;
            if (g_profile) {  // the main thread is always sampled: where it waits in every step
                if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                          g_main_tid)) {
                    ThreadSamples ts;
                    ts.tid = g_main_tid;
                    ts.handle = h;
                    threads.push_back(std::move(ts));
                }
            }
        }
        was_sampling = true;
        if (g_profile) {
            const int step = g_cur_step.load(std::memory_order_relaxed);
            if (step != bucket) {
                flush(bucket, now);
                bucket = step;
                bucket_since = now;
            }
        }
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
        Sleep(g_profile ? 8 : 4);
    }
}
#endif

void InGameEntered(int step) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (step == g_ingame_step) return;
    Emit("InGameStep", g_ingame_step, step, 0);
    g_ingame_step = step;
    g_cur_step = 100 + step;
    if (step == 2) g_move_step = -1;  // a new load: the next MoveMapStep is logged from its step 0
}
BB_COOP_SYSV void InGame2Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(2); }
BB_COOP_SYSV void InGame4Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(4); }
BB_COOP_SYSV void InGame6Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(6); }
BB_COOP_SYSV void InGame8Hook(u64, u64, u64, u64, u64, u64) { InGameEntered(8); }

} // namespace

void Install() {
    const char* e = std::getenv("BB_LOAD_TRACE");
    const char* prof = std::getenv("BB_LOAD_PROFILE");
    g_profile = prof && prof[0] && prof[0] != '0';
    g_profile_samples = g_profile && prof[0] >= '2';
    if (!g_profile && (!e || !e[0] || e[0] == '0')) return;
    if (!coop::Image()) {
        std::printf("Load trace: no image; off\n");
        return;
    }
    g_threads = g_profile || e[0] == '2';
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
    if (a && g_threads && (!g_profile || g_profile_samples)) std::thread(SamplerMain).detach();
    if (a && g_profile) std::thread(ScreenPoller).detach();
#endif
    std::printf("Load trace: %s (%s): MoveMapStep update %s, InGameStep 2/4/6/8 %d%d%d%d%s%s%s\n",
                a ? "on" : "OFF (the MoveMapStep update's prologue differs)", g_profile ? "BB_LOAD_PROFILE" : "BB_LOAD_TRACE",
                a ? "hooked" : "not hooked", b, c, d, f, g_threads ? "; busiest threads per step, sampled during the load" : "",
                g_profile ? "; world block / map part / obj / chr / collision states, the screen's own clock and threads by name" : "",
                g_profile_samples ? "; samples per step (BB_LOAD_PROFILE=2)" : "");
}

} // namespace BbLoadTrace
