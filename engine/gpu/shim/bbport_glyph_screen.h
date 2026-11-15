// SPDX-License-Identifier: GPL-3.0-or-later
// The community Chalice glyph list as a native item-list screen (the Ritual Altar's chalice picker): rows are the
// game's own chalice cards, one per glyph of the launcher's cache (party/party_chalice_glyphdb.h).
#pragma once

#include <cstdint>
#include <string>

namespace BbGlyphScreen {

/// Installs the hooks (bbgpu_patch_image, after coop::HooksInit). Test stage: BB_GLYPH_SCREEN_TEST=1 only.
void Install();

/// The glyph screen's own text for the game's text (`category`, `id`) while the chalice picker is the glyph screen
/// (its title, help line and tab names), else nullptr.
const char16_t* Text(std::uint32_t category, std::uint32_t id);

/// The game's text `id` stays as the game has it (no other override): "Search by Chalice Glyph" (262002, the altar item
/// and its help line) while the glyph screen is on.
bool KeepsGameText(std::uint32_t id);

/// The window thread, a letter or digit key (SDL `scancode`) pressed while no text box is open: whether it starts the
/// glyph screen's search (the screen is up with nothing over it and the key is not Search's own, Square's). True:
/// the window opens the quiet text box with that letter and the game never sees the key.
bool TypeStartsSearch(int scancode);
/// The search bar's text as last typed (kept after Done; empty after a cancel or the screen opening): a search started
/// by a letter continues it.
std::string SearchText();
/// The window thread, the quiet text box's controller button: the glyph screen's keyboard (shown from the first
/// press) moves its key, switches its view (kToggle: the A-Z grid / QWERTY) or presses its key. Returns the letter
/// typed (> 0), 0 for nothing or one of kTyped*.
enum class PadKey { kNone, kUp, kDown, kLeft, kRight, kPress, kDelete, kDone, kCancel, kToggle };
constexpr int kTypedDelete = -1, kTypedDone = -2, kTypedCancel = -3;
int KeyboardPress(PadKey key);
/// The window thread, a key typed on the keyboard in the quiet text box: the glyph screen's keyboard hides.
void KeyboardKeyTyped();
/// The glyph box's preset: a glyph picked on the glyph screen (once, within a few seconds of the pick), else nullptr.
const char* PickedGlyph(std::uint32_t max_length);

} // namespace BbGlyphScreen
