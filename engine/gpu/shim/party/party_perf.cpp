// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op cost probes (see party_perf.h).
#include "party_perf.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace coop::perf {

bool g_on = false;

namespace {

using u64 = std::uint64_t;

// Quarter-octave buckets over nanoseconds: 0-3 exact, then 4 per power of two (<= 19 % wide).
constexpr int kBuckets = 160;

int Bucket(u64 ns) {
    if (ns < 4) {
        return int(ns);
    }
    const int e = 63 - __builtin_clzll(ns); // >= 2
    const int idx = 4 * (e - 1) + int((ns >> (e - 2)) & 3);
    return idx < kBuckets ? idx : kBuckets - 1;
}

/// The upper end of a bucket (the next bucket's lower end), ns.
double BucketHigh(int idx) {
    if (idx < 4) {
        return double(idx + 1);
    }
    const int next = idx + 1;
    const int e = next / 4 + 1, sub = next % 4;
    return double(u64(4 + sub) << (e - 2));
}

struct Hist {
    std::array<std::atomic<u64>, kBuckets> b{};
    std::atomic<u64> n{0}, sum{0}, max{0}, over2ms{0}, over200us{0};
    void Add(u64 ns) {
        b[Bucket(ns)].fetch_add(1, std::memory_order_relaxed);
        n.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(ns, std::memory_order_relaxed);
        u64 m = max.load(std::memory_order_relaxed);
        while (ns > m && !max.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {
        }
        if (ns > 2000000) {
            over2ms.fetch_add(1, std::memory_order_relaxed);
        }
        if (ns > 200000) {
            over200us.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

struct Taken {
    std::array<u64, kBuckets> b{};
    u64 n = 0, sum = 0, max = 0, over2ms = 0, over200us = 0;
    void From(Hist& h) {
        for (int i = 0; i < kBuckets; ++i) {
            b[i] = h.b[i].exchange(0, std::memory_order_relaxed);
        }
        n = h.n.exchange(0, std::memory_order_relaxed);
        sum = h.sum.exchange(0, std::memory_order_relaxed);
        max = h.max.exchange(0, std::memory_order_relaxed);
        over2ms = h.over2ms.exchange(0, std::memory_order_relaxed);
        over200us = h.over200us.exchange(0, std::memory_order_relaxed);
    }
    /// The q quantile's bucket upper end (ns), at most the exact max.
    double Quantile(double q) const {
        u64 total = 0;
        for (u64 v : b) {
            total += v;
        }
        if (!total) {
            return 0;
        }
        const u64 want = u64(q * double(total - 1)) + 1;
        u64 seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += b[i];
            if (seen >= want) {
                const double hi = i == 0 ? 0.0 : BucketHigh(i);
                return hi < double(max) ? hi : double(max);
            }
        }
        return double(max);
    }
};

struct ZoneStats {
    Hist call;                 // every call, any thread
    Hist frame;                // per frame, tick thread
    std::atomic<u64> frame_sum{0};
    std::atomic<u64> frame_calls{0};
    std::atomic<u64> max_calls_per_frame{0};
    std::atomic<u64> off_thread{0}; // calls on other threads
};

std::array<ZoneStats, kZoneCount> g_z;
Hist g_frame_interval;
double g_ns_per_tick = 1.0;
int g_period_s = 10;
thread_local bool t_tick_thread = false;
u64 g_last_boundary = 0;

const char* kNames[kZoneCount] = {
    "tick", "lua_events", "director", "dir.read_state", "dir.npc_test", "dir.start", "progress",
    "progress.scan", "progress.guest_apply", "progress.bridge", "progress.dump", "dir.title", "dir.send",
    "status_bridge", "phantom", "dir.ring", "travel", "story", "story.sidecar", "items", "seamless",
    "seamless.params", "fourp", "hook.set_flag", "hook.set_flag_value", "hook.broadcast_flag", "hook.emk_end",
    "hook.emevd", "hook.award", "hook.give_list", "hook.funnel", "hook.lamp_warp", "hook.remo", "hook.fourp_slot",
    "hook.fourp_spfx", "log", "b2.state", "b2.work", "b2.travel", "hud.menu", "hud.plate", "hud.vitals", "balance", "pvp",
    "hook.collision", "collision.scan",
};

void Report(double secs) {
    Taken fi;
    fi.From(g_frame_interval);
    std::printf("Party perf: %.1f s, %llu frames on the coop tick (frame time p50 %.2f ms, p99 %.2f ms, max %.2f ms; "
                ">2 ms party work in %llu frames)\n",
                secs, static_cast<unsigned long long>(fi.n), fi.Quantile(0.5) / 1e6, fi.Quantile(0.99) / 1e6,
                double(fi.max) / 1e6, static_cast<unsigned long long>(g_z[0].frame.over2ms.load()));
    for (int z = 0; z < kZoneCount; ++z) {
        ZoneStats& s = g_z[z];
        Taken c, f;
        c.From(s.call);
        f.From(s.frame);
        const u64 off = s.off_thread.exchange(0, std::memory_order_relaxed);
        const u64 maxc = s.max_calls_per_frame.exchange(0, std::memory_order_relaxed);
        if (!c.n) {
            continue;
        }
        std::printf("Party perf:   %-21s per frame p50 %7.4f p99 %7.4f max %7.3f ms (%llu > 0.2 ms, %llu > 2 ms) | "
                    "%8llu calls (max %llu/frame, %llu off the tick thread) p50 %7.2f p99 %8.2f max %9.2f us, "
                    "total %.2f ms\n",
                    kNames[z], f.Quantile(0.5) / 1e6, f.Quantile(0.99) / 1e6, double(f.max) / 1e6,
                    static_cast<unsigned long long>(f.over200us), static_cast<unsigned long long>(f.over2ms),
                    static_cast<unsigned long long>(c.n), static_cast<unsigned long long>(maxc),
                    static_cast<unsigned long long>(off), c.Quantile(0.5) / 1e3, c.Quantile(0.99) / 1e3,
                    double(c.max) / 1e3, double(c.sum) / 1e6);
    }
    std::fflush(stdout);
}

void ReporterMain() {
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(g_period_s));
        const auto now = std::chrono::steady_clock::now();
        Report(std::chrono::duration<double>(now - last).count());
        last = now;
    }
}

} // namespace

const char* ZoneName(Zone z) {
    const int i = static_cast<int>(z);
    return i >= 0 && i < kZoneCount ? kNames[i] : "?";
}

u64 Now() {
#ifdef _WIN32
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return u64(t.QuadPart);
#else
    return u64(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

bool Init() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = std::getenv("BB_PARTY_PERF");
        if (!v || !v[0] || v[0] == '0') {
            return;
        }
#ifdef _WIN32
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_ns_per_tick = 1e9 / double(f.QuadPart);
#else
        g_ns_per_tick = 1.0;
#endif
        if (const char* p = std::getenv("BB_PARTY_PERF_SECONDS"); p && std::atoi(p) > 0) {
            g_period_s = std::atoi(p);
        }
        g_on = true;
        std::printf("Party perf: on (BB_PARTY_PERF=1), report every %d s\n", g_period_s);
        std::fflush(stdout);
        std::thread(ReporterMain).detach();
    });
    return g_on;
}

void Record(Zone z, u64 ticks) {
    const u64 ns = u64(double(ticks) * g_ns_per_tick);
    ZoneStats& s = g_z[static_cast<int>(z)];
    s.call.Add(ns);
    if (t_tick_thread) {
        s.frame_sum.fetch_add(ns, std::memory_order_relaxed);
        s.frame_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.off_thread.fetch_add(1, std::memory_order_relaxed);
    }
}

void FrameBoundary() {
    if (!g_on) {
        return;
    }
    t_tick_thread = true;
    const u64 now = Now();
    if (g_last_boundary) {
        g_frame_interval.Add(u64(double(now - g_last_boundary) * g_ns_per_tick));
        for (ZoneStats& s : g_z) {
            const u64 calls = s.frame_calls.exchange(0, std::memory_order_relaxed);
            const u64 sum = s.frame_sum.exchange(0, std::memory_order_relaxed);
            s.frame.Add(sum);
            u64 m = s.max_calls_per_frame.load(std::memory_order_relaxed);
            while (calls > m && !s.max_calls_per_frame.compare_exchange_weak(m, calls, std::memory_order_relaxed)) {
            }
        }
    }
    g_last_boundary = now;
}

} // namespace coop::perf
