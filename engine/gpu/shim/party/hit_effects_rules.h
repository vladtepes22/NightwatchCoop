// SPDX-License-Identifier: GPL-3.0-or-later
// hit_effects (Nightwatch 0.3.0, docs/modding/hit_effects.md): the pure half of hit_effects.cpp. The table is
// out/nightwatch_hit_effects.txt, written by bb-start (start/features.h HitEffectsFileText) from the mods'
// nightwatch/hit_effects.json: "<attacker_speffect> <base_fx> <fx>" per line, newest mod first, '#' comments.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace coop::hiteffects {

constexpr std::size_t kMaxRows = 4096;  // start/features.h kMaxHitRows

struct Row {
    std::int32_t attacker_speffect = 0, base_fx = 0, fx = 0;
};

/// The whole file, or false (no rows used) at the first line that is not three whole numbers 1..2147483647.
inline bool ParseTable(std::string_view text, std::vector<Row>* rows, std::size_t* bad_line = nullptr) {
    rows->clear();
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        std::int64_t v[3] = {0, 0, 0};
        int count = 0;
        bool ok = true;
        std::size_t i = 0;
        while (ok) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
            if (i == line.size()) break;
            if (count == 3 || line[i] < '0' || line[i] > '9') {
                ok = false;
                break;
            }
            std::int64_t n = 0;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                n = n * 10 + (line[i] - '0');
                if (n > 0x7FFFFFFF) ok = false;
                ++i;
            }
            if (n < 1) ok = false;
            v[count++] = n;
        }
        if (ok && count == 0) continue;  // blank or comment
        if (!ok || count != 3 || rows->size() >= kMaxRows) {
            rows->clear();
            if (bad_line) *bad_line = number;
            return false;
        }
        rows->push_back({std::int32_t(v[0]), std::int32_t(v[1]), std::int32_t(v[2])});
    }
    return true;
}

/// The replacement for overlay id `base_fx` when the attacker has the SpEffects `has(id)` says, or 0 (keep it): the
/// first matching row (the table lists the newest mod first, so the latest mod wins). `has` is only asked for rows
/// whose base_fx matches.
template <class Has>
std::int32_t Pick(const std::vector<Row>& rows, std::int32_t base_fx, Has&& has, std::int32_t* speffect = nullptr) {
    for (const Row& r : rows) {
        if (r.base_fx == base_fx && has(r.attacker_speffect)) {
            if (speffect) *speffect = r.attacker_speffect;
            return r.fx;
        }
    }
    return 0;
}

/// True when some row replaces `base_fx` (lets the hook skip walking the attacker's SpEffects for other ids).
inline bool Watched(const std::vector<Row>& rows, std::int32_t base_fx) {
    for (const Row& r : rows) {
        if (r.base_fx == base_fx) return true;
    }
    return false;
}

}  // namespace coop::hiteffects
