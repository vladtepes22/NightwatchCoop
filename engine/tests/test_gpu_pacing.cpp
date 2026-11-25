// SPDX-License-Identifier: GPL-2.0-or-later
// Judder with a steady FPS counter (NW-b9564e8): BbFrameDone::Watch, when each game frame really
// finished on the GPU (the videoout Pacing line counts frames as shown from it).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "gpu/shim/frame_done_watch.h"

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
    TestWatch();
    if (g_failed) {
        std::printf("gpu-pacing-test: %d failed\n", g_failed);
        return 1;
    }
    std::printf("gpu-pacing-test: all passed\n");
    return 0;
}
