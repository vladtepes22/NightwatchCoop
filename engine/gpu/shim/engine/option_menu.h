// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::Options {
bool Install(std::uint8_t* image, std::size_t size);
/// Nightwatch: the prepared option movie is in (its Environment screen has the port's Auto-Hide HUD slot).
bool Installed();
void Tick();
bool ClickDecides(std::uint8_t* list, int index);
void ListUpdate(std::uint8_t* list, bool focused);
}
