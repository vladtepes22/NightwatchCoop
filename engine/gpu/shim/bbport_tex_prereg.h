// SPDX-License-Identifier: GPL-3.0-or-later
// Texture pre-registration (hitch082, on by default, BB_TEX_PREREGISTER=0 off).
//
// Our texture cache learns a texture at its first draw: page protection, a Vulkan image, an upload
// of its bytes into the VRAM arena and a GPU copy into the image. When a new part of the map comes
// into view the game draws hundreds of textures for the first time within one or two frames
// (the sickroom doorway: 650 textures, 200 MB): one 45-110 ms frame for the registrations and one
// more waiting for the GPU to finish the uploads, at the same place on every visit
// (research/hitch082.md).
//
// The game has those textures long before it draws them. Its GX layer makes a shader resource view
// per texture when the texture is created (0x21669a0: an object of 0x50 bytes, vtable 0x535f460,
// the texture resource at +8, the 32-byte Gnm texture descriptor (T#) at +0x10), and builds the
// descriptor in 0x217a5d0 (rdi = 64 bytes out, the T# first), called from one place, 0x2166a98.
// That call is hooked: every block-compressed texture's T# goes into a queue, and the renderer
// registers queued textures ahead of their first draw (Rasterizer::PreregisterTextures): all that
// are due while the loading screen is up, a few MB a frame in the world.
#pragma once

#include <cstddef>
#include <cstdint>

namespace BbTexPrereg {

struct Entry {
    std::uint64_t sharp[4]; ///< the T# as the game built it
    std::uint32_t frame;    ///< BbStats::frame_number when it was built
};

/// bbgpu_patch_image, after coop::HooksInit: reads BB_TEX_PREREGISTER and hooks the call (byte-checked).
void Install();

/// The hook is in: the renderer's pass has work to look for.
bool Enabled();

/// Renderer: up to `max` queued descriptors built at least `quiet_frames` frames before `now_frame`
/// (the game may still be filling a texture it just made), oldest first. Returns how many.
std::size_t Take(Entry* out, std::size_t max, std::uint32_t now_frame, std::uint32_t quiet_frames);

/// Descriptors in the queue (due or not).
std::size_t Queued();

/// The game asked for its loading screen (NowLoading requested, 0x556286b).
bool LoadingScreenUp();

/// Renderer: totals for the log line (every 600 frames, with BB_FRAME_STATS).
void Count(std::uint64_t registered, std::uint64_t registered_bytes, std::uint64_t already, std::uint64_t rejected);
void Report();

} // namespace BbTexPrereg
