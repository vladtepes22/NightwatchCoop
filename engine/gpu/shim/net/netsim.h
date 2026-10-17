// SPDX-License-Identifier: GPL-3.0-or-later
// Network simulator for the party transport: BB_NET_SIM="lat=80,jitter=20,loss=2,dup=0.5,reorder=1,seed=7"
// (or a preset: lan, wifi, dsl, mobile, bad; a preset may be followed by overrides, "dsl,seed=3")
// delays, drops, duplicates and reorders every UDP datagram the game's sockets send
// (net_socket.cpp). lat/jitter are one-way milliseconds, loss/dup/reorder percentages. Without
// reorder a destination's datagrams keep their order (jitter only spreads them); a reordered one
// is held back so later ones overtake it. Burst outages: outage=<ms>,every=<ms> drops everything
// for `outage` ms once every `every` ms (the first one `every` ms after the first datagram).
// One-shot outage: outage=<ms>,at=<ms> (`at` ms after the simulator was configured: process start).
// `jit` is an alias of `jitter`.
// Only loopback destinations / peers (127.0.0.0/8) are simulated unless `any=1`: a test setting
// that leaks into a real game never slows a real internet session.
// Both transports: every UDP datagram the game sends (Queue, sender side) and the party link's TCP
// stream (StreamDelay, receiver side: each received chunk is held until its due time, in order;
// a lost segment costs a retransmission delay instead of a drop; an outage holds everything until
// it ends). Each instance applies it to what it sends (UDP) / receives (TCP): per direction, and
// --ienv gives an asymmetric link.
// Presets: lan = lat 1 jitter 0.5; wifi = lat 5 jitter 5 loss 0.5; dsl = lat 80 jitter 20
// loss 2 dup 0.5 reorder 1; mobile = lat 60 jitter 25 loss 2 dup 0.2 reorder 1; bad = lat 200
// jitter 80 loss 8.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace bbnet::netsim {

using Clock = std::chrono::steady_clock;

// Simulated NAT kinds (RFC 4787 terms). full: endpoint-independent filtering (anything gets in
// once a mapping exists); addr: address-dependent (in from an address we sent to, any port);
// port: address-and-port-dependent (the default of nat=1); sym: port filtering plus an
// endpoint-dependent mapping (each destination sees another external port, so the port a STUN
// server reports is not the one a peer would have to use).
enum class NatKind : std::uint8_t { PortRestricted, FullCone, AddrRestricted, Symmetric };
const char* nat_kind_name(NatKind k);

struct Config {
    bool enabled = false;
    double lat_ms = 0, jitter_ms = 0, loss_pct = 0, dup_pct = 0, reorder_pct = 0;
    double outage_ms = 0, outage_every_ms = 0;
    double outage_at_ms = 0;  // one-shot outage this long after the epoch (configure())
    bool any_addr = false;    // also non-loopback addresses
    bool nat = false;         // simulated NAT: unsolicited inbound UDP dropped, inbound TCP refused
    // The NAT's kind (nat=1 / port, full, addr, sym): what passes inbound and what a STUN server
    // would report (nat_advertised_port). Only meaningful with `nat`.
    NatKind nat_kind = NatKind::PortRestricted;
    std::uint64_t seed = 1;
};

// The outage covering `now` (periodic `every` from `first`, or the one-shot `at` from `epoch`);
// false when there is none. `end` = when it is over.
bool outage_at(const Config& c, Clock::time_point epoch, Clock::time_point first, Clock::time_point now,
               Clock::time_point* end);
// True when the simulator applies to this IPv4 address (network order): loopback, or any_addr.
bool applies_to(const Config& c, std::uint32_t addr_net_order);

// The party link's TCP stream (receiver side): the due time of each chunk received at `now`,
// never earlier than the previous chunk's (a byte stream stays in order).
class StreamDelay {
public:
    StreamDelay(const Config& c, Clock::time_point epoch, std::uint64_t salt);
    Clock::time_point schedule(Clock::time_point now);
    std::uint64_t retransmits() const { return retransmits_; }
    std::uint64_t outage_holds() const { return outage_holds_; }

private:
    Config cfg_;
    Clock::time_point epoch_;
    std::mt19937_64 rng_;
    Clock::time_point last_{};
    std::uint64_t retransmits_ = 0, outage_holds_ = 0;
};

// Parses a BB_NET_SIM value. Empty, "0" and "off" give a disabled config.
bool parse(const char* spec, Config* out, std::string* error);
std::string describe(const Config& c);

