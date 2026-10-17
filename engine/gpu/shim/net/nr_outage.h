#pragma once
// F5: carrying the game's reliable P2P stream through a network outage.
//
// The game's session transport (FrpgNet over the P2P socket, RE: docs/party/network_robustness.md
// section 7b) packs records into each datagram: [channel u8][length u16 BE][body]. Channel 1 is the
// reliable stream; its body starts [seq u16 BE][ack u16 BE][type u8][window u8] (type 3 / 7 data,
// +0x30 = the ack field is valid, 0x30 a bare ack, 2 a duplicate-ack report, 1 the handshake);
// channel 0 is unreliable state. Every channel-1 record resets the connection's 25 s idle deadline
// (FUN_0271b900 at the top of the record handler 0x271deb0); channel-0 records do not.
//
// Two things the transport cannot do by itself after a long outage, done here:
// 1. Keepalive: a peer that is only temporarily lost has its last reliable data record replayed to
//    the game every 2 s (ack flag cleared). An old data record is a duplicate: the transport drops
//    it (and acks it), but its idle deadline starts over. Replaying an unreliable datagram (what
//    0.6.1 did) never reached the deadline on the guest.
// 2. Resume: every reliable data record sent toward a peer while it was silent is kept, and when
//    the peer is heard again they are sent once more, in order, deduplicated by seq. Without this
//    the sender's window has collapsed to one segment and it walks the outage's backlog one
//    retransmission per round trip (2 datagrams/s live), while the game's own send queue grows
//    to its 1000-message limit and the host's game ends the session (OnIrregularLeaveSession from
//    0x17894e0, ~36 s after a 15 s outage). With the backlog delivered at once, one cumulative ack
//    clears it.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <vector>

namespace bbnet::nr {

using Clock = std::chrono::steady_clock;

struct Record {
    std::uint8_t channel = 0;
    const std::uint8_t* body = nullptr;  // after the 3-byte record header
    std::size_t len = 0;                 // body length
};

// Splits a game datagram into its records; false when it does not parse as records (then it is
// not the session transport's).
template <typename Fn>
inline bool for_each_record(const std::uint8_t* d, std::size_t n, Fn&& fn) {
    std::size_t at = 0;
    if (n < 3) return false;
    while (at < n) {
        if (n - at < 3) return false;
        const std::uint8_t ch = d[at];
        const std::size_t len = (static_cast<std::size_t>(d[at + 1]) << 8) | d[at + 2];
        if (ch > 1 || len > n - at - 3) return false;
        fn(Record{ch, d + at + 3, len});
        at += 3 + len;
    }
    return true;
}

inline std::uint8_t base_type(std::uint8_t t) { return t >= 0x30 ? static_cast<std::uint8_t>(t - 0x30) : t; }

// A reliable data record (type 3 or 7, with or without the ack flag).
inline bool is_data(const Record& r) {
    if (r.channel != 1 || r.len < 6) return false;
    const std::uint8_t b = base_type(r.body[4]);
    return b == 3 || b == 7;
}
inline std::uint16_t seq_of(const Record& r) { return static_cast<std::uint16_t>((r.body[0] << 8) | r.body[1]); }

// The record as a datagram of its own, ack flag cleared (its old ack number must not count).
inline std::vector<std::uint8_t> standalone(const Record& r) {
    std::vector<std::uint8_t> out(3 + r.len);
    out[0] = 1;
    out[1] = static_cast<std::uint8_t>(r.len >> 8);
    out[2] = static_cast<std::uint8_t>(r.len);
    for (std::size_t i = 0; i < r.len; ++i) out[3 + i] = r.body[i];
    out[3 + 4] = base_type(out[3 + 4]);
    out[3 + 2] = 0;
    out[3 + 3] = 0;
    return out;
}

// The last reliable data record in a datagram, as a keepalive datagram (empty when none).
inline std::vector<std::uint8_t> keepalive_from(const std::uint8_t* d, std::size_t n) {
    std::vector<std::uint8_t> out;
    for_each_record(d, n, [&](const Record& r) {
        if (is_data(r)) out = standalone(r);
    });
    return out;
}

// The datagram without its duplicate-ack reports (channel 1, type 2). Right after an outage the
// peer's first datagrams report the gap the outage left (the sender's in-flight segments were
// lost) before our replay has filled it; two of those put the sender into fast recovery with the
// whole backlog as its recovery point, and it then sends one segment per round trip (live: 10/s
// against the game's 30 messages/s, so its send queue overflows ~30 s later). Returns the number
// of records removed; `out` is empty when nothing else was left.
inline std::size_t strip_dup_acks(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>* out) {
    std::size_t removed = 0;
    out->clear();
    const bool ok = for_each_record(d, n, [&](const Record& r) {
        if (r.channel == 1 && r.len >= 6 && r.body[4] == 2) {
            ++removed;
            return;
        }
        out->push_back(r.channel);
        out->push_back(static_cast<std::uint8_t>(r.len >> 8));
        out->push_back(static_cast<std::uint8_t>(r.len));
        out->insert(out->end(), r.body, r.body + r.len);
    });
    if (!ok) {
        out->assign(d, d + n);
        return 0;
    }
    return removed;
}

// The transport's datagram size (the config's 0x4d0).
constexpr std::size_t kGameMtu = 0x4d0;

// Splits a datagram of records into datagrams of at most `max` bytes at record boundaries; false
// when it does not parse as records or one record alone is larger than `max`.
inline bool split_records(const std::uint8_t* d, std::size_t n, std::size_t max,
                          std::vector<std::vector<std::uint8_t>>* out) {
    out->clear();
    std::vector<std::uint8_t> cur;
    bool fits = true;
    const bool ok = for_each_record(d, n, [&](const Record& r) {
        const std::size_t sz = 3 + r.len;
        if (sz > max) fits = false;
        if (!cur.empty() && cur.size() + sz > max) {
            out->push_back(std::move(cur));
            cur.clear();
        }
        const std::uint8_t* start = r.body - 3;
        cur.insert(cur.end(), start, start + sz);
    });
    if (!cur.empty()) out->push_back(std::move(cur));
    return ok && fits;
}

// What one peer was sent, for a replay when it is heard again after a silence.
class PeerReplay {
public:
    struct Config {
        Clock::duration silent = std::chrono::seconds(4);  // what counts as an outage
        Clock::duration lead = std::chrono::seconds(3);       // sent this long before the silence began
        Clock::duration max_age = std::chrono::seconds(120);  // nothing older is replayed
        std::size_t max_records = 8192;
        std::size_t max_dgram = 1100;                         // replay datagram size (MTU 1232)
    };
    PeerReplay() = default;
    explicit PeerReplay(const Config& c) : cfg_(c) {}

