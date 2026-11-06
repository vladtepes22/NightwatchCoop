// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny fork-join helper and the build's progress / timing bookkeeping (no dependency on the game or the
// party code, so the table builder and its tests share it).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace coop::gamedata {

/// Weighted progress of the table build: units are bytes of input files, so a big map file counts for more
/// than a small script. Thread safe.
class Progress {
public:
    void SetTotal(std::uint64_t total) { total_.store(total ? total : 1); }
    void Add(std::uint64_t n) { done_.fetch_add(n); }
    void SetLabel(const std::string& l) {
        std::lock_guard<std::mutex> lk(mu_);
        label_ = l;
    }
    std::string Label() const {
        std::lock_guard<std::mutex> lk(mu_);
        return label_;
    }
    int Percent() const {
        const std::uint64_t t = total_.load(), d = done_.load();
        const std::uint64_t p = d >= t ? 100 : d * 100 / t;
        return static_cast<int>(p > 99 ? 99 : p);  // 100 only when the build says it is done
    }

private:
    std::atomic<std::uint64_t> done_{0}, total_{1};
    mutable std::mutex mu_;
    std::string label_;
};

/// Wall time per step, for the build log.
struct Timings {
    std::mutex mu;
    std::vector<std::pair<std::string, double>> steps;
    void Note(const std::string& name, double ms) {
        std::lock_guard<std::mutex> lk(mu);
        steps.emplace_back(name, ms);
    }
};

class StepTimer {
public:
    StepTimer(Timings* t, std::string name) : t_(t), name_(std::move(name)), t0_(std::chrono::steady_clock::now()) {}
    ~StepTimer() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count();
        if (t_) t_->Note(name_, ms);
        if (std::getenv("BB_PARTY_DATA_TRACE")) std::fprintf(stderr, "[party-data] %s %.0f ms\n", name_.c_str(), ms);
    }

private:
    Timings* t_;
    std::string name_;
    std::chrono::steady_clock::time_point t0_;
};

/// Runs fn(i) for i in [0, n) on up to `threads` threads (this thread included). Results are written by index
/// by the callers, so the outcome does not depend on scheduling. The exception of the lowest failing index is
/// rethrown after every thread ended.
inline void ParallelFor(std::size_t n, unsigned threads, const std::function<void(std::size_t)>& fn) {
    if (n == 0) return;
    const unsigned want = static_cast<unsigned>(std::min<std::size_t>(threads ? threads : 1, n));
    std::atomic<std::size_t> next{0};
    std::mutex emu;
    std::exception_ptr err;
    std::size_t err_index = static_cast<std::size_t>(-1);
    std::atomic<bool> failed{false};
    auto worker = [&](bool lower_priority) {
#ifdef _WIN32
        if (lower_priority) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
        (void)lower_priority;
#endif
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= n || failed.load()) return;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> lk(emu);
                if (i < err_index) {
                    err_index = i;
                    err = std::current_exception();
                }
                failed.store(true);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < want; ++k) pool.emplace_back(worker, true);
    worker(false);
    for (std::thread& t : pool) t.join();
    if (err) std::rethrow_exception(err);
}

/// Worker count for the build: cores - 1 (the game keeps one), at least 1; BB_PARTY_TABLES_THREADS overrides.
unsigned DefaultThreads();

}  // namespace coop::gamedata
