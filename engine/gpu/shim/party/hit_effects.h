// SPDX-License-Identifier: GPL-3.0-or-later
// hit_effects (Nightwatch 0.3.0 feature, visual): mods replace the engine's elemental hit overlay (Bolt 880 / 10880 /
// 10980, fire 800 / 806 families) while the ATTACKER has a SpEffect on. docs/modding/hit_effects.md.
#pragma once

namespace coop::hiteffects {

/// bbgpu_patch_image, every mode, before MarkImageHooksDone. Off with BB_HIT_EFFECTS=0. Logs
/// "feature hit_effects: installed (...)" or "feature hit_effects: not installed (why)".
void Install();

}  // namespace coop::hiteffects
