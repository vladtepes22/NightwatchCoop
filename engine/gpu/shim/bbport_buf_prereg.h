// SPDX-License-Identifier: GPL-3.0-or-later
// Buffer pre-upload (door082, on by default, BB_BUF_PREUPLOAD=0 off).
//
// With the copies model (the default: the game's GPU memory is copied into a VRAM arena) a vertex
// or index buffer is copied at its first draw: new arena memory, a copy of the whole buffer through
// staging, and write protection of its pages, all in that frame. When a new part of the map comes
// into view (a doorway, a corner) the game draws 60-120 map pieces for the first time in one or two
// frames: 45-200 MB of vertex buffers, 6-11 ms of page protection, 65-200 MB of new device memory
// (research/hitch082.md section 11). On cards up to 8 GB a copy unused for a minute leaves VRAM
// again, so the same place costs the same on a later visit.
//
// The game has those buffers long before it draws them. Its GX layer builds every buffer in
// 0x216d420 (out record, -, description {size, usage, bind flags, CPU access, misc flags, stride},
// initial data, object allocator, GPU memory allocator, -) -> 0 = ok, called from one place
// (0x2165e29, in the buffer constructor 0x2165c10): it takes the memory from the GPU allocator
// (out record +8) and copies the initial data in. The memory is given back in 0x216d690 (record),
// reached from the buffer's two destructors (a call at 0x216ce27, a tail jump at 0x216ce4e). The
// build call and both ways into the free are hooked: vertex and
// index buffers created with their data are queued, and the renderer copies queued buffers into
// the arena ahead of their first draw (Rasterizer::PreuploadBuffers): all that are due while the
// loading screen is up, a few MB a frame in the world. A live buffer's copy is kept in VRAM while
// VRAM is not short (BufferCache::EvictIdleCopies); once the game frees the buffer the copy goes
// idle like any other.
#pragma once

#include <cstddef>
#include <cstdint>

namespace BbBufPrereg {

struct Entry {
    std::uint64_t address; ///< the buffer's memory (guest address)
    std::uint64_t size;    ///< bytes
    std::uint32_t frame;   ///< BbStats::frame_number when it was created
};

/// bbgpu_patch_image, after coop::HooksInit: reads BB_BUF_PREUPLOAD and hooks the two calls (byte-checked).
void Install();

/// The hooks are in: the renderer's pass has work to look for.
bool Enabled();

/// Renderer: up to `max` queued buffers created at least `quiet_frames` frames before `now_frame`,
/// oldest first; buffers freed meanwhile are left out. Returns how many.
std::size_t Take(Entry* out, std::size_t max, std::uint32_t now_frame, std::uint32_t quiet_frames);

/// Renderer: buffers the game freed since the last call (address, size), up to `max`.
std::size_t TakeFreed(Entry* out, std::size_t max);

/// Buffers in the queue (due or not).
std::size_t Queued();

/// Renderer: totals for the log line (every 600 frames, with BB_FRAME_STATS).
void Count(std::uint64_t uploaded_buffers, std::uint64_t uploaded_bytes, std::uint64_t bound_bytes,
           std::uint64_t rejected);
void Report(std::uint64_t kept_bytes, bool paused);

} // namespace BbBufPrereg
