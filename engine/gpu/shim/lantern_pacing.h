// SPDX-License-Identifier: GPL-2.0-or-later
// Lantern Engine: frame pacing. Waits that land on time (an OS timer sleep for most of the wait,
// then a spin as long as the timer's recent wake-ups needed) and an even cadence of slots for
// vblank ticks and flips. On by default on Windows; BB_PRECISE_PACING=0 turns it off (=1 on Linux).
#pragma once

#include <chrono>

namespace LanternPacing {
using Clock = std::chrono::steady_clock;

/// BB_PRECISE_PACING: on by default on Windows, off on Linux.
bool Enabled();

/// Sleeps about `duration` on the thread's high resolution timer, without spinning. False when
/// interrupted (an APC with `alertable`).
bool Sleep(std::chrono::nanoseconds duration, bool alertable = false);

/// Returns at `deadline`. `exact`: the last stretch is spun (the margin the timer needed lately,
/// at most `max_spin`), so the return is within microseconds; otherwise only the timer is used.
void WaitUntil(Clock::time_point deadline, bool exact,
               std::chrono::nanoseconds max_spin = std::chrono::microseconds(1500));

/// Slots `period` apart. An event takes a slot (Taken); the next slot is one period later, unless
/// the event came more than a quarter period late: then the cadence restarts at the event, so a
/// late frame is not followed by a burst that catches up (with G-Sync/FreeSync each flip is a
/// refresh, and uneven flips are visible judder).
class Cadence {
public:
    explicit Cadence(std::chrono::nanoseconds period, Clock::time_point start = Clock::now())
        : period{period}, slot{start} {}

    void SetPeriod(std::chrono::nanoseconds new_period) {
        period = new_period;
    }
    [[nodiscard]] std::chrono::nanoseconds Period() const {
        return period;
    }
    [[nodiscard]] Clock::time_point Slot() const {
        return slot;
    }
    /// An event at `now` may take the slot: at most a sixteenth of a period early.
    [[nodiscard]] bool Ready(Clock::time_point now) const {
        return now + period / 16 >= slot;
    }
    void Taken(Clock::time_point now) {
        slot = now - slot > period / 4 ? now + period : slot + period;
    }
    /// Ticks without an event: the next slot, restarted at `now` after a stall of over a period.
    void Tick(Clock::time_point now) {
        slot = now - slot > period ? now + period : slot + period;
    }

private:
    std::chrono::nanoseconds period;
    Clock::time_point slot;
};
} // namespace LanternPacing
