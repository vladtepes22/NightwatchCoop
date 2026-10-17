// SPDX-License-Identifier: GPL-3.0-or-later
// party_rudp.h: a reliable stream over UDP datagrams (the party link's NAT path).
#include "party_rudp.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace party::rudp {

namespace {

void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
Clock::duration ms(double v) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(v));
}
// Sequence comparison with wrap-around.
bool seq_lt(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) < 0; }

}  // namespace

Stream::Stream(Endpoint peer, std::uint32_t conn, bool initiator, Clock::time_point now)
    : peer_(peer), conn_(conn), initiator_(initiator), created_(now), last_heard_(now) {
    if (!initiator) {
        state_ = State::Open;
        synack_due_ = true;
    }
}

std::vector<std::uint8_t> Stream::header(Type t, std::uint32_t seq) const {
    std::vector<std::uint8_t> h(kHeader, 0);
    h[0] = kMagic0;
    h[1] = kMagic1;
    h[2] = t;
    put32(h.data() + 4, conn_);
    put32(h.data() + 8, seq);
    put32(h.data() + 12, recv_next_);
    return h;
}

bool Stream::write(const std::uint8_t* d, std::size_t n) {
    if (state_ == State::Closed || state_ == State::Failed) return false;
    std::size_t buffered = pending_.size();
    for (const Seg& s : flight_) buffered += s.data.size();
    if (buffered + n > kMaxBuffered) return false;
    pending_.insert(pending_.end(), d, d + n);
    return true;
}

std::size_t Stream::read(std::uint8_t* out, std::size_t max) {
    const std::size_t k = std::min(max, in_.size());
    std::copy(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(k), out);
    in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(k));
    return k;
}

void Stream::close() {
    if (state_ == State::Open || state_ == State::Connecting) fin_due_ = true;
    if (state_ != State::Failed) state_ = State::Closed;
}

void Stream::on_packet(const std::uint8_t* d, std::size_t n, Clock::time_point now) {
    if (n < kHeader) return;
    const Type t = static_cast<Type>(d[2]);
    const std::uint32_t seq = get32(d + 8), ack = get32(d + 12);
    last_heard_ = now;
    switch (t) {
    case kSyn:
        if (!initiator_) synack_due_ = true;  // our SYNACK was lost: again
        return;
    case kSynAck:
        if (initiator_ && state_ == State::Connecting) {
            state_ = State::Open;
            const double rtt = ms_between(last_syn_ == Clock::time_point{} ? created_ : last_syn_, now);
            srtt_ms_ = std::max(1.0, rtt);
            rttvar_ms_ = srtt_ms_ / 2;
        }
        ack_now_ = true;  // tells the responder we are open
        return;
    case kFin:
        if (state_ != State::Failed) state_ = State::Closed;
        return;
    case kRst:
        state_ = State::Failed;
        return;
    default:
        break;
    }
    if (t != kData && t != kAck) return;
    // Cumulative acknowledgement of our segments.
    while (!flight_.empty() && seq_lt(flight_.front().seq, ack)) {
        const Seg& s = flight_.front();
        if (s.tries == 1) {  // Karn: only first transmissions time the round trip
            const double rtt = ms_between(s.sent, now);
            rttvar_ms_ = 0.75 * rttvar_ms_ + 0.25 * std::abs(srtt_ms_ - rtt);
            srtt_ms_ = 0.875 * srtt_ms_ + 0.125 * rtt;
        }
        flight_.pop_front();
    }
    if (t != kData) return;
    if (state_ == State::Connecting && initiator_) return;  // data before SYNACK: dropped, resent
    std::vector<std::uint8_t> payload(d + kHeader, d + n);
    if (seq == recv_next_) {
        in_.insert(in_.end(), payload.begin(), payload.end());
        ++recv_next_;
        for (auto it = ooo_.find(recv_next_); it != ooo_.end(); it = ooo_.find(recv_next_)) {
            in_.insert(in_.end(), it->second.begin(), it->second.end());
            ooo_.erase(it);
            ++recv_next_;
        }
        if (!ack_pending_) {
            ack_pending_ = true;
            ack_due_ = now + ms(20);
        }
    } else if (seq_lt(recv_next_, seq) && seq - recv_next_ < 4 * kWindow) {
        ooo_.emplace(seq, std::move(payload));
        ack_now_ = true;  // a gap: tell the sender at once
    } else {
        ack_now_ = true;  // a duplicate: our ack was lost
    }
}

