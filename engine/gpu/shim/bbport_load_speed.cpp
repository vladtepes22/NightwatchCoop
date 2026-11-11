// SPDX-License-Identifier: GPL-3.0-or-later
// Load speed: see bbport_load_speed.h. Offsets are ours (bbhost - 0x400000).
#include "bbport_load_speed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "bbport_load_policy.h"
#include "bbport_settings.h"
#include "party/coop_hooks.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef BB_HAVE_ZLIB_NG
#include "bbport_fast_inflate.h"
#ifdef BB_HAVE_ZSTD
#include "bbport_inflate_bench.h"
#include "bbport_zstd_inflate.h"
#include "bbport_zstd_files.h"
#endif
#endif

namespace BbLoadSpeed {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- more post-processor threads -------------------------------------------------------------------------------
// The resource repository (FUN_020c4240) reads "Core.Resource.PostProcessorNum" (default 1; the game's registry holds
// 3) and makes that many PostProcessor objects of 0x4c8 bytes, each with its own thread "Core.Res.PostProcessor_%d"
// (FUN_020d8eb0); the thread affinity lookups past the third fall back to "none". The call is the one at +0x20c4761.
constexpr u64 kNumCall = 0x20c4761, kRegistryGetInt = 0x20ebb50;
using GetIntFn = u32(BB_COOP_SYSV*)(u64 registry, u64 name, u32 fallback);
std::atomic<u32> g_wanted{0};

BB_COOP_SYSV u32 PostProcessorNum(u64 registry, u64 name, u32 fallback) {
    const u32 game = reinterpret_cast<GetIntFn>(coop::Guest(kRegistryGetInt))(registry, name, fallback);
    const u32 n = std::max(game, g_wanted.load(std::memory_order_relaxed));
    std::printf("Load speed: post-processor threads %u (the game's %u; BB_LOAD_WORKERS=0 keeps the game's)\n", n, game);
    std::fflush(stdout);
    return n;
}

// Default (0.8.1, loadux081): min(4, max(3, usable CPUs - 2)): never below the game's 3, 4 on a PC with 6+ logical CPUs.
// With the guarded take below, 4-8 threads are safe (research/workers081.md: 192 loads, no hang, no panic) and shorten a
// lamp warp by 0.1-0.2 s on the mean with zlib-ng in (0.3-0.5 s on CPU-bound warps). bbport.ini load_workers=0 (launcher:
// Advanced) or BB_LOAD_WORKERS=0 keeps the game's own count; BB_LOAD_WORKERS=N (1..8) always wins.
u32 DefaultWorkers(u32 usable_cpus) {
    return bbport::loadpolicy::DefaultWorkers(usable_cpus, BbSettings::Get().load_workers.load());
}

u32 ParseWorkers(const char* v, u32 hw) { return bbport::loadpolicy::ParseWorkers(v, DefaultWorkers(hw)); }

// Logical CPUs the game may run on: the machine's, less what an affinity mask takes away (BB_AFFINITY_MASK is applied
// later by the audio start, so it is read here too).
u32 UsableCpus() {
    u32 n = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        n = std::min<u32>(n, u32(__builtin_popcountll(u64(process))));
    }
#endif
    if (const char* m = std::getenv("BB_AFFINITY_MASK")) {
        const u64 mask = std::strtoull(m, nullptr, 16);
        if (mask) n = std::min<u32>(n, u32(__builtin_popcountll(mask)));
    }
    return std::max(1u, n);
}

// The same repository init reads "Core.Resource.MaxFileLoadNum" (+0x20c4374) and "Core.Resource.WorkBufferNum"
// (+0x20c444c) as floats (FUN_020ec0f0: the default comes in xmm0) and "Core.Resource.OnceReadSize" (+0x20c4532,
// FUN_020eba00, int). The work-buffer pool (FUN_020ce840 at repository+0x650) holds WorkBufferNum*2 buffers of
// OnceReadSize (+0x10) bytes from the resource heap; the file loaders block on its semaphore when it is empty.
constexpr u64 kMaxFileLoadCall = 0x20c4374, kWorkBufCall = 0x20c444c, kOnceReadCall = 0x20c4532;
constexpr u64 kRegistryGetFloat = 0x20ec0f0, kRegistryGetIntDef = 0x20eba00;
using GetFloatFn = float(BB_COOP_SYSV*)(float fallback, u64 registry, u64 name);
using GetIntDefFn = int(BB_COOP_SYSV*)(u64 registry, u64 name, int fallback);
std::atomic<u32> g_workbuf{0};

BB_COOP_SYSV float MaxFileLoadNum(float fallback, u64 registry, u64 name) {
    const float game = reinterpret_cast<GetFloatFn>(coop::Guest(kRegistryGetFloat))(fallback, registry, name);
    std::printf("Load speed: Core.Resource.MaxFileLoadNum %g (default %g)\n", double(game), double(fallback));
    return game;
}

