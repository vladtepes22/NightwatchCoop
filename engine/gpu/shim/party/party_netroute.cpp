// SPDX-License-Identifier: GPL-3.0-or-later
// F2 (0.7.1): IPv6 and the member relay in the running party (party_netroute.h).
#include "party/party_netroute.h"

#include "party/party_crypto.h"
#include "party/party_netpath.h"
#include "party/party_rudp.h"
#include "party/party_traversal.h"
#include "bbport_settings.h"
#include "../net/bbnet_internal.h"
#include "../net/json.h"
#include "../net/party_host_service.h"
#include "../net/party_transport.h"
#include "../net/party_route.h"
#include "../net/party_util.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace party::netroute {

namespace {

using Clock = std::chrono::steady_clock;
namespace udp = bbnet::udp;
using bbnet::party::int_of;
using bbnet::party::str_of;

std::uint16_t sw16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }
std::uint64_t key_of(std::uint32_t a, std::uint16_t p) { return a | (static_cast<std::uint64_t>(p) << 32); }

struct Net6 {
    std::uint32_t addr = 0;  // the member's IPv4 key (its room address), network order
    std::uint16_t port = 0;
    udp::Ep6 e6;
};

struct State {
    std::mutex mu;
    bool started = false, host = false, lb = false;
    // guest
    netpath::Chooser chooser;
    std::uint32_t host_addr = 0;  // the host's key (its code address), network order
    std::uint16_t host_port = 0;
    bool host6 = false;  // the code's IPv6 tail and IPv6 on
    udp::Ip6 host_ip6;
    bool have6 = false;  // our own global IPv6 address
    udp::Ip6 own6;
    crypto::Key rkey{};
    bool have_key = false;
    std::string own_name;
    bool connected = false;
    std::string base_line;
    bool rdv = false;  // the DHT option: announce our relay, look members up
    std::uint32_t own_addr = 0;  // our party port's public mapping (the guest code's)
    std::uint16_t own_port = 0;
    Clock::time_point down_since = Clock::now();
    // host
    // slot -> its IPv6 endpoint (net6) until the nonce sent there came back over its link (net6_echo)
    // and its room address is known (M3: the host never lists an endpoint the member does not own).
    struct Pending6 {
        udp::Ep6 e6;
        std::array<std::uint8_t, 16> nonce{};
        bool verified = false;
        int sent = 0;
    };
    std::map<int, Pending6> net6_pending;
    std::map<std::string, Net6> net6;      // member name -> entry
    bool net6_dirty = false;
    std::string host_line;
    std::set<std::uint64_t> relayed_seen;
    std::map<int, std::string> key_owner;  // slot -> member name that gave us a relay key
};
State& S() {
    static State* s = new State;
    return *s;
}

std::atomic<bool> g_serving{false};  // we relay for others now (the DHT announce's want)

// The link's route hint (PartyLink asks with its own lock held: lock-free here).
std::atomic<int> g_hint{0};  // 0 none, 1 routed (IPv6), 2 routed, UDP at once (member relay)
std::atomic<std::uint64_t> g_hint_key{0};

// Wrong relay proofs per address (party_link.h AuthThrottle, the F3 rule).
std::mutex g_throttle_mu;
AuthThrottle g_throttle;
std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
udp::Router::Throttle throttle() {
    udp::Router::Throttle t;
    t.locked = [](std::uint32_t ip, std::int64_t* remaining) {
        std::lock_guard<std::mutex> lk(g_throttle_mu);
        return g_throttle.Locked(ip, steady_ms(), remaining);
    };
    t.fail = [](std::uint32_t ip) {
        std::lock_guard<std::mutex> lk(g_throttle_mu);
        g_throttle.Fail(ip, steady_ms());
    };
    t.success = [](std::uint32_t ip) {
        std::lock_guard<std::mutex> lk(g_throttle_mu);
        g_throttle.Success(ip);
    };
    return t;
}

// The runtime's callbacks under a lock of their own: logging while S().mu is held must not take it
// again (live relay2: host_tick logged a relayed guest under S().mu, deadlocked, and the party port's
// reader then waited on it forever in the member check).
std::mutex g_env_mu;
Env g_env;
Env env_copy() {
    std::lock_guard<std::mutex> lk(g_env_mu);
    return g_env;
}
void log(const std::string& line) {
    const Env e = env_copy();
    if (e.log) e.log(line);
}
bool relay_on() { return BbSettings::Get().party_member_relay.load(); }
bool ipv6_on() { return BbSettings::Get().party_ipv6.load(); }