void Stream::send_ack(std::vector<Datagram>& out) {
    out.push_back({peer_, header(kAck, next_seq_)});
    ack_pending_ = ack_now_ = false;
}

void Stream::poll(Clock::time_point now, std::vector<Datagram>& out) {
    if (state_ == State::Failed) return;
    if (fin_due_) {
        // smokefix (2026-10-16): what was written before close() goes out ahead of the FIN, once
        // (the stream is done after this poll: no window, no resends). The host's BYE at Party >
        // Leave never left on the UDP path: a guest linked over UDP (after the host's crash restart)
        // did not see the host end the party and waited for it to come back.
        while (!pending_.empty()) {
            const std::size_t k = std::min(kMss, pending_.size());
            std::vector<std::uint8_t> pkt = header(kData, next_seq_++);
            pkt.insert(pkt.end(), pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(k));
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(k));
            out.push_back({peer_, std::move(pkt)});
        }
        out.push_back({peer_, header(kFin, next_seq_)});
        fin_due_ = false;
        return;
    }
    if (state_ == State::Closed) return;
    if (state_ == State::Connecting) {
        if (ms_between(created_, now) > connect_timeout_ms) {
            state_ = State::Failed;
            return;
        }
        if (last_syn_ == Clock::time_point{} || ms_between(last_syn_, now) >= 250) {
            last_syn_ = now;
            out.push_back({peer_, header(kSyn, 0)});
        }
        return;
    }
    if (ms_between(last_heard_, now) > idle_timeout_ms) {
        state_ = State::Failed;
        return;
    }
    if (synack_due_) {
        out.push_back({peer_, header(kSynAck, 0)});
        synack_due_ = false;
    }
    // New segments, within the window.
    while (!pending_.empty() && flight_.size() < kWindow) {
        Seg s;
        s.seq = next_seq_++;
        const std::size_t k = std::min(kMss, pending_.size());
        s.data.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(k));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(k));
        s.sent = s.first = now;
        s.tries = 1;
        std::vector<std::uint8_t> pkt = header(kData, s.seq);
        pkt.insert(pkt.end(), s.data.begin(), s.data.end());
        out.push_back({peer_, std::move(pkt)});
        flight_.push_back(std::move(s));
        ack_pending_ = false;  // the data carries our ack
    }
    // Resends.
    const double rto = std::max(150.0, srtt_ms_ + 4 * rttvar_ms_);
    for (Seg& s : flight_) {
        const double wait = std::min(2000.0, rto * static_cast<double>(1u << std::min(s.tries - 1, 4)));
        if (ms_between(s.sent, now) >= wait) {
            ++s.tries;
            ++resends_;
            s.sent = now;
            std::vector<std::uint8_t> pkt = header(kData, s.seq);
            pkt.insert(pkt.end(), s.data.begin(), s.data.end());
            out.push_back({peer_, std::move(pkt)});
        }
    }
    if (ack_now_ || (ack_pending_ && now >= ack_due_)) send_ack(out);
    // Keepalive ack once a second while idle (the peer's idle timer).
    if (ms_between(last_ack_sent_, now) >= 1000 && flight_.empty()) {
        last_ack_sent_ = now;
        send_ack(out);
    }
}

Clock::time_point Stream::next_due() const {
    Clock::time_point t = last_heard_ + ms(idle_timeout_ms);
    if (state_ == State::Connecting) return last_syn_ + ms(250);
    if (synack_due_ || fin_due_ || ack_now_ || !pending_.empty()) return Clock::time_point{};
    if (ack_pending_) t = std::min(t, ack_due_);
    const double rto = std::max(150.0, srtt_ms_ + 4 * rttvar_ms_);
    for (const Seg& s : flight_) t = std::min(t, s.sent + ms(std::min(2000.0, rto * (1u << std::min(s.tries - 1, 4)))));
    return std::min(t, last_ack_sent_ + ms(1000));
}

// ---- Mux ----

Mux::Mux(std::uint32_t seed) : rng_(seed ? seed : 1) {}

bool Mux::on_datagram(Endpoint from, const std::uint8_t* d, std::size_t n, Clock::time_point now) {
    if (!is_rudp(d, n)) return false;
    const Type t = static_cast<Type>(d[2]);
    if (t == kPunch) return true;
    const Key k{from, get32(d + 4)};
    auto it = streams_.find(k);
    std::shared_ptr<Stream> s = it != streams_.end() ? it->second.lock() : nullptr;
    if (!s && t == kSyn && accepting) {
        s = std::make_shared<Stream>(from, k.conn, false, now);
        streams_[k] = s;
        accepted_.push_back(s);
    }
    if (s) s->on_packet(d, n, now);
    return true;
}