BB_COOP_SYSV float WorkBufferNum(float fallback, u64 registry, u64 name) {
    const float game = reinterpret_cast<GetFloatFn>(coop::Guest(kRegistryGetFloat))(fallback, registry, name);
    const u32 want = g_workbuf.load(std::memory_order_relaxed);
    const float n = want ? float(want) : game;
    std::printf("Load speed: Core.Resource.WorkBufferNum %g (the game's %g, default %g): %g*2 work buffers\n", double(n),
                double(game), double(fallback), double(n));
    std::fflush(stdout);
    return n;
}

BB_COOP_SYSV int OnceReadSize(u64 registry, u64 name, int fallback) {
    const int game = reinterpret_cast<GetIntDefFn>(coop::Guest(kRegistryGetIntDef))(registry, name, fallback);
    std::printf("Load speed: Core.Resource.OnceReadSize %d (default %d)\n", game, fallback);
    return game;
}

// ---- the resource manager's heap (FUN_020bea70 -> FUN_020bf4d0) --------------------------------------------------
// The game (FUN_00f7f8d0 at +0xf80502) creates the DLResourceManager with a parent heap and two sizes from its config
// object; FUN_020bf4d0 carves one block of that size and builds the resource heap (+0x630) every pool above comes
// from. A bigger WorkBufferNum fails inside FUN_020c4240 (FUN_020ce840's allocation) and FUN_020c1250 panics
// "DLResourceManagerImpl.cpp(491)". BB_LOAD_HEAP_SCALE=<percent> grows the sizes (experiment).
constexpr u64 kResMgrCreate = 0x20bea70;
using ResMgrCreateFn = void(BB_COOP_SYSV*)(u64 heap, u64 size_a, u64 size_b, u32 p4, u32 p5, u32 p6, u64 p7, u64 p8,
                                           u64 p9, u64 p10, u64 p11);
ResMgrCreateFn g_resmgr_create = nullptr;
std::atomic<u32> g_heap_scale{100};

BB_COOP_SYSV void ResMgrCreateHook(u64 heap, u64 size_a, u64 size_b, u32 p4, u32 p5, u32 p6, u64 p7, u64 p8, u64 p9,
                                   u64 p10, u64 p11) {
    const u32 scale = g_heap_scale.load(std::memory_order_relaxed);
    const u64 a = size_a * scale / 100, b = size_b * scale / 100;
    std::printf("Load speed: resource manager heap %llx, sizes %llu / %llu bytes (%u / %u / %u); scale %u%% -> %llu / %llu\n",
                static_cast<unsigned long long>(heap), static_cast<unsigned long long>(size_a),
                static_cast<unsigned long long>(size_b), p4, p5, p6, scale, static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b));
    std::fflush(stdout);
    g_resmgr_create(heap, a, b, p4, p5, p6, p7, p8, p9, p10, p11);
}

void InstallHeapScale() {
    const char* e = std::getenv("BB_LOAD_HEAP_SCALE");
    const long v = e ? std::strtol(e, nullptr, 10) : 0;
    if (v <= 0) return;
    g_heap_scale.store(u32(std::min<long>(std::max<long>(v, 100), 800)));
    const bool ok = coop::ReplacePrologue(kResMgrCreate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                                          reinterpret_cast<const void*>(&ResMgrCreateHook),
                                          reinterpret_cast<void**>(&g_resmgr_create), "load speed: resource manager heap");
    std::printf("Load speed: BB_LOAD_HEAP_SCALE %u%%: %s\n", g_heap_scale.load(), ok ? "hooked" : "NOT hooked");
}

// ---- worker trace (BB_LOAD_WORKERS_TRACE=1) ----------------------------------------------------------------------
// PostProcessor (0x4c8 bytes, FUN_020d9990 is its thread loop): three decompress work units at +0x140 / +0x218 / +0x2f0
// (0xd8 each, the stream's path key at unit+0xf8: +0x238 / +0x310 / +0x3e8), three decrypt units at +0x30 (0x68 each),
// the decrypt queue vector at +0x3f0 (items 0x70, begin +0x3f8, end +0x400), the decompress queue at +0x420 (items
// 0x48, begin +0x428, end +0x430), the wake event +0x450, mutexes +0x470 / +0x488 / +0x4a0, an "accepting" flag +0x4c0.
// A decompress queue item: [0] buffer (0 = close), [1] path, [2] bytes, [3] != 0 on a stream's first chunk, [6] the
// task's owner-PP slot, [7] int* task state. The loop: a first chunk takes a free unit or panics
// "PostProcessor.cpp(580) pWork could not be allocated"; a later chunk looks its unit up by path and, when none
// matches, drops the buffer silently (PostProcessor.cpp around line 0x270): the stream never finishes.
// FUN_020cee30(list, pp): an idle PP takes the first pending stream (a node holding the chunks the file loader
// queued before any PP owned the stream) and writes itself into the stream's owner slot.
constexpr u64 kTake = 0x20cee30, kPushDecomp = 0x20dacf0, kPushDecrypt = 0x20daba0, kCancel = 0x20d2c50;
constexpr u32 kPpSize = 0x4c8, kMaxPp = 16;
std::atomic<u64> g_pp_base{0};
struct PpStat {
    std::atomic<u64> pushes{0}, first{0}, takes{0}, takes_full{0}, drops{0}, closes{0}, decrypt{0};
    std::atomic<u32> max_units{0};   // most open decompress units seen at a push or take
};
PpStat g_pp[kMaxPp];
std::atomic<u64> g_cancels{0}, g_trace_lines{0}, g_pending_list{0};
std::atomic<bool> g_trace_on{false};
void NoteUnits(PpStat& s, u32 units) {
    u32 m = s.max_units.load(std::memory_order_relaxed);
    while (units > m && !s.max_units.compare_exchange_weak(m, units)) {}
}

