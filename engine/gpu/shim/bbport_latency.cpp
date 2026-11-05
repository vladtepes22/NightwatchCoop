// SPDX-License-Identifier: GPL-2.0-or-later
// Input latency probe (bbport_latency.h).
#include "bbport_latency.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace BbLatency {

namespace {
std::atomic<std::uint64_t> g_pump_ns{0}, g_read_ns{0};
std::mutex g_mu;
std::vector<std::uint32_t> g_age_us, g_present_us, g_flip_us, g_queue_us;
std::uint64_t g_reads = 0;
std::uint64_t g_window_start = 0;

std::uint64_t NowNsRaw() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

std::uint32_t Clamp(std::uint64_t us, std::uint64_t max) {
    return std::uint32_t(std::min(us, max));
}

struct Stats {
    double avg = 0.0, p95 = 0.0, max = 0.0;
};

Stats Summary(std::vector<std::uint32_t>& v, double scale) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0.0;
    for (auto x : v) sum += x;
    s.avg = sum / double(v.size()) / scale;
    s.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))] / scale;
    s.max = v.back() / scale;
    return s;
}
} // namespace

bool Enabled() {
    static const bool on = std::getenv("BB_LATENCY_LOG") != nullptr;
    return on;
}

void NotePump() {
    if (!Enabled()) return;
    g_pump_ns.store(NowNsRaw(), std::memory_order_relaxed);
}

void NotePadRead() {
    if (!Enabled()) return;
    const auto now = NowNsRaw();
    const auto pump = g_pump_ns.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_reads;
        if (pump && now >= pump) g_age_us.push_back(Clamp((now - pump) / 1000, 1000000));
    }
    g_read_ns.store(now, std::memory_order_relaxed);
}

std::uint64_t NowNs() {
    return Enabled() ? NowNsRaw() : 0;
}

std::uint64_t LastReadNs() {
    return Enabled() ? g_read_ns.load(std::memory_order_relaxed) : 0;
}

void NotePresent(std::uint64_t read_ns, std::uint64_t flip_ns) {
    if (!Enabled() || !read_ns) return;
    const auto now = NowNsRaw();
    std::lock_guard<std::mutex> lk(g_mu);
    if (flip_ns >= read_ns) g_flip_us.push_back(Clamp((flip_ns - read_ns) / 1000, 10000000));
    if (flip_ns && now >= flip_ns) g_queue_us.push_back(Clamp((now - flip_ns) / 1000, 10000000));
    if (now >= read_ns) g_present_us.push_back(Clamp((now - read_ns) / 1000, 10000000));
    if (!g_window_start) g_window_start = now;
    if (now - g_window_start < 1000000000ull) return;
    const size_t n = g_present_us.size();
    const Stats age = Summary(g_age_us, 1.0), present = Summary(g_present_us, 1000.0),
                flip = Summary(g_flip_us, 1000.0), queue = Summary(g_queue_us, 1000.0);
    std::printf("latency: n=%zu pad_age_us avg/p95/max=%.0f/%.0f/%.0f "
                "read_to_present_ms avg/p95/max=%.2f/%.2f/%.2f reads=%llu "
                "read_to_flip_ms avg/p95/max=%.2f/%.2f/%.2f flip_to_present_ms avg/p95/max=%.2f/%.2f/%.2f\n",
                n, age.avg, age.p95, age.max, present.avg, present.p95, present.max,
                (unsigned long long)g_reads, flip.avg, flip.p95, flip.max, queue.avg, queue.p95, queue.max);
    g_age_us.clear();
    g_present_us.clear();
    g_flip_us.clear();
    g_queue_us.clear();
    g_reads = 0;
    g_window_start = now;
}

} // namespace BbLatency
