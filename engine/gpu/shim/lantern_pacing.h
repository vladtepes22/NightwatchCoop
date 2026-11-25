// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
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
/// `flipped`, finished at `done`: shown then. When that was later than `usual` (the frames'
/// usual finish after their flips, UsualLate) by more than a sixteenth of the pace, the next flip
/// is due a whole pace after it (less the usual lateness, which the next frame has too), not at
/// its own slot, which would show the next frame (already rendered) right after the late one.
/// Without this the counter and the flip times stay even while the frames on screen alternate
/// long and short. A pipeline whose frames all finish a little after their flips is even on
/// screen and is left alone.
inline Clock::time_point GpuPacedSlot(Clock::time_point slot, Clock::time_point flipped,
                                      Clock::time_point done, std::chrono::nanoseconds pace,
                                      std::chrono::nanoseconds usual) {
    if (pace.count() <= 0 || done - flipped - usual <= pace / 16) {
        return slot;
    }
    return std::max(slot, done + pace - usual);
}

/// The frames' usual finish after their flips (0 when before): the median of the last few. A
/// frame whose flip GpuPacedSlot moved finishes early against it: not added (it would pull the
/// median down and make the next normal frame look late).
class UsualLate {
public:
    /// Enough frames seen to tell a late one.
    [[nodiscard]] bool Learned() const {
        return filled >= 5;
    }
    void Add(std::chrono::nanoseconds late) {
        recent[next] = std::max(late, std::chrono::nanoseconds(0));
        next = (next + 1) % Count;
        filled = std::min<std::size_t>(filled + 1, Count);
    }
    [[nodiscard]] std::chrono::nanoseconds Get() const {
        if (filled == 0) return std::chrono::nanoseconds(0);
        std::array<std::chrono::nanoseconds, Count> sorted{};
        std::copy_n(recent.begin(), filled, sorted.begin());
        std::nth_element(sorted.begin(), sorted.begin() + filled / 2, sorted.begin() + filled);
        return sorted[filled / 2];
    }

private:
    static constexpr std::size_t Count = 15;
    std::array<std::chrono::nanoseconds, Count> recent{};
    std::size_t next = 0, filled = 0;
};

/// How long a flip waits for the last frame's GPU work before going on its own clock (a frame
/// that took longer is a stall: the flips after it keep their slots as before).
inline std::chrono::nanoseconds GpuHoldLimit(std::chrono::nanoseconds pace) {
    return pace * 3;
}
}
