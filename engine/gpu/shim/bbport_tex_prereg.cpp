// SPDX-License-Identifier: GPL-3.0-or-later
// Texture pre-registration: the game-side hook and the queue (see bbport_tex_prereg.h).
#include "bbport_tex_prereg.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>

#include "bbport_toggles.h"
#include "party/coop_hooks.h"
#include "party/game_state.h"

namespace BbTexPrereg {

namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;

// Our offsets, byte-checked by HookCallSite (the site must be `call rel32` to the builder).
constexpr u64 kBuildViewCall = 0x2166a98;   // GX shader resource view constructor 0x21669a0: call BuildDescriptor
constexpr u64 kBuildDescriptor = 0x217a5d0; // (out 64 bytes, allocator, texture resource, view description) -> 0 = ok
constexpr u64 kNowLoadingRequested = 0x556286b; // u8

// T# fields used here (AmdGpu::Image, video_core/amdgpu/resource.h): data format = bits 20..25 of the
// second dword, the resource type = the top 4 bits of the fourth dword.
constexpr u32 kFormatBc1 = 35, kFormatBc7 = 41;

std::atomic<bool> g_on{false};
std::mutex g_mutex;
std::deque<Entry> g_queue;
std::unordered_set<u64> g_seen; // descriptors queued since the last clear (a texture gets several views)
std::atomic<u64> g_noted{0}, g_dupes{0}, g_other_format{0}, g_dropped{0};
std::atomic<u64> g_registered{0}, g_registered_bytes{0}, g_already{0}, g_rejected{0};

u64 HashSharp(const u64* s) {
    u64 h = s[0] * 0x9e3779b97f4a7c15ull ^ s[1];
    h = (h ^ h >> 29) * 0xbf58476d1ce4e5b9ull ^ s[2];
    return (h ^ h >> 32) * 0x94d049bb133111ebull ^ s[3];
}

BB_COOP_SYSV u64 BuildDescriptorHook(u64 out, u64 allocator, u64 texture, u64 view_desc) {
    const u64 r = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64, u64)>(coop::Guest(kBuildDescriptor))(
        out, allocator, texture, view_desc);
    if (static_cast<int>(r) != 0 || !out) {
        return r;
    }
    Entry e{};
    std::memcpy(e.sharp, reinterpret_cast<const void*>(out), sizeof e.sharp);
    const u32 format = u32(e.sharp[0] >> 52) & 0x3f;
    const u32 type = u32(e.sharp[1] >> 60) & 0xf;
    // Block-compressed 2D / cube / array textures only: what the game loads from its texture files. Render
    // targets, depth buffers and GPU-written textures get views here too; their contents do not exist yet.
    if (format < kFormatBc1 || format > kFormatBc7 || (type & 0x8) == 0 || (e.sharp[0] & 0x3fffffffffull) == 0) {
        g_other_format.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    e.frame = BbStats::frame_number.load(std::memory_order_relaxed);
    std::scoped_lock lk{g_mutex};
    if (g_seen.size() > 200'000) {
        g_seen.clear();
    }
    if (!g_seen.insert(HashSharp(e.sharp)).second) {
        g_dupes.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    if (g_queue.size() >= 60'000) { // the renderer is not taking them (no frames): the oldest go
        g_queue.pop_front();
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_queue.push_back(e);
    g_noted.fetch_add(1, std::memory_order_relaxed);
    return r;
}

} // namespace

void Install() {
    const char* env = std::getenv("BB_TEX_PREREGISTER");
    if (env && (env[0] == '0' || !std::strcmp(env, "off"))) {
        std::printf("Texture pre-registration: off (BB_TEX_PREREGISTER=0)\n");
        return;
    }
    if (!coop::Image()) {
        return;
    }
    // "probe" in the name: a render-side hook, left out of the party's hook-set check (both games may differ).
    const bool ok = coop::HookCallSite(kBuildViewCall, kBuildDescriptor, reinterpret_cast<const void*>(&BuildDescriptorHook),
                                       "texture pre-registration probe: GX view descriptor");
    g_on.store(ok, std::memory_order_release);
    std::printf("Texture pre-registration: %s (BB_TEX_PREREGISTER=0 off)\n",
                ok ? "on: textures are registered when the game creates them, ahead of their first draw"
                   : "NOT applied: the view constructor's call is not the expected code");
}

bool Enabled() {
    return g_on.load(std::memory_order_acquire);
}

std::size_t Take(Entry* out, std::size_t max, u32 now_frame, u32 quiet_frames) {
    std::scoped_lock lk{g_mutex};
    std::size_t n = 0;
    while (n < max && !g_queue.empty() && now_frame - g_queue.front().frame >= quiet_frames) {
        out[n++] = g_queue.front();
        g_queue.pop_front();
    }
    return n;
}

std::size_t Queued() {
    std::scoped_lock lk{g_mutex};
    return g_queue.size();
}

bool LoadingScreenUp() {
    unsigned char requested = 0;
    return coop::Image() && coop::SafeGet(coop::Guest(kNowLoadingRequested), &requested) && requested != 0;
}

void Count(u64 registered, u64 registered_bytes, u64 already, u64 rejected) {
    g_registered.fetch_add(registered, std::memory_order_relaxed);
    g_registered_bytes.fetch_add(registered_bytes, std::memory_order_relaxed);
    g_already.fetch_add(already, std::memory_order_relaxed);
    g_rejected.fetch_add(rejected, std::memory_order_relaxed);
}

void Report() {
    std::size_t queued = 0;
    {
        std::scoped_lock lk{g_mutex};
        queued = g_queue.size();
    }
    std::printf("Texture pre-registration: %llu descriptors noted (%llu repeats, %llu of other formats, %llu dropped), "
                "%llu registered ahead (%llu MiB), %llu were registered already, %llu rejected, %zu queued\n",
                (unsigned long long)g_noted.load(), (unsigned long long)g_dupes.load(),
                (unsigned long long)g_other_format.load(), (unsigned long long)g_dropped.load(),
                (unsigned long long)g_registered.load(), (unsigned long long)(g_registered_bytes.load() >> 20),
                (unsigned long long)g_already.load(), (unsigned long long)g_rejected.load(), queued);
    // hitch082b: the protection changes so far and what the trap batches joined (BB_PROTECT_BATCH).
    std::printf("Image trap batches: %llu run(s) set as %llu; %llu protection change(s), %llu page(s) in all\n",
                (unsigned long long)BbStats::trap_batch_runs.load(), (unsigned long long)BbStats::trap_batch_joined.load(),
                (unsigned long long)BbStats::protect_calls.load(), (unsigned long long)BbStats::protect_pages.load());
}

} // namespace BbTexPrereg
