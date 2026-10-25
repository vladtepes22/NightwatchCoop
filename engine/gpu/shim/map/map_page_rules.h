// SPDX-License-Identifier: GPL-2.0-or-later
// The map page's pure rules (tests/test_map_page.cpp): which baked area a game map shows, the
// zoom range, the inputs that open the map, and the map view's pointer, panning, floors and
// lamp selection.
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

/// The controller pointer (left stick), in the canvas as 0..1 of its width / height: `sx`, `sy`
/// the stick (-1..1), `speed` pixels a second, `w` x `h` the canvas in pixels. Stays inside.
inline void MovePointer(float* u, float* v, float sx, float sy, float dt, float speed, float w, float h) {
    if (!std::isfinite(*u) || !std::isfinite(*v)) *u = *v = .5f;
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(dt)) return;
    *u = std::clamp(*u + sx * speed * dt / std::max(1.0f, w), 0.0f, 1.0f);
    *v = std::clamp(*v + sy * speed * dt / std::max(1.0f, h), 0.0f, 1.0f);
}

/// Panning: the map centre (world x, z) after the right stick / arrows moved it `dx`, `dy`
/// (-1..1) for `dt` seconds at `speed` screen pixels a second; `zoom` pixels per world unit. Screen
/// down is world -z (the map's north is up).
inline void Pan(float* cx, float* cz, float dx, float dy, float dt, float speed, float zoom) {
    if (!(zoom > 0) || !std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dt)) return;
    *cx += dx * speed * dt / zoom;
    *cz -= dy * speed * dt / zoom;
}

/// A mouse drag of `mx`, `my` screen pixels: the map follows the pointer.
inline void Drag(float* cx, float* cz, float mx, float my, float zoom) {
    if (!(zoom > 0) || !std::isfinite(mx) || !std::isfinite(my)) return;
    *cx -= mx / zoom;
    *cz += my / zoom;
}

/// A shown icon on the canvas (screen position, pick radius).
struct Hit {
    float x = 0, y = 0, radius = 0;
};
/// The icon nearest the pointer within its radius; -1 none.
inline int Nearest(const std::vector<Hit>& hits, float px, float py) {
    int best = -1;
    float best_d = 0;
    for (int i = 0; i < int(hits.size()); ++i) {
        const float d = std::hypot(px - hits[i].x, py - hits[i].y);
        if (d < hits[i].radius && (best < 0 || d < best_d)) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

/// The floor shown after a floor step (D-pad up / down, Page Up / Page Down): up is the next
/// higher floor, down the next lower one (`heights[i]`: floor i's height; equal heights in floor
/// order), wrapping from the highest to the lowest. `current`: the floor drawn now (-1 none: up
/// starts at the lowest, down at the highest). -1 when the area has one floor or none.
inline int StepFloor(const std::vector<float>& heights, int current, int step) {
    const int count = int(heights.size());
    if (count <= 1 || step == 0) return -1;
    std::vector<int> order(count);
    for (int i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const float ha = std::isfinite(heights[a]) ? heights[a] : 0.0f, hb = std::isfinite(heights[b]) ? heights[b] : 0.0f;
        return ha < hb;
    });
    if (current < 0 || current >= count) return step > 0 ? order.front() : order.back();
    const int at = int(std::find(order.begin(), order.end(), current) - order.begin());
    return order[((at + (step > 0 ? 1 : -1)) % count + count) % count];
}

/// What Cross / Enter / a click on the map does. `under`: the lamp under the pointer (-1 none);
/// `selected`: the lamp being shown (-1 none); `travel`: map travel can start now; `click`: a mouse
/// click (it acts only on the lamp under it; Cross and Enter also travel to the selected lamp with
/// nothing under the pointer). The first press on a lamp selects it (centres it and its floor), a
/// second press on the same one travels.
enum class Confirm { None, Select, Travel };
inline Confirm OnConfirm(int under, int selected, bool travel, bool click) {
    if (under >= 0 && under != selected) return Confirm::Select;
    if (selected < 0 || !travel) return Confirm::None;
    if (under == selected || (!click && under < 0)) return Confirm::Travel;
    return Confirm::None;
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
