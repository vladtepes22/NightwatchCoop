// SPDX-License-Identifier: GPL-2.0-or-later
// The map view's pictures: the frame behind the map (background.bbra) and the icon sheet
// (icons.bbra: lamp, ladder, NPC, gate, player, compass), Hunter's Atlas's own art (not game files),
// shipped beside parchment.bbra (tools/map/art-bbra.py). Adapted from Hunter's Atlas
// source/native/gpu/shim/bbport_map_art.cpp (MikeTat1988, GPL-2.0-or-later).
#pragma once

#include "imgui.h"

#include <filesystem>

namespace BbMapArt {

/// Reads the pictures from `dir` (any thread); a missing or bad file leaves that picture out.
void Load(const std::filesystem::path& dir);
/// Inside an ImGui frame: makes the read pictures into textures (once). False until the icons are there.
bool Ready();
/// The frame picture stretched over [min, max]; nothing without it.
void DrawBackground(ImDrawList* draw, ImVec2 min, ImVec2 max);
/// The frame picture again over the map's edge, fading in towards the map ([map_min, map_max]).
void DrawBackgroundFade(ImDrawList* draw, ImVec2 min, ImVec2 max, ImVec2 map_min, ImVec2 map_max, float fade);
/// One icon: "lamp", "ladder", "talk", "gate", "player", "compass". False without the sheet.
bool DrawIcon(ImDrawList* draw, const char* name, ImVec2 center, float size, float rotation = 0.0f,
              ImU32 tint = IM_COL32_WHITE);

}  // namespace BbMapArt
