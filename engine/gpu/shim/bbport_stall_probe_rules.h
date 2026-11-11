// SPDX-License-Identifier: GPL-3.0-or-later
// Stall probe (BB_STALL_PROBE): the pure rules, unit-tested in tests/test_stall_probe.cpp.
//
// The game's main loop ends every frame in the GX device's frame end (our 0x2198fd0, called by
// the present step at 0x1d97853). In the device's queued mode that call first waits for the GX
// render thread's "done" event (play object = device +0x348, event at +0x68, the wait at
// 0x21998db) and then hands the next frame to it (+0x18, done reset, work event +0x28 set). The
// render thread (0x21b6910) sets "done" at 0x21b6a68 when it has played a frame. So a long
// main-loop frame splits into:
//   main work  = this frame-end entry - the previous frame-end exit (game logic, loads, our party
//                tick, page faults, log writes ... on the main thread);
//   frame end  = this frame-end exit - its entry (almost all of it: waiting for the render thread
//                to finish the previous frame: its Gnm submits, our HLE GPU calls).
// Found with droogie/bbhost (GPL-3.0-or-later; src/engine/live_resolution.cpp at bdc5cdf, its
// "quiet point" after the frame end); ported by intent, not code.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace BbStallProbe {

using u64 = std::uint64_t;

/// BB_STALL_PROBE: unset, empty, "0" or "off" -> 0 (off); "1" / "on" -> the default threshold
/// (250 ms); a number from 20 to 60000 -> that threshold in ms (below 20: 20).
inline int ThresholdMsFromEnv(const char* v) {
    if (!v || !*v || !std::strcmp(v, "0") || !std::strcmp(v, "off")) {
        return 0;
    }
    if (!std::strcmp(v, "1") || !std::strcmp(v, "on")) {
        return 250;
    }
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v || n <= 0) {
        return 0;
    }
    return int(std::clamp(n, 20L, 60000L));
}

struct Split {
    double main_ms = 0;  ///< the main thread's own work in the frame
    double end_ms = 0;   ///< inside the frame end: waiting for the render thread, then the hand-off
    double total_ms = 0;
};

/// prev_exit: the previous frame end's exit; entry / exit: this one's (ns, one clock). A first
/// frame (prev_exit 0) has no main-work part.
inline Split SplitFrame(u64 prev_exit, u64 entry, u64 exit) {
    Split s;
    if (exit < entry) {
        exit = entry;
    }
    if (prev_exit && entry >= prev_exit) {
        s.main_ms = double(entry - prev_exit) / 1e6;
    }
    s.end_ms = double(exit - entry) / 1e6;
    s.total_ms = s.main_ms + s.end_ms;
    return s;
}

/// Samples of one stall: where a thread was (a key per guest offset, or per host module offset
/// plus its first guest caller), at most kSlots distinct places; the rest count as "other".
/// Single writer (the sampler thread), no allocation.
class SampleTable {
public:
    static constexpr int kSlots = 48;
    struct Row {
        u64 a = 0;        ///< guest offset, or host module offset
        u64 caller = 0;   ///< host rows: the first guest return address (offset), 0 if none
        int module = -1;  ///< -1: guest code; else an index into the caller's module name list
        unsigned count = 0;
        u64 via = 0;      ///< host rows in a system DLL: the first return address into our exe (offset + 1), 0 if none
    };
    void Clear() {
        n_ = 0;
        total_ = 0;
        other_ = 0;
    }
    void Add(int module, u64 a, u64 caller, u64 via = 0) {
        ++total_;
        for (int i = 0; i < n_; ++i) {
            Row& r = rows_[i];
            if (r.module == module && r.a == a && r.caller == caller && r.via == via) {
                ++r.count;
                return;
            }
        }
        if (n_ < kSlots) {
            rows_[n_++] = Row{a, caller, module, 1, via};
            return;
        }
        ++other_;
    }
    unsigned Total() const { return total_; }
    unsigned Other() const { return other_; }
    int Size() const { return n_; }
    /// The rows by count (stable for ties: first seen first), at most `max` of them into `out`.
    int Top(Row* out, int max) const {
        Row sorted[kSlots];
        std::copy(rows_, rows_ + n_, sorted);
        std::stable_sort(sorted, sorted + n_, [](const Row& x, const Row& y) { return x.count > y.count; });
        const int k = std::min(max, n_);
        std::copy(sorted, sorted + k, out);
        return k;
    }

private:
    Row rows_[kSlots];
    int n_ = 0;
    unsigned total_ = 0, other_ = 0;
};

/// "+0x1abc 40%" / "host:ucrtbase.dll+0xee401 (from +0x114171) 30%" for the top rows; `modules`
/// names the host modules by index. Empty for no samples.
inline std::string FormatTop(const SampleTable& t, const char* const* modules, int module_count, int max_rows) {
    std::string out;
    if (!t.Total()) {
        return out;
    }
    SampleTable::Row rows[SampleTable::kSlots];
    const int n = t.Top(rows, std::min(max_rows, SampleTable::kSlots));
    char b[192];
    for (int i = 0; i < n; ++i) {
        const SampleTable::Row& r = rows[i];
        const int pct = int((100.0 * r.count) / t.Total() + 0.5);
        if (r.module < 0) {
            std::snprintf(b, sizeof b, "%s+0x%llx %d%%", i ? "; " : "", static_cast<unsigned long long>(r.a), pct);
        } else {
            const char* m = (r.module < module_count && modules[r.module]) ? modules[r.module] : "?";
            char via[48] = "";
            if (r.via) {
                std::snprintf(via, sizeof via, " via exe+0x%llx", static_cast<unsigned long long>(r.via - 1));
            }
            if (r.caller) {
                std::snprintf(b, sizeof b, "%shost:%s+0x%llx%s (from +0x%llx) %d%%", i ? "; " : "", m,
                              static_cast<unsigned long long>(r.a), via, static_cast<unsigned long long>(r.caller), pct);
            } else {
                std::snprintf(b, sizeof b, "%shost:%s+0x%llx%s %d%%", i ? "; " : "", m, static_cast<unsigned long long>(r.a),
                              via, pct);
            }
        }
        out += b;
    }
    return out;
}

/// At most `per_window` lines per `window_ns`; the dropped ones are counted and reported with the
/// next allowed line.
class RateLimit {
public:
    RateLimit(int per_window, u64 window_ns) : per_(per_window), window_(window_ns) {}
    /// True when a line may be printed at `now`; `*dropped` then holds the lines dropped since the
    /// last printed one.
    bool Allow(u64 now, unsigned* dropped) {
        if (now - start_ >= window_ || !start_) {
            start_ = now ? now : 1;
            used_ = 0;
        }
        if (used_ < per_) {
            ++used_;
            *dropped = dropped_;
            dropped_ = 0;
            return true;
        }
        ++dropped_;
        return false;
    }

private:
    int per_;
    u64 window_;
    u64 start_ = 0;
    int used_ = 0;
    unsigned dropped_ = 0;
};

} // namespace BbStallProbe
