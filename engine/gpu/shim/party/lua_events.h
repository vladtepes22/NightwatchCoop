// SPDX-License-Identifier: GPL-3.0-or-later
// Named Lua events (e.g. the bells: host "OnEvent_Call_SOS", guest
// "OnEvent_SendSoulSign_NormalCoop"), queued from any thread and raised on the game's main
// thread (coop_tick, once a frame) through the game's LuaEvent_DispatchByName (0x1339870).
// Ported from droogie/bbhost src/engine/lua_events.cpp @8f2746c (GPL-3.0-or-later).
#pragma once

#include <cstdint>

namespace coop {

/// Checks the dispatcher's code and binds it; false (events refused) when it differs.
bool LuaEventsInit();
/// Queues `name` (at most 64 pending); false when refused or full.
bool LuaEventQueue(const char* name);
/// Main thread only: raises everything queued.
void LuaEventsTick();
/// Main thread only: raises `name` now; false when there is no event manager yet. *result gets
/// the dispatcher's answer (its low byte): 0 = no handler for the name (or it failed), 1 = ran.
bool LuaEventRaiseNow(const char* name, std::int64_t* result);
std::uint64_t LuaEventsRaised();

} // namespace coop
