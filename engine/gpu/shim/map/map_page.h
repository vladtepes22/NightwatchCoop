// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map page: the overlay menu's Map tab (Insert / L3+R3), drawing the current area's
// baked map (BbMapVector) around the player. Off unless BB_MAP_DIR points at bb-map-bake output.
#pragma once

#include "map_page_rules.h"

#include "video_core/renderer_vulkan/vk_common.h"

namespace BbMapPage {

/// After the ImGui Vulkan backend is up (bbport_overlay.cpp): the map's GPU pipeline for the
/// overlay's colour format and the baked maps from BB_MAP_DIR (BB_MAP_PARCHMENT: the parchment
/// texture, default <BB_MAP_DIR>/parchment.bbra). No BB_MAP_DIR: the map stays off.
void Init(VkDevice device, VkPhysicalDevice physical, VkFormat format);
/// The Map tab is shown.
bool Available();
/// Game frame thread (engine FrameHook): the current area and the player's position. Read only;
/// does nothing while the map is off.
void Tick();
/// Fills the rest of the current ImGui window (the Map tab's page) with the map.
void Draw(float scale);

} // namespace BbMapPage
