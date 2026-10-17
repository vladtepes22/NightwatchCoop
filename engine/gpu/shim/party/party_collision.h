// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <initializer_list>

namespace coop::collision {

// The registry predicate compares addresses; a stale collision record must never be dereferenced.
template <class IsRemote, class IsLive>
bool ClearStaleHits(std::uint64_t chr, IsRemote&& is_remote, IsLive&& is_live) {
    if (!chr || !is_remote(chr)) {
        return false;
    }
    bool cleared = false;
    for (const std::uint64_t offset : {0x288ull, 0x350ull}) {
        auto* hit = reinterpret_cast<std::uint64_t*>(chr + offset);
        if (*hit && !is_live(*hit)) {
            *hit = 0;
            cleared = true;
        }
    }
    return cleared;
}

} // namespace coop::collision
