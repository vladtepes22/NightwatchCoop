// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::Loading {
// The game's image (loader startup, after the frame hook is in place).
void Install(std::uint8_t* image);
// Each frame on the game's main thread, at the start of its frame pacer (SprjFlipper::Update),
// `flipper` being the pacer object.
void Tick(std::uint64_t flipper);
// A loading screen is up (the presenter may lift its frame limit then).
bool ScreenUp();
} // namespace BbEngine::Loading
