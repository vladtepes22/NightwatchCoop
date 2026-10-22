// SPDX-License-Identifier: GPL-2.0-or-later
// Lantern Engine: frame pacing (lantern_pacing.h).
#include "lantern_pacing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <immintrin.h>
#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <time.h>
#endif

namespace LanternPacing {
namespace {
using namespace std::chrono_literals;

/// How late the last timer sleeps woke, in microseconds: a small ring shared by the threads
/// that wait (the present thread, frame generation). The spin margin is its 90th percentile, so
/// one slow wake-up does not make every later wait spin long, and a run of them does.
struct Lateness {
    static constexpr std::size_t Size = 32;
    std::array<std::atomic<std::uint32_t>, Size> us{};
    std::atomic<std::uint32_t> next{0};

    Lateness() {
        for (auto& v : us) {
            v.store(300, std::memory_order_relaxed); // until measured: 0.3 ms
        }
    }
    void Add(std::chrono::nanoseconds late) {
        const auto value = std::clamp<std::int64_t>(late.count() / 1000, 0, 4000);
        us[next.fetch_add(1, std::memory_order_relaxed) % Size].store(
            static_cast<std::uint32_t>(value), std::memory_order_relaxed);
    }
    std::chrono::nanoseconds Margin() const {
        std::array<std::uint32_t, Size> copy;
        for (std::size_t i = 0; i < Size; ++i) {
            copy[i] = us[i].load(std::memory_order_relaxed);
        }
        constexpr std::size_t P90 = Size * 9 / 10;
        std::nth_element(copy.begin(), copy.begin() + P90, copy.end());
        return std::chrono::microseconds(copy[P90] + 50);
    }
};
Lateness lateness;

#ifdef _WIN32
HANDLE ThreadTimer() {
    thread_local const HANDLE timer = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }();
    return timer;
}
#endif
} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("BB_PRECISE_PACING");
        if (env && env[0]) {
            return !(env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' ||
                     env[0] == 'F');
        }
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }();
    return on;
}

bool Sleep(std::chrono::nanoseconds duration, bool alertable) {
    if (duration <= 0ns) {
        std::this_thread::yield();
        return true;
    }
#ifdef _WIN32
    if (const HANDLE timer = ThreadTimer()) {
        LARGE_INTEGER due{.QuadPart = -std::max<LONGLONG>(1, duration.count() / 100)};
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            return WaitForSingleObjectEx(timer, INFINITE, alertable ? TRUE : FALSE) ==
                   WAIT_OBJECT_0;
        }
    }
    return SleepEx(static_cast<DWORD>(std::max<std::int64_t>(1, duration.count() / 1'000'000)),
                   alertable ? TRUE : FALSE) == 0;
#else
    timespec ts{static_cast<time_t>(duration.count() / 1'000'000'000),
                static_cast<long>(duration.count() % 1'000'000'000)};
    return nanosleep(&ts, nullptr) == 0;
#endif
}

void WaitUntil(Clock::time_point deadline, bool exact, std::chrono::nanoseconds max_spin) {
    const auto margin =
        exact ? std::clamp<std::chrono::nanoseconds>(lateness.Margin(), 100us,
                                                     std::max<std::chrono::nanoseconds>(max_spin, 100us))
              : 0ns;
    const auto start = Clock::now();
    if (const auto left = deadline - start; left > margin) {
        const auto sleep = std::chrono::duration_cast<std::chrono::nanoseconds>(left - margin);
        if (Sleep(sleep)) {
            lateness.Add(Clock::now() - start - sleep);
        }
    }
    while (Clock::now() < deadline) {
        _mm_pause();
    }
}
} // namespace LanternPacing
