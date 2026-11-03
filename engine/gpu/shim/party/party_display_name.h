// SPDX-License-Identifier: GPL-3.0-or-later
// How another player's name is SHOWN (party list, name tags, notices, subtitles). A hateful name is replaced by a
// neutral one ("Hunter 3F2") when the setting party_hide_bad_names is on (default); the real name stays the member's
// identity everywhere else (kick, matching, logs). Header-only so pure code (party_status.cpp, tests) can call it
// without linking the game: the game registers the real function (party_names_game.cpp); without one every name is
// shown as it is.
#pragma once
#include <atomic>
#include <string>

namespace bbname {

using DisplayFn = std::string (*)(const std::string& real);
using ScrubFn = std::string (*)(const std::string& text);
inline std::atomic<DisplayFn> g_display{nullptr};
inline std::atomic<ScrubFn> g_scrub{nullptr};

inline void Register(DisplayFn display, ScrubFn scrub) {
    g_display.store(display);
    g_scrub.store(scrub);
}
// The name to show for `real`.
inline std::string Display(const std::string& real) {
    const DisplayFn f = g_display.load();
    return f && !real.empty() ? f(real) : real;
}
// `text` with every hateful name seen so far replaced by its display name.
inline std::string Scrub(const std::string& text) {
    const ScrubFn f = g_scrub.load();
    return f && !text.empty() ? f(text) : text;
}

// Installs the game's filter (party_names_game.cpp: the shared matcher behind party_hide_bad_names). Called once when a
// party starts; an explicit call because the file would otherwise be dropped from the static library at link time.
void InstallGameFilter();

}  // namespace bbname
