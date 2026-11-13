// SPDX-License-Identifier: GPL-3.0-or-later
// The community Chalice glyph list as a native item-list screen (the Ritual Altar's chalice picker): rows are the
// game's own chalice cards, one per glyph of the launcher's cache (party/party_chalice_glyphdb.h).
#pragma once

#include <cstdint>

namespace BbGlyphScreen {

/// Installs the hooks (bbgpu_patch_image, after coop::HooksInit). Test stage: BB_GLYPH_SCREEN_TEST=1 only.
void Install();

/// The glyph screen's own text for the game's text (`category`, `id`) while the chalice picker is the glyph screen
/// (its title, help line and tab names), else nullptr.
const char16_t* Text(std::uint32_t category, std::uint32_t id);

/// The game's text `id` stays as the game has it (no other override): "Search by Chalice Glyph" (262002, the altar item
/// and its help line) while the glyph screen is on.
bool KeepsGameText(std::uint32_t id);

/// The glyph box's preset: a glyph picked on the glyph screen (once, within a few seconds of the pick), else nullptr.
const char* PickedGlyph(std::uint32_t max_length);

} // namespace BbGlyphScreen
