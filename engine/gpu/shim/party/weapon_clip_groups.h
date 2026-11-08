// SPDX-License-Identifier: GPL-3.0-or-later
// weapon_clip_groups (Nightwatch 0.3.0 feature, gameplay): a mod gives weapon W its own attack clip group (aGGG)
// while W keeps its wepmotionCategory, so the game code keyed on the category (Blades of Mercy's split trick on 27)
// still runs. docs/modding/weapon_clip_groups.md.
#pragma once

namespace coop::clipgroups {

/// bbgpu_patch_image, every mode, before MarkImageHooksDone. Off with BB_WEAPON_CLIP_GROUPS=0. Logs
/// "feature weapon_clip_groups: installed (...)" or "feature weapon_clip_groups: not installed (why)".
void Install();

}  // namespace coop::clipgroups
