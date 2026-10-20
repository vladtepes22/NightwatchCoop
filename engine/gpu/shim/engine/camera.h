// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace BbEngine::Camera {
bool Install(std::uint8_t* image, std::size_t size);
void Tick();
/// The LockCamParam hook is installed (camera controls, or Vert+ on a screen narrower than 16:9).
bool Active();
}
