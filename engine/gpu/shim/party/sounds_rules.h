// SPDX-License-Identifier: GPL-3.0-or-later
// sounds (Nightwatch 0.3.0, docs/modding/sounds.md): the pure half of sounds.cpp. The table is
// out/nightwatch_sounds.txt, written by bb-start (start/features.h SoundsFileText) from the mods'
// nightwatch/sounds.json: "<event> <volume> <0|1 replace> <absolute file>" per line, '#' comment lines.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace coop::sounds {

constexpr std::size_t kMaxRows = 256;  // start/features.h kMaxSoundRows
constexpr std::string_view kTypes = "acfopsmvxbgyz";  // the game's event letters (1.09 table at 0x4735D30)

/// The key of an event name the game builds ("s000252531": letter, 9 digits, then NUL or the end of `max` bytes):
/// (letter index << 32) | id. False for any other name. Reads at most 11 bytes.
inline bool EventKey(const char* name, std::size_t max, std::uint64_t* key) {
    if (!name || max < 10) return false;
    const std::size_t type = kTypes.find(name[0]);
    if (type == std::string_view::npos) return false;
    std::uint64_t id = 0;
    for (int i = 1; i <= 9; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        id = id * 10 + std::uint64_t(name[i] - '0');
    }
    if (max > 10 && name[10] != '\0') return false;
    *key = (std::uint64_t(type) << 32) | id;
    return true;
}

struct Row {
    std::uint64_t key = 0;
    float volume = 1.0f;
    bool replace = false;
    std::string file;
};

/// The whole file, or false (no rows used) at the first line that is not "<event> <volume 0..4> <0|1> <file>".
/// Rows come back sorted by key (a key twice: the later line wins).
inline bool ParseTable(std::string_view text, std::vector<Row>* rows, std::size_t* bad_line = nullptr) {
    rows->clear();
    std::size_t number = 0;
    const auto fail = [&]() {
        rows->clear();
        if (bad_line) *bad_line = number;
        return false;
    };
    while (!text.empty()) {
        ++number;
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        Row row;
        if (line.size() < 16 || line[10] != ' ' || !EventKey(line.data(), 10, &row.key)) return fail();
        line.remove_prefix(11);
        const std::size_t sp = line.find(' ');
        if (sp == std::string_view::npos) return fail();
        const std::string volume(line.substr(0, sp));
        char* stop = nullptr;
        const double v = std::strtod(volume.c_str(), &stop);
        if (volume.empty() || *stop || !(v >= 0.0 && v <= 4.0)) return fail();
        row.volume = float(v);
        line.remove_prefix(sp + 1);
        if (line.size() < 3 || (line[0] != '0' && line[0] != '1') || line[1] != ' ') return fail();
        row.replace = line[0] == '1';
        row.file = std::string(line.substr(2));
        if (rows->size() >= kMaxRows) return fail();
        rows->push_back(std::move(row));
    }
    std::stable_sort(rows->begin(), rows->end(), [](const Row& a, const Row& b) { return a.key < b.key; });
    std::vector<Row> unique;
    for (Row& r : *rows) {
        if (!unique.empty() && unique.back().key == r.key) unique.back() = std::move(r);
        else unique.push_back(std::move(r));
    }
    *rows = std::move(unique);
    return true;
}

/// The row of `key`, or null. `rows` sorted by key.
inline const Row* Find(const std::vector<Row>& rows, std::uint64_t key) {
    const auto it = std::lower_bound(rows.begin(), rows.end(), key, [](const Row& r, std::uint64_t k) { return r.key < k; });
    return it != rows.end() && it->key == key ? &*it : nullptr;
}

constexpr float kFullDistance = 3.0f;   // metres: full volume up to here, then volume * kFullDistance / distance
constexpr float kSilentDistance = 60.0f;  // metres: not played past here

/// Left / right gains of a sound at world `pos` heard from the camera (`view` = world to view, 3x4 rows, view z > 0
/// in front; `proj_x` = the projection's x scale, whose sign says which way screen right is). No camera
/// (`camera` false): centred at `volume`. Equal-power pan on the horizontal angle. False when it is too far to hear.
inline bool Gains(bool camera, const float* view, float proj_x, const float* pos, float volume, float* left,
                  float* right) {
    float gain = volume, pan = 0.0f;
    if (camera) {
        float v[3];
        for (int i = 0; i < 3; ++i)
            v[i] = view[4 * i] * pos[0] + view[4 * i + 1] * pos[1] + view[4 * i + 2] * pos[2] + view[4 * i + 3];
        const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(d <= kSilentDistance)) return false;  // also NaN
        if (d > kFullDistance) gain *= kFullDistance / d;
        const float flat = std::sqrt(v[0] * v[0] + v[2] * v[2]);
        if (flat > 1e-4f) pan = std::clamp((proj_x < 0.0f ? -v[0] : v[0]) / flat, -1.0f, 1.0f);
    }
    const float angle = (pan + 1.0f) * 0.78539816f;  // 0 = all left, pi/2 = all right
    *left = gain * std::cos(angle);
    *right = gain * std::sin(angle);
    return gain > 0.0f;
}

// The game's own sound-effect level (1.09 disassembly, docs/modding/sounds.md "Volume"): the sound volume manager
// (slot 0x556BCE0) computes each frame, in 0x17F4DC0, the SE category's volume = SE option byte / 10 (Se fader) *
// InGameMaster * MenuMaster, sends it to FMOD when it changes and keeps the value sent at +0x84; its Master fader goes
// to the master volume message (0x27D7D60), the value sent kept at +0xAC.
constexpr std::uint64_t kVolumeManager = 0x556bce0;
constexpr std::uint64_t kSeSent = 0x84, kMasterSent = 0xac;

/// The factor a mod sound gets from the game's levels (`se` = +0x84, `master` = +0xAC as read): their product,
/// each clamped to 0..1; a value that is not a number (unreadable) counts as 1.
inline float SfxGain(float se, float master) {
    const auto unit = [](float v) { return std::isnan(v) ? 1.0f : std::clamp(v, 0.0f, 1.0f); };
    return unit(se) * unit(master);
}

}  // namespace coop::sounds
