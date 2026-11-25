// SPDX-License-Identifier: GPL-2.0-or-later
// Judder with a steady FPS counter (NW-b9564e8): flips not locked to the vblanks follow the GPU's
// real finish times (LanternPacing::GpuPacedSlot), read by BbFrameDone::Watch.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "gpu/shim/frame_done_watch.h"
#include "gpu/shim/lantern_pacing.h"

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int g_failed = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failed;
    }
}

constexpr auto Pace = std::chrono::nanoseconds(11'111'111); // 90 FPS

// Frames shown at max(flip, GPU finish); the GPU renders frame i in gpu[i] after the previous one
// finished and after the game handed it over: a pace before its flip, or (cpu_ms > 0, a game that
// starts a frame when its last flip was taken) cpu_ms after the last flip. Returns the intervals
// between frames on screen, with or without the rule.
std::vector<double> Simulate(const std::vector<double>& gpu_ms, bool rule, double cpu_ms = 0,
                             int* flips_count = nullptr, double* span_ms = nullptr) {
    LanternPacing::Cadence flips{Pace, Clock::time_point{} + Pace};
    LanternPacing::UsualLate usual;
    std::vector<double> shown;
    Clock::time_point last_done{}, last_flip{}, last_shown{}, first_shown{};
    bool have = false;
    const auto dur = [](double ms) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(ms));
    };
    for (double g : gpu_ms) {
        Clock::time_point flip = flips.Slot();
        if (have) {
            const auto u = usual.Get();
            usual.Add(last_done - last_flip);
            if (rule) {
                flips.PushTo(LanternPacing::GpuPacedSlot(flips.Slot(), last_flip, last_done, Pace, u));
                flip = flips.Slot();
            }
        }
        const auto handed = cpu_ms > 0 && have ? last_flip + dur(cpu_ms) : flip - Pace;
        const auto start = std::max(last_done, handed);
        const auto done = start + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::duration<double, std::milli>(g));
        const auto on_screen = std::max(flip, done);
        if (have) {
            shown.push_back(std::chrono::duration<double, std::milli>(on_screen - last_shown).count());
        } else {
            first_shown = on_screen;
        }
        flips.Taken(flip);
        last_flip = flip;
        last_done = done;
        last_shown = on_screen;
        have = true;
    }
    if (flips_count) *flips_count = int(gpu_ms.size());
    if (span_ms) *span_ms = std::chrono::duration<double, std::milli>(last_shown - first_shown).count();
    return shown;
}

int Uneven(const std::vector<double>& shown) {
    int n = 0;
    for (double ms : shown) n += ms > 11.111 * 1.25 || ms < 11.111 * 0.75;
    return n;
}

void TestRule() {
    const auto t0 = Clock::time_point{} + 1s;
    const auto slot = t0 + Pace;
    // On time, or late by less than a sixteenth: the slot stays.
    const auto none = std::chrono::nanoseconds(0);
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 - 2ms, Pace, none) == slot, "a frame done before its flip moves nothing");
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 500us, Pace, none) == slot, "half a millisecond late moves nothing");
    // Late: a whole pace after the finish.
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 4ms, Pace, none) == t0 + 4ms + Pace, "4 ms late: a pace after the finish");
    // Late as usual: nothing; later than usual: by the difference.
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 3ms, Pace, 3ms) == slot, "as late as usual moves nothing");
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 7ms, Pace, 3ms) == t0 + 4ms + Pace, "4 ms later than usual: by 4 ms");
    // Never earlier than the slot.
    Check(LanternPacing::GpuPacedSlot(slot + 10ms, t0, t0 + 4ms, Pace, none) == slot + 10ms, "never before the slot");
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 4ms, none, none) == slot, "no pace: no change");
    // The usual lateness: a median, so one late frame does not move it.
    LanternPacing::UsualLate u;
    Check(u.Get() == none, "nothing yet: 0");
    for (int i = 0; i < 10; ++i) u.Add(3ms);
    u.Add(20ms);
    u.Add(-5ms);
    Check(u.Get() == 3ms, "median of mostly 3 ms: 3 ms");

    // One frame 4 ms over in a run at the pace: without the rule a long frame and a short one on
    // screen; with it the long one only.
    const std::vector<double> gpu = {10, 10, 10, 15, 6, 10, 10, 10, 10};
    const auto before = Simulate(gpu, false), after = Simulate(gpu, true);
    Check(Uneven(before) == 2, "without the rule: a long and a short frame on screen");
    Check(Uneven(after) <= 1, "with the rule: the late frame only");
    // GPU near the pace with some frames over: the rule never makes it worse.
    const std::vector<double> busy = {11, 13, 9, 12, 14, 8, 11, 12.5, 10, 13, 9, 11, 14, 9, 10};
    Check(Uneven(Simulate(busy, true)) <= Uneven(Simulate(busy, false)), "busy GPU: no more uneven frames");
    // A game that starts each frame when its last flip was taken, the GPU finishing 3 ms after
    // every flip: even on screen, and the rule must not slow it down (every frame "late").
    int n = 0;
    double span_before = 0, span_after = 0;
    const std::vector<double> lagging(40, 6.0);
    const auto lag_before = Simulate(lagging, false, 8.0, &n, &span_before);
    const auto lag_after = Simulate(lagging, true, 8.0, &n, &span_after);
    Check(Uneven(lag_before) == 0 && Uneven(lag_after) == 0, "steady lateness: even either way");
    Check(span_after <= span_before + 12.0, "steady lateness: no lower frame rate");
    // The same game with one slow GPU frame: the rule does not leave it slower afterwards.
    std::vector<double> lag_spike(40, 6.0);
    lag_spike[10] = 11.0;
    double spike_before = 0, spike_after = 0;
    Simulate(lag_spike, false, 8.0, &n, &spike_before);
    const auto spike_shown = Simulate(lag_spike, true, 8.0, &n, &spike_after);
    Check(spike_after <= spike_before + 12.0, "one slow frame: at most a frame's time lost");
    Check(Uneven(spike_shown) <= Uneven(Simulate(lag_spike, false, 8.0)), "one slow frame: no more uneven frames");
    // Steady GPU: identical.
    const std::vector<double> steady(20, 9.0);
    Check(Uneven(Simulate(steady, true)) == 0 && Uneven(Simulate(steady, false)) == 0, "steady GPU: even");
}

