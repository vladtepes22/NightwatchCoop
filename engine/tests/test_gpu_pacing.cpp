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
// finished (or after its flip's frame was handed over, a pace before its flip). Returns the
// intervals between frames on screen, with or without the rule.
std::vector<double> Simulate(const std::vector<double>& gpu_ms, bool rule) {
    LanternPacing::Cadence flips{Pace, Clock::time_point{} + Pace};
    std::vector<double> shown;
    Clock::time_point last_done{}, last_flip{}, last_shown{};
    bool have = false;
    for (double g : gpu_ms) {
        Clock::time_point flip = flips.Slot();
        if (rule && have) {
            flips.PushTo(LanternPacing::GpuPacedSlot(flips.Slot(), last_flip, last_done, Pace));
            flip = flips.Slot();
        }
        const auto start = std::max(last_done, flip - Pace);
        const auto done = start + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::duration<double, std::milli>(g));
        const auto on_screen = std::max(flip, done);
        if (have) {
            shown.push_back(std::chrono::duration<double, std::milli>(on_screen - last_shown).count());
        }
        flips.Taken(flip);
        last_flip = flip;
        last_done = done;
        last_shown = on_screen;
        have = true;
    }
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
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 - 2ms, Pace) == slot, "a frame done before its flip moves nothing");
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 500us, Pace) == slot, "half a millisecond late moves nothing");
    // Late: a whole pace after the finish.
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 4ms, Pace) == t0 + 4ms + Pace, "4 ms late: a pace after the finish");
    // Never earlier than the slot.
    Check(LanternPacing::GpuPacedSlot(slot + 10ms, t0, t0 + 4ms, Pace) == slot + 10ms, "never before the slot");
    Check(LanternPacing::GpuPacedSlot(slot, t0, t0 + 4ms, std::chrono::nanoseconds(0)) == slot, "no pace: no change");

    // One frame 4 ms over in a run at the pace: without the rule a long frame and a short one on
    // screen; with it the long one only.
    const std::vector<double> gpu = {10, 10, 10, 15, 6, 10, 10, 10, 10};
    const auto before = Simulate(gpu, false), after = Simulate(gpu, true);
    Check(Uneven(before) == 2, "without the rule: a long and a short frame on screen");
    Check(Uneven(after) <= 1, "with the rule: the late frame only");
    // GPU near the pace with some frames over: the rule never makes it worse.
    const std::vector<double> busy = {11, 13, 9, 12, 14, 8, 11, 12.5, 10, 13, 9, 11, 14, 9, 10};
    Check(Uneven(Simulate(busy, true)) <= Uneven(Simulate(busy, false)), "busy GPU: no more uneven frames");
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
