// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch: the Contract screen's name (character creation, "Enter Name") typed in place: the name shows live in its
// own row (the value column's grey, a gold cursor), typed on the keyboard or, with a controller, on a keyboard drawn
// over the character preview (the generated chrmaketop.gfx: tools/engine/menu_assets.cpp NameKeyboardMovie), with
// the typing's button prompts and the menu's sounds. The game's name box (an ImeDialog) still takes the name: only
// its text window is replaced, by a quiet text box. Off (the text window as before) when a game function is not
// 1.09's.
#pragma once

#include "bbport_osk.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace BbNameEntry {

/// Installs the hooks (bbgpu_patch_image, after coop::HooksInit) and mounts the generated movie.
void Install();

/// The game thread, an ImeDialog of `max_length` characters opening (before its text box): noted for Begin.
void NoteImeLength(std::uint32_t max_length);
/// The game thread, the ImeDialog's text box about to open: whether it is the Contract screen's name box (the screen
/// is up). Then `initial` is the character's name as it is (empty: none yet) and `limit` the most characters, and
/// the name entry has started (its sounds, the controller's keyboard when a controller opened it).
bool Begin(std::string* initial, std::size_t* limit);

/// The window thread: the quiet text box open is the name's.
bool Typing();
/// The window thread, the quiet text box's controller button: the name keyboard (shown from the first press) moves
/// its key, switches its view, Shift, or presses its key. Returns the character typed (> 0), 0 or a BbOsk::kTyped*.
int KeyboardPress(BbOsk::PadKey key);
/// The window thread, a key typed on the keyboard: the name keyboard hides.
void KeyboardKeyTyped();

}  // namespace BbNameEntry