    // A datagram the game sent to this peer. `last_heard` is when the peer was last heard (epoch 0:
    // never). Keeps its data records while they may still be needed.
    void on_send(Clock::time_point now, Clock::time_point last_heard, const std::uint8_t* d, std::size_t n) {
        for_each_record(d, n, [&](const Record& r) {
            if (!is_data(r)) return;
            Kept k;
            k.at = now;
            k.seq = seq_of(r);
            k.bytes = standalone(r);
            kept_.push_back(std::move(k));
        });
        // While the peer is heard, only the last `lead` is needed (what may be in flight when an
        // outage begins); in a silence everything from `lead` before it.
        const Clock::time_point keep_from =
            (last_heard.time_since_epoch().count() ? std::min(now, last_heard) : now) - cfg_.lead;
        while (!kept_.empty() && (kept_.front().at < keep_from || kept_.front().at < now - cfg_.max_age ||
                                  kept_.size() > cfg_.max_records))
            kept_.pop_front();
    }

    // The peer was heard at `now`, last before at `prev`. After a silence of at least cfg.silent,
    // returns the kept data records as datagrams (in send order, each seq once) and forgets them.
    std::vector<std::vector<std::uint8_t>> on_heard(Clock::time_point now, Clock::time_point prev,
                                                    std::size_t* records = nullptr) {
        std::vector<std::vector<std::uint8_t>> out;
        if (records) *records = 0;
        if (!prev.time_since_epoch().count() || now - prev < cfg_.silent || kept_.empty()) return out;
        std::set<std::uint16_t> seen;
        std::vector<std::uint8_t> cur;
        for (const Kept& k : kept_) {
            if (!seen.insert(k.seq).second) continue;
            if (!cur.empty() && cur.size() + k.bytes.size() > cfg_.max_dgram) {
                out.push_back(std::move(cur));
                cur.clear();
            }
            cur.insert(cur.end(), k.bytes.begin(), k.bytes.end());
            if (records) ++*records;
        }
        if (!cur.empty()) out.push_back(std::move(cur));
        kept_.clear();
        return out;
    }
    std::size_t kept() const { return kept_.size(); }

private:
    struct Kept {
        Clock::time_point at;
        std::uint16_t seq = 0;
        std::vector<std::uint8_t> bytes;
    };
    Config cfg_;
    std::deque<Kept> kept_;
};

}  // namespace bbnet::nr
