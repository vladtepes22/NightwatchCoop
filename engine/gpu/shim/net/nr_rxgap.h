#pragma once
// 0.7.6 diagnostics (BB_NET_RXGAP_LOG=1, launcher: Advanced / Debug > "Network receive gap log"):
// tells a peer that really went quiet from a host whose own receive path stalled.
//
// Playtest 2026-10-24: the host logged "outage resume ... heard again after 4.0 s" for both
// guests at once, nine times. Two readings: (a) the host's reader thread (or the OS socket) did
// not run for ~4 s, (b) the peers sent nothing for ~4 s (their own loading screens, a travel).
// The reader loop wakes every 200 ms even on an idle socket, so (a) shows as a poll that took much
// longer than its timeout, or as a long stretch of work between two polls; (b) shows as a quiet
// peer while the reader loop ran on time and datagrams from other sources kept arriving.
//
// Pure bookkeeping (no sockets, no logging) so tests can drive it with a fake clock. The reader
// thread owns one instance; nothing here locks.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace bbnet::nr {

class RxGapMonitor {
public:
    using Clock = std::chrono::steady_clock;
    struct Config {
        Clock::duration gap = std::chrono::seconds(2);         // a silence this long is reported when it ends
        Clock::duration stall = std::chrono::milliseconds(250);  // a reader stall this long is remembered
        Clock::duration keep = std::chrono::seconds(90);        // stalls are remembered this long
        int max_gap_lines_per_min = 20;
    };
    // What ended: `peer` was heard again after `silent_s`.
    struct Gap {
        std::uint64_t key = 0;
        double silent_s = 0;
        double worst_stall_ms = 0;     // the reader's longest stall while the peer was silent (0: none)
        const char* stall_kind = "";   // "poll" (the wait overran its timeout) / "work" (time between polls)
        std::uint64_t others = 0;      // datagrams from other sources that arrived meanwhile
        int silent_peers = 0;          // other peers silent for at least the gap threshold right now
        int peers = 0;                 // peers known
    };
    struct Summary {
        std::uint64_t datagrams = 0;
        int peers = 0;
        int gaps = 0;           // gaps of at least `gap` that ended in this minute
        int suppressed = 0;     // of those, not logged individually (rate limit)
        double worst_stall_ms = 0;
        const char* stall_kind = "";
        std::uint64_t polls = 0, timeouts = 0;
        struct Peer {
            std::uint64_t key = 0;
            double longest_s = 0;  // the longest silence (also one still going on)
            std::uint64_t datagrams = 0;
        };
        std::map<std::uint64_t, Peer> by_peer;
    };

    RxGapMonitor() = default;
    explicit RxGapMonitor(const Config& c) : cfg_(c) {}

    // The reader is about to wait for a datagram (`timeout_ms`), and has come back from the wait.
    void poll_begin(Clock::time_point now) {
        if (ret_.time_since_epoch().count()) note_stall(now, now - ret_, "work", Clock::duration::zero());
        begin_ = now;
    }
    void poll_end(Clock::time_point now, bool timed_out, int timeout_ms) {
        ++polls_;
        if (timed_out) {
            ++timeouts_;
            note_stall(now, now - begin_, "poll", std::chrono::milliseconds(timeout_ms));
        }
        ret_ = now;
    }

    // A game datagram from `key` reached the reader. Returns true (and fills *g) when the source had
    // been silent for at least cfg.gap and the line is within the rate limit.
    bool heard(std::uint64_t key, Clock::time_point now, Gap* g) {
        ++total_;
        ++win_.datagrams;
        Peer& p = peers_[key];
        const bool known = p.last.time_since_epoch().count() != 0;
        bool fire = false;
        if (known && now - p.last >= cfg_.gap) {
            ++win_.gaps;
            const double silent = std::chrono::duration<double>(now - p.last).count();
            Summary::Peer& sp = win_.by_peer[key];
            sp.key = key;
            sp.longest_s = std::max(sp.longest_s, silent);
            if (line_budget(now)) {
                fire = true;
                g->key = key;
                g->silent_s = silent;
                g->worst_stall_ms = 0;
                g->stall_kind = "";
                for (const Stall& s : stalls_)
                    if (s.at >= p.last && s.ms > g->worst_stall_ms) {
                        g->worst_stall_ms = s.ms;
                        g->stall_kind = s.kind;
                    }
                g->others = total_ - 1 - p.total_at_last;
                g->peers = static_cast<int>(peers_.size());
                g->silent_peers = 0;
                for (const auto& [k, q] : peers_)
                    if (k != key && q.last.time_since_epoch().count() && now - q.last >= cfg_.gap)
                        ++g->silent_peers;
            } else {
                ++win_.suppressed;
            }
        }
        p.last = now;
        p.total_at_last = total_;
        ++win_.by_peer[key].datagrams;
        win_.by_peer[key].key = key;
        return fire;
    }

    // Once a minute: fills *s with the minute's shape and starts the next one. False before the minute is up.
    bool summary(Clock::time_point now, Summary* s) {
        if (!win_start_.time_since_epoch().count()) win_start_ = now;
        if (now - win_start_ < std::chrono::seconds(60)) return false;
        for (const auto& [k, p] : peers_) {  // a silence still going on counts
            if (!p.last.time_since_epoch().count()) continue;
            Summary::Peer& sp = win_.by_peer[k];
            sp.key = k;
            sp.longest_s = std::max(sp.longest_s, std::chrono::duration<double>(now - p.last).count());
        }
        win_.peers = static_cast<int>(peers_.size());
        win_.polls = polls_;
        win_.timeouts = timeouts_;
        for (const Stall& st : stalls_)
            if (st.at >= win_start_ && st.ms > win_.worst_stall_ms) {
                win_.worst_stall_ms = st.ms;
                win_.stall_kind = st.kind;
            }
        *s = std::move(win_);
        win_ = Summary{};
        polls_ = timeouts_ = 0;
        win_start_ = now;
        lines_ = 0;
        line_min_ = now;
        return true;
    }

private:
    struct Peer {
        Clock::time_point last{};
        std::uint64_t total_at_last = 0;
    };
    struct Stall {
        Clock::time_point at{};
        double ms = 0;
        const char* kind = "";
    };
    void note_stall(Clock::time_point now, Clock::duration took, const char* kind, Clock::duration expected) {
        const Clock::duration over = took - expected;
        if (over >= cfg_.stall)
            stalls_.push_back(Stall{now, std::chrono::duration<double, std::milli>(over).count(), kind});
        while (!stalls_.empty() && now - stalls_.front().at > cfg_.keep) stalls_.pop_front();
    }
    bool line_budget(Clock::time_point now) {
        if (!line_min_.time_since_epoch().count() || now - line_min_ >= std::chrono::seconds(60)) {
            line_min_ = now;
            lines_ = 0;
        }
        return lines_++ < cfg_.max_gap_lines_per_min;
    }
    Config cfg_;
    std::map<std::uint64_t, Peer> peers_;
    std::deque<Stall> stalls_;
    Clock::time_point begin_{}, ret_{}, win_start_{}, line_min_{};
    std::uint64_t total_ = 0, polls_ = 0, timeouts_ = 0;
    int lines_ = 0;
    Summary win_;
};

}  // namespace bbnet::nr