// The relay handshake's PAKE (CPace on X25519) under the party's relay key (N1: no offline oracle).
udp::Router::Pake pake_for(const crypto::Key& k) {
    udp::Router::Pake p;
    p.generator = [k](const std::uint8_t sid[16], std::uint8_t g[32]) { crypto::cpace_generator(k, sid, g); };
    p.x25519 = [](const std::uint8_t sc[32], const std::uint8_t pt[32], std::uint8_t out[32]) {
        return crypto::x25519(sc, pt, out);
    };
    p.hash = [](const std::uint8_t* d, std::size_t n, std::uint8_t out[32]) {
        const crypto::Key h = crypto::blake2b256(d, n);
        std::memcpy(out, h.data(), 32);
    };
    return p;
}

// V / v frames: BLAKE2b-128 under the member's own relay key.
void keyed_mac(const std::uint8_t key[32], const std::uint8_t* d, std::size_t n, std::uint8_t out[16]) {
    crypto::Key k;
    std::memcpy(k.data(), key, 32);
    crypto::mac16(k, d, n, out);
}

std::string hex_of(const std::uint8_t* d, std::size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(x[d[i] >> 4]);
        s.push_back(x[d[i] & 15]);
    }
    return s;
}
bool unhex(const std::string& t, std::uint8_t* out, std::size_t n) {
    if (t.size() != 2 * n) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (std::size_t i = 0; i < n; ++i) {
        const int a = v(t[2 * i]), b = v(t[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = static_cast<std::uint8_t>(a << 4 | b);
    }
    return true;
}

std::string ep6_text(const udp::Ep6& e) { return "[" + udp::ip6_text(e.addr) + "]:" + std::to_string(sw16(e.port)); }

// mu held. The route table follows the chooser's step for the next attempt.
void apply_locked(const netpath::Step& st) {
    State& s = S();
    udp::Router& r = bbnet::p2p_router();
    g_hint_key.store(key_of(s.host_addr, s.host_port));
    if (st.route != netpath::Route::Relay) r.relay_stop();
    if (st.route != netpath::Route::Ipv6 && s.host6) r.route_v6(s.host_addr, s.host_port, false);
    switch (st.route) {
    case netpath::Route::Direct: g_hint.store(0); break;
    case netpath::Route::Ipv6:
        r.bind_v6(s.host_addr, s.host_port, udp::Ep6{s.host_ip6, s.host_port}, true);
        g_hint.store(1);
        break;
    case netpath::Route::Relay: {
        const auto& c = s.chooser.relays();
        if (st.relay < 0 || st.relay >= static_cast<int>(c.size()) || !s.have_key) {
            g_hint.store(0);
            break;
        }
        const netpath::Candidate& m = c[static_cast<std::size_t>(st.relay)];
        r.relay_use(m.addr, m.port, s.host_addr, s.host_port, pake_for(s.rkey), Clock::now());
        g_hint.store(2);
        break;
    }
    }
}

void network_line(const std::string& text) {
    const Env e = env_copy();
    if (e.network) e.network(text);
}

// "a.b.c.d:port" / "BBG1-..." -> a member endpoint.
bool parse_relay_token(const std::string& t, std::uint32_t* addr, std::uint16_t* port) {
    if (rudp::decode_guest_code(t, addr, port)) return true;
    std::array<std::uint8_t, 4> ip{};
    const auto colon = t.rfind(':');
    if (colon == std::string::npos || !parse_ipv4(t.substr(0, colon), &ip)) return false;
    const long p = std::strtol(t.c_str() + colon + 1, nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    std::memcpy(addr, ip.data(), 4);
    *port = sw16(static_cast<std::uint16_t>(p));
    return true;
}

// ---- host tick: the members' IPv6 table, relayed members ----
void host_tick() {
    State& s = S();
    const Env e = env_copy();
    PartyLink* link = e.link ? e.link() : nullptr;
    if (!link) return;
    const std::vector<RosterEntry> roster = link->roster();
    std::string table_json;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        // A pending endpoint gets the member's room address (what the other guests' games address it
        // by) once the host service has it; the address must be the member's own (its link's address,
        // the synthetic key of that very IPv6 endpoint, or loopback in a local test).
        for (auto it = s.net6_pending.begin(); it != s.net6_pending.end();) {
            if (!it->second.verified) {
                // The nonce goes to the claimed IPv6 endpoint (5 tries, 1 s apart); the member echoes it.
                if (it->second.sent < 5) {
                    ++it->second.sent;
                    bbnet::p2p_send_wire(bbnet::p2p_router().nonce_wire(it->second.e6, it->second.nonce));
                    ++it;
                } else {
                    log("IPv6: slot " + std::to_string(it->first) + "'s endpoint " + ep6_text(it->second.e6) +
                        " not taken (our nonce sent there never came back over its link)");
                    it = s.net6_pending.erase(it);
                }
                continue;
            }
            std::string name;
            for (const RosterEntry& r : roster)
                if (r.slot == it->first && r.connected) name = r.name;
            json::Value rec;
            if (name.empty()) {
                it = s.net6_pending.erase(it);
                continue;
            }
            if (!bbnet::party::PartyHostService::instance().resolve(name, &rec)) {
                ++it;
                continue;
            }
            std::string a = str_of(rec, "MappedAddr");
            long long p = int_of(rec, "MappedPort", 0);
            if (a.empty() || p <= 0) {
                a = str_of(rec, "Addr");
                p = int_of(rec, "Port", 0);
            }
            const std::uint32_t ka = bbnet::party::ip_parse(a);
            if (!ka || p <= 0 || p > 65535) {
                ++it;
                continue;
            }
            const std::uint16_t kp = sw16(static_cast<std::uint16_t>(p));
            std::uint32_t sk = 0;
            std::uint16_t sp = 0;
            const bool known = bbnet::p2p_router().known_key(it->second.e6, &sk, &sp);
            const bool own = link->member_ip(it->first) == ka || (known && sk == ka && sp == kp) ||
                             (bbnet::party::ip_is_loopback(ka) && bbnet::party::ip_is_loopback(link->member_ip(it->first)));
            if (!own) {
                log("IPv6: " + name + "'s endpoint " + ep6_text(it->second.e6) + " not taken (its room address " +
                    udp::key_text(ka, kp) + " is not its own)");
                it = s.net6_pending.erase(it);
                continue;
            }
            Net6& n = s.net6[name];
            if (n.addr != ka || n.port != kp || !(n.e6 == it->second.e6)) {
                n = {ka, kp, it->second.e6};
                s.net6_dirty = true;
                log("IPv6: " + name + " (" + udp::key_text(ka, kp) + ") is also at " + ep6_text(it->second.e6) +
                    "; the other guests try it before the host relay");
            }
            it = s.net6_pending.erase(it);
        }
        for (auto it = s.key_owner.begin(); it != s.key_owner.end();) {
            bool in = false;
            for (const RosterEntry& r : roster)
                if (r.slot == it->first && r.name == it->second && r.connected) in = true;
            if (!in) {
                bbnet::p2p_router().drop_member(it->second);
                log("member relay: " + it->second + "'s relay key dropped (not in the party any more)");
                it = s.key_owner.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = s.net6.begin(); it != s.net6.end();) {
            bool in = false;
            for (const RosterEntry& r : roster)
                if (r.name == it->first && r.connected) in = true;
            if (!in) {
                it = s.net6.erase(it);
                s.net6_dirty = true;
            } else {
                ++it;
            }
        }
        if (s.net6_dirty) {
            s.net6_dirty = false;
            json::Value m = json::Value::make_array();
            for (const auto& [name, n] : s.net6) {
                json::Value v = json::Value::make_object();
                v.set("Name", name);
                v.set("Addr", bbnet::party::ip_text(n.addr));
                v.set("Port", static_cast<int>(sw16(n.port)));
                v.set("Addr6", udp::ip6_text(n.e6.addr));
                v.set("Port6", static_cast<int>(sw16(n.e6.port)));
                m.push(std::move(v));
            }
            json::Value body = json::Value::make_object();
            body.set("Members", std::move(m));
            table_json = json::dump(body, 0);
        }
    }
    if (!table_json.empty()) link->send_event(kBroadcast, kNet6TableEvent, table_json);

    // Party members' own IPv6 keys are never evicted to make room for strangers (N2).
    for (const RosterEntry& r : roster) {
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        if (r.slot != kHostSlot && r.connected && link->member_endpoint(r.slot, &ip, &port) && udp::is_synthetic(ip))
            bbnet::p2p_router().mark_trusted(ip, port);
    }
    // Guests a member relays for: logged once each, and on the host's Network line.
    const std::vector<udp::Router::HostRoute> routes = bbnet::p2p_router().host_routes();
    std::string relayed;
    bool fresh = false;
    for (const udp::Router::HostRoute& hr : routes) {
        std::string guest;
        const std::string member = hr.member;
        for (const RosterEntry& r : roster) {
            std::uint32_t ip = 0;
            std::uint16_t port = 0;
            if (r.slot == kHostSlot || !link->member_endpoint(r.slot, &ip, &port)) continue;
            if (ip == hr.guest_addr && port == hr.guest_port) guest = r.name;
        }
        if (guest.empty()) continue;  // not in the party (yet)
        if (!relayed.empty()) relayed += ", ";
        relayed += guest + (member.empty() ? "" : " via " + member);
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.relayed_seen.insert(key_of(hr.guest_addr, hr.guest_port)).second) {
            fresh = true;
            log("member relay: " + guest + " (" + udp::key_text(hr.guest_addr, hr.guest_port) +
                ") is in the party through " + (member.empty() ? std::string("a member") : member) + "'s relay (" +
                udp::key_text(hr.member_addr, hr.member_port) + "): it cannot reach us directly");
        }
    }
    std::string line;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (routes.empty()) s.relayed_seen.clear();
        line = s.host_line + (relayed.empty() ? "" : " Through a member's relay: " + relayed + ".");
    }
    static std::string last;
    if (line != last || fresh) {
        last = line;
        if (!line.empty()) network_line(line);
    }
}

// The member that relays for us is, to our game, the endpoint we reach it at (its public one), not
// the address the host lists for it (its LAN address when its own link came over the LAN): its room
// records are rewritten to it (real 2-PC run 20261021_1929: our game sent to the member's LAN
// address, heard it from its public one, and never finished joining the room). Called from the
// watcher and when the link connects (before the game's join asks for the room).
void sync_relay_peer() {
    static std::mutex mu;
    static std::string peer_set;
    netpath::Step step;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        step = S().chooser.current();
    }
    udp::Router& r = bbnet::p2p_router();
    std::string member;
    std::uint32_t ma = 0;
    std::uint16_t mp = 0;
    if (step.route != netpath::Route::Relay || r.relay_state(&member) != udp::Router::ClientState::Ready ||
        !r.relay_member(&ma, &mp))
        member.clear();
    std::lock_guard<std::mutex> lk(mu);
    if (member == peer_set) return;
    peer_set = member;
    bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest();
    if (!rg) return;
    rg->clear_peer_endpoints();
    if (member.empty()) return;
    rg->set_peer_endpoint(member, ma, sw16(mp));
    log("member relay: our game reaches " + member + " at " + udp::key_text(ma, mp) +
        " (where it relays for us), whatever address the host lists for it");
}

