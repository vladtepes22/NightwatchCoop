// SPDX-License-Identifier: GPL-2.0-or-later
// The map page's pure rules (tests/test_map_page.cpp): which baked area a game map shows, the
// zoom range, and the inputs that open the map.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace BbMapPage {

/// The baked area for the game's map id (0xAABBCCDD = mAA_BB_CC_DD, the atlas region name); ""
/// where the map shows none: unknown (all ones, between loads) or a Chalice Dungeon (area 29: its
/// layout is assembled at load time, so no baked map fits it).
inline std::string Region(std::uint32_t map_id) {
    if (map_id == 0xffffffffu || (map_id >> 24) == 29) {
        return {};
    }
    char text[16];
    std::snprintf(text, sizeof text, "m%02u_%02u_%02u_%02u", unsigned(map_id >> 24 & 0xff), unsigned(map_id >> 16 & 0xff),
                  unsigned(map_id >> 8 & 0xff), unsigned(map_id & 0xff));
    return text;
}

constexpr float kMinZoom = 0.15f, kMaxZoom = 25.0f, kDefaultZoom = 4.0f;

/// One mouse wheel step's zoom (pixels per world unit), kept in range; a bad value resets.
inline float WheelZoom(float zoom, float wheel) {
    if (!std::isfinite(zoom) || zoom <= 0) {
        zoom = kDefaultZoom;
    }
    if (!std::isfinite(wheel)) {
        wheel = 0;
    }
    return std::clamp(zoom * std::pow(1.15f, wheel), kMinZoom, kMaxZoom);
}

/// The inputs of a bbport.ini binding value (bind.map, pad.map: "M, Ctrl+Mouse4"), as their base
/// names: the map opens on the key or button alone, so a combination's modifiers are dropped.
inline std::vector<std::string> InputNames(const std::string& value) {
    std::vector<std::string> names;
    std::size_t at = 0;
    while (at <= value.size()) {
        std::size_t end = value.find(',', at);
        if (end == std::string::npos) end = value.size();
        std::string name = value.substr(at, end - at);
        at = end + 1;
        for (bool stripped = true; stripped;) {
            stripped = false;
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(0, 1);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            for (const char* prefix : {"Ctrl+", "Shift+", "Alt+"}) {
                const std::size_t n = std::char_traits<char>::length(prefix);
                if (name.size() > n && name.compare(0, n, prefix) == 0) {
                    name.erase(0, n);
                    stripped = true;
                }
            }
        }
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

} // namespace BbMapPage
