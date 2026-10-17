// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: progress of shader/pipeline compilation for the on-screen indicator (bbport_overlay)
// and the BB_FRAME_STATS "Shaders:" line. Any thread may update or read it: the counters are
// atomics, a batch is a group of compilations shown together (the startup warm-up, the shader
// cache rebuild for another GPU, or compilations that queue up while playing).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace BbCompileProgress {

enum class Phase : int { Idle = 0, Startup, Rebuild, Runtime };

inline std::atomic<int> phase{int(Phase::Idle)};
inline std::atomic<std::uint32_t> total{0}, done{0}, failed{0};
/// Async graphics pipelines (BB_ASYNC_SHADERS): queued, draws skipped while one compiled, draws
/// that waited for one, and waits forced because a job stayed pending too many frames.
inline std::atomic<std::uint32_t> async_pending{0};
inline std::atomic<std::uint64_t> async_done{0}, skipped_draws{0}, sync_waits{0}, timeouts{0};
inline std::atomic<std::uint64_t> batch_start_ns{0}, last_done_ns{0};
/// Monotonic counts for the world warper (gpu/shim/party/world_warper.h; never reset): pipelines
/// created in this session (graphics + compute, new to the cache), draws skipped and GPU-thread
/// waits for a pipeline still compiling, and hitches (a flip more than 50 ms after the previous
/// one) with their summed length and the longest frame since the last TakeWorstFrameUs().
inline std::atomic<std::uint64_t> created_total{0}, skipped_total{0}, waits_total{0};
inline std::atomic<std::uint64_t> hitch_frames{0}, hitch_us_total{0}, flips_total{0};
inline std::atomic<std::uint32_t> worst_frame_us{0};
/// Hitches in whose frame a pipeline was created: the shader stutter part of the hitches.
inline std::atomic<std::uint64_t> compile_hitch_frames{0}, compile_hitch_us_total{0};
/// Time the GPU thread spent compiling pipelines itself (a frame waits for that), monotonic.
inline std::atomic<std::uint64_t> gpu_thread_compile_us{0};
inline void RecordFlipMs(double ms) {
    ++flips_total;
    const std::uint32_t us = ms <= 0 ? 0u : ms > 4.0e6 ? 4000000000u : std::uint32_t(ms * 1000.0);
    static std::uint64_t last_created = 0;
    const std::uint64_t created = created_total.load() + gpu_thread_compile_us.load();
    if (ms > 50.0) {
        ++hitch_frames;
        hitch_us_total += us;
        if (created != last_created) {
            ++compile_hitch_frames;  // a pipeline was made (or compiled on the GPU thread) in this frame
            compile_hitch_us_total += us;
        }
    }
    last_created = created;
    std::uint32_t w = worst_frame_us.load();
    while (us > w && !worst_frame_us.compare_exchange_weak(w, us)) {
    }
}
inline std::uint32_t TakeWorstFrameUs() {
    return worst_frame_us.exchange(0);
}
/// The indicator was on screen during this batch (it then stays at 100% for a moment).
inline std::atomic<bool> shown{false};
/// The startup precompile follows a rebuild of the shader cache (first launch, new GPU or
/// driver/build): the driver compiles everything cold.
inline std::atomic<bool> first_launch{false};

inline std::uint64_t NowNs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

/// Starts a batch of `n` compilations (replaces the previous one).
inline void BeginBatch(Phase p, std::uint32_t n) {
    done = 0;
    failed = 0;
    total = n;
    shown = false;
    last_done_ns = 0;
    batch_start_ns = NowNs();
    phase = int(p);
}

/// Adds `n` compilations: to the running batch, or as a new batch when the last one finished.
inline void Add(Phase p, std::uint32_t n = 1) {
    const std::uint32_t t = total.load(), d = done.load();
    if (t == 0 || d >= t) {
        BeginBatch(p, n);
    } else {
        total += n;
    }
}

