// SPDX-License-Identifier: GPL-3.0-or-later
// The party port's route table (F2, 0.7.1; docs/party/network_robustness.md 4a): IPv6 and the
// member relay under the IPv4 endpoints the game knows. Every datagram the party port sends or
// receives passes Router (net_socket.cpp), so the game, the party link's reliable UDP stream,
// STUN and the host relay keep seeing IPv4 endpoints ("keys") and need no change.
//
//  - IPv6: a key bound to an IPv6 endpoint (bind_v6) is sent there when its route is on, and
//    datagrams from that endpoint are presented as the key. An IPv6 source with no binding gets a
//    synthetic key 240.x.y.z:port (240/5 is reserved and never routed). The key is a keyed hash
//    (SipHash-2-4 under a per-process random key) of the address: nobody can aim at another peer's
//    key, and a clash never evicts a bound entry (the newcomer probes for a free key, or is
//    dropped). Keys are local to each PC; other PCs learn them from the host (room records,
//    net6_table). Synthetic entries: at most kMaxV6Per64 per /64 and kMaxV6Per48 per /48; a full
//    prefix or table makes room by evicting its oldest untrusted entry (trusted: a party member's,
//    mark_trusted; bound entries never go). throttle_key() puts a whole /56 under one key (password
//    / relay lockouts cannot be dodged by rotating addresses).
//  - Member relay, frames [0xfc][type] on the party port:
//      Q1 hello     guest -> member  [fc 'Q' 1][cn 16][Ya 32][pad]   (72 bytes: no amplification)
//      Q2 point     member -> guest  [fc 'Q' 2][cn 16][Yb 32][cookie 16]  (stateless; nothing password-checkable)
//      Q3 confirm   guest -> member  [fc 'Q' 3][cn 16][Ya 32][Yb 32][cookie 16][confirm A 16]
//                   (the cookie, a MAC of the member's over the guest's endpoint, cn, Ya, Yb and the
//                   minute, is checked with one BLAKE2b before any curve work: a forged Q3 costs nothing)
//      Q4 welcome   member -> guest  [fc 'Q' 4][cn 16][session u32][name len u8][name][confirm B 16]
//      Q5 refusal   member -> guest  [fc 'Q' 5][cn 16][code u8][wait s u16]   (a hint only)
//      T  to relay  guest -> member  [fc 'T'][session u32][counter u64][mac 16] + a datagram for the host
//                   (empty: keepalive); sealed under a key from the PAKE's ISK, with a replay window
//      V  to host   member -> host   [fc 'V'][flags][key id u32][counter u64][guest addr 4][port 2][mac 16] + datagram
//      v  from host host -> member   [fc 'v'][key id u32][counter u64][guest addr 4][port 2][mac 16] + datagram
//      t  to guest  member -> guest  [fc 't'][session u32][counter u64][mac 16] + that datagram (likewise)
//      N  nonce     host -> member over IPv6 [fc 'N'][nonce 16][mac 16] (MAC under the party's nonce
//                   key: strangers cannot fill the slots): the member echoes it over the link
//                   (net6_echo) before the host lists its IPv6 endpoint (net6_table)
//    The Q handshake is a PAKE (CPace on X25519, Pake): the generator comes from the party's relay
//    key (from the code and the password) and the guest's nonce, each side sends its point, both
//    derive ISK = H(cn, Ya, Yb, K) and confirm it (the guest first, counted by the member's lockout;
//    then the member). A stranger, an eavesdropper or an impostor member gets no offline password
//    test, only one online guess per handshake, and the member's guesses are throttled. V / v
//    are MACed (KeyedMac) under the member's own random key, which the member gives the host over its
//    encrypted, authenticated party link (relay_key event), with a counter and a replay window: a
//    stranger, even on the member's address, cannot inject, replay or redirect relayed traffic.
//  - Security (F3): a member serves only while told to (in the party, its own link not relayed, the
//    switch on); wrong proofs go to a Throttle (party_link.h AuthThrottle, by throttle_key); it
//    forwards only between its sessions (kMaxSessions, rate-limited, idle kSessionIdle) and its own
//    host endpoint; a live session is never replaced by a replayed or recent handshake. The host
//    accepts V only under a registered member key, from that member's link address, bound to the
//    first endpoint it came from; at most kMaxRoutesPerMember guests per member; a route moves to
//    another member only after it was quiet kRouteMoveIdle and the new member's session has just
//    started; a new route never for an endpoint heard directly within kDirectGuard, nor for a
//    loopback / unspecified / multicast / own address (unless the member itself is on loopback: tests).
// Pure state machine plus a mutex: no sockets, no threads (party-nat-test drives it over
// simulated NATs).
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bbnet::udp {

using RClock = std::chrono::steady_clock;

struct Ip6 {
    std::array<std::uint8_t, 16> b{};
    bool operator<(const Ip6& o) const { return b < o.b; }
    bool operator==(const Ip6& o) const { return b == o.b; }
};
struct Ep6 {
    Ip6 addr;
    std::uint16_t port = 0;  // network order
    bool operator<(const Ep6& o) const { return addr < o.addr || (addr == o.addr && port < o.port); }
    bool operator==(const Ep6& o) const { return addr == o.addr && port == o.port; }
};

bool parse_ip6(const std::string& text, Ip6* out);  // "2001:db8::1", "::1" ("[...]" accepted)
std::string ip6_text(const Ip6& a);                 // compressed form
bool ip6_global(const Ip6& a);                      // 2000::/3 (global unicast)
bool ip6_loopback(const Ip6& a);                    // ::1
bool ip6_zero(const Ip6& a);
std::string key_text(std::uint32_t addr_nbo, std::uint16_t port_nbo);  // "a.b.c.d:port"

// A synthetic key's address range (240.0.0.0 - 247.255.255.255).
bool is_synthetic(std::uint32_t addr_nbo);
// Cryptographically random bytes (BCryptGenRandom; rand_s / /dev/urandom as the fallback).
void secure_random(void* out, std::size_t n);

constexpr std::uint8_t kRouteMagic = 0xfc;
inline bool is_route_frame(const std::uint8_t* d, std::size_t n) { return n >= 2 && d[0] == kRouteMagic; }

// What goes on the wire: an IPv4 datagram or an IPv6 one.
struct Wire {
    bool v6 = false;
    std::uint32_t addr = 0;  // IPv4, network order
    std::uint16_t port = 0;  // network order (both families)
    Ip6 addr6;
    std::vector<std::uint8_t> data;
};

class Router {
public:
    Router();
    // The relay handshake's PAKE (CPace on X25519; the party runtime's party_crypto): the generator
    // for a session id from the relay key, X25519 (false: a low-order result), BLAKE2b-256.
    struct Pake {
        std::function<void(const std::uint8_t sid[16], std::uint8_t g[32])> generator;
        std::function<bool(const std::uint8_t scalar[32], const std::uint8_t point[32], std::uint8_t out[32])> x25519;
        std::function<void(const std::uint8_t* d, std::size_t n, std::uint8_t out[32])> hash;
    };
    // A MAC (16 bytes) under an explicit 32-byte key (the members' relay keys, the PAKE's ISK).
    using KeyedMac = std::function<void(const std::uint8_t key[32], const std::uint8_t* d, std::size_t n,
                                        std::uint8_t out[16])>;
    // Online password guessing at the member (party_link.h AuthThrottle over a throttle key).
    struct Throttle {
        std::function<bool(std::uint32_t ip, std::int64_t* remaining_ms)> locked;
        std::function<void(std::uint32_t ip)> fail, success;
    };
    // The member's key for its V / v frames (given to the host over the party link).
    struct MemberKey {
        std::uint32_t id = 0;
        std::array<std::uint8_t, 32> key{};
    };
    static MemberKey new_member_key();

    static constexpr std::size_t kMaxSessions = 4;
    static constexpr std::size_t kMaxHostRoutes = 16;
    static constexpr std::size_t kMaxRoutesPerMember = 4;
    static constexpr std::size_t kMaxMemberKeys = 8;
    static constexpr std::size_t kMaxV6 = 1024;
    static constexpr std::size_t kMaxV6Per64 = 32;
    static constexpr std::size_t kMaxV6Per48 = 64;
    static constexpr std::size_t kMaxNonceSlots = 32;

    static constexpr auto kSessionIdle = std::chrono::seconds(30);
    static constexpr auto kSessionReplaceIdle = std::chrono::seconds(5);
    static constexpr auto kSessionStart = std::chrono::seconds(3);
    static constexpr auto kHostRouteIdle = std::chrono::seconds(60);
    static constexpr auto kRouteMoveIdle = std::chrono::seconds(10);
    static constexpr auto kDirectGuard = std::chrono::seconds(10);
    static constexpr auto kHandshakeTimeout = std::chrono::seconds(4);
    static constexpr auto kKeepalive = std::chrono::seconds(4);
    // Per relayed session at the member: datagrams and bytes per second (bursts twice that). In
    // play a relayed guest sends ~3 KB/s; a join's snapshot and mirror image are ~15 KB.
    static constexpr double kSessionFrameRate = 300, kSessionByteRate = 128.0 * 1024;
    static constexpr std::size_t kQ1Size = 72;
    // Relay handshakes per source /56 and per second (Q1 and Q3 each), and for everyone together.
    static constexpr double kHandshakeRatePerGroup = 5, kHandshakeRateGlobal = 200;
    // Of the global budget, this much is kept for sources whose /56 completed a handshake within the
    // last hour or holds a party member's key (L2: a flood of strangers cannot starve the party).
    static constexpr double kHandshakeRateReserved = 50;
    // A member whose confirmation failed (an impostor, or another party's) is not asked again for
    // kBackoffFirst, doubling to kBackoffMax. An unauthenticated refusal (Q5) sets at most
    // kBackoffFirst and never grows it (L1: anyone can send one).
    static constexpr auto kBackoffFirst = std::chrono::seconds(30);
    static constexpr auto kBackoffMax = std::chrono::hours(1);
    enum Refusal : std::uint8_t { kWrongKey = 1, kLockedOut = 2, kNotServing = 3, kFull = 4, kBackoff = 0xfe, kTimeout = 0xff };

    // The MAC for V / v frames (the party runtime's BLAKE2b); without one no V / v is sent or taken.
    void set_keyed_mac(KeyedMac mac);

    // ---- IPv6 ----
    void set_ipv6(bool on);
    bool ipv6() const;
    // `key` (the IPv4 endpoint the game knows) is reached at `e6`: datagrams from e6 are presented
    // as the key; datagrams to the key go to e6 while `route` is on. False: e6 is bound to another key.
    bool bind_v6(std::uint32_t key_addr, std::uint16_t key_port, const Ep6& e6, bool route);
    void route_v6(std::uint32_t key_addr, std::uint16_t key_port, bool route);
    void unbind_v6(std::uint32_t key_addr, std::uint16_t key_port);
    bool v6_of(std::uint32_t key_addr, std::uint16_t key_port, Ep6* e6, bool* route = nullptr) const;
    // The key an IPv6 source is presented as (its binding, else its synthetic key, remembered).
    // False: none could be given (the table or the source's /64 is full of live entries).
    bool key_of(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port);
    // The key e6 has now, without making one.
    bool known_key(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port) const;
    // The address password and relay lockouts count under: the IPv6 /56 of a synthetic key (looked
    // up by address and port), the address itself otherwise.
    std::uint32_t throttle_key(std::uint32_t addr, std::uint16_t port) const;
    // A key the party knows (a member's link endpoint): never evicted to make room for strangers.
    void mark_trusted(std::uint32_t addr, std::uint16_t port);
    // Tests: the synthetic-key hash (address -> 27 bits).
    void set_key_hash_for_tests(std::function<std::uint32_t(const Ip6&)> h);

    // ---- IPv6 endpoint checks (M3) ----
    // The party's key for the nonce frames (from the relay key: party members only).
    void set_nonce_key(const std::array<std::uint8_t, 32>& key);
    // The nonce frame the host sends to a member's claimed IPv6 endpoint (MACed under the nonce key).
    Wire nonce_wire(const Ep6& to, const std::array<std::uint8_t, 16>& nonce) const;
    // Curve operations the member side has done (tests: a forged Q3 costs none).
    std::uint64_t curve_ops() const;
    // Nonces that reached us over IPv6 since the last call (the member echoes them over the link).
    std::vector<std::array<std::uint8_t, 16>> take_nonces();

    // ---- member relay: the guest that cannot reach the host ----
    enum class ClientState { Off, Handshake, Ready, Refused };
    // Reach our host (its key) through the member at `member` (its key): the handshake starts now.
    void relay_use(std::uint32_t member_addr, std::uint16_t member_port, std::uint32_t host_addr,
                   std::uint16_t host_port, Pake pake, RClock::time_point now);
    void relay_stop();
    // `name`: the member's name from its welcome; `code`: the refusal (Refusal); `wait_s`: its wait.
    ClientState relay_state(std::string* name = nullptr, int* code = nullptr, int* wait_s = nullptr) const;
    bool relay_member(std::uint32_t* addr, std::uint16_t* port) const;

    // ---- member relay: the member serving ----
    // Serve guests of our party that cannot reach our host (its key, as our link reaches it); `key`
    // is ours for the V / v frames (the host got it over our party link).
    void serve(bool on, std::uint32_t host_addr, std::uint16_t host_port, Pake pake, std::string own_name,
               Throttle throttle, const MemberKey& key);
    bool serving() const;
    struct Served {
        std::uint32_t addr = 0;
        std::uint16_t port = 0;
        std::uint64_t up = 0, down = 0;  // datagrams to / from the host
    };
    std::vector<Served> served() const;

    // ---- the host ----
    void accept_relayed(bool on);
    // A member's relay key from its party link (`ip`: the address its link comes from); replaces an
    // earlier key of that member (`member`: its name).
    void set_member_key(const MemberKey& key, std::uint32_t ip, const std::string& member);
    void drop_member(const std::string& member);
    // This PC's own addresses: never a relayed guest's.
    void set_own_addrs(std::vector<std::uint32_t> addrs);
    struct HostRoute {
        std::uint32_t guest_addr = 0, member_addr = 0;
        std::uint16_t guest_port = 0, member_port = 0;
        std::string member;
    };
    std::vector<HostRoute> host_routes() const;
    bool relayed_guest(std::uint32_t addr, std::uint16_t port, std::uint32_t* member_addr = nullptr,
                       std::uint16_t* member_port = nullptr) const;

    // ---- datagrams ----
    // A datagram for key to_addr:to_port: what goes on the wire, appended to *out (nothing when it
    // cannot be sent: a synthetic key without IPv6, the relay handshake still running).
    void outgoing(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                  RClock::time_point now, std::vector<Wire>* out);
    // An IPv6 datagram for key to_addr:to_port regardless of its route (probes); false: no binding.
    bool outgoing_v6(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                     std::vector<Wire>* out);
    // A datagram that arrived (`from`: its source and bytes). True: deliver `*data` from key
    // *src_addr:*src_port to the layers above (*via_v6: it came over IPv6; *via_relay: through a
    // member relay). False: consumed (answers, if any, appended to *out).
    bool incoming(const Wire& from, RClock::time_point now, std::vector<std::uint8_t>* data, std::uint32_t* src_addr,
                  std::uint16_t* src_port, std::vector<Wire>* out, bool* via_v6 = nullptr, bool* via_relay = nullptr);
    // Handshake retries, keepalives and expiry.
    void poll(RClock::time_point now, std::vector<Wire>* out);
    // Log lines since the last call (path changes, sessions, refusals).
    std::vector<std::string> take_notes();

private:
    struct V6 {
        Ep6 e6;
        bool route = false;
        bool synthetic = false;
        bool trusted = false;
        RClock::time_point seen{};
    };
    struct Bucket {
        double tokens = 0, rate = 0, burst = 0;
        RClock::time_point at{};
        bool take(double n, RClock::time_point now);
    };
    struct Replay {  // a 64-counter sliding window
        std::uint64_t top = 0, mask = 0;
        bool seen_any = false;
        bool accept(std::uint64_t c);
    };
    struct Session {
        std::uint32_t addr = 0;
        std::uint16_t port = 0;
        std::uint32_t id = 0;
        std::array<std::uint8_t, 16> cn{};
        std::array<std::uint8_t, 32> k_up{}, k_down{};  // T / t keys from the ISK
        std::uint64_t down_ctr = 0;
        Replay up_in;
        RClock::time_point created{}, last{};
        Bucket frames, bytes;
        std::uint64_t up = 0, down = 0, dropped = 0;
    };
    struct Member {
        std::string name;
        std::array<std::uint8_t, 32> key{};
        std::uint32_t ip = 0;              // its link's address
        std::uint32_t bound_addr = 0;      // where its frames come from (the first authenticated one)
        std::uint16_t bound_port = 0;
        RClock::time_point last{};
        std::uint64_t out_ctr = 0;
        Replay in;
    };
    struct HostRouteState {
        std::uint32_t guest_addr = 0;
        std::uint16_t guest_port = 0;
        std::uint32_t member_id = 0;
        RClock::time_point last{};
    };
    static std::uint64_t k(std::uint32_t a, std::uint16_t p) { return a | (static_cast<std::uint64_t>(p) << 32); }
    void note(std::string s);
    bool key_of_locked(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port);
    std::uint32_t hash27(const Ip6& a) const;
    std::uint32_t group_of_locked(std::uint32_t addr, std::uint16_t port) const;
    void wire_to(std::uint32_t addr, std::uint16_t port, std::vector<std::uint8_t> data, std::vector<Wire>* out) const;
    void member_scalar(std::uint32_t a, std::uint16_t p, const std::uint8_t* cn, const std::uint8_t* ya_pub,
                       std::int64_t minute, std::uint8_t out[32]) const;
    static void session_key(const Pake& pk, const std::uint8_t* cn, const std::uint8_t* ya, const std::uint8_t* yb,
                            const std::uint8_t* kk, std::uint8_t out[32]);
    void client_send_hello(RClock::time_point now, std::vector<Wire>* out);
    void client_send_auth(RClock::time_point now, std::vector<Wire>* out);
    void cookie(std::uint32_t a, std::uint16_t p, const std::uint8_t* cn, const std::uint8_t* ya, const std::uint8_t* yb,
                std::int64_t minute, std::uint8_t out[16]) const;
    bool budget(bool confirm, std::uint32_t group, bool party, RClock::time_point now);
    void member_failed(RClock::time_point now, bool authenticated);
    bool valid_guest_key(std::uint32_t ga, std::uint16_t gp, std::uint32_t member_src) const;
    bool on_frame(std::uint32_t sa, std::uint16_t sp, bool v6, std::vector<std::uint8_t>& d, RClock::time_point now,
                  std::uint32_t* src_addr, std::uint16_t* src_port, std::vector<Wire>* out, bool* via_relay);

    mutable std::mutex mu_;
    std::vector<std::string> notes_;
    KeyedMac kmac_;
    std::array<std::uint8_t, 16> hash_key_{};
    std::function<std::uint32_t(const Ip6&)> test_hash_;
    // IPv6
    bool ipv6_ = false;
    std::map<std::uint64_t, V6> v6_by_key_;
    std::map<Ep6, std::uint64_t> key_by_v6_;
    std::map<std::uint64_t, std::array<std::uint8_t, 16>> nonces_;  // by the source's /48
    // client
    struct Client {
        ClientState st = ClientState::Off;
        std::uint32_t m_addr = 0, h_addr = 0;
        std::uint16_t m_port = 0, h_port = 0;
        Pake pake;
        std::array<std::uint8_t, 16> cn{};
        std::array<std::uint8_t, 32> ya_s{}, ya{}, yb{}, isk{}, k_up{}, k_down{};
        std::array<std::uint8_t, 16> cookie{};
        std::uint64_t up_ctr = 0;
        Replay down_in;
        bool have_isk = false;
        bool impostor_noted = false;
        std::uint32_t session = 0;
        RClock::time_point started{}, next_send{}, last_sent{};
        std::string name;
        int code = 0, wait_s = 0;
    } cl_;
    // server
    bool serving_ = false;
    std::uint32_t sv_h_addr_ = 0;
    std::uint16_t sv_h_port_ = 0;
    Pake sv_pake_;
    std::array<std::uint8_t, 32> sv_cookie_key_{};
    Bucket hello_budget_{0, kHandshakeRateGlobal - kHandshakeRateReserved,
                         2 * (kHandshakeRateGlobal - kHandshakeRateReserved), {}};
    Bucket confirm_budget_{0, kHandshakeRateGlobal - kHandshakeRateReserved,
                           2 * (kHandshakeRateGlobal - kHandshakeRateReserved), {}};
    Bucket hello_reserved_{0, kHandshakeRateReserved, 2 * kHandshakeRateReserved, {}};
    Bucket confirm_reserved_{0, kHandshakeRateReserved, 2 * kHandshakeRateReserved, {}};
    std::map<std::uint32_t, RClock::time_point> good_groups_;  // /56 groups with a recent handshake
    std::map<std::uint32_t, std::pair<Bucket, Bucket>> group_budget_;  // by /56 group: hellos, confirms
    std::uint64_t curve_ops_ = 0;
    struct Backoff {
        int failures = 0;
        RClock::time_point until{};
    };
    std::map<std::uint64_t, Backoff> backoff_;  // members whose confirmation failed (by endpoint)
    std::array<std::uint8_t, 32> nonce_key_{};
    bool have_nonce_key_ = false;
    std::string sv_name_;
    Throttle sv_throttle_;
    MemberKey sv_key_;
    std::uint64_t sv_out_ctr_ = 0;
    Replay sv_in_;
    std::array<std::uint8_t, 16> sv_salt_{};
    std::map<std::uint64_t, Session> sessions_;  // by guest key
    std::deque<std::array<std::uint8_t, 16>> recent_cn_;  // handshakes seen (no replay replaces a session)
    // host
    bool accept_ = false;
    std::map<std::uint32_t, Member> members_;               // by key id
    std::set<std::uint32_t> own_addrs_;
    std::map<std::uint64_t, HostRouteState> host_routes_;   // by guest key
    std::map<std::uint64_t, RClock::time_point> direct_;    // keys heard directly (kDirectGuard)
    RClock::time_point direct_pruned_{};
};

}  // namespace bbnet::udp
