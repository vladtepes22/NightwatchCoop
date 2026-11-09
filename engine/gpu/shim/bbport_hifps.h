// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: high frame rate fixes ported from droogie/bbhost (GPL-3.0-or-later): the lights' blink, the water wakes
// and the boxes' OK (bbport_hifps.cpp). The plants' wind is in bbport_frame_rate.cpp.
#pragma once

#include <cstddef>

namespace BbHiFps {
/// Installs the hooks (bbgpu_patch_image, after coop::HooksInit and the Live clock (BbEngine::Install); byte-verified).
void Install();
/// The counters, one line (BB_HIFPS_LOG=1 prints it every 5 s).
void Report(char* out, std::size_t size);
}  // namespace BbHiFps