/// Closes the running batch as finished (work that was counted but will not run, e.g. stale or
/// damaged cache entries): the indicator must not stay at a partial percentage.
inline void FinishBatch() {
    const std::uint32_t d = done.load();
    if (total.load() > d) {
        total = d;
        last_done_ns = NowNs();
    }
}

/// One compilation of the batch finished.
inline void Done(bool ok) {
    if (!ok) {
        ++failed;
    }
    if (++done >= total.load()) {
        last_done_ns = NowNs();
    }
}

/// Whether the indicator should be on screen, with the phase and percentage to show. Shown
/// while work remains and the batch is large (8+) or has run for 300 ms; stays at 100% for
/// 750 ms after the batch finished; capped at 99% until then.
inline bool Shown(Phase& p, int& percent) {
    const std::uint32_t t = total.load(), d = done.load();
    if (t == 0) {
        return false;
    }
    const std::uint64_t now = NowNs();
    p = Phase(phase.load());
    if (d >= t) {
        const std::uint64_t finished = last_done_ns.load();
        if (shown.load() && finished && now - finished < 750'000'000ull) {
            percent = 100;
            return true;
        }
        return false;
    }
    if (t < 8 && now - batch_start_ns.load() < 300'000'000ull) {
        return false;
    }
    percent = std::min(99, int(std::uint64_t(d) * 100 / t));
    shown = true;
    return true;
}

/// Job durations (us) for the stats line's percentiles: a small ring any thread writes.
inline std::array<std::atomic<std::uint32_t>, 512> job_us{};
inline std::atomic<std::uint32_t> job_count{0};
inline void RecordJob(std::uint32_t us) {
    job_us[job_count.fetch_add(1) % job_us.size()] = us;
}

/// BB_FRAME_STATS: one line per stats window, when anything was compiled asynchronously.
inline void PrintStats() {
    const std::uint32_t n = std::min<std::uint32_t>(job_count.exchange(0), job_us.size());
    const std::uint64_t d = async_done.exchange(0), s = skipped_draws.exchange(0),
                        w = sync_waits.exchange(0), to = timeouts.exchange(0);
    if (n == 0 && d == 0 && s == 0 && w == 0 && to == 0) {
        return;
    }
    std::vector<std::uint32_t> v(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i] = job_us[i].load();
    }
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return n ? v[std::min<std::uint32_t>(n - 1, std::uint32_t(q * n))] / 1e3 : 0.0; };
    std::printf("Shaders: async done %llu pending %u, skipped %llu, waited %llu, timeouts %llu, "
                "job ms p50/p95/max %.1f/%.1f/%.1f\n",
                static_cast<unsigned long long>(d), async_pending.load(),
                static_cast<unsigned long long>(s), static_cast<unsigned long long>(w),
                static_cast<unsigned long long>(to), at(0.5), at(0.95), n ? v[n - 1] / 1e3 : 0.0);
    // The same counts in upstream 0.5-pre4's wording (BB_ASYNC_PIPELINES), for shared log tools.
    if (d != 0 || s != 0) {
        std::printf("GPU: %llu pipelines compiled in the background, %llu draws went without "
                    "theirs meanwhile, %llu waited for one\n",
                    static_cast<unsigned long long>(d), static_cast<unsigned long long>(s),
                    static_cast<unsigned long long>(w));
    }
}

/// BB_COMPILE_PROGRESS_TEST=1: a fake batch shortly after start, to check the indicator.
inline void StartTestIfRequested() {
    const char* env = std::getenv("BB_COMPILE_PROGRESS_TEST");
    if (!env || env[0] != '1') {
        return;
    }
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        BeginBatch(Phase::Rebuild, 100);
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            Done(true);
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        BeginBatch(Phase::Startup, 300);
        for (int i = 0; i < 300; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            Done(true);
        }
    }).detach();
}

} // namespace BbCompileProgress
