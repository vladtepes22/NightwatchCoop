// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::FrameRate {
bool Install(std::uint8_t* image, std::size_t size);
void Tick();
float HeldRate();
// Diagnostics for the co-op lift log (visy's BbFrameRate counters): game frames ticked, their summed
// length (s) and the last one's; 0 while the rate is not followed.
std::uint64_t Steps();
double GameSeconds();
float LastDt();
}
