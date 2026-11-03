// SPDX-License-Identifier: GPL-2.0-or-later
// Live FPS clock rules (gpu/shim/engine/live_clock_rules.h); cases from visy's tests/test_frame_rate.cpp
// (0.7.8). ninja -C out/gpu live-clock-test
#include "live_clock_rules.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace BbEngine::LiveClock;

static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

int main() {
    assert(!LiveWanted("uncap", nullptr, false));
    assert(LiveWanted("uncap", nullptr, true));
    assert(LiveWanted("60", nullptr, true));
    assert(LiveWanted(nullptr, nullptr, true));
    assert(!LiveWanted("30", nullptr, true));  // the game's own rate
    assert(!LiveWanted("uncap", "0", true));   // FPS patch off
    assert(TargetRate("60", "60") == 60 && TargetRate("uncap", "144") == 144 && TargetRate("uncap", "0") == MaxRate);
    assert(LiveTargetFromRate(60) == 60 && LiveTargetFromRate(144) == 144);
    assert(LiveTargetFromRate(MaxRate) == 0 && LiveTargetFromRate(240) == 0 && LiveTargetFromRate(10) == 30);

    LiveClockState st;
    float interval = 0;
    const float t60 = 1.0f / 60;
    assert(Near(LiveClockStep(st, t60 * 1.01f, 60, 30, interval), t60, 1e-6f) && Near(interval, t60, 1e-6f));
    // A long frame is stepped as it is (up to 1/30 s), no lag: time is conserved.
    { LiveClockState a; assert(Near(LiveClockStep(a, 0.025f, 60, 30, interval), 0.025f, 1e-6f)); }
    { LiveClockState a; assert(Near(LiveClockStep(a, 0.5f, 60, 30, interval), Frame30, 1e-6f)); }
    { LiveClockState a; assert(Near(LiveClockStep(a, 0.005f, 60, 30, interval), 0.005f, 1e-6f)); }
    // Over a run of jittery frames the stepped time equals the real time (below the clamps).
    {
        LiveClockState sj;
        double real = 0, game = 0;
        for (int i = 0; i < 20000; ++i) {
            const float m = t60 * (1.0f + 0.4f * std::sin(float(i) * 0.7f)) * (1.0f + 0.01f * std::sin(float(i)));
            real += double(m);
            game += double(LiveClockStep(sj, m, 60, 30, interval));
        }
        assert(std::fabs(game / real - 1.0) < 0.002);
        LiveClockState sk; // a PC that holds 58 FPS: game time stays 1.0
        double r2 = 0, g2 = 0;
        for (int i = 0; i < 20000; ++i) {
            r2 += double(1.0f / 58);
            g2 += double(LiveClockStep(sk, 1.0f / 58, 60, 30, interval));
        }
        assert(std::fabs(g2 / r2 - 1.0) < 0.002);
    }
    // Invalid measurements count as the target; uncapped paces at the 240 FPS ceiling.
    { LiveClockState a; assert(Near(LiveClockStep(a, NAN, 120, 30, interval), 1.0f / 120, 1e-6f)); }
    {
        LiveClockState a;
        assert(Near(LiveClockStep(a, 0.0f, 0, 30, interval), 1.0f / 240, 1e-6f) && Near(interval, 1.0f / 240, 1e-6f));
    }
    { LiveClockState a; assert(Near(LiveClockStep(a, 0.2f, 30, 240, interval), Frame30, 1e-6f)); }
    // Switching the target resets the carry.
    {
        LiveClockState a;
        LiveClockStep(a, t60 * 1.01f, 60, 30, interval);
        assert(Near(LiveClockStep(a, 1.0f / 120, 120, 30, interval), 1.0f / 120, 1e-6f));
    }
    std::puts("live-clock-test: OK");
    return 0;
}