// The repository is the static DLResourceManager (+0x56a4590) + 0x680: its PP array pointer and count are the
// globals +0x56a5208 / +0x56a5210 (FUN_020bec80 reads them).
constexpr u64 kPpArrayGlobal = 0x56a5208, kPpCountGlobal = 0x56a5210;
u64 PpArray() { return *reinterpret_cast<const u64*>(coop::Image() + kPpArrayGlobal); }
u64 PpCount() { return *reinterpret_cast<const u64*>(coop::Image() + kPpCountGlobal); }
u32 PpIndex(u64 pp) {
    u64 base = PpArray();
    if (base == 0 || pp < base) {
        base = g_pp_base.load(std::memory_order_relaxed);
        while (base == 0 || pp < base) {
            if (g_pp_base.compare_exchange_weak(base, pp)) { base = pp; break; }
        }
    }
    const u64 i = (pp - base) / kPpSize;
    return i < kMaxPp ? u32(i) : kMaxPp - 1;
}
u32 OpenUnits(u64 pp, u64 path, bool* has_path) {
    u32 n = 0;
    *has_path = false;
    for (u64 off : {0x238ull, 0x310ull, 0x3e8ull}) {
        const u64 key = *reinterpret_cast<const u64*>(pp + off);
        if (key) ++n;
        if (key && key == path) *has_path = true;
    }
    return n;
}
u64 QueueLen(u64 pp, u64 begin_off, u64 item) {
    const u64 b = *reinterpret_cast<const u64*>(pp + begin_off), e = *reinterpret_cast<const u64*>(pp + begin_off + 8);
    return e >= b ? (e - b) / item : 0;
}
void TraceLine(const char* what, u32 pp, u32 units, u64 dq, u64 cq, const char* extra) {
    if (g_trace_lines.fetch_add(1, std::memory_order_relaxed) > 4000) return;   // a hang or panic floods otherwise
    std::printf("Load workers: %s PP%u units %u queues %llu/%llu %s\n", what, pp, units,
                static_cast<unsigned long long>(dq), static_cast<unsigned long long>(cq), extra);
    std::fflush(stdout);
}

void TakeStats(u64 list, u64 pp) {
    const u32 i = PpIndex(pp);
    bool has = false;
    const u32 units = OpenUnits(pp, 0, &has);
    g_pp[i].takes.fetch_add(1, std::memory_order_relaxed);
    NoteUnits(g_pp[i], units);
    g_pending_list.store(list, std::memory_order_relaxed);
    const u64 pending = *reinterpret_cast<const u64*>(list + 0x18);
    if (units >= 3 && pending) {
        g_pp[i].takes_full.fetch_add(1, std::memory_order_relaxed);
        char buf[96];
        std::snprintf(buf, sizeof buf, "TAKE with every unit busy, %llu pending (next first chunk panics)",
                      static_cast<unsigned long long>(pending));
        TraceLine("take", i, units, QueueLen(pp, 0x3f8, 0x70), QueueLen(pp, 0x428, 0x48), buf);
    }
}

BB_COOP_SYSV void PushDecompHook(u64 pp, u64 item, u64, u64, u64, u64) {
    const u32 i = PpIndex(pp);
    const u64* t = reinterpret_cast<const u64*>(item);
    const u64 buffer = t[0], path = t[1];
    const bool first = t[3] != 0;
    bool has = false;
    const u32 units = OpenUnits(pp, path, &has);
    g_pp[i].pushes.fetch_add(1, std::memory_order_relaxed);
    NoteUnits(g_pp[i], units);
    const int state = t[7] ? *reinterpret_cast<const int*>(t[7]) : -1;
    if (buffer == 0) {
        g_pp[i].closes.fetch_add(1, std::memory_order_relaxed);
        if (!has) TraceLine("close", i, units, QueueLen(pp, 0x3f8, 0x70), QueueLen(pp, 0x428, 0x48), "CLOSE for a stream with no unit here");
        return;
    }
    if (first) {
        g_pp[i].first.fetch_add(1, std::memory_order_relaxed);
        if (units >= 3) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "FIRST chunk with every unit busy (state %d): pWork panic ahead", state);
            TraceLine("push", i, units, QueueLen(pp, 0x3f8, 0x70), QueueLen(pp, 0x428, 0x48), buf);
        }
        return;
    }
    if (!has) {
        // Not conclusive by itself: the unit may be allocated by a first chunk still queued ahead of this one.
        const u64 cq = QueueLen(pp, 0x428, 0x48);
        if (cq == 0) {
            g_pp[i].drops.fetch_add(1, std::memory_order_relaxed);
            char buf[96];
            std::snprintf(buf, sizeof buf, "chunk for a stream with no unit here and nothing queued (state %d): DROP", state);
            TraceLine("push", i, units, QueueLen(pp, 0x3f8, 0x70), cq, buf);
        }
    }
}

