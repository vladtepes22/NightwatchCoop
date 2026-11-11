// SPDX-License-Identifier: GPL-3.0-or-later
// Map-parts budget on the loading screen (BB_LOAD_PARTS_BUDGET, on by default): the MapIns block update
// (0x15934D0) walks a block's map parts with a budget of 20 parts per call; a lamp warp whose masks cover
// thousands of parts (Cathedral Ward: ~2900 of m24_00's 4048) spends ~300 loading-screen frames in that crawl
// while the loaders idle (research/cwprof081.md). While the loading screen is up the budget is raised; in the
// world it is the game's 20 (streaming on block crossings unchanged).
#pragma once

namespace BbPartsBudget {

/// bbgpu_patch_image, after coop::HooksInit: reads BB_LOAD_PARTS_BUDGET (unset: the default; 0 / off: nothing;
/// N: that budget) and checks the code bytes; all or nothing, one log line.
void Install();

} // namespace BbPartsBudget
