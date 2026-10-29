// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace VideoCore::GcPolicy {

/// bbport: the texture collector's rules on a discrete GPU (TextureCache::GarbageCollectImages).
///
/// Until 0.7.2 the marks were percentages of the driver's budget (pressure 80 %, critical 88 %):
/// on an 8 GB card with ~5.8 GB of budget left, textures went from 4.7 GB on, 1.2 GB short of the
/// budget, and VRAM copies of the game's memory (enlarged graphics heaps) kept the usage there.
/// Past the critical mark the collector freed images used a fraction of a second ago (160
/// submissions) on every submission, wrote GPU-written ones back with a GPU wait each, and
/// compared the usage before the freed images were destroyed (they go a frame later) and with the
/// holes they leave in shared memory blocks: hundreds of textures went at once on entering a big
/// room (Research Hall), were uploaded again in the next frames, and the frame stalled.
///
/// Now the marks sit a fixed reserve under the budget, the usage counts only memory a new image
/// cannot reuse, images go in least-recently-used order a few per frame, ahead of the hard mark,
/// none used in the last frames, and GPU-written images are written back only past the budget
/// itself (one a frame).

using u64 = std::uint64_t;
using u32 = std::uint32_t;
inline constexpr u64 MiB = u64(1) << 20;

struct Marks {
    u64 trigger = 0; ///< textures unused for a long time (an area left behind) go
    u64 soft = 0;    ///< old textures go gradually, ahead of the hard mark
    u64 hard = 0;    ///< textures unused for a couple of seconds go, more per frame
    u64 budget = 0;  ///< the driver's budget: past it, all but the last frames' images may go
};

/// Marks on all of our device-local memory (images, VRAM copies of the game's memory, buffers,
/// the swapchain and upscaler targets, all inside the usage already): the hard mark leaves room
/// for one burst of new allocations (a room's textures arriving before the collector caught up),
/// the soft band below it is where old textures go a few per frame.
constexpr Marks MarksFor(u64 budget) {
    Marks m{};
    m.budget = budget;
    const u64 headroom = std::clamp<u64>(budget / 20, 160 * MiB, 384 * MiB);
    const u64 band = std::clamp<u64>(budget / 10, 256 * MiB, 768 * MiB);
    m.hard = budget > 2 * headroom ? budget - headroom : budget / 2;
    m.soft = m.hard > 2 * band ? m.hard - band : m.hard / 2;
    m.trigger = std::min<u64>(budget / 100 * 70, m.soft);
    return m;
}

/// The budget the marks are taken from: the driver's (VK_EXT_memory_budget, what this process may
/// use now with the other programs' share taken off), never more than the VRAM heaps themselves.
constexpr u64 BudgetFor(u64 driver_budget, u64 local_heaps) {
    if (driver_budget == 0) {
        return local_heaps;
    }
    return local_heaps != 0 ? std::min(driver_budget, local_heaps) : driver_budget;
}

/// Memory a new image cannot take: the usage less the free space in shared memory blocks (an
/// evicted small texture's hole takes the next one) and images freed but not destroyed yet (the
/// GPU may still read them for a frame).
constexpr u64 EffectiveUsage(u64 usage, u64 block_slack, u64 pending_free) {
    const u64 reusable = block_slack + pending_free;
    return usage > reusable ? usage - reusable : 0;
}

/// The usage the game's data is measured by (BufferCache::UpdateVramLevel): the collector's own
/// (EffectiveUsage), less the free slots of the game's VRAM chunks, which its next copies take
/// without new VRAM. It is never above the collector's, so the game's data leaves VRAM only while
/// the collector is past its hard mark too. On the raw usage before, a GTX 1650 stayed at level 2
/// for good (VMA block slack and 624 MiB of free slots counted) while the collector, under its
/// hard mark by its own measure, freed nothing: 2-3 GB a frame read over PCIe, 1-3 FPS.
constexpr u64 GuestUsage(u64 usage, u64 block_slack, u64 pending_free, u64 residency_unused) {
    const u64 effective = EffectiveUsage(usage, block_slack, pending_free);
    return effective > residency_unused ? effective - residency_unused : 0;
}

/// The guest memory's VRAM level (BufferCache::UpdateVramLevel) on the same marks: 1 (copies of
/// the game's memory unused for a while go back in place) only once evicting textures from the
/// soft mark on did not keep the usage under the hard mark, 2 (no new copies) halfway from there
/// to the budget.
/// Before, it was 85 / 90 % of the driver's budget while the collector's marks sat higher (on 4 GB
/// cards at the VRAM heap, over the budget): the game's data went in place first, textures
/// stayed, and the GPU read GBs a frame over PCIe (1-3 FPS on a GTX 1650).
constexpr int GuestLevelFor(u64 used, const Marks& m) {
    if (m.budget == 0) {
        return 0;
    }
    return used >= m.hard + (m.budget - m.hard) / 2 ? 2 : used >= m.hard ? 1 : 0;
}