// ---- guest watcher: relaying for others, the relay handshake, our IPv6 endpoint ----
void guest_tick(bool* serving, std::size_t* served_n, bool* net6_sent, bool* dialed) {
    State& s = S();
    const Env e = env_copy();
    PartyLink* link = e.link ? e.link() : nullptr;
    if (!link) return;
    const LinkState st = link->state();
    netpath::Step step;
    bool have_key, have6;
    std::uint32_t ha;
    std::uint16_t hp;
    crypto::Key k;
    std::string name;
    udp::Ip6 own6;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        step = s.chooser.current();
        have_key = s.have_key;
        ha = s.host_addr;
        hp = s.host_port;
        k = s.rkey;
        name = s.own_name;
        have6 = s.have6;
        own6 = s.own6;
    }
    udp::Router& r = bbnet::p2p_router();
    // Relay for members that cannot reach the host: while our own link is up and not relayed.
    const bool want = relay_on() && have_key && st == LinkState::Connected && step.route != netpath::Route::Relay;
    if (want != *serving) {
        *serving = want;
        g_serving.store(want);
        // A fresh key for our V / v frames each time we start serving, given to the host over our
        // encrypted party link (H1: nobody else can speak for us, even from our address).
        const udp::Router::MemberKey mk = want ? udp::Router::new_member_key() : udp::Router::MemberKey{};
        r.serve(want, ha, hp, pake_for(k), name, throttle(), mk);
        if (want) {
            json::Value v = json::Value::make_object();
            char id[16];
            std::snprintf(id, sizeof id, "%08x", static_cast<unsigned>(mk.id));
            v.set("Id", std::string(id));
            v.set("Key", hex_of(mk.key.data(), mk.key.size()));
            link->send_event(0, kRelayKeyEvent, json::dump(v, 0));
        }
    }
    // The host's IPv6 nonces (M3): echoed over the link, proving the IPv6 endpoint is ours.
    // One event for all of them (N6: a flood of strangers' nonces costs one event per tick).
    if (const auto nonces = r.take_nonces(); !nonces.empty()) {
        json::Value list = json::Value::make_array();
        for (const auto& nonce : nonces) list.push(hex_of(nonce.data(), nonce.size()));
        json::Value v = json::Value::make_object();
        v.set("Nonces", std::move(list));
        link->send_event(0, kNet6EchoEvent, json::dump(v, 0));
    }
    // The DHT option: while our link is down for 10 s, members that relay are looked up there.
    bool rdv, lb;
    Clock::time_point down;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (st == LinkState::Connected) s.down_since = Clock::now();
        rdv = s.rdv;
        lb = s.lb;
        down = s.down_since;
    }
    if (rdv && relay_on() && st != LinkState::Connected && Clock::now() - down > std::chrono::seconds(10) &&
        !traversal::relay_lookup_running()) {
        traversal::start_relay_lookup(
            k.data(), lb,
            [](std::uint32_t a, std::uint16_t p) {
                {
                    std::lock_guard<std::mutex> lk(S().mu);
                    if (a == S().own_addr && p == S().own_port) return;  // our own announce
                }
                AddRelayText(udp::key_text(a, p), "DHT");
            },
            [](const std::string& m) { log(m); });
    } else if (st == LinkState::Connected && traversal::relay_lookup_running()) {
        traversal::stop_relay_lookup();
    }
    const std::size_t n = want ? r.served().size() : 0;
    if (n != *served_n) {
        *served_n = n;
        std::string base;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            base = s.base_line;
        }
        if (!base.empty())
            network_line(base + (n ? " Relaying for " + std::to_string(n) + " member(s) that cannot reach the host." : ""));
    }
    // Our relay answered: dial now instead of waiting out the backoff.
    if (step.route == netpath::Route::Relay) {
        const udp::Router::ClientState cs = r.relay_state();
        if (cs == udp::Router::ClientState::Ready && st == LinkState::Reconnecting && !*dialed) {
            *dialed = true;
            link->reconnect_now();
        }
        if (cs != udp::Router::ClientState::Ready) *dialed = false;
    } else {
        *dialed = false;
    }
    sync_relay_peer();
    // Our IPv6 endpoint for the other guests (the host checks and shares it: net6_table).
    if (st == LinkState::Connected && !*net6_sent && have6 && ipv6_on() && bbnet::p2p_ipv6_open()) {
        *net6_sent = true;
        json::Value v = json::Value::make_object();
        v.set("Addr6", udp::ip6_text(own6));
        v.set("Port6", static_cast<int>(bbnet::settings().party_port));
        link->send_event(0, kNet6Event, json::dump(v, 0));
    }
    if (st != LinkState::Connected) *net6_sent = false;
}

}  // namespace

