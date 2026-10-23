// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map: the map view (held Options / Start, M; bbport_overlay.cpp) and the overlay menu's
// Map tab, drawing the current area's baked map (BbMapVector) around the player. Off unless
// BB_MAP_DIR points at bb-map-bake output.
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
/// Game frame thread (engine FrameHook): the current area and the player's position. Read only;
/// does nothing while the map is off.
void Tick();
/// Fills the rest of the current ImGui window with the map. pad_zoom: L2 / R2 zoom (the map view;
/// in the menu they switch tabs).
void Draw(float scale, bool pad_zoom = false);
/// The inputs that open and close the map view (bbport.ini bind.map, default M; pad.map, default
/// start, read by runtime_pad.c too): an SDL scancode, mouse button (1-5) or gamepad button.
bool OpensWithKey(int scancode);
bool OpensWithMouse(int button);
bool OpensWithPad(int button);

} // namespace BbMapPage