std::shared_ptr<Stream> Mux::connect(Endpoint to, Clock::time_point now, int connect_timeout_ms) {
    rng_ = rng_ * 1664525u + 1013904223u;
    auto s = std::make_shared<Stream>(to, rng_ | 1u, true, now);
    s->connect_timeout_ms = connect_timeout_ms;
    streams_[{to, s->conn()}] = s;
    return s;
}

std::shared_ptr<Stream> Mux::accept() {
    if (accepted_.empty()) return nullptr;
    auto s = accepted_.front();
    accepted_.pop_front();
    return s;
}

void Mux::punch(Endpoint to, Clock::time_point now, int duration_ms, int interval_ms) {
    for (Punch& p : punches_list_) {
        if (p.to == to) {
            p.until = now + ms(duration_ms);
            return;
        }
    }
    punches_list_.push_back({to, now + ms(duration_ms), now, interval_ms});
}

void Mux::linger(std::shared_ptr<Stream> s) {
    if (s) lingering_.push_back(std::move(s));
}

void Mux::poll(Clock::time_point now, std::vector<Datagram>& out) {
    for (auto it = streams_.begin(); it != streams_.end();) {
        std::shared_ptr<Stream> s = it->second.lock();
        if (!s) {
            it = streams_.erase(it);
            continue;
        }
        s->poll(now, out);
        ++it;
    }
    lingering_.clear();  // polled above: their data and FIN are out
    for (auto it = punches_list_.begin(); it != punches_list_.end();) {
        if (now >= it->until) {
            it = punches_list_.erase(it);
            continue;
        }
        if (now >= it->next) {
            std::vector<std::uint8_t> h(kHeader, 0);
            h[0] = kMagic0;
            h[1] = kMagic1;
            h[2] = kPunch;
            out.push_back({it->to, std::move(h)});
            ++punches_;
            it->next = now + ms(it->interval_ms);
        }
        ++it;
    }
}

Clock::time_point Mux::next_due(Clock::time_point now) const {
    Clock::time_point t = now + std::chrono::seconds(1);
    for (const auto& [k, w] : streams_)
        if (auto s = w.lock()) t = std::min(t, s->next_due());
    for (const Punch& p : punches_list_) t = std::min(t, p.next);
    return t;
}

// ---- guest codes ----

namespace {
constexpr const char* kB32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford
std::uint8_t crc8(const std::uint8_t* d, std::size_t n) {
    std::uint8_t c = 0x5a;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = static_cast<std::uint8_t>(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}
}  // namespace

std::string encode_guest_code(std::uint32_t addr_nbo, std::uint16_t port_nbo) {
    std::uint8_t b[7];
    std::memcpy(b, &addr_nbo, 4);
    std::memcpy(b + 4, &port_nbo, 2);
    b[6] = crc8(b, 6);
    std::uint64_t v = 0;
    for (std::uint8_t x : b) v = v << 8 | x;  // 56 bits
    std::string s;
    // 56 bits -> 12 symbols of 5 bits (the top symbol carries 1 bit).
    for (int i = 11; i >= 0; --i) s.push_back(kB32[(v >> (5 * i)) & 31]);
    return "BBG1-" + s.substr(0, 4) + "-" + s.substr(4, 4) + "-" + s.substr(8, 4);
}

bool decode_guest_code(const std::string& code, std::uint32_t* addr_nbo, std::uint16_t* port_nbo) {
    std::string s;
    for (char c : code) {
        if (c == '-' || c == ' ') continue;
        s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (s.rfind("BBG1", 0) != 0) return false;
    s = s.substr(4);
    if (s.size() != 12) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        const char* p = std::strchr(kB32, c);
        if (!p || !*p) return false;
        v = v << 5 | static_cast<std::uint64_t>(p - kB32);
    }
    if (v >> 56) return false;
    std::uint8_t b[7];
    for (int i = 6; i >= 0; --i) {
        b[i] = static_cast<std::uint8_t>(v);
        v >>= 8;
    }
    if (crc8(b, 6) != b[6]) return false;
    std::memcpy(addr_nbo, b, 4);
    std::memcpy(port_nbo, b + 4, 2);
    return *port_nbo != 0;
}

}  // namespace party::rudp
