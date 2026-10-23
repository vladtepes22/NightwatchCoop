// SPDX-License-Identifier: GPL-3.0-or-later
// BB_HEAP_WATCH_LOG=1: live allocations per guest call site in one Dantelion heap (see the header).
//
// The game's heaps are C++ objects: vtable +0x50 Allocate(size), +0x58 AllocateAligned(size,
// align), +0x70 Free(p) (DL_PANIC 0x20b55b0 and the containers call them; FUN_0207b720 finds the
// heap of a block and calls its +0x70). We copy the heap's vtable, point those entries at
// wrappers that forward to the game's functions and record (block -> call site, size), and put the
// copy's address into the heap object (only that object: other heaps keep the shared vtable).
// Blocks allocated before the swap are unknown to us; their frees are counted and ignored.
#include "party_heapwatch.h"

#include "coop_hooks.h"
#include "game_state.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace coop {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kVtableBytes = 0x100;  // 32 entries copied (the used ones are below 0x80)

using AllocFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);
using FreeFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);

struct Block {
    u64 site;
    u64 size;
};
struct Site {
    u64 live_bytes = 0, live_count = 0, allocs = 0;
};

std::mutex g_mu;
std::unordered_map<u64, Block> g_blocks;
std::unordered_map<u64, Site> g_sites;
u64 g_heap = 0, g_base = 0, g_live = 0, g_peak = 0, g_unknown_frees = 0, g_failed = 0;
AllocFn g_alloc50 = nullptr, g_alloc58 = nullptr;
FreeFn g_free70 = nullptr;
alignas(16) std::uint8_t g_vtable[kVtableBytes];
int g_state = 0;  // 0 not checked, 1 waiting for the heap, 2 installed, -1 off
Clock::time_point g_last_log;

void Record(u64 p, u64 size, u64 ra) {
    const u64 site = ra >= g_base ? ra - g_base : ra;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!p) {
        if (g_failed++ < 20) {
            std::printf("[coop] Heap watch: allocation of %llu bytes FAILED from +0x%llx (live %llu bytes in %zu blocks, peak %llu)\n",
                        static_cast<unsigned long long>(size), static_cast<unsigned long long>(site),
                        static_cast<unsigned long long>(g_live), g_blocks.size(), static_cast<unsigned long long>(g_peak));
            std::fflush(stdout);
        }
        return;
    }
    auto [it, fresh] = g_blocks.emplace(p, Block{site, size});
    if (!fresh) {  // reused address we missed the free of: drop the stale record
        Site& old = g_sites[it->second.site];
        old.live_bytes -= it->second.size;
        --old.live_count;
        g_live -= it->second.size;
        it->second = Block{site, size};
    }
    Site& s = g_sites[site];
    s.live_bytes += size;
    ++s.live_count;
    ++s.allocs;
    g_live += size;
    g_peak = std::max(g_peak, g_live);
}

BB_COOP_SYSV u64 Alloc50(u64 heap, u64 size, u64 a2, u64 a3, u64 a4, u64 a5) {
    const u64 p = g_alloc50(heap, size, a2, a3, a4, a5);
    Record(p, size, reinterpret_cast<u64>(__builtin_return_address(0)));
    return p;
}

BB_COOP_SYSV u64 Alloc58(u64 heap, u64 size, u64 align, u64 a3, u64 a4, u64 a5) {
    const u64 p = g_alloc58(heap, size, align, a3, a4, a5);
    Record(p, size, reinterpret_cast<u64>(__builtin_return_address(0)));
    return p;
}

BB_COOP_SYSV u64 Free70(u64 heap, u64 p, u64 a2, u64 a3, u64 a4, u64 a5) {
    if (p) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_blocks.find(p);
        if (it == g_blocks.end()) {
            ++g_unknown_frees;
        } else {
            Site& s = g_sites[it->second.site];
            s.live_bytes -= it->second.size;
            --s.live_count;
            g_live -= it->second.size;
            g_blocks.erase(it);
        }
    }
    return g_free70(heap, p, a2, a3, a4, a5);
}

void Install(u64 slot) {
    u64 heap = 0, vt = 0;
    if (!SafeGet(Guest(slot), &heap) || !heap || !SafeGet(heap, &vt) || !vt) return;  // not made yet
    if (!SafeRead(vt, g_vtable, sizeof g_vtable)) {
        std::printf("[coop] Heap watch: heap +0x%llx: its vtable is unreadable; off\n", static_cast<unsigned long long>(slot));
        g_state = -1;
        return;
    }
    std::memcpy(&g_alloc50, g_vtable + 0x50, 8);
    std::memcpy(&g_alloc58, g_vtable + 0x58, 8);
    std::memcpy(&g_free70, g_vtable + 0x70, 8);
    const AllocFn a50 = &Alloc50, a58 = &Alloc58;
    const FreeFn f70 = &Free70;
    std::memcpy(g_vtable + 0x50, &a50, 8);
    std::memcpy(g_vtable + 0x58, &a58, 8);
    std::memcpy(g_vtable + 0x70, &f70, 8);
    g_heap = heap;
    g_base = Guest(0);
    const u64 copy = reinterpret_cast<u64>(g_vtable);
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(heap)), &copy, 8);
    g_state = 2;
    g_last_log = Clock::now();
    std::printf("[coop] Heap watch: heap +0x%llx (object 0x%llx, vtable +0x%llx) watched: live bytes per call site every 20 s\n",
                static_cast<unsigned long long>(slot), static_cast<unsigned long long>(heap),
                static_cast<unsigned long long>(vt - g_base));
}

void Report() {
    std::vector<std::pair<u64, Site>> top;
    u64 live, peak, unknown, blocks;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        top.assign(g_sites.begin(), g_sites.end());
        live = g_live, peak = g_peak, unknown = g_unknown_frees, blocks = g_blocks.size();
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.live_bytes > b.second.live_bytes; });
    char line[1024];
    int n = std::snprintf(line, sizeof line, "Heap watch: live %llu bytes in %llu blocks (peak %llu), %llu frees of older blocks; top:",
                          static_cast<unsigned long long>(live), static_cast<unsigned long long>(blocks),
                          static_cast<unsigned long long>(peak), static_cast<unsigned long long>(unknown));
    for (std::size_t i = 0; i < top.size() && i < 8 && n > 0 && n < static_cast<int>(sizeof line) - 80; ++i) {
        n += std::snprintf(line + n, sizeof line - n, " +0x%llx %llu B/%llu (%llu made);",
                           static_cast<unsigned long long>(top[i].first),
                           static_cast<unsigned long long>(top[i].second.live_bytes),
                           static_cast<unsigned long long>(top[i].second.live_count),
                           static_cast<unsigned long long>(top[i].second.allocs));
    }
    std::printf("[coop] %s\n", line);
}

}  // namespace

void HeapWatchTick() {
    if (g_state < 0) return;
    static u64 slot = 0x5540438;
    if (g_state == 0) {
        const char* on = std::getenv("BB_HEAP_WATCH_LOG");
        if (!on || on[0] != '1') {
            g_state = -1;
            return;
        }
        if (const char* s = std::getenv("BB_HEAP_WATCH_SLOT"); s && *s) slot = std::strtoull(s, nullptr, 0);
        g_state = 1;
    }
    if (g_state == 1) {
        Install(slot);
        return;
    }
    const auto now = Clock::now();
    if (now - g_last_log >= std::chrono::seconds(20)) {
        g_last_log = now;
        Report();
    }
}

}  // namespace coop
