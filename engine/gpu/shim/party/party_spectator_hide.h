// SPDX-License-Identifier: GPL-3.0-or-later
// Which peer copies are hidden as a boss-fight spectator's dead body. Pure logic (unit test:
// tests/test_party_spectator_hide.cpp); the game side is SpectatorRemoveTick in party_b2.cpp.
//
// A copy is hidden only after it has read HP 0 for `dwell` seconds without a break (a copy built a
// moment ago by a load / re-join / revive reads HP 0 until its first state arrives: hiding it
// left a LIVING member invisible to the others for good, and a boss ignored it - 3-4 player
// bug "invisible after a death in a boss fight"). A hidden copy that reads HP > 0 is shown again.
#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <set>

namespace coop::spectator {

enum class Act { None, Hide, Show };

struct HideTracker {
    double dwell = 1.5;                          ///< seconds at HP 0 before a copy is hidden
    std::map<std::uint64_t, double> dead_since;  ///< copies seen at HP 0, since when
    std::set<std::uint64_t> hidden;              ///< the copies hidden here

    /// One look at a peer copy. window: a spectator's limbo message arrived lately.
    Act Step(std::uint64_t chr, int hp, double now, bool window) {
        if (hp != 0) {
            dead_since.erase(chr);
            return hidden.erase(chr) ? Act::Show : Act::None;
        }
        if (hidden.count(chr)) {
            return Act::None;
        }
        if (!window) {
            dead_since.erase(chr);
            return Act::None;
        }
        const auto it = dead_since.emplace(chr, now).first;
        if (now - it->second < dwell) {
            return Act::None;
        }
        hidden.insert(chr);
        return Act::Hide;
    }

    /// Forget copies that are gone (a load frees them; the address may be reused by a new chr).
    void Prune(const std::set<std::uint64_t>& present) {
        for (auto it = hidden.begin(); it != hidden.end();) {
            it = present.count(*it) ? std::next(it) : hidden.erase(it);
        }
        for (auto it = dead_since.begin(); it != dead_since.end();) {
            it = present.count(it->first) ? std::next(it) : dead_since.erase(it);
        }
    }

    void Clear() {
        dead_since.clear();
        hidden.clear();
    }
};

}  // namespace coop::spectator