// The PP loop's silent drop: at +0x20da6e3 (right after the unit search for a later chunk failed) the loop reloads
// the PP (`mov rax, [rbp-0xa0]`), returns the buffer to the pool and skips the chunk. r14 holds the queue item.
constexpr u64 kDropSite = 0x20da6e3, kDropResume = 0x20da6ea;
std::atomic<u64> g_real_drops{0};
BB_COOP_SYSV u64 DropFilter(u64 item, u64 pp) {
    const u64* t = reinterpret_cast<const u64*>(item);
    const u64 n = g_real_drops.fetch_add(1, std::memory_order_relaxed) + 1;
    const u32 i = PpIndex(pp);
    g_pp[i].drops.fetch_add(1, std::memory_order_relaxed);
    if (n <= 200) {
        bool has = false;
        char buf[128];
        std::snprintf(buf, sizeof buf, "REAL DROP #%llu: %llu-byte chunk, state %d, owner slot %s this PP",
                      static_cast<unsigned long long>(n), static_cast<unsigned long long>(t[2]),
                      t[7] ? *reinterpret_cast<const int*>(t[7]) : -1,
                      (t[6] && *reinterpret_cast<const u64*>(t[6]) == pp) ? "=" : "!=");
        TraceLine("loop", i, OpenUnits(pp, 0, &has), QueueLen(pp, 0x3f8, 0x70), QueueLen(pp, 0x428, 0x48), buf);
    }
    return pp;   // rax as the original `mov rax, [rbp-0xa0]` left it
}

// FUN_020daef0(pp, accepting): a PP vtable slot the game calls; log who (the return address is the caller's).
constexpr u64 kSetAccepting = 0x20daef0;
using SetAcceptingFn = void(BB_COOP_SYSV*)(u64 pp, int v);
SetAcceptingFn g_set_accepting = nullptr;
BB_COOP_SYSV void SetAcceptingHook(u64 pp, int v) {
    const u64 ret = reinterpret_cast<u64>(__builtin_return_address(0));
    const u64 image = reinterpret_cast<u64>(coop::Image());
    std::printf("Load workers: PP%u accepting := %d (was %d) from %s+0x%llx\n", PpIndex(pp), v,
                *reinterpret_cast<const int*>(pp + 0x4c0), ret >= image ? "guest" : "host",
                static_cast<unsigned long long>(ret >= image ? ret - image : ret));
    std::fflush(stdout);
    g_set_accepting(pp, v);
}
// The game-facing entry: FUN_020bec80(index, v) (static, bounds-checked against the PP count) -> FUN_020c1c20(mgr,
// index, v) -> FUN_020ca150(repo, index, v): index * 0x4c8 into the PP array, +0x4c0 := v, event set when v != 0.
constexpr u64 kSetAcceptingByIndex = 0x20bec80;
using SetByIndexFn = void(BB_COOP_SYSV*)(u32 index, int v);
SetByIndexFn g_set_by_index = nullptr;
BB_COOP_SYSV void SetByIndexHook(u32 index, int v) {
    const u64 ret = reinterpret_cast<u64>(__builtin_return_address(0));
    const u64 image = reinterpret_cast<u64>(coop::Image());
    std::printf("Load workers: PP%u accepting := %d (by index) from %s+0x%llx\n", index, v, ret >= image ? "guest" : "host",
                static_cast<unsigned long long>(ret >= image ? ret - image : ret));
    std::fflush(stdout);
    g_set_by_index(index, v);
}

BB_COOP_SYSV void PushDecryptHook(u64 pp, u64, u64, u64, u64, u64) {
    g_pp[PpIndex(pp)].decrypt.fetch_add(1, std::memory_order_relaxed);
}

BB_COOP_SYSV void CancelHook(u64, u64 task, u64, u64, u64, u64) {
    const u64 n = g_cancels.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64* t = reinterpret_cast<const u64*>(task);
    const u64 owner_slot = t[0x14];
    const u64 owner = owner_slot ? *reinterpret_cast<const u64*>(owner_slot) : 0;
    if (n <= 50 || n % 100 == 0) {
        std::printf("Load workers: cancel #%llu task %llx: decomp %s, state %d, owner PP %s\n",
                    static_cast<unsigned long long>(n), static_cast<unsigned long long>(task), t[0xc] ? "yes" : "no",
                    t[6] ? *reinterpret_cast<const int*>(t[6]) : -1, owner ? "set" : "NONE");
        std::fflush(stdout);
    }
}

