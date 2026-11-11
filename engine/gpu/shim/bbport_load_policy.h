// SPDX-License-Identifier: GPL-3.0-or-later
// Pure rules of the 0.8.1 loading speed-ups (loadux081), unit-tested in tests/test_fast_loading.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace bbport::loadpolicy {

/// Unpaced loading screen: a real frame (drawn and flipped) is due when `period_ns` have passed since the last one.
inline bool DrawDue(std::int64_t now_ns, std::int64_t last_draw_ns, std::int64_t period_ns) {
    return last_draw_ns == 0 || now_ns - last_draw_ns >= period_ns;
}

/// Resource post-processor threads when BB_LOAD_WORKERS is unset: min(4, max(3, usable CPUs - 2)) (never below the
/// game's own 3). 0 when the ini / launcher switch is off.
inline unsigned DefaultWorkers(unsigned usable_cpus, bool enabled) {
    if (!enabled) return 0;
    const unsigned spare = usable_cpus > 2 ? usable_cpus - 2 : 0;
    return std::min(4u, std::max(3u, spare));
}

/// BB_LOAD_WORKERS: unset / empty = `fallback` (the default above); "0" / "off" / "false" = 0 (the game's own);
/// a number = that many (1..8).
inline unsigned ParseWorkers(const char* v, unsigned fallback) {
    if (!v || !*v) return fallback;
    if (!std::strcmp(v, "off") || !std::strcmp(v, "false")) return 0;
    const long n = std::strtol(v, nullptr, 10);
    return n <= 0 ? 0 : unsigned(std::min<long>(n, 8));
}

/// Faster fades: a fade's remaining time counts down by the game's own step each frame. Given the remaining time seen
/// at the previous frame start (`prev`) and now (`now`), the extra amount to take off so the fade runs `factor`
/// times as fast, or 0 when this is not a running countdown (a new fade started, the same value, a jump). The
/// result never takes `now` to 0 (the game ends the fade itself) and one step never exceeds `max_step` seconds.
inline float FadeExtra(float prev, float now, float factor, float max_step) {
    if (!(factor > 1.0f) || !(prev > 0.0f) || !(now > 0.0f) || !(now < prev)) return 0.0f;
    const float step = prev - now;
    if (step > max_step) return 0.0f;
    const float extra = step * (factor - 1.0f);
    return std::min(extra, now * 0.9f);
}

}  // namespace bbport::loadpolicy
