// SPDX-License-Identifier: GPL-3.0-or-later
// Load trace (BB_LOAD_TRACE=1, off by default): one "Load trace:" line per step change of the game's
// map load (InGameStep steps 2 / 4 / 6 / 8 and every MoveMapStep step, research/remp081.md section 3)
// with what happened since the previous step: wall time, frames, file opens / reads / MB / time inside
// the host reads, shader compiles, image uploads, process CPU time and the busiest threads. Diagnostic
// prologue hooks only ("probe" names: outside the party's hook-set check).
#pragma once

namespace BbLoadTrace {

/// bbgpu_patch_image, after coop::HooksInit: reads BB_LOAD_TRACE and installs the hooks. Nothing when off.
void Install();

} // namespace BbLoadTrace
