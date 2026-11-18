// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch: the in-game Settings screen (replaces the pause menu's System list): the item-list book of the glyph
// browser (bbport_glyph_screen.cpp opens and drives it), with its own title, tabs and rows.
#pragma once

#include <cstdint>
#include <span>

namespace BbSettingsScreen {

/// Work in progress: the pause menu's System opens the Settings screen only with BB_SETTINGS_SCREEN=1.
bool Enabled();

struct Tab {
    std::uint32_t id;      ///< the tab descriptor's id (the book's own four first, then tabs added after them)
    std::uint32_t frame;     ///< the inventory.gfx tab-tile frame its icon is (the generated movie's for most)
    std::uint32_t fallback;  ///< the frame without the generated movie (a game tab picture)
    std::uint32_t text;    ///< its name's menu text id (category 0xc8)
    const char16_t* name;  ///< English
};
/// The tabs, left to right: Display, Sound, Enhancements, Controls, Party, System.
std::span<const Tab> Tabs();
/// The tabs the book's own maker does not make (added after its four).
inline constexpr std::size_t kBookTabs = 4;

/// Its text for the book's text (`category`, `id`) while the Settings screen is up, else nullptr.
const char16_t* Text(std::uint32_t category, std::uint32_t id);

} // namespace BbSettingsScreen
