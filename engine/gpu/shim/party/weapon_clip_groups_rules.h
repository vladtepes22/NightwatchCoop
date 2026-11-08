// SPDX-License-Identifier: GPL-3.0-or-later
// weapon_clip_groups (Nightwatch 0.3.0, docs/modding/weapon_clip_groups.md): the pure half of weapon_clip_groups.cpp.
// The table is out/nightwatch_weapon_clip_groups.txt, written by bb-start (start/features.h WeaponClipGroupsFileText)
// from the mods' nightwatch/weapon_clip_groups.json: "<weapon> <clip_group>" per line, '#' comments.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace coop::clipgroups {

constexpr std::size_t kMaxRows = 4096;  // start/features.h kMaxClipRows

struct Row {
    std::int32_t weapon = 0, clip_group = 0;
};

/// The whole file, or false (no rows used) at the first line that is not "<weapon> <clip_group>" with weapon a
/// multiple of 10000 in 10000..2147480000 and clip_group 1..999. Rows come back sorted by weapon.
inline bool ParseTable(std::string_view text, std::vector<Row>* rows, std::size_t* bad_line = nullptr) {
    rows->clear();
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        std::int64_t v[2] = {0, 0};
        int count = 0;
        bool ok = true;
        std::size_t i = 0;
        while (ok) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
            if (i == line.size()) break;
            if (count == 2 || line[i] < '0' || line[i] > '9') {
                ok = false;
                break;
            }
            std::int64_t n = 0;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                n = n * 10 + (line[i] - '0');
                if (n > 0x7FFFFFFF) ok = false;
                ++i;
            }
            v[count++] = n;
        }
        if (ok && count == 0) continue;  // blank or comment
        ok = ok && count == 2 && v[0] >= 10000 && v[0] <= 2147480000LL && v[0] % 10000 == 0 && v[1] >= 1 &&
             v[1] <= 999 && rows->size() < kMaxRows;
        if (!ok) {
            rows->clear();
            if (bad_line) *bad_line = number;
            return false;
        }
        rows->push_back({std::int32_t(v[0]), std::int32_t(v[1])});
    }
    std::stable_sort(rows->begin(), rows->end(), [](const Row& a, const Row& b) { return a.weapon < b.weapon; });
    return true;
}

/// The clip group of weapon id `weapon` (any of its rows: the table's key is the id without its last four digits,
/// so +0..+10 and every reinforcement row share it), or 0 when the table has none. `rows` sorted by weapon.
inline std::int32_t GroupOf(const std::vector<Row>& rows, std::int32_t weapon) {
    if (weapon < 10000) return 0;
    const std::int32_t key = weapon - weapon % 10000;
    const auto it = std::lower_bound(rows.begin(), rows.end(), key, [](const Row& r, std::int32_t k) { return r.weapon < k; });
    return it != rows.end() && it->weapon == key ? it->clip_group : 0;
}

}  // namespace coop::clipgroups
