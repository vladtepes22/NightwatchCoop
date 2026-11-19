// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::ChangeAppearance {
bool Watch(std::uint8_t* image);
void EnableEditor();
void Tick();
// Enhancements' Hunter's Dream Mirror, live: the mirror's character in the Dream is turned off while it is off.
void MirrorTick();
bool PreviewShown();
}
