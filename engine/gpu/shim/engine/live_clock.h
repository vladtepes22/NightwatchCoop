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
/// True once after the live clock asked for a swapchain re-create (visy's issue #35): the presenter does it next.
bool TakeSwapchainRefresh();
std::uint64_t Steps();
double GameSeconds();
float LastDt();
/// Whether the frame in progress closes a whole 1/30 s of game time (Step30Gate; true every frame while the clock is
/// off): the water wakes and the lights' blink step only on these (bbport_hifps.cpp). Exact: closed only when whole.
bool WholeStep30();
bool WholeStep30Exact();
}
