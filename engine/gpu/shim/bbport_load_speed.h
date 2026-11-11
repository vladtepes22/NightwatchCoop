// SPDX-License-Identifier: GPL-3.0-or-later
// Load speed (0.8.1, research/reload081.md section 5): shorter loading screens by engine changes, each with a kill switch.
//   BB_LOAD_WORKERS=N   the resource post-processor threads (the game starts 3). OFF by default: more than 3 loses a
//                       load now and then (research/fastload081.md). N = 4..8; 0 / unset = the game's own count.
//   BB_FAST_INFLATE=0   the engine's zlib 1.2.3 inflate is the game's own (default: zlib-ng, bbport_fast_inflate.h).
#pragma once

namespace BbLoadSpeed {
/// bbgpu_patch_image, after coop::HooksInit: reads the switches and installs the hooks.
void Install();
} // namespace BbLoadSpeed
