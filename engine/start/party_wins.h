// SPDX-License-Identifier: GPL-3.0-or-later
// In a party the "Party: " patches win on any byte another patch also writes: a community patch (60 FPS++ of another
// patch list writes the same timers as "Party: Fast rejoin") must not leave a party site with bytes the party's check does
// not know (it then skips the site: "MISMATCH"). Same rule as scripts/patches.py party_wins.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace partywins {

using Bytes = std::vector<std::uint8_t>;
using Write = std::pair<std::uint64_t, Bytes>;
using Writes = std::vector<Write>;

inline bool Overlaps(const Writes& a, const Writes& b) {
    for (const Write& x : a)
        for (const Write& y : b)
            if (x.first < y.first + y.second.size() && y.first < x.first + x.second.size()) return true;
    return false;
}

/// writes without the bytes party writes, followed by the party's own writes.
inline Writes PartyWins(const Writes& writes, const Writes& party) {
    Writes kept;
    for (const Write& w : writes) {
        if (std::find(party.begin(), party.end(), w) != party.end()) continue;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces{{w.first, w.first + w.second.size()}};
        for (const Write& p : party) {
            const std::uint64_t start = p.first, end = p.first + p.second.size();
            std::vector<std::pair<std::uint64_t, std::uint64_t>> next;
            for (const auto& [a, b] : pieces) {
                if (end <= a || b <= start) {
                    next.emplace_back(a, b);
                    continue;
                }
                if (a < start) next.emplace_back(a, start);
                if (end < b) next.emplace_back(end, b);
            }
            pieces = std::move(next);
        }
        for (const auto& [a, b] : pieces)
            kept.emplace_back(a, Bytes(w.second.begin() + std::ptrdiff_t(a - w.first),
                                       w.second.begin() + std::ptrdiff_t(b - w.first)));
    }
    kept.insert(kept.end(), party.begin(), party.end());
    return kept;
}

}  // namespace partywins
