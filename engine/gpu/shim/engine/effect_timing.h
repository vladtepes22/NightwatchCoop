// SPDX-License-Identifier: GPL-3.0-or-later
// BB_EFFECT_TIMING=1: how much time the game's status effects are advanced by each second, per
// character and per caller (GitHub issue 15). See effect_timing.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::EffectTiming {
/// Loader start, after the executable is verified: nothing unless BB_EFFECT_TIMING=1.
bool Install(std::uint8_t* image, std::size_t size);
} // namespace BbEngine::EffectTiming
