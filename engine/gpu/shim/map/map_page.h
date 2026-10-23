// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map: the map view (held Options / Start, M; bbport_overlay.cpp) and the overlay menu's
// Map tab, drawing an area's baked map (BbMapVector) with its lamps, markers and the party
// (map_data_rules.h). Off unless BB_MAP_DIR points at bb-map-bake output; the generator's data is
// read from BB_MAP_DATA (default <BB_MAP_DIR>/../atlas/maps), the art from beside the parchment.
#pragma once

#include "map_page_rules.h"

#include "video_core/renderer_vulkan/vk_common.h"

namespace BbMapPage {

/// After the ImGui Vulkan backend is up (bbport_overlay.cpp): the map's GPU pipeline for the
/// overlay's colour format and the baked maps from BB_MAP_DIR (BB_MAP_PARCHMENT: the parchment
/// texture, default <BB_MAP_DIR>/parchment.bbra). No BB_MAP_DIR: the map stays off.
void Init(VkDevice device, VkPhysicalDevice physical, VkFormat format);
/// The map is on (the map view opens, the Map tab is shown).
bool Available();
/// Present thread, every frame: maps that were still being made at the start turn the map on when
/// they appear (no restart).
void Poll();
/// The note over the game, while one is due: the maps still being made, or just ready.
const char* NoteText();
/// Game frame thread (engine FrameHook): the current area, the player's position and facing, and
/// (while the map is drawn) the event flags its rules read. Read only; nothing while the map is off.
void Tick();
/// The overlay menu's Map tab: fills the rest of the current ImGui window (mouse controls only:
/// the pad navigates the menu).
void Draw(float scale);
/// The map view was just opened (inside the ImGui frame, before DrawView): it recentres on the
/// player, and no button held from before (the held Options that opened it) acts on it.
void Opened();
/// The map view: its own full-screen window, controller and keyboard controls (the overlay feeds
/// it the pad and keys while it is open). False: its Back was clicked, close it.
bool DrawView();
/// The inputs that open and close the map view (bbport.ini bind.map, default M; pad.map, default
/// start, read by runtime_pad.c too): an SDL scancode, mouse button (1-5) or gamepad button.
bool OpensWithKey(int scancode);
bool OpensWithMouse(int button);
bool OpensWithPad(int button);

} // namespace BbMapPage
