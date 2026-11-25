// SPDX-License-Identifier: GPL-2.0-or-later
#include "frame_done_watch.h"

namespace BbFrameDone {

Watch& Get() {
    static Watch watch;
    return watch;
}

void Watch::Start(Waiter new_waiter) {
    Stop();
    std::scoped_lock lock{mutex};
    waiter = std::move(new_waiter);
    entries.clear();
    next_wait = 0;
    running = true;
    thread = std::jthread([this](std::stop_token token) { Run(token); });
}

void Watch::Stop() {
    if (thread.joinable()) {
        thread.request_stop();
        changed.notify_all();
        thread.join();
    }
    std::scoped_lock lock{mutex};
    running = false;
    entries.clear();
    next_wait = 0;
    changed.notify_all();
}

void Watch::Note(std::uint64_t tick) {
    std::scoped_lock lock{mutex};
    if (!running || (!entries.empty() && tick <= entries.back().tick)) {
        return;
    }
    entries.push_back({tick, Clock::time_point{}});
    Trim();
    changed.notify_all();
}

void Watch::Trim() {
    // Finished ones beyond the kept few go; a pending one is never dropped (the thread waits on it).
    while (entries.size() > Kept && next_wait > 0) {
        entries.pop_front();
        --next_wait;
    }
}

State Watch::Find(std::uint64_t tick) const {
    if (!running) {
        return {};
    }
    for (const Entry& e : entries) {
        if (e.tick == tick) {
            return e.done == Clock::time_point{} ? State{State::Kind::Pending, {}}
                                                 : State{State::Kind::Done, e.done};
        }
    }
    return {};
}

State Watch::Get(std::uint64_t tick) const {
    std::scoped_lock lock{mutex};
    return Find(tick);
}

State Watch::WaitFor(std::uint64_t tick, Clock::time_point until) const {
    std::unique_lock lock{mutex};
    State state = Find(tick);
    while (state.kind == State::Kind::Pending) {
        if (changed.wait_until(lock, until) == std::cv_status::timeout) {
            return Find(tick);
        }
        state = Find(tick);
    }
    return state;
}

void Watch::Run(std::stop_token token) {
    for (;;) {
        std::uint64_t tick;
        {
            std::unique_lock lock{mutex};
            changed.wait(lock, token, [this] { return next_wait < entries.size(); });
            if (token.stop_requested()) {
                return;
            }
            tick = entries[next_wait].tick;
        }
        bool failed = false;
        while (!waiter(tick, std::chrono::milliseconds(50), failed)) {
            if (failed || token.stop_requested()) {
                std::scoped_lock lock{mutex};
                if (failed) {
                    running = false; // no more answers: every flip keeps its own clock
                    entries.clear();
                    next_wait = 0;
                    changed.notify_all();
                }
                return;
            }
        }
        const auto now = Clock::now();
        std::scoped_lock lock{mutex};
        // Note() only appends and drops finished ones from the front, so the entry is still at
        // next_wait unless Stop() cleared the list.
        if (next_wait < entries.size() && entries[next_wait].tick == tick) {
            // A later tick finished with it (the GPU signals in order): same time for those noted.
            for (std::size_t i = next_wait; i < entries.size() && entries[i].tick <= tick; ++i) {
                entries[i].done = now;
                ++next_wait;
            }
            Trim();
        }
        changed.notify_all();
    }
}

} // namespace BbFrameDone
