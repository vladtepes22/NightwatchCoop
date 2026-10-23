// SPDX-License-Identifier: GPL-2.0-or-later
// The map's renderer: draws one area's compiled vector map (bb-map-bake output) on the GPU from
// an ImGui draw callback. Areas load on a worker thread; world geometry is uploaded once per area.
// Adapted from Hunter's Atlas source/compact/map_vector.cpp (gpu/shim/map/README.md).
#pragma once

#include "imgui.h"

#include "video_core/renderer_vulkan/vk_common.h"

#include <filesystem>
#include <string>
#include <vector>

namespace BbMapVector {

/// Creates the map's pipeline for colour attachments of `format` (dynamic rendering) on the ImGui
/// Vulkan backend's device. False (logged): the map cannot draw.
bool InitGpu(VkDevice device, VkPhysicalDevice physical, VkFormat format);
/// The compiled maps (atlas2-index.json and AreaNN folders) and the parchment texture (BBRA).
void Configure(const std::filesystem::path& compiled, const std::filesystem::path& parchment);
bool Configured();
/// Starts loading `region` ("m24_01_00_00"); true once it is loaded.
bool Load(const std::string& region);
/// The floor at a position (the manual floor when one is set); -1 while loading or invalid.
/// `continuity`: keeps the previous floor near its boundary (no flicker on stairs).
int Select(const std::string& region, float x, float y, float z, bool valid, bool continuity = true);
/// The floor with the largest footprint; -1 while loading.
int Primary(const std::string& region);
int Floors(const std::string& region);
/// A floor's height (its anchor, lowest and highest point); false while loading or invalid.
bool FloorHeights(const std::string& region, int floor, float* anchor, float* low, float* high);
/// -1: floors follow the player.
void SetManualFloor(int floor);
int ManualFloor();
/// Queues the area's drawing into `draw`: the world point (cx, cz) at `center`, `zoom` pixels per
/// unit. `revealed`: per reveal region, whether it is visible (null: no fog of war).
void Draw(ImDrawList* draw, const std::string& region, int floor, ImVec2 center, float cx, float cz, float zoom,
          float opacity, bool shading, float ui_scale, const std::vector<bool>* revealed);
/// The last load or draw error ("" when none).
std::string Error();
/// Frees the GPU buffers; call before the ImGui Vulkan backend shuts down.
void ReleaseGpu();
void Shutdown();

} // namespace BbMapVector
