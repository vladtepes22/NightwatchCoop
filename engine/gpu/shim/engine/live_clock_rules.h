// SPDX-License-Identifier: GPL-2.0-or-later
// Live FPS clock: the pure rules (live_clock.cpp). From visy's bbport_frame_rate_logic.h (0.7.8,
// 18907808), itself Mrsuss60/bloodborne_pc_windows_port a7cbfae6, 83a168ee (GPL-2.0-or-later).
// Unit test: ninja -C out/gpu live-clock-test.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace BbEngine::LiveClock {

constexpr float Frame30 = 1.0f / 30.0f;
/// The highest rate BB_FPS / BB_FPS_LIMIT can ask for.
constexpr int MaxRate = 480;
/// The pacer's hard ceiling in live mode (its four frame time constants are lowered to 1/240 s).
/// A target of 0 means "uncapped" = this.
constexpr int LiveCeiling = 240;

/// Live mode wanted? `live_fps` the setting (BB_LIVE_FPS wins over bbport.ini live_fps, read by the
/// caller). BB_FPS=30 (the game's own rate) and BB_FPS_PATCH=0 keep it off.
inline bool LiveWanted(const char* bb_fps, const char* bb_fps_patch, bool live_fps) {
    auto off = [](const char* v) {
        return v && (!std::strcmp(v, "0") || !std::strcmp(v, "off") || !std::strcmp(v, "false"));
    };
    if (!live_fps) return false;
    if (bb_fps && !std::strcmp(bb_fps, "30")) return false;
    return !off(bb_fps_patch);
}

/// The start's rate: BB_FPS (uncap = MaxRate, or a number) and BB_FPS_LIMIT, clamped to 30..MaxRate.
inline int TargetRate(const char* bb_fps, const char* bb_fps_limit) {
    int rate = !bb_fps || !std::strcmp(bb_fps, "uncap") ? MaxRate : std::atoi(bb_fps);
    rate = std::clamp(rate <= 0 ? MaxRate : rate, 30, MaxRate);
    if (bb_fps_limit && std::atoi(bb_fps_limit) > 0) rate = std::min(rate, std::clamp(std::atoi(bb_fps_limit), 30, MaxRate));
    return rate;
}

/// A TargetRate() as the live target: 0 (uncapped) from the ceiling up.
inline int LiveTargetFromRate(int rate) {
    return rate >= LiveCeiling ? 0 : std::clamp(rate, 30, LiveCeiling);
}

struct LiveClockState {
    float smoothed = 0.0f; ///< the time carried from frames locked to the target
    int previous_target = -1;
};

/// The step for a frame the pacer measured at `measured` s: the frame's time, clamped to
/// [1/240 s, 1/max_dt_hz] (below that rate the game slows down, as it always did); only micro-jitter
/// (+-1.5% of the target: timer granularity) is locked to the target, the difference carried to the
/// next frame so no time is lost. `target_fps` 0 = uncapped; `interval` receives the pacer's frame
/// time target. (Mrsuss60's +-8% deadband and +12% / -15% slew made the game run at 0.83 of real
/// time under jitter; visy dropped them.)
inline float LiveClockStep(LiveClockState& st, float measured, int target_fps, int max_dt_hz, float& interval) {
    const float floor_dt = 1.0f / float(LiveCeiling);
    const float target = target_fps > 0 ? 1.0f / float(target_fps) : floor_dt;
    float max_dt = max_dt_hz > 0 ? 1.0f / float(max_dt_hz) : Frame30;
    if (max_dt < target) max_dt = target;
    interval = target;
    float raw = std::isfinite(measured) && measured > 0.0f ? measured : target;
    raw = std::clamp(raw, floor_dt, max_dt);
    if (st.previous_target != target_fps) { // the target changed in the menu: no carry across it
        st.previous_target = target_fps;
        st.smoothed = 0.0f;
    }
    float dt = raw;
    if (target_fps > 0) {
        const float band = target * 0.015f;
        const float want = raw + st.smoothed;
        if (want >= target - band && want <= target + band) {
            dt = target;
            st.smoothed = want - target;
        } else {
            dt = want;
            st.smoothed = 0.0f;
        }
    }
    return std::clamp(dt, floor_dt, max_dt);
}

/// The pacer's frame time target while fast loading holds a loading screen at 30 FPS (loading.cpp): the
/// live target (1/60, 1/120, 1/240 uncapped) would draw the loading screen 2 to 8 times as often and take
/// that CPU time from the loading threads.
inline float LoadingInterval(float interval, bool fast_loading_pace) {
    return fast_loading_pace ? std::max(interval, Frame30) : interval;
}

/// Whole 1/30 s steps of game time, however many frames they take (visy 0.8.0, droogie/bbhost 57d2b9f, f24d18f): a
/// frame closes a step once the time banked reaches three quarters of one, and a step takes a whole 1/30 s from the
/// bank, so the steps are 30 a game second at any rate and every frame at 30 (the quarter is for the clock's rounding;
/// `closes` 0.98: a step closed only when whole, for what counts the time between steps, the lights' blink). The
/// water wakes and the lights' blink begin their steps only on these frames, as at 30 (bbport_hifps.cpp).
inline bool Step30Gate(float& banked, float dt, float closes = 0.75f) {
    banked += dt;
    if (!(banked >= closes * Frame30)) return false;
    banked -= Frame30;
    if (banked > Frame30) banked = Frame30;
    return true;
}

/// The steep-ground slide (0x1a46bc0, no step of its own: it runs once a frame): while the ground is steeper than
/// 35 degrees it adds the game's gain (1.0, 0x5128370) to the slide each frame, else it keeps the decay's share
/// (0.65, 0x5128374). Both are 30 FPS values; above 30 the hunter was pushed off a step's corner 2 to 4 times as
/// often and held there, the feet into the step (Lantern 0.9.40, notes ground-stuck-high-fps). `ticked`: the game's
/// gain once a whole 1/30 s has passed (`tick_closed`, Step30Gate exact) and none between, as the console samples
/// it; else the gain spread over the frames (dt x 30). BB_FRAMERATE_SLOPE_TICK=0 picks the spread.
inline float SlopeGain(float game_gain, float dt, bool tick_closed, bool ticked) {
    if (ticked) return tick_closed ? game_gain : 0.0f;
    return game_gain * dt / Frame30;
}

/// The decay for a frame of `dt` s: the game's per-frame share at 30, so a second keeps as much at any rate.
inline float SlopeDecay(float game_decay, float dt) {
    return std::pow(game_decay, dt / Frame30);
}

} // namespace BbEngine::LiveClock
