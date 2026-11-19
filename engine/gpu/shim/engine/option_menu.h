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
/// Nightwatch: the Settings screen (bbport_settings_screen.cpp) shows and changes the PC rows by their name's text
/// id (117001 Anti-Aliasing, 125004 Picture Tuning, ...): a row's value (Toggle 0 / 1, Slider 0 .. 10, Pick an
/// index) and its default; false when `id` is not a PC row (or the settings cannot be bound).
bool PcValue(std::uint32_t id, std::int32_t* value, std::int32_t* def);
/// Changes it as the PC pages do (a picture row turns Picture Tuning on) and saves bbport.ini.
bool SetPcValue(std::uint32_t id, std::int32_t value);
}
