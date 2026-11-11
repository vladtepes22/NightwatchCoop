// SPDX-License-Identifier: GPL-3.0-or-later
// BB_INFLATE_BENCH=1 (off by default; diagnostics only): zstd081 phase 1. Times every hooked inflate call and, for a
// sample of streams, captures the compressed input and the inflated output in memory, recompresses the output with
// zstd (BB_INFLATE_BENCH_LEVEL, default 19) and times zstd decode against zlib-ng decode of the same stream, replayed
// one-shot on the same thread. Nothing is written to disk; the captures are freed when the stream ends. The totals are
// printed as an "Inflate bench:" line (cumulative; differences between lines give one load).
#pragma once

#include <zlib-ng.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bbport_fast_inflate.h"

namespace BbInflateBench {

inline double Now() {
    using C = std::chrono::steady_clock;
    return std::chrono::duration<double>(C::now().time_since_epoch()).count();
}

struct Cap {
    bool sampled = false;
    std::vector<std::uint8_t> in, out;
    double hooked = 0;
    bool over = false;
};

struct Totals {
    std::atomic<std::uint64_t> all_calls{0}, all_streams{0}, all_in{0}, all_out{0};
    std::atomic<std::uint64_t> all_ns{0};  // time inside the hooked zlib-ng calls, every stream
    // sampled streams only (under g_mu)
    std::uint64_t s_streams = 0, s_in = 0, s_out = 0, s_zstd = 0, s_small = 0;
    double s_zlib_replay = 0, s_zstd_dec = 0, s_hooked = 0, s_compress = 0;
};

inline Totals g_t;
inline std::mutex g_mu;
inline std::unordered_map<const void*, Cap> g_caps;
inline bool g_on = false;
inline int g_level = 19, g_every = 4;
inline std::uint64_t g_seq = 0;
inline double g_last_print = 0;

inline void Worker();
inline void Init() {
    const char* e = std::getenv("BB_INFLATE_BENCH");
    g_on = e && *e && std::string(e) != "0";
    if (const char* l = std::getenv("BB_INFLATE_BENCH_LEVEL")) g_level = std::atoi(l);
    if (const char* n = std::getenv("BB_INFLATE_BENCH_EVERY")) g_every = std::max(1, std::atoi(n));
    if (g_on) std::thread(Worker).detach();
    if (g_on) std::printf("Inflate bench: on, zstd level %d, every %d. stream sampled (zstd %s)\n", g_level, g_every, ZSTD_versionString());
}

inline void Print() {
    std::printf(
        "Inflate bench: all streams %llu calls %llu in %.1f MB out %.1f MB zlib-ng %.3f s | sampled %llu streams (%llu small) in %.1f MB "
        "out %.1f MB (zstd %.1f MB, %.1f%% of zlib) hooked %.3f s replay zlib-ng %.3f s zstd %.3f s compress %.1f s\n",
        (unsigned long long)g_t.all_streams.load(), (unsigned long long)g_t.all_calls.load(),
        g_t.all_in.load() / 1048576.0, g_t.all_out.load() / 1048576.0, g_t.all_ns.load() * 1e-9,
        (unsigned long long)g_t.s_streams, (unsigned long long)g_t.s_small, g_t.s_in / 1048576.0,
        g_t.s_out / 1048576.0, g_t.s_zstd / 1048576.0, g_t.s_in ? 100.0 * g_t.s_zstd / g_t.s_in : 0.0, g_t.s_hooked,
        g_t.s_zlib_replay, g_t.s_zstd_dec, g_t.s_compress);
    std::fflush(stdout);
}

inline void Process(Cap& c) {
    if (!c.sampled || c.over || c.out.empty()) return;
    const std::size_t n = c.out.size();
    double t = Now();
    const std::size_t bound = ZSTD_compressBound(n);
    std::vector<std::uint8_t> z(bound);
    const std::size_t zn = ZSTD_compress(z.data(), bound, c.out.data(), n, g_level);
    const double t_comp = Now() - t;
    if (ZSTD_isError(zn)) return;
    std::vector<std::uint8_t> back(n);
    static thread_local ZSTD_DCtx* dctx = ZSTD_createDCtx();
    double best_z = 1e9, best_s = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        zng_stream s{};
        zng_inflateInit2(&s, 15);
        s.next_in = c.in.data();
        s.avail_in = (unsigned)c.in.size();
        s.next_out = back.data();
        s.avail_out = (unsigned)n;
        t = Now();
        const int rc = zng_inflate(&s, Z_FINISH);
        const double dt = Now() - t;
        zng_inflateEnd(&s);
        if (rc != Z_STREAM_END) return;
        best_z = std::min(best_z, dt);
        t = Now();
        const std::size_t r = ZSTD_decompressDCtx(dctx, back.data(), n, z.data(), zn);
        const double ds = Now() - t;
        if (r != n) return;
        best_s = std::min(best_s, ds);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_t.s_streams++;
    g_t.s_in += c.in.size();
    g_t.s_out += n;
    g_t.s_zstd += zn;
    g_t.s_zlib_replay += best_z;
    g_t.s_zstd_dec += best_s;
    g_t.s_hooked += c.hooked;
    g_t.s_compress += t_comp;
    if (n < 16384) g_t.s_small++;
}

// The recompression (slow at -19) and the replays run on one background thread; the game's threads only enqueue.
inline std::mutex g_qmu;
inline std::condition_variable g_qcv;
inline std::deque<Cap> g_q;
inline std::size_t g_qbytes = 0;
inline std::uint64_t g_dropped = 0;

inline void Worker() {
    std::uint64_t last_calls = 0;
    for (;;) {
        Cap c;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(g_qmu);
            g_qcv.wait_for(lk, std::chrono::milliseconds(500), [] { return !g_q.empty(); });
            if (!g_q.empty()) {
                c = std::move(g_q.front());
                g_q.pop_front();
                g_qbytes -= c.out.size() + c.in.size();
                have = true;
            }
        }
        if (have) Process(c);
        const double now = Now();
        if (now - g_last_print >= 1.0 && g_t.all_calls.load() != last_calls) {
            g_last_print = now;
            last_calls = g_t.all_calls.load();
            std::lock_guard<std::mutex> lk(g_mu);
            Print();
        }
    }
}

