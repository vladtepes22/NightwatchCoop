// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch: the in-game Settings screen's content (bbport_settings_screen.h).
#include "bbport_settings_screen.h"

#include <cstdlib>
#include <iterator>

namespace BbSettingsScreen {
namespace {

// The book's own tabs keep their ids (0x24..0x27, the chalice picker's) and their name texts (40435..40438); the two
// after them get ids past the glyph browser's Custom tab (0x28) and menu text ids of ours (0xc8, not the game's).
// Frames: the generated inventory.gfx's (tools/engine/menu_assets.cpp SettingsTabPictures: 63..66; the glyph
// screen's grey System icon 61) and the game's weapon hand (4); without that movie, game tab pictures stand in.
constexpr Tab kTabs[] = {
    {0x24, 63, 2, 40435, u"Display"},
    {0x25, 64, 8, 40436, u"Sound"},
    {0x26, 65, 0x3c, 40437, u"Enhancements"},
    {0x27, 4, 4, 40438, u"Controls"},
    {0x29, 66, 0x39, 263193, u"Party"},
    {0x2a, 61, 0x3a, 263194, u"System"},
};
static_assert(std::size(kTabs) >= kBookTabs);

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("BB_SETTINGS_SCREEN");
        return v && *v == '1';
    }();
    return on;
}

std::span<const Tab> Tabs() { return kTabs; }

const char16_t* Text(std::uint32_t category, std::uint32_t id) {
    if (category == 0xc8) {
        if (id == 263100) return u"Settings";  // the book's title
        for (const Tab& t : kTabs)
            if (t.text == id) return t.name;
        return nullptr;
    }
    if (category == 0xc9 && id == 263000) return u"Choose a tab.";  // the help line (step 1: no rows yet)
    return nullptr;
}

} // namespace BbSettingsScreen
