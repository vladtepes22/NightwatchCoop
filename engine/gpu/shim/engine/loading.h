// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::Loading {
void Install(std::uint8_t* image);
void Tick(std::uint64_t pacer);
bool ScreenUp();
/// The game shows no 3D world this frame: a loading screen is up (also one still up over the arrived world), or no
/// world has been shown since the last load (the title, name entry) and no cutscene plays. Its scene target is then an
/// empty grey, or a world still hidden. False all through play (measured: a new game to the second area).
bool WorldHidden();
/// Fast loading holds this loading screen at 30 FPS (BB_FAST_LOADING=1, no cutscene): the Live FPS
/// clock (live_clock.cpp), which sets the pacer's frame time every frame, keeps that pace too.
bool FastPace();
}
