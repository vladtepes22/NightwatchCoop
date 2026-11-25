// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace LanternPacing {
using Clock = std::chrono::steady_clock;

bool VblankPacing();

void WatchVblank(const wchar_t* device);
struct VblankTime {
    Clock::time_point last;
    std::chrono::nanoseconds period;
};
std::optional<VblankTime> Vblank();
/// Nightwatch (diagnostics): the watch's last predicted vblank and period even while it judges the
/// refresh variable or is still learning (Vblank() returns only a steady one); nullopt before any.
std::optional<VblankTime> VblankAny();

bool Enabled();

bool Sleep(std::chrono::nanoseconds duration, bool alertable = false);

void WaitUntil(Clock::time_point deadline, bool exact,
               std::chrono::nanoseconds max_spin = std::chrono::microseconds(1500));

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
    [[nodiscard]] bool Ready(Clock::time_point now) const {
        return now + period / 16 >= slot;
    }
    void Taken(Clock::time_point now) {
        slot = now - slot > period / 4 ? now + period : slot + period;
    }
    void Tick(Clock::time_point now) {
        slot = now - slot > period ? now + period : slot + period;
    }
    /// The next flip no sooner than `t` (GpuPacedSlot).
    void PushTo(Clock::time_point t) {
        if (t > slot) slot = t;
    }
    void Align(Clock::time_point vblank, std::chrono::nanoseconds refresh,
               std::chrono::nanoseconds offset) {
        const std::int64_t r = refresh.count();
        const auto target = vblank + offset;
        const std::int64_t ahead = (slot - target).count();
        const std::int64_t k = ahead >= 0 ? (ahead + r / 2) / r : -((-ahead + r / 2) / r);
        slot = target + refresh * k;
    }

private:
    std::chrono::nanoseconds period;
    Clock::time_point slot;
};

/// Nightwatch (judder report NW-b9564e8): flips not locked to vblanks (a variable refresh display,
/// or the engine's own clock) are shown when the GPU finishes them. The last flip, taken at
/// `flipped`, finished at `done`: shown then. When that was late (more than a sixteenth of the
/// pace), the next flip is due a whole pace after it, not at its own slot, which would show the
/// next frame (already rendered) right after the late one. Without this the counter and the flip
/// times stay even while the frames on screen alternate long and short.
inline Clock::time_point GpuPacedSlot(Clock::time_point slot, Clock::time_point flipped,
                                      Clock::time_point done, std::chrono::nanoseconds pace) {
    if (pace.count() <= 0 || done - flipped <= pace / 16) {
        return slot;
    }
    return std::max(slot, done + pace);
}

/// How long a flip waits for the last frame's GPU work before going on its own clock (a frame
/// that took longer is a stall: the flips after it keep their slots as before).
inline std::chrono::nanoseconds GpuHoldLimit(std::chrono::nanoseconds pace) {
    return pace * 3;
}
}