void InstallWorkerTrace() {
    const char* e = std::getenv("BB_LOAD_WORKERS_TRACE");
    if (!e || !*e || !std::strcmp(e, "0")) return;
    g_trace_on.store(true);
    const bool ok = coop::HookPrologue(kPushDecomp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &PushDecompHook,
                                       "load workers: push decompress") &&
                    coop::HookPrologue(kPushDecrypt, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &PushDecryptHook,
                                       "load workers: push decrypt") &&
                    coop::HookPrologue(kCancel, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &CancelHook, "load workers: cancel") &&
                    coop::HookValueFilter(kDropSite, {0x48, 0x8b, 0x85, 0x60, 0xff, 0xff, 0xff}, {0x4c, 0x89, 0xf7, 0x48, 0x89, 0xc6},
                                          &DropFilter, kDropResume, "load workers: dropped chunk") &&
                    coop::ReplacePrologue(kSetAccepting, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                                          reinterpret_cast<const void*>(&SetAcceptingHook),
                                          reinterpret_cast<void**>(&g_set_accepting), "load workers: set accepting") &&
                    coop::ReplacePrologue(kSetAcceptingByIndex, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                                          reinterpret_cast<const void*>(&SetByIndexHook),
                                          reinterpret_cast<void**>(&g_set_by_index), "load workers: set accepting by index");
    std::printf("Load speed: BB_LOAD_WORKERS_TRACE: %s\n", ok ? "hooked" : "NOT hooked");
    if (!ok) return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            char line[1600];
            int n = std::snprintf(line, sizeof line, "Load workers: summary (push/first/take/take-full/drop/close/decrypt/max-units):");
            const u64 base = PpArray() ? PpArray() : g_pp_base.load(std::memory_order_relaxed);
            u32 live = 0;
            for (u32 i = 0; i < kMaxPp && n < int(sizeof line) - 120; ++i) {
                const PpStat& s = g_pp[i];
                if (!s.pushes.load() && !s.takes.load() && !s.decrypt.load()) continue;
                live = i + 1;
                n += std::snprintf(line + n, sizeof line - n, " PP%u %llu/%llu/%llu/%llu/%llu/%llu/%llu/%u", i,
                                   static_cast<unsigned long long>(s.pushes.load()), static_cast<unsigned long long>(s.first.load()),
                                   static_cast<unsigned long long>(s.takes.load()), static_cast<unsigned long long>(s.takes_full.load()),
                                   static_cast<unsigned long long>(s.drops.load()), static_cast<unsigned long long>(s.closes.load()),
                                   static_cast<unsigned long long>(s.decrypt.load()), s.max_units.load());
            }
            std::printf("%s cancels %llu\n", line, static_cast<unsigned long long>(g_cancels.load()));
            // Live state: the pending list and every PP's open units / queues / accepting flag. A pending stream
            // while every PP sits idle with a free unit is a lost wakeup; an open unit that never closes, a lost chunk.
            const u64 list = g_pending_list.load(std::memory_order_relaxed);
            n = std::snprintf(line, sizeof line, "Load workers: now pending %llu (array %llx x%llu);",
                              static_cast<unsigned long long>(list ? *reinterpret_cast<const u64*>(list + 0x18) : 0),
                              static_cast<unsigned long long>(base), static_cast<unsigned long long>(PpCount()));
            for (u32 i = 0; base && i < live && n < int(sizeof line) - 120; ++i) {
                const u64 pp = base + u64(i) * kPpSize;
                bool has = false;
                n += std::snprintf(line + n, sizeof line - n, " PP%u u%u q%llu/%llu a%d", i, OpenUnits(pp, 0, &has),
                                   static_cast<unsigned long long>(QueueLen(pp, 0x3f8, 0x70)),
                                   static_cast<unsigned long long>(QueueLen(pp, 0x428, 0x48)),
                                   *reinterpret_cast<const int*>(pp + 0x4c0));
            }
            std::printf("%s\n", line);
            std::fflush(stdout);
        }
    }).detach();
}

// ---- the guarded take (what makes more than 3 post-processors safe) -----------------------------------------------
// FUN_020cee30(list, pp) gives the first pending stream to `pp` whenever its queues are empty, whatever its three
// decompress units hold. With more PPs the file loaders run ahead more often, a PP sits idle between the chunks of
// the big streams it already owns, takes a fourth stream and the next first chunk panics "PostProcessor.cpp(580)
// pWork could not be allocated" (the fastload081 R6 session). The replacement keeps the engine's own take but
// chooses the PP: this one while it has a free unit, else another accepting PP with a free unit (the engine's take
// run with that PP as its argument fills that PP's queue and owner slot; its event is then set like a push does),
// and nothing when every PP is full (the stream stays pending until a unit frees up).
using TakeFn = u32(BB_COOP_SYSV*)(u64 list, u64 pp);
TakeFn g_take = nullptr;
constexpr u64 kSignalEvent = 0x207e780;
using SignalFn = void(BB_COOP_SYSV*)(u64 event, u32 value);
std::atomic<u64> g_handovers{0}, g_refusals{0};
constexpr u32 kUnitsPerPp = 3;