void TestWatch() {
    std::atomic<std::uint64_t> gpu{0};
    std::atomic<bool> lost{false};
    BbFrameDone::Watch watch;
    Check(watch.Get(1).kind == BbFrameDone::State::Kind::Unknown, "before start: unknown");
    watch.Start([&](std::uint64_t tick, std::chrono::nanoseconds timeout, bool& failed) {
        const auto until = Clock::now() + timeout;
        while (Clock::now() < until) {
            if (lost) {
                failed = true;
                return false;
            }
            if (gpu.load() >= tick) return true;
            std::this_thread::sleep_for(200us);
        }
        return false;
    });
    watch.Note(5);
    watch.Note(7);
    watch.Note(6); // older than the last: ignored
    Check(watch.Get(5).kind == BbFrameDone::State::Kind::Pending, "noted, not finished: pending");
    Check(watch.Get(6).kind == BbFrameDone::State::Kind::Unknown, "an older tick is not noted");
    Check(watch.Get(4).kind == BbFrameDone::State::Kind::Unknown, "never noted: unknown");
    const auto before = Clock::now();
    Check(watch.WaitFor(5, Clock::now() + 20ms).kind == BbFrameDone::State::Kind::Pending, "WaitFor times out");
    Check(Clock::now() - before >= 15ms, "WaitFor waited");
    gpu = 5;
    const auto s5 = watch.WaitFor(5, Clock::now() + 1s);
    Check(s5.kind == BbFrameDone::State::Kind::Done && s5.done >= before, "finished: done with its time");
    Check(watch.Get(7).kind == BbFrameDone::State::Kind::Pending, "the next one still pending");
    gpu = 7;
    Check(watch.WaitFor(7, Clock::now() + 1s).kind == BbFrameDone::State::Kind::Done, "7 done");
    // Many frames: old finished ones go, the newest stay.
    for (std::uint64_t t = 8; t < 60; ++t) watch.Note(t);
    gpu = 59;
    Check(watch.WaitFor(59, Clock::now() + 1s).kind == BbFrameDone::State::Kind::Done, "59 done");
    Check(watch.Get(8).kind == BbFrameDone::State::Kind::Unknown, "old ones dropped");
    // A lost device: unknown from then on (the flips keep their own clock), no hang.
    watch.Note(70);
    lost = true;
    const auto s70 = watch.WaitFor(70, Clock::now() + 2s);
    Check(s70.kind == BbFrameDone::State::Kind::Unknown, "lost device: unknown, not pending");
    watch.Note(71);
    Check(watch.Get(71).kind == BbFrameDone::State::Kind::Unknown, "after a lost device nothing is noted");
    watch.Stop();
    // Stop with a frame never finished does not hang.
    lost = false;
    watch.Start([&](std::uint64_t, std::chrono::nanoseconds timeout, bool&) {
        std::this_thread::sleep_for(timeout);
        return false;
    });
    watch.Note(100);
    watch.Stop();
    Check(watch.Get(100).kind == BbFrameDone::State::Kind::Unknown, "stopped: unknown");
}
} // namespace

int main() {
    TestRule();
    TestWatch();
    if (g_failed) {
        std::printf("gpu-pacing-test: %d failed\n", g_failed);
        return 1;
    }
    std::printf("gpu-pacing-test: all passed\n");
    return 0;
}