void Configure(Env env) {
    std::lock_guard<std::mutex> lk(g_env_mu);
    g_env = std::move(env);
}

// ---- host ----

void HostStart(bool loopback, const std::string& password, const std::array<std::uint8_t, 8>& secret) {
    // The party's nonce key for the IPv6 checks (R4: strangers cannot fill the members' slots).
    bbnet::p2p_router().set_keyed_mac(keyed_mac);
    bbnet::p2p_router().set_nonce_key(crypto::relay_key_for(password, secret.data()));
    log(std::string("member relay key: from ") +
        (password.empty() ? "the party code (no password)" : "the password (members who joined with a plain address too)"));
    {
        std::lock_guard<std::mutex> lk(S().mu);
        if (S().started) return;
        S().started = true;
        S().host = true;
        S().lb = loopback;
    }
    if (ipv6_on()) {
        bbnet::p2p_set_ipv6(true);
        udp::Ip6 a;
        std::string how;
        if (bbnet::local_ipv6(&a, &how))
            log("IPv6: on; this PC is [" + udp::ip6_text(a) + "] (" + how + ")" +
                (bbnet::p2p_ipv6_open() ? "" : "; but the party port has no IPv6 socket"));
        else
            log("IPv6: on; no global IPv6 address here (" + how + "): IPv4 only");
    } else {
        log("IPv6: off (BB_PARTY_IPV6=0 / System > Party network)");
    }
    bbnet::p2p_router().set_keyed_mac(keyed_mac);
    if (relay_on()) {
        // Only under a key a member gave us over its link (relay_key) does a relayed guest get in.
        bbnet::p2p_router().accept_relayed(true);
        log("member relay: on (a member that a guest reaches may relay that guest to us)");
    } else {
        log("member relay: off (BB_PARTY_MEMBER_RELAY=0 / System > Party network)");
    }
    std::thread([] {
        for (;;) {
            const Env e = env_copy();
            if (e.stopping && e.stopping()) return;
            host_tick();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}

bool HostCodeIpv6(bool port_open, std::array<std::uint8_t, 16>* out) {
    udp::Ip6 a;
    const bool on = ipv6_on();
    if (!netpath::code_carries_ipv6(on, on && bbnet::p2p_ipv6_open(), on && bbnet::local_ipv6(&a), port_open))
        return false;
    std::memcpy(out->data(), a.b.data(), 16);
    return true;
}

void HostPunched(std::uint32_t addr_nbo, std::uint16_t port_nbo) {
    if (!relay_on()) return;
    const Env e = env_copy();
    PartyLink* l = e.link ? e.link() : nullptr;
    if (!l || l->roster().size() < 2) return;
    json::Value v = json::Value::make_object();
    v.set("Addr", bbnet::party::ip_text(addr_nbo));
    v.set("Port", static_cast<int>(sw16(port_nbo)));
    l->send_event(kBroadcast, kRelayPunchEvent, json::dump(v, 0));
}

void HostNetworkLine(const std::string& advice) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().host_line = advice;
}

// ---- guest ----

void GuestStart(PartyLink* link, const LinkConfig& cfg, const PartyCode& code, std::uint32_t host_addr_nbo,
                std::uint16_t host_port, bool loopback) {
    State& s = S();
    bool have6 = false, host6 = false;
    udp::Ip6 own6, h6;
    std::string how;
    if (ipv6_on()) {
        bbnet::p2p_set_ipv6(true);
        have6 = bbnet::local_ipv6(&own6, &how) && bbnet::p2p_ipv6_open();
        if (code.has_ipv6) {
            std::memcpy(h6.b.data(), code.ipv6.data(), 16);
            host6 = true;
        }
        log("IPv6: on; " + (have6 ? "this PC is [" + udp::ip6_text(own6) + "] (" + how + ")" : "no global IPv6 address here") +
            "; the party code " + (host6 ? "has the host's [" + udp::ip6_text(h6) + "]" : "has no IPv6 address") +
            (have6 && host6 ? ": the link tries IPv6 after TCP" : ""));
    } else {
        log("IPv6: off (BB_PARTY_IPV6=0 / System > Party network)");
    }
    bbnet::p2p_router().set_keyed_mac(keyed_mac);
    const crypto::Key relay = crypto::relay_key_for(cfg.password, code.secret.data());
    log(std::string("member relay key: from ") +
        (cfg.password.empty() ? "the party code (no password); members who joined with a plain address and a "
                                "password cannot relay with us"
                              : "the password (the same for code and plain-address members)"));
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.started = true;
        s.host = false;
        s.lb = loopback;
        s.host_addr = host_addr_nbo;
        s.host_port = sw16(host_port);
        s.have6 = have6;
        s.own6 = own6;
        s.host6 = host6 && have6;
        s.host_ip6 = h6;
        s.rkey = relay;
        bbnet::p2p_router().set_nonce_key(s.rkey);
        s.have_key = true;
        s.own_name = cfg.name;
        s.chooser.set_ipv6(s.host6);
        s.chooser.start(Clock::now());
    }
    if (const char* via = std::getenv("BB_PARTY_RELAY_VIA"); via && *via) AddRelayText(via, "BB_PARTY_RELAY_VIA");
    {
        std::lock_guard<std::mutex> lk(s.mu);
        apply_locked(s.chooser.current());
    }
    log(std::string("member relay: ") +
        (relay_on() ? "on (we relay for members that cannot reach the host, and use one when we cannot)"
                    : "off (BB_PARTY_MEMBER_RELAY=0 / System > Party network)"));
    link->set_udp_route_hint([](std::uint32_t a, std::uint16_t p) {
        return key_of(a, p) == g_hint_key.load() ? g_hint.load() : 0;
    });
    std::thread([] {
        bool serving = false, net6_sent = false, dialed = false;
        std::size_t served = 0;
        for (;;) {
            const Env e = env_copy();
            if (e.stopping && e.stopping()) {
                bbnet::p2p_router().serve(false, 0, 0, {}, {}, {}, {});
                return;
            }
            guest_tick(&serving, &served, &net6_sent, &dialed);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }).detach();
}

void GuestLinkState(LinkState st) {
    if (st == LinkState::Connected) sync_relay_peer();  // before the game asks for the room
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.started || s.host) return;
    if (st == LinkState::Connected) {
        if (!s.connected) {
            s.connected = true;
            s.chooser.connected();
        }
        return;
    }
    if (st != LinkState::Reconnecting) return;
    if (s.connected) {
        s.connected = false;
        s.chooser.lost();
        return;
    }
    const netpath::Step before = s.chooser.current();
    const netpath::Step next = s.chooser.failed(Clock::now());
    if (next == before && next.route != netpath::Route::Relay) return;
    apply_locked(next);
    if (!(next == before)) {
        const Env e = env_copy();
        const std::string line = "network path: the attempt over " + s.chooser.describe(before) +
                                 " failed; next: " + s.chooser.describe(next);
        if (e.log) e.log(line);
    }
}

int AddRelayText(const std::string& text, const std::string& label) {
    int added = 0;
    std::string cur;
    auto one = [&] {
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\r')) cur.pop_back();
        while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
        std::uint32_t a = 0;
        std::uint16_t p = 0;
        if (!cur.empty() && parse_relay_token(cur, &a, &p)) {
            std::lock_guard<std::mutex> lk(S().mu);
            if (!(a == S().host_addr && p == S().host_port) && S().chooser.add_relay(a, p, label)) {
                ++added;
                const Env e = env_copy();
                if (e.log)
                    e.log("member relay: " + udp::key_text(a, p) + " (" + label +
                          ") may relay for us if the host cannot be reached directly");
            }
        }
        cur.clear();
    };
    for (char c : text) {
        if (c == ',' || c == '\n') one();
        else cur.push_back(c);
    }
    one();
    return added;
}

std::string GuestPathText(bool udp) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    const netpath::Step st = s.chooser.current();
    std::string base;
    if (st.route == netpath::Route::Relay && udp) {
        std::string who;
        bbnet::p2p_router().relay_state(&who);
        std::uint32_t ma = 0;
        std::uint16_t mp = 0;
        bbnet::p2p_router().relay_member(&ma, &mp);
        base = "UDP through " + (who.empty() ? std::string("a member") : who) + "'s relay (" + udp::key_text(ma, mp) +
               "): the host cannot be reached directly";
    } else if (st.route == netpath::Route::Ipv6 && udp) {
        base = "UDP via IPv6 [" + udp::ip6_text(s.host_ip6) + "]:" + std::to_string(sw16(s.host_port)) +
               " (no NAT in the way)";
    } else {
        base = udp ? "UDP (the host's NAT let us in or punched towards us)" : "TCP (the host's port is open)";
    }
    return base;
}