u32 BusyUnits(u64 pp) {   // open units plus chunks queued (a queued first chunk takes a unit when it runs)
    bool has = false;
    const u64 queued = QueueLen(pp, 0x428, 0x48);
    const u32 open = OpenUnits(pp, 0, &has);
    return queued > kUnitsPerPp ? kUnitsPerPp : open + u32(queued);
}

BB_COOP_SYSV u32 GuardedTake(u64 list, u64 pp) {
    if (g_trace_on.load(std::memory_order_relaxed)) TakeStats(list, pp);
    if (*reinterpret_cast<const u64*>(list + 0x18) == 0) return g_take(list, pp);   // nothing pending: the quick path
    if (BusyUnits(pp) < kUnitsPerPp) return g_take(list, pp);
    const u64 base = PpArray(), count = PpCount();
    if (base && count > 1 && pp >= base) {
        const u64 self = (pp - base) / kPpSize;
        for (u64 k = 1; k < count; ++k) {
            const u64 other = base + ((self + k) % count) * kPpSize;
            if (*reinterpret_cast<const int*>(other + 0x4c0) == 0) continue;   // not accepting
            if (BusyUnits(other) >= kUnitsPerPp) continue;
            const u32 r = g_take(list, other);
            if (r) {
                reinterpret_cast<SignalFn>(coop::Guest(kSignalEvent))(other + 0x450, 1);
                const u64 n = g_handovers.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 20 || n % 1000 == 0) {
                    std::printf("Load workers: PP%llu full, pending stream handed to PP%llu (%llu so far)\n",
                                static_cast<unsigned long long>(self), static_cast<unsigned long long>((other - base) / kPpSize),
                                static_cast<unsigned long long>(n));
                    std::fflush(stdout);
                }
            }
            return r;
        }
    }
    const u64 n = g_refusals.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 1000 == 0) {
        std::printf("Load workers: every post-processor full, pending stream left waiting (%llu so far)\n",
                    static_cast<unsigned long long>(n));
        std::fflush(stdout);
    }
    return 0;
}

void InstallWorkers() {
    InstallHeapScale();
    InstallWorkerTrace();
    const u32 hw = UsableCpus();
    const u32 n = ParseWorkers(std::getenv("BB_LOAD_WORKERS"), hw);
    if (const char* w = std::getenv("BB_LOAD_WORKBUF")) {   // experiment: the work-buffer pool count (0 = the game's)
        const long v = std::strtol(w, nullptr, 10);
        g_workbuf.store(v <= 0 ? 0 : u32(std::min<long>(v, 256)));
    }
    if (n <= 3 && g_workbuf.load() == 0) {   // the game starts 3: nothing to add (off, or a PC with fewer than 6 CPUs)
        std::printf("Load speed: post-processor threads are the game's own (BB_LOAD_WORKERS=%s, %u usable CPUs)\n",
                    std::getenv("BB_LOAD_WORKERS") ? std::getenv("BB_LOAD_WORKERS") : "unset", hw);
        return;
    }
    // All or nothing: the count only grows when the guarded take is in (the registry call sites are checked by
    // HookCallSite, the take's prologue by ReplacePrologue, the event setter's by Matches).
    const bool guard =
        coop::Matches(kSignalEvent, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x41, 0x89, 0xf7, 0x48, 0x89, 0xfb, 0xf6, 0x43, 0x18, 0x01}) &&
        coop::CallsTo(kNumCall, kRegistryGetInt) && coop::CallsTo(kMaxFileLoadCall, kRegistryGetFloat) &&
        coop::CallsTo(kWorkBufCall, kRegistryGetFloat) && coop::CallsTo(kOnceReadCall, kRegistryGetIntDef) &&
        coop::ReplacePrologue(kTake, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&GuardedTake),
                              reinterpret_cast<void**>(&g_take), "load speed: guarded take");
    if (!guard) {
        std::printf("Load speed: BB_LOAD_WORKERS %u NOT applied: this executable's resource pipeline differs\n", n);
        return;
    }
    g_wanted.store(n);
    const bool ok = coop::HookCallSite(kNumCall, kRegistryGetInt, reinterpret_cast<const void*>(&PostProcessorNum),
                                       "load speed: post-processor count") &&
                    coop::HookCallSite(kMaxFileLoadCall, kRegistryGetFloat, reinterpret_cast<const void*>(&MaxFileLoadNum),
                                       "load speed: max file load num") &&
                    coop::HookCallSite(kWorkBufCall, kRegistryGetFloat, reinterpret_cast<const void*>(&WorkBufferNum),
                                       "load speed: work buffer num") &&
                    coop::HookCallSite(kOnceReadCall, kRegistryGetIntDef, reinterpret_cast<const void*>(&OnceReadSize),
                                       "load speed: once read size");
    std::printf("Load speed: BB_LOAD_WORKERS %u, BB_LOAD_WORKBUF %u (%u hardware threads): %s\n", n, g_workbuf.load(), hw,
                ok ? "hooked" : "NOT hooked");
}