inline void Finish(Cap& c) {
    if (!c.sampled || c.over || c.out.empty()) return;
    std::lock_guard<std::mutex> lk(g_qmu);
    if (g_qbytes > (192u << 20)) {
        g_dropped++;
        return;
    }
    g_qbytes += c.out.size() + c.in.size();
    g_q.push_back(std::move(c));
    g_qcv.notify_one();
}

/// One hooked inflate call. `step` performs the real call (zlib-ng, through the guest's fields).
template <class StepFn>
inline int Call(BbFastInflate::GuestZ* z, StepFn&& step) {
    const std::uint8_t* in0 = z->next_in;
    const std::uint32_t ain0 = z->avail_in;
    std::uint8_t* out0 = z->next_out;
    Cap* cap = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_caps.find(z);
        if (it == g_caps.end()) {
            it = g_caps.emplace(z, Cap{}).first;
            it->second.sampled = (g_seq++ % g_every) == 0;
        }
        cap = &it->second;  // references into unordered_map survive other inserts
    }
    const double t0 = Now();
    const int rc = step();
    const double dt = Now() - t0;
    g_t.all_calls++;
    g_t.all_ns += (std::uint64_t)(dt * 1e9);
    g_t.all_in += ain0 - z->avail_in;
    g_t.all_out += z->next_out - out0;
    cap->hooked += dt;
    if (cap->sampled && !cap->over) {
        cap->in.insert(cap->in.end(), in0, in0 + (ain0 - z->avail_in));
        cap->out.insert(cap->out.end(), out0, z->next_out);
        if (cap->out.size() > (256u << 20)) cap->over = true;
    }
    if (rc == 1) {  // Z_STREAM_END
        g_t.all_streams++;
        Cap done;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            done = std::move(*cap);
            g_caps.erase(z);
        }
        Finish(done);
    }
    return rc;
}

/// The stream is gone without Z_STREAM_END (reset / end).
inline void Drop(const void* z) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_caps.erase(z);
}

}  // namespace BbInflateBench
