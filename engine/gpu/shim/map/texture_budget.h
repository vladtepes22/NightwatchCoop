// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
namespace Atlas2 {
inline constexpr size_t CacheSlots = 64;
// Includes resident AND retiring tile slots. Reserve covers art/font atlases,
// including their old in-flight versions; descriptors do not allocate images.
inline constexpr unsigned RendererTextureReserve = 32;
inline constexpr unsigned RendererImageDescriptors = CacheSlots + RendererTextureReserve;
inline constexpr unsigned LineSegmentLimit = 16384;
}