// ---- zlib-ng behind the engine's inflate -----------------------------------------------------------------------
#ifdef BB_HAVE_ZLIB_NG
// zlib 1.2.3 inside the executable: inflateReset +0x2067850, inflateInit2_ +0x2067900, inflate +0x2067ac0,
// inflateEnd +0x20697c0 (the stream readers at +0x2065cf0 and +0x2066b70 call init2_ / inflate; their ends and
// resets run through the same functions).
constexpr u64 kReset = 0x2067850, kInit2 = 0x2067900, kInflate = 0x2067ac0, kEnd = 0x20697c0;
using Z = BbFastInflate::GuestZ;
BbFastInflate::Pool g_pool_zlib{BbFastInflate::kZlibWindowBits}, g_pool_raw{BbFastInflate::kRawWindowBits};
// z->reserved: the zlib-ng stream's address, bit 0 set for a raw (windowBits -15) one (stream objects are 8-aligned).
// Bit 1 set (zstd081): a zstd stream; the address is its BbZstdInflate::State, which holds the zlib-ng stream.
BbFastInflate::Pool& PoolOf(std::uint64_t tagged) { return tagged & 1 ? g_pool_raw : g_pool_zlib; }
zng_stream* StreamOf(std::uint64_t tagged) { return reinterpret_cast<zng_stream*>(tagged & ~std::uint64_t(3)); }
std::atomic<std::uint64_t> g_streams{0}, g_fast_calls{0}, g_in_bytes{0}, g_out_bytes{0};

#ifdef BB_HAVE_ZSTD
BbZstdInflate::Pool g_zstd_pool;
std::atomic<std::uint64_t> g_zstd_streams{0}, g_zstd_in{0}, g_zstd_out{0}, g_zstd_ns{0};
BbZstdInflate::State* ZStateOf(std::uint64_t tagged) { return BbZstdInflate::StateOf(tagged); }
bool ZstdInflate(Z* z, std::int32_t flush, std::int64_t* rc) {
    BbZstdInflate::Stats st;
    const auto t0 = std::chrono::steady_clock::now();
    if (!BbZstdInflate::Route(z, flush, g_zstd_pool, rc, &st)) return false;
    g_zstd_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                        std::memory_order_relaxed);
    g_zstd_streams.fetch_add(st.streams, std::memory_order_relaxed);
    g_zstd_in.fetch_add(st.in_bytes, std::memory_order_relaxed);
    g_zstd_out.fetch_add(st.out_bytes, std::memory_order_relaxed);
    static std::atomic<std::uint64_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) % 4000 == 3999) {
        std::printf("Load speed: zstd inflate: %llu streams, %llu MB in, %llu MB out, %.2f s in the decoder\n",
                    static_cast<unsigned long long>(g_zstd_streams.load()),
                    static_cast<unsigned long long>(g_zstd_in.load() >> 20),
                    static_cast<unsigned long long>(g_zstd_out.load() >> 20), g_zstd_ns.load() * 1e-9);
        std::fflush(stdout);
    }
    return true;
}
#endif

using InitFn = std::int64_t(BB_COOP_SYSV*)(Z*, std::int32_t, const char*, std::int32_t);
using InflateFn = std::int64_t(BB_COOP_SYSV*)(Z*, std::int32_t);
using OneFn = std::int64_t(BB_COOP_SYSV*)(Z*);
InitFn g_init = nullptr;
InflateFn g_inflate = nullptr;
OneFn g_reset = nullptr, g_end = nullptr;