struct Packet {
    Clock::time_point due;
    std::uint64_t seq = 0;
    std::uintptr_t fd = 0;
    std::uint32_t addr = 0;  // network order
    std::uint16_t port = 0;  // network order
    std::vector<std::uint8_t> data;
};

struct Stats {
    std::uint64_t submitted = 0, dropped = 0, duplicated = 0, reordered = 0, delivered = 0;
    std::uint64_t blacked_out = 0;  // dropped by an outage (counted in dropped too)
};

// The delay line, without threads or sockets (tests drive it with their own clock).
class Queue {
public:
    explicit Queue(const Config& c);
    // Schedules a datagram submitted at `now`; returns the copies queued (0: lost).
    int submit(Clock::time_point now, std::uintptr_t fd, std::uint32_t addr, std::uint16_t port, const void* data,
               std::size_t len);
    // Appends every packet due at `now` to `out`, earliest first.
    void take_due(Clock::time_point now, std::vector<Packet>& out);
    // Drops everything submitted in [from, until) (tests: a burst outage on demand).
    void blackout(Clock::time_point from, Clock::time_point until);
    // The time `at` counts from (default: the first datagram).
    void set_epoch(Clock::time_point t) { epoch_ = t; has_epoch_ = true; }
    // True when `now` is inside an outage (configured or blackout()).
    bool in_outage(Clock::time_point now);
    // The earliest due time; false when empty.
    bool next_due(Clock::time_point* when) const;
    std::size_t size() const { return heap_.size(); }
    const Stats& stats() const { return stats_; }
    const Config& config() const { return cfg_; }

private:
    double uniform();
    Clock::duration delay();
    Config cfg_;
    std::mt19937_64 rng_;
    std::vector<Packet> heap_;  // min-heap on (due, seq)
    std::unordered_map<std::uint64_t, Clock::time_point> last_due_;  // per destination
    std::uint64_t seq_ = 0;
    Stats stats_;
    Clock::time_point first_{};
    bool started_ = false;
    Clock::time_point black_from_{}, black_until_{};
    Clock::time_point epoch_{};
    bool has_epoch_ = false;
};

// A wait that honours millisecond deadlines. On Windows (MinGW) std::condition_variable and
// sleep_for round every timeout up to the 15.6 ms scheduler tick, even under timeBeginPeriod(1),
// which would add ~15 ms to every simulated delay: this waits on an event and a
// high-resolution waitable timer instead. notify() wakes the current or the next wait.
class Waiter {
public:
    Waiter();
    ~Waiter();
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    void wait_until(Clock::time_point t);
    void notify();

private:
#if defined(_WIN32)
    void* event_ = nullptr;
    void* timer_ = nullptr;
#else
    std::mutex mu_;
    std::condition_variable cv_;
    bool flag_ = false;
#endif
};

// Simulated NAT (nat=1): an inbound datagram from addr:port (network order) passes only after
// this process sent one to that endpoint (an address-and-port restricted cone NAT; the hole a
// guest or a host punches). Process-wide.
// The kind (nat=full / addr / sym) changes the rule as its comment says; nat_allow_in follows the
// process-wide config. nat_allow_in_kind: the same rule for an explicit kind (tests).
void nat_note_out(std::uint32_t addr, std::uint16_t port);
bool nat_allow_in(std::uint32_t addr, std::uint16_t port);
bool nat_allow_in_kind(NatKind kind, std::uint32_t addr, std::uint16_t port);
// The external port (network order) a STUN server number `server` (0, 1: two servers) would
// report for our local port `port_nbo`: the same port for every kind but sym, where each server
// sees another one (an endpoint-dependent mapping). Without a simulated NAT: port_nbo.
std::uint16_t nat_advertised_port(std::uint16_t port_nbo, int server = 0);

// A total outage from now for `ms` (tests: director steps host_outage / guest_outage): every UDP
// datagram this process sends is dropped and every party-link byte it receives is held until it
// ends. Works only while the simulator is on (BB_NET_SIM set, e.g. "lat=0").
void outage_now(double ms);
// The outage_now() window covering `now`, if any.
bool blackout_at(Clock::time_point now, Clock::time_point* end);

// Process-wide simulator (configured once by bbnet_configure / the first party socket).
void configure(const Config& c);
// Tests: replaces the process-wide config (and the delay line) even when one is set.
void reconfigure_for_tests(const Config& c);
bool enabled();
// The process-wide config and its epoch (configure()); disabled config when off.
Config config();
Clock::time_point epoch();
Stats stats();
// Hands a datagram to the simulator's sender thread; returns len (a lost datagram was "sent").
int sendto(std::uintptr_t fd, const void* data, std::size_t len, std::uint32_t addr, std::uint16_t port);

}  // namespace bbnet::netsim
