// SPDX-License-Identifier: GPL-3.0-or-later
// A small reliable, ordered byte stream over UDP datagrams, for the party link when TCP cannot
// reach the host (a host behind a NAT without UPnP / port forward). The guest's connection
// attempts and the host's "punch" datagrams to the guest's public endpoint open both NAT
// mappings (UDP hole punching; serverless rendezvous: the party code carries the host's endpoint,
// a guest code carries the guest's). In the game the datagrams share the party UDP port with the
// game's own traffic, so one opened mapping serves both.
//
// Datagram: [0xfa]['L'][type][flags][conn u32][seq u32][ack u32] + payload (DATA only, <= kMss).
//   SYN (guest -> host, retried), SYNACK, DATA (seq = first byte's stream offset / kMss units are
//   not used: seq counts segments), ACK (cumulative: every segment < ack received), FIN, PUNCH (no
//   conn; host -> a guest endpoint, opens the host's NAT mapping), RST.
// Reliability: each DATA segment is resent until acknowledged (RTO from a smoothed RTT, at least
// 150 ms, doubling up to 2 s per resend); a window of kWindow unacknowledged segments; the
// receiver buffers out-of-order segments and acknowledges cumulatively at most every 20 ms (or at
// once for a duplicate). A stream with nothing heard for idle_timeout is failed (the link's own
// lost timeout is shorter in practice: its PING runs every second).
// Pure state machine: no sockets, no threads; the caller passes datagrams in and sends what poll()
// returns. Not thread-safe by itself (PartyLink drives it under its lock).
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace party::rudp {

using Clock = std::chrono::steady_clock;

constexpr std::uint8_t kMagic0 = 0xfa, kMagic1 = 'L';
constexpr std::size_t kHeader = 16;
constexpr std::size_t kMss = 1180;      // payload per segment (fits a 1280-byte path MTU)
constexpr std::size_t kWindow = 256;    // unacknowledged segments in flight
constexpr std::size_t kMaxBuffered = 16u << 20;  // bytes waiting to be sent: past this write() fails

enum Type : std::uint8_t { kSyn = 1, kSynAck = 2, kData = 3, kAck = 4, kFin = 5, kPunch = 6, kRst = 7 };

inline bool is_rudp(const std::uint8_t* d, std::size_t n) { return n >= kHeader && d[0] == kMagic0 && d[1] == kMagic1; }

struct Endpoint {
    std::uint32_t addr = 0;  // network order
    std::uint16_t port = 0;  // network order
    bool operator<(const Endpoint& o) const { return addr != o.addr ? addr < o.addr : port < o.port; }
    bool operator==(const Endpoint& o) const { return addr == o.addr && port == o.port; }
};

struct Datagram {
    Endpoint to;
    std::vector<std::uint8_t> data;
};

class Stream {
public:
    enum class State { Connecting, Open, Closed, Failed };
    Stream(Endpoint peer, std::uint32_t conn, bool initiator, Clock::time_point now);
    State state() const { return state_; }
    Endpoint peer() const { return peer_; }
    std::uint32_t conn() const { return conn_; }
    // Queues bytes; false when the send buffer is full (kMaxBuffered).
    bool write(const std::uint8_t* d, std::size_t n);
    // Reads up to `max` in-order bytes; 0 when none.
    std::size_t read(std::uint8_t* out, std::size_t max);
    std::size_t readable() const { return in_.size(); }
    void close();
    // A datagram for this stream (already matched by peer and conn).
    void on_packet(const std::uint8_t* d, std::size_t n, Clock::time_point now);
    // Datagrams due now (handshake retries, data, resends, acks).
    void poll(Clock::time_point now, std::vector<Datagram>& out);
    // Earliest time poll() has something to do.
    Clock::time_point next_due() const;
    std::uint64_t resends() const { return resends_; }
    int srtt_ms() const { return static_cast<int>(srtt_ms_); }

    int connect_timeout_ms = 5000, idle_timeout_ms = 15000;

private:
    struct Seg {
        std::uint32_t seq;
        std::vector<std::uint8_t> data;
        Clock::time_point sent{};
        Clock::time_point first{};
        int tries = 0;
    };
    std::vector<std::uint8_t> header(Type t, std::uint32_t seq) const;
    void send_ack(std::vector<Datagram>& out);
    Endpoint peer_;
    std::uint32_t conn_ = 0;
    bool initiator_ = false;
    State state_ = State::Connecting;
    Clock::time_point created_, last_heard_, last_syn_{}, ack_due_{}, last_ack_sent_{};
    bool ack_pending_ = false, ack_now_ = false, synack_due_ = false, fin_due_ = false;
    std::deque<std::uint8_t> pending_;      // written, not yet cut into segments
    std::deque<Seg> flight_;                // sent, unacknowledged (ascending seq)
    std::uint32_t next_seq_ = 0;            // next segment number to send
    std::uint32_t recv_next_ = 0;           // next in-order segment expected
    std::map<std::uint32_t, std::vector<std::uint8_t>> ooo_;  // out-of-order segments
    std::deque<std::uint8_t> in_;           // in-order bytes for read()
    double srtt_ms_ = 200, rttvar_ms_ = 100;
    std::uint64_t resends_ = 0;
};

// Every stream on one UDP endpoint: dispatches datagrams, accepts (host), connects (guest),
// punches. `send` puts a datagram on the wire.
class Mux {
public:
    explicit Mux(std::uint32_t seed = 1);
    // Host: accept inbound SYNs.
    bool accepting = false;
    // A datagram that arrived; false when it is not ours (not is_rudp).
    bool on_datagram(Endpoint from, const std::uint8_t* d, std::size_t n, Clock::time_point now);
    // Guest: a new stream to `to` (SYN retried until SYNACK or connect_timeout).
    std::shared_ptr<Stream> connect(Endpoint to, Clock::time_point now, int connect_timeout_ms);
    // Host: the next stream that completed SYN (nullptr when none).
    std::shared_ptr<Stream> accept();
    // Host: send PUNCH datagrams to `to` every interval_ms for duration_ms (opens our NAT mapping
    // towards a guest that is about to connect).
    void punch(Endpoint to, Clock::time_point now, int duration_ms = 30000, int interval_ms = 200);
    void poll(Clock::time_point now, std::vector<Datagram>& out);
    // A stream its owner closed and lets go: kept until its next poll has sent what was written
    // before close() and the FIN (a BYE / REJECT written just before closing reaches the peer).
    void linger(std::shared_ptr<Stream> s);
    Clock::time_point next_due(Clock::time_point now) const;
    std::size_t streams() const { return streams_.size(); }
    std::uint64_t punches_sent() const { return punches_; }

private:
    struct Key {
        Endpoint ep;
        std::uint32_t conn;
        bool operator<(const Key& o) const { return ep < o.ep || (ep == o.ep && conn < o.conn); }
    };
    std::map<Key, std::weak_ptr<Stream>> streams_;
    std::deque<std::shared_ptr<Stream>> accepted_;
    std::vector<std::shared_ptr<Stream>> lingering_;
    struct Punch {
        Endpoint to;
        Clock::time_point until, next;
        int interval_ms;
    };
    std::vector<Punch> punches_list_;
    std::uint64_t punches_ = 0;
    std::uint32_t rng_;
};

// ---- guest codes: a guest's public UDP endpoint, given to the host out of band ----
// "BBG1-XXXX-XXXX" (Crockford base32 of ip[4] port[2] crc8; dashes / case ignored).
std::string encode_guest_code(std::uint32_t addr_nbo, std::uint16_t port_nbo);
bool decode_guest_code(const std::string& code, std::uint32_t* addr_nbo, std::uint16_t* port_nbo);

}  // namespace party::rudp
