// SPDX-License-Identifier: GPL-3.0-or-later
// Buffer pre-upload: the game-side hooks and the queue (see bbport_buf_prereg.h).
#include "bbport_buf_prereg.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bbport_toggles.h"
#include "party/coop_hooks.h"

namespace BbBufPrereg {

namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;

// Our offsets, byte-checked by HookCallSite (each site must be `call rel32` to its target).
constexpr u64 kBuildBufferCall = 0x2165e29; // GX buffer constructor 0x2165c10: call BuildBuffer
constexpr u64 kBuildBuffer = 0x216d420;     // (out record, -, description, initial data, allocator, GPU allocator, -) -> 0 = ok
constexpr u64 kFreeBufferCall = 0x216ce27;  // GX buffer deleting destructor 0x216ce10: call FreeBuffer
constexpr u64 kFreeBufferJump = 0x216ce4e;  // GX buffer destructor 0x216ce40 (the one the release path runs): jmp FreeBuffer
constexpr u64 kFreeBuffer = 0x216d690;      // (record): gives the memory back

// The description (6 dwords): size, usage, bind flags, CPU access, misc flags, stride.
constexpr u32 kUsageDynamic = 2; // rewritten by the game (not worth copying ahead)
constexpr u32 kBindVertex = 1, kBindIndex = 2;
constexpr u32 kMiscCpuMemory = 0x4000; // the memory comes from the object allocator, not the GPU's
// Smaller bindings are streamed (BufferCache::STREAM_THRESHOLD): no arena copy to make ahead.
constexpr u64 kMinBytes = 16 * 1024 + 1;

std::atomic<bool> g_on{false};
std::mutex g_mutex;
std::deque<Entry> g_queue;
std::unordered_map<u64, u64> g_live; // queued or uploaded buffers the game has not freed: address -> size
std::vector<Entry> g_freed;
// Census: [0 other, 1 vertex, 2 index, 3 both][0 no initial data, 1 with][0 static, 1 dynamic]
std::atomic<u64> g_count[4][2][2], g_bytes[4][2][2];
std::atomic<u64> g_noted{0}, g_noted_bytes{0}, g_small{0}, g_dropped{0}, g_frees{0};
std::atomic<u64> g_uploaded{0}, g_uploaded_bytes{0}, g_bound_bytes{0}, g_rejected{0};

BB_COOP_SYSV u64 BuildBufferHook(u64 out, u64 a1, u64 desc, u64 initial, u64 allocator, u64 gpu_allocator, u64 a6) {
    const u64 r = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64)>(coop::Guest(kBuildBuffer))(
        out, a1, desc, initial, allocator, gpu_allocator, a6);
    if (static_cast<int>(r) != 0 || !out || !desc) {
        return r;
    }
    u32 d[6];
    std::memcpy(d, reinterpret_cast<const void*>(desc), sizeof d);
    u64 address = 0;
    std::memcpy(&address, reinterpret_cast<const void*>(out + 8), sizeof address);
    const u64 size = d[0];
    const u32 kind = d[2] & (kBindVertex | kBindIndex);
    const bool dynamic = d[1] == kUsageDynamic;
    g_count[kind][initial != 0][dynamic].fetch_add(1, std::memory_order_relaxed);
    g_bytes[kind][initial != 0][dynamic].fetch_add(size, std::memory_order_relaxed);
    if (kind == 0 || dynamic || !initial || (d[4] & kMiscCpuMemory) || !address) {
        return r;
    }
    if (size < kMinBytes) {
        g_small.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    const Entry e{address, size, BbStats::frame_number.load(std::memory_order_relaxed)};
    std::scoped_lock lk{g_mutex};
    if (g_queue.size() >= 100'000) { // the renderer is not taking them (no frames): the oldest go
        g_live.erase(g_queue.front().address);
        g_queue.pop_front();
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_live[address] = size;
    g_queue.push_back(e);
    g_noted.fetch_add(1, std::memory_order_relaxed);
    g_noted_bytes.fetch_add(size, std::memory_order_relaxed);
    return r;
}

BB_COOP_SYSV u64 FreeBufferHook(u64 record) {
    u64 address = 0;
    if (record) {
        std::memcpy(&address, reinterpret_cast<const void*>(record + 8), sizeof address);
    }
    if (address) {
        std::scoped_lock lk{g_mutex};
        if (const auto it = g_live.find(address); it != g_live.end()) {
            if (g_freed.size() < 200'000) {
                g_freed.push_back({address, it->second, 0});
            }
            g_live.erase(it);
            g_frees.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(coop::Guest(kFreeBuffer))(record);
}

} // namespace

void Install() {
    const char* env = std::getenv("BB_BUF_PREUPLOAD");
    if (env && (env[0] == '0' || !std::strcmp(env, "off"))) {
        std::printf("Buffer pre-upload: off (BB_BUF_PREUPLOAD=0)\n");
        return;
    }
    if (!coop::Image()) {
        return;
    }
    // "probe" in the names: render-side hooks, left out of the party's hook-set check (both games may differ).
    // The free hook first: without it a freed buffer's copy would be kept in VRAM for good.
    const bool free_ok = coop::HookCallSite(kFreeBufferCall, kFreeBuffer, reinterpret_cast<const void*>(&FreeBufferHook),
                                            "buffer pre-upload probe: GX buffer free (deleting)") &&
                         coop::HookTailJump(kFreeBufferJump, kFreeBuffer, reinterpret_cast<const void*>(&FreeBufferHook),
                                            "buffer pre-upload probe: GX buffer free");
    const bool ok = free_ok && coop::HookCallSite(kBuildBufferCall, kBuildBuffer,
                                                  reinterpret_cast<const void*>(&BuildBufferHook),
                                                  "buffer pre-upload probe: GX buffer build");
    g_on.store(ok, std::memory_order_release);
    std::printf("Buffer pre-upload: %s (BB_BUF_PREUPLOAD=0 off)\n",
                ok ? "on: vertex and index buffers are copied to VRAM when the game creates them, ahead of their first draw"
                   : "NOT applied: the buffer constructor's or destructor's call is not the expected code");
}

bool Enabled() {
    return g_on.load(std::memory_order_acquire);
}

std::size_t Take(Entry* out, std::size_t max, u32 now_frame, u32 quiet_frames) {
    std::scoped_lock lk{g_mutex};
    std::size_t n = 0;
    while (n < max && !g_queue.empty() && now_frame - g_queue.front().frame >= quiet_frames) {
        const Entry& e = g_queue.front();
        // Freed meanwhile (or freed and its address given to a newer buffer, queued behind): left out.
        if (const auto it = g_live.find(e.address); it != g_live.end() && it->second == e.size) {
            out[n++] = e;
        }
        g_queue.pop_front();
    }
    return n;
}

std::size_t TakeFreed(Entry* out, std::size_t max) {
    std::scoped_lock lk{g_mutex};
    const std::size_t n = std::min(max, g_freed.size());
    std::copy(g_freed.end() - n, g_freed.end(), out);
    g_freed.resize(g_freed.size() - n);
    return n;
}

std::size_t Queued() {
    std::scoped_lock lk{g_mutex};
    return g_queue.size();
}

void Count(u64 uploaded_buffers, u64 uploaded_bytes, u64 bound_bytes, u64 rejected) {
    g_uploaded.fetch_add(uploaded_buffers, std::memory_order_relaxed);
    g_uploaded_bytes.fetch_add(uploaded_bytes, std::memory_order_relaxed);
    g_bound_bytes.fetch_add(bound_bytes, std::memory_order_relaxed);
    g_rejected.fetch_add(rejected, std::memory_order_relaxed);
}

void Report(u64 kept_bytes, bool paused) {
    std::size_t queued = 0, live = 0;
    {
        std::scoped_lock lk{g_mutex};
        queued = g_queue.size();
        live = g_live.size();
    }
    const auto mib = [](const std::atomic<u64>& v) { return (unsigned long long)(v.load() >> 20); };
    const auto n = [](const std::atomic<u64>& v) { return (unsigned long long)v.load(); };
    std::printf("Buffer pre-upload: %llu buffers noted (%llu MiB; %llu under 16 KiB left out, %llu dropped), %llu copied "
                "ahead (%llu MiB of %llu MiB were not in VRAM yet), %llu rejected, %llu freed by the game, %zu alive, "
                "%zu queued, %llu MiB kept in VRAM%s\n",
                n(g_noted), mib(g_noted_bytes), n(g_small), n(g_dropped), n(g_uploaded), mib(g_uploaded_bytes),
                mib(g_bound_bytes), n(g_rejected), n(g_frees), live, queued, (unsigned long long)(kept_bytes >> 20),
                paused ? "; paused: VRAM is short" : "");
    static const bool census = std::getenv("BB_DOOR_DIAG") != nullptr;
    if (census) {
        static const char* kinds[] = {"other", "vertex", "index", "vertex+index"};
        std::printf("Buffer pre-upload census (count / MiB; static+data, static no data, dynamic+data, dynamic no data):");
        for (int k = 0; k < 4; ++k) {
            std::printf(" %s %llu/%llu %llu/%llu %llu/%llu %llu/%llu;", kinds[k], n(g_count[k][1][0]), mib(g_bytes[k][1][0]),
                        n(g_count[k][0][0]), mib(g_bytes[k][0][0]), n(g_count[k][1][1]), mib(g_bytes[k][1][1]),
                        n(g_count[k][0][1]), mib(g_bytes[k][0][1]));
        }
        std::printf("\n");
    }
}

} // namespace BbBufPrereg
