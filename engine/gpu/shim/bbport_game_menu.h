// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's settings as pages of the game's own System menu ("Display", "Game effects",
// "Game patches" after "Screen/Sound"), built and drawn by the game's menu code: rows are the
// game's list, toggle and slider rows, texts come from a hook on its message lookup, the values live
// in the port and are applied as the player changes them. BB_GAME_MENU=0 leaves the menu as it is.
#pragma once

#include <cstdint>

namespace BbGameMenu {
/// Installs the hooks (the loader calls it once, before any game code runs, while the image is
/// still writable). Each hooked function is checked byte for byte against the 1.09 eboot.
void PatchImage(unsigned char* image, std::uint64_t size);

/// Screen changes, for the test aids' event-driven menus (test_newgame.cpp): `seq` counts the
/// message lookups of ids the game had not looked up in the last second (a new screen or dialog
/// building its texts); `last_ms` is that last lookup's steady-clock time in ms. Both 0 while the
/// hooks are off.
void TextActivity(std::uint64_t* seq, std::int64_t* last_ms);
/// The last steady-clock ms a message text containing `needle` (ASCII, case-insensitive) was looked
/// up (a screen looks its texts up while it is shown); 0 when not among the recent lookups.
std::int64_t TextSeenMs(const char* needle);
/// Every recent lookup's text looked up after `since_ms` (test aids' text logs).
void RecentTexts(std::int64_t since_ms, void (*fn)(const char16_t* text, std::int64_t ms, void* user), void* user);

/// The steady-clock ms of the last lookup of message `id` (any category), 0 when never; shown
/// texts are looked up every frame, so "within the last ~300 ms" = on screen (test aids).
std::int64_t LastLookupMs(std::uint32_t id);
/// The game's own message `id` of FMG `category` (1 talk lines, 10 goods names, 11 weapon names,
/// 12 armor names) in the game's language, through its message lookup (with the repository and
/// table of its last own lookup); null before the game looked anything up or when there is none.
const char16_t* GameText(std::uint32_t category, std::uint32_t id);
/// Message ids looked up within the last `within_ms` (up to `max` of them into `out`); the count.
int RecentLookups(std::int64_t within_ms, std::uint32_t* out, int max);

/// A text override for the game's own messages (party co-op: the altar's "Enter the party's
/// dungeon"): called for each lookup with the original text; non-null replaces it. Any thread.
using TextOverrideFn = const char16_t* (*)(std::uint32_t category, std::uint32_t id, const char16_t* original);
void SetTextOverride(TextOverrideFn fn);
/// The text an ImeDialog of `max_length` characters starts confirmed with (party co-op: the party
/// dungeon's glyph); null for none. SetImePreset registers the provider.
using ImePresetFn = const char* (*)(std::uint32_t max_length);
void SetImePreset(ImePresetFn fn);
const char* ImePreset(std::uint32_t max_length);

/// Applies what the player changed on the port's pages (each presented frame).
void Poll();
} // namespace BbGameMenu
