// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace BbEngine::Widescreen {
bool Install(std::uint8_t* image,std::size_t size);
void Tick();
/// Screens narrower than 16:9 (16:10, 4:3, 5:4): tan(vertical FOV / 2) is multiplied by this so
/// the 16:9 horizontal view is kept and the extra height is added (Vert+); 1 otherwise.
float NarrowFovFactor();
}
