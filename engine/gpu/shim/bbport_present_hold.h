// SPDX-License-Identifier: GPL-2.0-or-later
// Present hold: the window keeps showing the last picture for a moment while the game goes on.
// Used by the party code to hide a one-frame chr visit on the host's screen (a guest-started
// first fight, docs/party/quest_audit.md P5: the host's chr stands in the boss's region for
// ~0.07 s). Header-only clock; the presenter (vk_presenter.cpp) shows the held frame again
// instead of new game frames while it is active (not with frame generation).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace BbPresentHold {

inline std::atomic<std::int64_t>& Until() {
    static std::atomic<std::int64_t> until{0};
    return until;
}

inline std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Hold from now for `seconds` (a shorter value ends an earlier, longer hold sooner; 0 ends it).
inline void For(double seconds) {
    Until().store(NowNs() + std::int64_t(seconds * 1e9));
}

inline bool Active() {
    return NowNs() < Until().load();
}

}  // namespace BbPresentHold
