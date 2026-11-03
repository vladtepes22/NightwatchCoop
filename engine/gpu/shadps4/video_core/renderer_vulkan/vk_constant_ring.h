// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: small read-only guest buffers (constants) copied by the GPU command thread (stage A of
// the draw pipeline, vk_draw_pipe.h) into this ring, so the draw recording thread only binds
// them. Positions are monotonic byte counts; a region is reused once the submission that
// recorded its last draw has completed on the GPU. Stage B stamps each packet it records with
// the submission tick (Stamp), stage A retires stamps whose tick the GPU has passed.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <map>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class ConstantRing {
public:
    static constexpr u64 Capacity = 32ull << 20;

    ConstantRing(const Instance& instance, Scheduler& scheduler_)
        : scheduler{scheduler_},
          buffer{instance, 0, Capacity, VideoCore::MemoryType::Stream, "lantern constant ring"} {
        ASSERT(!buffer.mapped_data.empty());
    }

    [[nodiscard]] vk::Buffer Handle() const noexcept {
        return buffer.Handle();
    }

    /// Stage A: `size` bytes at `alignment`; returns the ring offset, or nullopt when the ring is
    /// still in use by the GPU (the caller leaves the binding to stage B).
    std::optional<u64> Allocate(u64 size, u64 alignment) {
        u64 at = (position + alignment - 1) & ~(alignment - 1);
        if (at % Capacity + size > Capacity) {
            at += Capacity - at % Capacity; // wrap to the ring start
        }
        const auto in_use = [&] { return at + size > Capacity && at + size - Capacity > retired; };
        if (in_use()) {
            Retire();
            if (in_use()) {
                ++stats.full;
                return std::nullopt;
            }
        }
        // Bytes that were handed out again by Reuse stay until the GPU is done with that use.
        while (!holds.empty() && (holds.begin()->first << 16) + Capacity < at + size) {
            if (retired < holds.begin()->second) {
                Retire();
                if (retired < holds.begin()->second) {
                    ++stats.full;
                    ++stats.full_by_hold;
                    return std::nullopt;
                }
            }
            holds.erase(holds.begin());
        }
        position = at + size;
        last_at = at;
        ++stats.allocations;
        stats.bytes += size;
        return at % Capacity;
    }

    /// The absolute position (not modulo Capacity) of the last successful Allocate.
    [[nodiscard]] u64 LastAllocated() const noexcept {
        return last_at;
    }

    /// Stage A, BB_RING_DEDUP: the bytes of an earlier allocation (`at`, from LastAllocated) are
    /// wanted again unchanged. Returns their ring offset when they are still intact in the ring
    /// (recent, and held against being overwritten until this use has completed on the GPU).
    /// Mrsuss60 dcdb834f.
    std::optional<u64> Reuse(u64 at, u64 size) {
        if (at + size > position || at + Capacity / 4 < position) {
            return std::nullopt;
        }
        u64& until = holds[at >> 16];
        until = std::max(until, position + 1);
        ++stats.reused;
        stats.bytes_reused += size;
        return at % Capacity;
    }

    /// Stage A counters (BB_FRAME_STATS), read and cleared by the same thread.
    struct Stats {
        u64 allocations = 0, bytes = 0, full = 0, full_by_hold = 0, reused = 0, bytes_reused = 0;
    };
    Stats TakeStats() noexcept {
        const Stats out = stats;
        stats = {};
        return out;
    }

    [[nodiscard]] u8* Data(u64 offset) noexcept {
        return buffer.mapped_data.data() + offset;
    }

    /// Stage A, after writing: makes the bytes visible to the GPU.
    void Flush(u64 offset, u64 size) {
        buffer.Flush(offset, size);
    }

    /// Stage A: the position the next packet's allocations end at.
    [[nodiscard]] u64 Position() const noexcept {
        return position;
    }

    /// Stage B, after recording a packet whose allocations end at `end`. Stamps of one
    /// submission tick are merged: only the last end per tick is published.
    void Stamp(u64 end) {
        if (end == pending_end) {
            return;
        }
        const u64 tick = scheduler.CurrentTick();
        if (tick != pending_tick && pending_end != last_published) {
            Publish(pending_end, pending_tick);
        }
        pending_end = end;
        pending_tick = tick;
    }

private:
    void Publish(u64 end, u64 tick) {
        const u64 at = stamps_head.load(std::memory_order_relaxed);
        if (at - stamps_tail.load(std::memory_order_acquire) >= stamps.size()) {
            return; // full: this region retires with a later stamp
        }
        stamps[at % stamps.size()] = {end, tick};
        stamps_head.store(at + 1, std::memory_order_release);
        last_published = end;
    }

    void Retire() {
        auto* semaphore = scheduler.GetWorkSemaphore();
        semaphore->Refresh();
        u64 at = stamps_tail.load(std::memory_order_relaxed);
        const u64 head = stamps_head.load(std::memory_order_acquire);
        while (at < head && semaphore->IsFree(stamps[at % stamps.size()].tick)) {
            retired = stamps[at % stamps.size()].end;
            ++at;
        }
        stamps_tail.store(at, std::memory_order_release);
    }

    struct StampEntry {
        u64 end;
        u64 tick;
    };
    Scheduler& scheduler;
    VideoCore::Buffer buffer;
    u64 position = 0;     ///< stage A
    u64 retired = 0;      ///< stage A: the GPU is done with everything before this
    u64 last_at = 0;      ///< stage A
    std::map<u64, u64> holds; ///< stage A: 64 KiB granule of reused bytes -> retired position needed
    Stats stats;          ///< stage A
    u64 pending_end = 0, pending_tick = 0, last_published = 0; ///< stage B
    std::array<StampEntry, 1024> stamps{};
    alignas(64) std::atomic<u64> stamps_head{0}; ///< stage B writes
    alignas(64) std::atomic<u64> stamps_tail{0}; ///< stage A reads
};

} // namespace Vulkan