/// The guest level on the driver's own count (BufferCache::UpdateVramLevel): past the budget on
/// the raw usage the game's data is at least tight (1), and stays so until the raw usage is back
/// halfway between the hard mark and the budget. GuestUsage takes off VMA block slack and the free
/// slots of the game's chunks, but the driver counts both: a GTX 1650 (0.2.5 beta 3) sat at 3282
/// of 3151 MiB for good, the game's data at level 0 (2826 MiB by GuestUsage), its chunks growing
/// from 896 to 1024 MiB and none leaving, while the texture collector's evictions left holes in
/// VMA blocks that never shrank (1893 MiB). Level 1 lets the sparsest chunk empty and go back.
constexpr int GuestLevelOnDriver(int level, int was, u64 raw, const Marks& m) {
    if (m.budget == 0) {
        return level;
    }
    const u64 release = m.hard + (m.budget - m.hard) / 2;
    const bool tight = raw >= m.budget || (was >= 1 && raw >= release);
    return tight ? std::max(level, 1) : level;
}

enum class Level : u32 { None = 0, Idle = 1, Soft = 2, Hard = 3, Over = 4 };

constexpr Level LevelFor(u64 used, const Marks& m) {
    if (m.budget == 0 || used < m.trigger) {
        return Level::None;
    }
    if (used >= m.budget) {
        return Level::Over;
    }
    if (used >= m.hard) {
        return Level::Hard;
    }
    return used >= m.soft ? Level::Soft : Level::Idle;
}

struct Rule {
    u32 idle_seconds;     ///< unused for at least this long (wall clock)
    u32 protect_frames;   ///< and not used in this many last frames
    u32 images_per_frame; ///< evictions per presented frame, all submissions together
    u64 bytes_per_frame;  ///< and their memory
    bool write_back;      ///< GPU-written linear images may be written back (a GPU wait each)
    bool drop_old_tiled;  ///< GPU-written tiled images unused for a minute go unwritten
};

/// idle / pressure seconds: BB_GC_IDLE_SECONDS (20) and BB_GC_PRESSURE_IDLE_SECONDS (5).
constexpr Rule RuleFor(Level level, u32 idle_seconds = 20, u32 pressure_seconds = 5) {
    switch (level) {
    case Level::Over:
        return {0, 8, 96, 384 * MiB, true, true};
    case Level::Hard:
        return {std::min<u32>(2, pressure_seconds), 120, 48, 192 * MiB, false, false};
    case Level::Soft:
        return {pressure_seconds, 300, 24, 96 * MiB, false, false};
    case Level::Idle:
        return {idle_seconds, 600, 8, 32 * MiB, false, false};
    default:
        return {idle_seconds, 600, 0, 0, false, false};
    }
}

/// What one presented frame may still evict (the collector runs at every guest submission, tens a
/// frame: the quota spreads a burst over frames instead of freeing it within one).
struct FrameQuota {
    u32 frame = ~u32{0};
    u64 start_tick = 0;
    u32 images = 0;
    u64 bytes = 0;
    u32 write_backs = 0;

    /// A new frame starts a new quota, and so do 64 submissions without one (loading screens).
    void Begin(u32 now_frame, u64 tick) {
        if (now_frame != frame || tick - start_tick >= 64) {
            frame = now_frame;
            start_tick = tick;
            images = 0;
            bytes = 0;
            write_backs = 0;
        }
    }
    bool Left(const Rule& rule) const {
        return images < rule.images_per_frame && bytes < rule.bytes_per_frame;
    }
    void Take(u64 image_bytes) {
        ++images;
        bytes += image_bytes;
    }
};

/// The gc tick at the start of each of the last frames, for "used in the last N frames".
template <std::size_t N = 1024>
struct FrameTicks {
    std::array<u64, N> ticks{};
    u32 last_frame = 0;
    bool any = false;

    void Note(u32 frame, u64 tick) {
        if (any && frame == last_frame) {
            return;
        }
        // Frames the collector did not see (no submission in them) start at this tick too.
        u32 from = any ? last_frame + 1 : frame;
        if (frame - from >= N) {
            from = frame - u32(N - 1);
        }
        for (u32 f = from; f != frame + 1; ++f) {
            ticks[f % N] = tick;
        }
        last_frame = frame;
        any = true;
    }
    /// The tick at the start of the frame `frames` ago: an image last used before it was not used
    /// in the last `frames` frames. 0 (nothing that old) while fewer frames were seen.
    u64 TickFramesAgo(u32 frames) const {
        if (!any || frames >= N || frames > last_frame) {
            return 0;
        }
        return ticks[(last_frame - frames) % N];
    }
};

} // namespace VideoCore::GcPolicy