std::string GuestCode6() {
    if (!ipv6_on()) return {};
    udp::Ip6 a;
    if (!bbnet::local_ipv6(&a) || !bbnet::p2p_ipv6_open()) return {};
    return rudp::encode_guest_code6(a.b.data(), sw16(bbnet::settings().party_port));
}

void GuestRendezvous(std::uint32_t addr_nbo, std::uint16_t port_nbo) {
    State& s = S();
    bool lb;
    crypto::Key k;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        lb = s.lb;
        k = s.rkey;
        s.own_addr = addr_nbo;
        s.own_port = port_nbo;
        if (!s.have_key || !relay_on() || !port_nbo || !traversal::rendezvous_enabled(lb)) return;
        s.rdv = true;
    }
    traversal::start_relay_announce(k.data(), lb, sw16(port_nbo), [] { return g_serving.load(); },
                                    [](const std::string& m) { log("member relay: " + m); });
}

void GuestNetworkLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().base_line = line;
}

// ---- both ----

bool OnEvent(bool host, int slot, const std::string& name, const std::string& body) {
    if (name != kNet6Event && name != kNet6TableEvent && name != kRelayPunchEvent && name != kRelayKeyEvent &&
        name != kNet6EchoEvent)
        return false;
    json::Value v;
    std::string err;
    if (!json::parse(body, v, err) || v.type != json::Value::Type::Object) {
        log(name + " event refused: " + err);
        return true;
    }
    if (host && name == kNet6Event) {
        udp::Ep6 e6;
        const long long p = int_of(v, "Port6", 0);
        if (!udp::parse_ip6(str_of(v, "Addr6"), &e6.addr) || p <= 0 || p > 65535 ||
            (!udp::ip6_global(e6.addr) && !(udp::ip6_loopback(e6.addr) && S().lb))) {
            log("net6 from slot " + std::to_string(slot) + " refused: not a global IPv6 endpoint");
            return true;
        }
        e6.port = sw16(static_cast<std::uint16_t>(p));
        State::Pending6 pd;
        pd.e6 = e6;
        udp::secure_random(pd.nonce.data(), pd.nonce.size());
        std::lock_guard<std::mutex> lk(S().mu);
        // The same endpoint again keeps its nonce and its tries (N6: no reset by repeated net6).
        auto it = S().net6_pending.find(slot);
        if (it != S().net6_pending.end() && it->second.e6 == e6) return true;
        if (S().net6_pending.size() < 16) S().net6_pending[slot] = pd;
        return true;
    }
    if (host && name == kNet6EchoEvent) {
        const json::Value* list = v.find("Nonces");
        if (!list || list->type != json::Value::Type::Array) return true;
        std::lock_guard<std::mutex> lk(S().mu);
        auto it = S().net6_pending.find(slot);
        int k = 0;
        for (const json::Value& e : list->array) {
            if (++k > 64 || it == S().net6_pending.end() || it->second.verified) break;
            std::array<std::uint8_t, 16> nonce{};
            if (e.type != json::Value::Type::String || !unhex(e.string, nonce.data(), nonce.size())) continue;
            if (it->second.nonce == nonce) {
                it->second.verified = true;
                log("IPv6: slot " + std::to_string(slot) + " owns " + ep6_text(it->second.e6) + " (our nonce came back)");
            }
        }
        return true;
    }
    if (host && name == kRelayKeyEvent) {
        udp::Router::MemberKey mk;
        std::uint8_t id[4];
        if (!unhex(str_of(v, "Id"), id, 4) || !unhex(str_of(v, "Key"), mk.key.data(), mk.key.size())) {
            log("relay_key from slot " + std::to_string(slot) + " refused: malformed");
            return true;
        }
        mk.id = static_cast<std::uint32_t>(id[0]) << 24 | static_cast<std::uint32_t>(id[1]) << 16 |
                static_cast<std::uint32_t>(id[2]) << 8 | id[3];
        const Env e = env_copy();
        PartyLink* l = e.link ? e.link() : nullptr;
        if (!l || !relay_on()) return true;
        std::string member;
        for (const RosterEntry& r : l->roster())
            if (r.slot == slot && r.connected) member = r.name;
        const std::uint32_t ip = l->member_ip(slot);
        if (member.empty() || !ip) return true;
        bbnet::p2p_router().set_member_key(mk, ip, member);
        {
            std::lock_guard<std::mutex> lk(S().mu);
            S().key_owner[slot] = member;
        }
        log("member relay: " + member + " may relay guests to us (its relay key, from its party link)");
        return true;
    }
    if (!host && name == kNet6TableEvent) {
        bool have6;
        std::string own;
        {
            std::lock_guard<std::mutex> lk(S().mu);
            have6 = S().have6;
            own = S().own_name;
        }
        if (!have6 || !ipv6_on()) return true;
        const json::Value* m = v.find("Members");
        if (!m || m->type != json::Value::Type::Array) return true;
        int n = 0;
        for (const json::Value& e : m->array) {
            if (++n > 8) break;
            if (str_of(e, "Name") == own) continue;
            udp::Ep6 e6;
            const long long p = int_of(e, "Port", 0), p6 = int_of(e, "Port6", 0);
            const std::uint32_t a = bbnet::party::ip_parse(str_of(e, "Addr"));
            if (!a || p <= 0 || p > 65535 || p6 <= 0 || p6 > 65535 || !udp::parse_ip6(str_of(e, "Addr6"), &e6.addr))
                continue;
            e6.port = sw16(static_cast<std::uint16_t>(p6));
            bbnet::p2p_peer_v6(a, static_cast<std::uint16_t>(p), e6);
        }
        return true;
    }
    if (!host && name == kRelayPunchEvent) {
        const std::uint32_t a = bbnet::party::ip_parse(str_of(v, "Addr"));
        const long long p = int_of(v, "Port", 0);
        if (!relay_on() || !a || p <= 0 || p > 65535) return true;
        // A guest the host is punching may need us as its relay: open our NAT towards it too.
        bbnet::p2p_punch("a guest that may need our relay", a, static_cast<std::uint16_t>(p), 0, 0);
        return true;
    }
    return true;
}

void Stop() {
    bbnet::p2p_router().serve(false, 0, 0, {}, {}, {}, {});
    bbnet::p2p_router().relay_stop();
}

}  // namespace party::netroute
