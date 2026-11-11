// SPDX-License-Identifier: GPL-3.0-or-later
// Load speed (0.8.1, research/reload081.md section 5): shorter loading screens by engine changes, each with a kill switch.
//   BB_LOAD_WORKERS=N   the resource post-processor threads (the game starts 3). Default (0.8.1): min(4, max(3, usable
//                       CPUs - 2)), bbport.ini load_workers=0 or BB_LOAD_WORKERS=0 = the game's own count; N = 4..8 wins.
//   BB_FAST_INFLATE=0   the engine's zlib 1.2.3 inflate is the game's own (default: zlib-ng, bbport_fast_inflate.h).
#pragma once

namespace BbLoadSpeed {
/// bbgpu_patch_image, after coop::HooksInit: reads the switches and installs the hooks.
void Install();
} // namespace BbLoadSpeed
