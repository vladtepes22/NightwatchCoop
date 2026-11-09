// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbCrashGuards {
/// bbgpu_patch_image, every mode: the Scaleform HUD movie lock, the text-layout guard and the FMOD sound-ring guard
/// (visy 0.8.0, from droogie/bbhost; bbport_crash_guards.cpp has the switches).
void PatchImage(unsigned char* image, std::uint64_t size);
} // namespace BbCrashGuards
