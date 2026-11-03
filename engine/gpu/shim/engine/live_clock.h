// SPDX-License-Identifier: GPL-2.0-or-later
// Live FPS clock (visy 0.7.8, from Mrsuss60's Windows port): with the "60 FPS++ (Deltatime)" patch the
// game steps by each frame's measured time, at a target the game menu changes live (bbport.ini
// target_fps). Chosen by the start (BB_LIVE_FPS_ACTIVE=1, start/patches.cpp); live_clock.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::LiveClock {
/// Hooks the step block the Deltatime patch left; false (logged) leaves the patch's static 60 FPS.
bool Install(std::uint8_t* image, std::size_t size);
bool Active();
std::uint64_t Steps();
double GameSeconds();
float LastDt();
}
