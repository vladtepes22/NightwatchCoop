// SPDX-License-Identifier: GPL-3.0-or-later
// A render pass whose target has no surface is skipped instead of crashing. See render_guard.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::RenderGuard {
/// Loader start, after the executable is verified. Commits its own hook.
bool Install(std::uint8_t* image, std::size_t size);
} // namespace BbEngine::RenderGuard
