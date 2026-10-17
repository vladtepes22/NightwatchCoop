// SPDX-License-Identifier: GPL-3.0-or-later
// Rendezvous without a server of ours: the guest's public UDP endpoint reaches a host behind a NAT
// through the public BitTorrent DHT (BEP 5 "mainline"), so hole punching needs no guest code.
//
//   key   = the party key (party_crypto: from the code's secret and the password), which only
//           members have; the DHT key is info_hash(key) = BLAKE2b("bbport rendezvous v1" || key)
//           truncated to 20 bytes: nodes see an opaque hash, nobody else can find the party.
//   guest = when its link gets no TCP answer, announce_peer(info_hash, its STUN-mapped port of the
//           party UDP port) to the 8 DHT nodes closest to the hash (iterative get_peers lookup
//           first, for their tokens). The DHT node records the request's source address, which is
//           the guest's public IP; the port is the party port's mapping. Repeated every 45 s
//           while the link is not up.
//   host  = without an open port, a get_peers lookup for info_hash every 5 s (seeded with the
//           closest nodes of the previous lookup, so one hop after the first): every endpoint
//           found is punched exactly like a pasted guest code (party_link punch_guest).
//
// What this exposes: DHT nodes see the guest's IP and the host's lookups under an opaque hash
// (like any torrent client). Only when needed (guest: TCP failed; host: no open port), and off
// unless the player opts in (BB_PARTY_RENDEZVOUS=1 / System > Party rules, on both PCs). A forged
// peer can only make the host send a few punch datagrams (the link still needs the party key);
// punching is limited per endpoint.
//
// Pure parts (bencode, KRPC messages, the iterative Lookup) take datagrams in and return
// datagrams out like party_rudp; Rendezvous owns a UDP socket and a thread.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "party/party_rudp.h"

namespace party::rdv {

using Clock = std::chrono::steady_clock;
using Id = std::array<std::uint8_t, 20>;
using rudp::Datagram;
using rudp::Endpoint;

// The DHT key for a party key (32 bytes).
Id info_hash(const std::uint8_t* party_key, std::size_t n);

// ---- bencode (the subset KRPC uses) ----
struct BNode {
    enum Kind : std::uint8_t { Int, Str, List, Dict } kind = Str;
    std::int64_t i = 0;
    std::string s;
    std::vector<BNode> list;
    std::vector<std::pair<std::string, BNode>> dict;  // kept sorted by key when encoding
    const BNode* get(const char* key) const;
    static BNode str(std::string v);
    static BNode num(std::int64_t v);
};
// Depth and size limited (64 levels, 4096 items); false on anything malformed or trailing bytes.
bool bdecode(const std::uint8_t* d, std::size_t n, BNode* out);
std::string bencode(const BNode& v);

// ---- KRPC ----
std::string make_get_peers(const std::string& tid, const Id& self, const Id& target);
std::string make_announce(const std::string& tid, const Id& self, const Id& target, std::uint16_t port_host_order,
                          const std::string& token);
std::string make_find_node(const std::string& tid, const Id& self, const Id& target);

struct Reply {
    std::string tid;
    bool is_error = false;
    bool is_query = false;
    bool has_id = false;
    Id id{};
    std::string token;
    std::vector<Endpoint> values;                     // compact peers (get_peers)
    std::vector<std::pair<Id, Endpoint>> nodes;       // compact nodes
};
bool parse_reply(const std::uint8_t* d, std::size_t n, Reply* out);

// XOR distance compare: true when a is closer to t than b.
bool closer(const Id& t, const Id& a, const Id& b);

// ---- one iterative get_peers lookup (optionally followed by announce_peer) ----
class Lookup {
public:
    struct Options {
        int alpha = 4;               // queries in flight
        std::size_t k = 8;           // closest nodes to finish on / announce to
        int query_timeout_ms = 1500;
        int total_ms = 15000;
        bool announce = false;
        std::uint16_t announce_port = 0;  // host order
        bool allow_private = false;       // accept LAN / loopback node addresses (tests)
    };
    Lookup(const Id& target, const Id& self, Options opt, Clock::time_point now);
    void seed(const Endpoint& e);
    void on_datagram(const Endpoint& from, const std::uint8_t* d, std::size_t n, Clock::time_point now);
    void poll(Clock::time_point now, std::vector<Datagram>& out);
    bool done() const { return phase_ == Phase::Done; }
    const std::vector<Endpoint>& peers() const { return peers_; }
    // The closest nodes that answered (seeds for the next lookup).
    std::vector<Endpoint> closest() const;
    int responses() const { return responses_; }
    int announced() const { return announced_; }  // announce_peer answers
    int queried() const { return queried_; }

private:
    enum class Phase : std::uint8_t { Search, Announce, Done };
    enum class NState : std::uint8_t { Fresh, InFlight, Answered, Failed };
    struct Node {
        Endpoint ep;
        Id id{};
        bool has_id = false;
        NState st = NState::Fresh;
        Clock::time_point sent{};
        std::string token;
        std::string tid;
        int tries = 0;
    };
    Node* by_tid(const std::string& tid);
    std::string next_tid();
    void add_node(const Id& id, const Endpoint& ep);
    std::vector<Node*> ranked();  // nodes with an id by distance, then seeds without one

    Id target_, self_;
    Options opt_;
    Clock::time_point start_;
    Phase phase_ = Phase::Search;
    std::vector<Node> nodes_;
    std::vector<Endpoint> peers_;
    std::uint16_t tid_ = 0;
    int responses_ = 0, announced_ = 0, queried_ = 0, announce_sent_ = 0;
    Clock::time_point announce_until_{};
};

// Node addresses a lookup may send to: public unicast IPv4 (or anything with allow_private).
bool usable_node(const Endpoint& e, bool allow_private);

// The default bootstrap nodes (BB_PARTY_DHT_BOOTSTRAP: comma list of host:port replaces them).
std::vector<std::string> default_bootstrap();

// ---- the socket + thread around Lookup ----
class Rendezvous {
public:
    using OnPeer = std::function<void(const Endpoint&)>;
    using Log = std::function<void(const std::string&)>;
    Rendezvous(const Id& target, std::vector<std::string> bootstrap, bool loopback, Log log);
    ~Rendezvous();
    Rendezvous(const Rendezvous&) = delete;
    Rendezvous& operator=(const Rendezvous&) = delete;
    // Host: look the hash up every period_ms; on_peer for each endpoint found (every time).
    bool start_host(OnPeer on_peer, int period_ms = 10000);
    // Guest: announce `port` (host order: the party port's public mapping) now and every
    // period_ms while want() says so (checked once a second).
    bool start_guest(std::uint16_t port, std::function<bool()> want, int period_ms = 45000);
    void stop();
    int lookups() const { return lookups_.load(); }
    int announces() const { return announces_.load(); }  // announce answers in total

private:
    void run(bool host, std::uint16_t port, int period_ms);
    bool open_socket();
    Id target_, self_;
    std::vector<std::string> bootstrap_;
    bool loopback_;
    Log log_;
    OnPeer on_peer_;
    std::function<bool()> want_;
    std::intptr_t sock_ = -1;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::atomic<int> lookups_{0}, announces_{0};
};

}  // namespace party::rdv