BB_COOP_SYSV std::int64_t InitHook(Z* z, std::int32_t window_bits, const char* version, std::int32_t size) {
    if (z) z->reserved = 0;
    const std::int64_t rc = g_init(z, window_bits, version, size);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 6) {
        std::printf("Load speed: inflateInit2_ window_bits %d rc %lld size %d\n", window_bits, static_cast<long long>(rc), size);
    }
    const bool raw = window_bits == BbFastInflate::kRawWindowBits;
    if (rc == 0 && z && (raw || window_bits == BbFastInflate::kZlibWindowBits)) {
        if (zng_stream* s = (raw ? g_pool_raw : g_pool_zlib).Acquire()) {
            z->reserved = reinterpret_cast<std::uint64_t>(s) | (raw ? 1 : 0);
            g_streams.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return rc;
}

BB_COOP_SYSV std::int64_t InflateHook(Z* z, std::int32_t flush) {
    if (z && z->reserved && z->state) {
#ifdef BB_HAVE_ZSTD
        if (std::int64_t zrc; ZstdInflate(z, flush, &zrc)) return zrc;
#endif
        const std::uint64_t n = g_fast_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t in0 = z->total_in, out0 = z->total_out;
#ifdef BB_HAVE_ZSTD
        const std::int64_t rc = BbInflateBench::g_on
                                    ? BbInflateBench::Call(z, [&] { return BbFastInflate::Step(z, StreamOf(z->reserved), flush); })
                                    : BbFastInflate::Step(z, StreamOf(z->reserved), flush);
#else
        const std::int64_t rc = BbFastInflate::Step(z, StreamOf(z->reserved), flush);
#endif
        g_in_bytes.fetch_add(z->total_in - in0, std::memory_order_relaxed);
        g_out_bytes.fetch_add(z->total_out - out0, std::memory_order_relaxed);
        if (n % 1000 == 0) {
            std::printf("Load speed: fast inflate: %llu streams, %llu calls, %llu MB in, %llu MB out\n",
                        static_cast<unsigned long long>(g_streams.load()), static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(g_in_bytes.load() >> 20),
                        static_cast<unsigned long long>(g_out_bytes.load() >> 20));
            std::fflush(stdout);
        }
        return rc;
    }
    return g_inflate(z, flush);
}

BB_COOP_SYSV std::int64_t ResetHook(Z* z) {
#ifdef BB_HAVE_ZSTD
    if (BbInflateBench::g_on) BbInflateBench::Drop(z);
#endif
    const std::int64_t rc = g_reset(z);
#ifdef BB_HAVE_ZSTD
    if (z && (z->reserved & 2) && rc == 0) {
        BbZstdInflate::State* st = ZStateOf(z->reserved);
        BbZstdInflate::Reset(st);
        if (st->zng) zng_inflateReset(reinterpret_cast<zng_stream*>(st->zng));
        return rc;
    }
#endif
    if (z && z->reserved && rc == 0) zng_inflateReset(StreamOf(z->reserved));
    return rc;
}

BB_COOP_SYSV std::int64_t EndHook(Z* z) {
#ifdef BB_HAVE_ZSTD
    if (BbInflateBench::g_on) BbInflateBench::Drop(z);
#endif
    if (z && z->reserved && z->state) {
#ifdef BB_HAVE_ZSTD
        BbZstdInflate::Unroute(z, g_zstd_pool);
#endif
        PoolOf(z->reserved).Release(StreamOf(z->reserved));
        z->reserved = 0;
    }
    return g_end(z);
}

bool g_inflate_installed = false;
void InstallInflate() {
    const char* e = std::getenv("BB_FAST_INFLATE");
    if (e && (!std::strcmp(e, "0") || !std::strcmp(e, "off") || !std::strcmp(e, "false"))) {
        std::printf("Load speed: the engine's inflate is the game's own (BB_FAST_INFLATE=0)\n");
        return;
    }
    // All four sites or none: a half-installed set would leak or step a stream the engine did not make.
    const bool match =
        coop::Matches(kReset, {0xb8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x85, 0xff}) &&
        coop::Matches(kInit2, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x41, 0x89, 0xf6, 0x48, 0x89, 0xfb}) &&
        coop::Matches(kInflate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                 0x81, 0xec, 0x88, 0x00, 0x00, 0x00}) &&
        coop::Matches(kEnd, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb});
    if (!match) {
        std::printf("Load speed: fast inflate not installed: this executable's zlib differs\n");
        return;
    }
    const bool ok =
        coop::ReplacePrologue(kInit2, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x41, 0x89, 0xf6, 0x48, 0x89, 0xfb},
                              reinterpret_cast<const void*>(&InitHook), reinterpret_cast<void**>(&g_init),
                              "load speed: inflateInit2_") &&
        coop::ReplacePrologue(kInflate,
                              {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                               0x81, 0xec, 0x88, 0x00, 0x00, 0x00},
                              reinterpret_cast<const void*>(&InflateHook), reinterpret_cast<void**>(&g_inflate),
                              "load speed: inflate") &&
        coop::ReplacePrologue(kReset, {0xb8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x85, 0xff},
                              reinterpret_cast<const void*>(&ResetHook), reinterpret_cast<void**>(&g_reset),
                              "load speed: inflateReset") &&
        coop::ReplacePrologue(kEnd, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb},
                              reinterpret_cast<const void*>(&EndHook), reinterpret_cast<void**>(&g_end),
                              "load speed: inflateEnd");
    std::printf("Load speed: zlib-ng %s behind the engine's inflate: %s\n", zlibng_version(),
                ok ? "installed" : "NOT (partly) installed");
    g_inflate_installed = ok;
}
#else
void InstallInflate() { std::printf("Load speed: this build has no zlib-ng; the engine's inflate is the game's own\n"); }
#endif

} // namespace

void Install() {
    if (!coop::Image()) return;
    InstallWorkers();
#if defined(BB_HAVE_ZLIB_NG) && defined(BB_HAVE_ZSTD)
    BbInflateBench::Init();
#endif
    InstallInflate();
#if defined(BB_HAVE_ZLIB_NG) && defined(BB_HAVE_ZSTD)
    // zstd081: converted game files, only where the inflate hooks that decode them are installed.
    if (const char* dir = std::getenv("BB_ZSTD_DIR"); dir && *dir) {
        if (g_inflate_installed) BbZstdFiles::Enable(dir);
        else std::printf("Zstd files: not used: the inflate hooks are not installed\n");
    }
#endif
}

} // namespace BbLoadSpeed
