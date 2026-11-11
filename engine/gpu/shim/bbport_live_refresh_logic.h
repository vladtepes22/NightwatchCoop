// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Live FPS stuck after a respawn (issue #35). With the frame rate uncapped the rate sometimes stays 25-35%
// below the pre-death one until the next load; the reporter cleared it twice by minimising the window and restoring it
// (a swapchain re-create). The pure rule that decides when the presenter re-creates its swapchain once; the caller
// (bbport_frame_rate.cpp) feeds it one sample a real second. Test: tests/test_frame_rate.cpp.
#pragma once

#include <algorithm>
#include <array>

namespace BbLiveRefresh {

constexpr int Window = 8;          ///< seconds of in-world rate the baseline is the median of
constexpr int SettleSeconds = 4;   ///< in-world seconds after a load before the rate is judged
constexpr int SustainSeconds = 6;  ///< consecutive slow seconds before the swapchain is re-created
constexpr double DropRatio = 0.80; ///< slow: below this share of the stay before the load
constexpr double MinBaseline = 50; ///< a baseline under this rate (a slow scene, a 30/60 cap) never counts

struct State {
    std::array<double, Window> ring{};
    int ring_n = 0, ring_head = 0;
    double baseline = 0;   ///< median rate of the stay before the last load (0 = none yet)
    int world_s = 0;       ///< in-world seconds since the last load
    int slow_s = 0;        ///< consecutive slow seconds
    bool fired = false;    ///< already fired in this stay
    bool was_loading = true;
    int target_key = -1;
};

inline double Median(const State& s) {
    if (s.ring_n <= 0) return 0.0;
    std::array<double, Window> v = s.ring;
    const int n = std::min(s.ring_n, Window);
    for (int i = 1; i < n; ++i) { // insertion sort of n <= Window values
        const double x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; --j; }
        v[j + 1] = x;
    }
    return v[n / 2];
}

/// One call a second. `fps` the game steps of the last second; `in_world` the world is up with no loading screen;
/// `target_key` anything that changes when the frame limit does (the baseline is dropped then). `test_at` > 0: test
/// aid, fires that many in-world seconds after every load whatever the rate. True: re-create the swapchain now.
inline bool Second(State& s, double fps, bool in_world, int target_key, int test_at = 0) {
    if (target_key != s.target_key) {
        s.target_key = target_key;
        s.baseline = 0;
        s.ring_n = s.ring_head = 0;
    }
    if (!in_world) {
        if (!s.was_loading && s.ring_n >= Window) s.baseline = Median(s); // the stay that just ended, if it was long enough
        s.was_loading = true;
        s.ring_n = s.ring_head = 0;
        s.world_s = 0, s.slow_s = 0, s.fired = false;
        return false;
    }
    s.was_loading = false;
    ++s.world_s;
    if (test_at > 0) {
        if (!s.fired && s.world_s >= test_at) return s.fired = true;
        return false;
    }
    if (s.world_s <= SettleSeconds) return false; // right after the load: shaders, streaming, the first frames
    s.ring[s.ring_head] = fps;
    s.ring_head = (s.ring_head + 1) % Window;
    s.ring_n = std::min(s.ring_n + 1, Window);
    if (s.fired || s.baseline < MinBaseline) return false;
    s.slow_s = fps < DropRatio * s.baseline ? s.slow_s + 1 : 0;
    if (s.slow_s >= SustainSeconds) return s.fired = true;
    return false;
}

} // namespace BbLiveRefresh
