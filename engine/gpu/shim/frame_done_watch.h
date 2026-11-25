// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Nightwatch (judder report NW-b9564e8, 25 Nov): when each game frame really finished on the GPU.
// The flip clock paces when frames are handed to the swapchain; on a variable refresh display (or
// any display the flips are not locked to) a frame is shown when the GPU finishes it, so a frame
// the GPU finished late was shown late, and the next one, already rendered, a moment after it:
// uneven frames on screen while the FPS counter and the flip times stay even. The presenter notes
// each game frame's tick; a thread waits for them in order and keeps the finish times of the last
// few, so the flip clock can start its next frame from the time the last one was really shown.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace BbFrameDone {
using Clock = std::chrono::steady_clock;

struct State {
    enum class Kind { Unknown, Pending, Done } kind = Kind::Unknown;
    Clock::time_point done{};
};

class Watch {
public:
    /// Waits until the GPU reached `tick` or `timeout` passed: true when reached. False with
    /// `failed` set ends the watch (a lost device): every tick is Unknown from then on.
    using Waiter = std::function<bool(std::uint64_t tick, std::chrono::nanoseconds timeout, bool& failed)>;

    Watch() = default;
    ~Watch() { Stop(); }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

    void Start(Waiter waiter);
    void Stop();
    /// A game frame's tick, in submission order (ticks only grow).
    void Note(std::uint64_t tick);
    /// Unknown: not a noted game frame (or the watch is off); Pending: not finished yet.
    State Get(std::uint64_t tick) const;
    /// Blocks until `tick` is finished or `until`; Get(tick) after it.
    State WaitFor(std::uint64_t tick, Clock::time_point until) const;

private:
    static constexpr std::size_t Kept = 16;
    struct Entry {
        std::uint64_t tick;
        Clock::time_point done; // epoch: not finished yet
    };
    State Find(std::uint64_t tick) const; // with mutex held
    void Run(std::stop_token token);

    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    std::deque<Entry> entries; // oldest first; pending ones at the back
    std::size_t next_wait = 0; // index in entries of the oldest pending one
    bool running = false;
    Waiter waiter;
    std::jthread thread;
};

/// The process's watch over the presenter's game frames.
Watch& Get();

} // namespace BbFrameDone
