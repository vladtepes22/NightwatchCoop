// SPDX-License-Identifier: GPL-3.0-or-later
// animated_attachments (Nightwatch feature, cosmetic): while a player character has a SpEffect on, the engine loads a
// mod's part file (parts/<part>.partsbnd.dcx) into one of the character's two empty part slots (15, 16); the game
// builds and animates it like the helmet (its anibnd, when the part has one). Unloaded when the
// SpEffect ends. docs/modding/animated_attachments.md.
#pragma once

namespace coop::attachments {

/// bbgpu_patch_image, every mode, before MarkImageHooksDone. Off with BB_ANIMATED_ATTACHMENTS=0. Logs
/// "feature animated_attachments: installed (...)" or "... not installed (why)".
void Install();

}  // namespace coop::attachments
