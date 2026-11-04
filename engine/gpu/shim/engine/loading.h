// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::Loading {
void Install(std::uint8_t* image);
void Tick(std::uint64_t pacer);
bool ScreenUp();
/// Fast loading holds this loading screen at 30 FPS (BB_FAST_LOADING=1, no cutscene): the Live FPS
/// clock (live_clock.cpp), which sets the pacer's frame time every frame, keeps that pace too.
bool FastPace();
}
