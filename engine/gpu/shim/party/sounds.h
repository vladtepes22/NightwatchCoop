// SPDX-License-Identifier: GPL-3.0-or-later
// sounds (Nightwatch 0.3.0 feature, cosmetic): a mod plays its own WAV when the game asks for a sound event
// ("s000252531") at a world position, mixed into the game's main audio port; "replace" also drops the game's own
// sound. docs/modding/sounds.md.
#pragma once

namespace coop::sounds {

/// bbgpu_patch_image, every mode, before MarkImageHooksDone. Off with BB_SOUNDS=0. Logs
/// "feature sounds: installed (...)" or "feature sounds: not installed (why)".
void Install();

}  // namespace coop::sounds
