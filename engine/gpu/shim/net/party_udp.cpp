// SPDX-License-Identifier: GPL-3.0-or-later
// Party UDP framing and the host relay (party_udp.h). Derived from the relay in
// droogie/bbhost src/hle/net.cpp @8f2746c (moved out of net_socket.cpp).
#include "party_udp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bbnet::udp {

namespace {

std::uint16_t bswap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }

std::string addr_text(std::uint32_t addr_nbo, std::uint16_t port_nbo) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&addr_nbo);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], bswap16(port_nbo));
    return buf;
}

void key_bytes(std::uint64_t k, std::uint8_t* out) {
    for (std::size_t i = 0; i < net::stun::kTokenLen; ++i) out[i] = static_cast<std::uint8_t>(k >> (8 * i));
}

}  // namespace

// ---- vport header ----

std::size_t p2p_header(const std::uint8_t* d, std::size_t n, std::uint16_t* src, std::uint16_t* dst) {
    if (n < 4 || d[0] != kP2pMagic) return 0;
    const std::uint8_t fl = d[1];
    std::size_t at = 2;
    if (fl & kP2pFlagComid) at += 4;
    std::size_t hdr;
    if (fl & kP2pFlagByteVports) {
        if (at + 2 > n) return 0;
        *src = d[at];
        *dst = d[at + 1];
        hdr = at + 2;
    } else {
        if (at + 4 > n) return 0;
        *src = static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
        *dst = static_cast<std::uint16_t>((d[at + 2] << 8) | d[at + 3]);
        hdr = at + 4;
    }
    return hdr;
}

std::size_t p2p_write_header(std::uint8_t* out, std::uint16_t src, std::uint16_t dst) {
    out[0] = kP2pMagic;
    if (src < 256 && dst < 256) {
        out[1] = kP2pFlagP2p | kP2pFlagByteVports | kP2pType;
        out[2] = static_cast<std::uint8_t>(src);
        out[3] = static_cast<std::uint8_t>(dst);
        return 4;
    }
    out[1] = kP2pFlagP2p | kP2pType;
    out[2] = static_cast<std::uint8_t>(src >> 8);
    out[3] = static_cast<std::uint8_t>(src);
    out[4] = static_cast<std::uint8_t>(dst >> 8);
    out[5] = static_cast<std::uint8_t>(dst);
    return 6;
}

// ---- RelayServer ----

RelayServer::RelayServer(std::uint16_t own_port)
    : own_port_(own_port),
      rng_(std::random_device{}() ^ static_cast<std::uint64_t>(Clock::now().time_since_epoch().count())) {}

void RelayServer::set_own_port(std::uint16_t p) {
    std::lock_guard<std::mutex> lk(mu_);
    own_port_ = p;
}

void RelayServer::set_admit(std::function<bool(std::uint32_t)> admit) {
    std::lock_guard<std::mutex> lk(admit_mu_);
    admit_ = std::move(admit);
}

bool RelayServer::admitted(std::uint32_t addr) const {
    std::function<bool(std::uint32_t)> f;
    {
        std::lock_guard<std::mutex> lk(admit_mu_);
        f = admit_;
    }
    return !f || f(addr);
}

std::uint16_t RelayServer::preferred_port(std::uint64_t token) {
    // splitmix64 finalizer: any token bits spread over the range.
    std::uint64_t z = token + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<std::uint16_t>(kFirstPort + z % kPortSpan);
}

RelayServer::Client* RelayServer::register_locked(std::uint64_t token, std::uint32_t addr, std::uint16_t port_nbo,
                                                  std::string* note, bool re) {
    std::size_t same_addr = 0;
    for (const auto& [k, o] : by_token_) same_addr += o.addr == addr ? 1 : 0;
    if (by_token_.size() >= kMaxClients || same_addr >= kMaxClientsPerAddr) {
        ++stats_.refused;
        if (note && (stats_.refused == 1 || stats_.refused % 100 == 0))
            *note = "relay: table full; refused " + std::to_string(stats_.refused) + " registration(s)";
        return nullptr;
    }
    Client c;
    c.token = token;
    c.addr = addr;
    c.port = port_nbo;
    c.seen = Clock::now();
    std::uint16_t vp = preferred_port(token);
    bool free_port = false;
    for (unsigned i = 0; i < kPortSpan; ++i) {
        if (vp != own_port_ && !by_vport_.count(vp)) {
            free_port = true;
            break;
        }
        vp = static_cast<std::uint16_t>(vp + 1 >= kFirstPort + kPortSpan ? kFirstPort : vp + 1);
    }
    // Never hand out a taken port: that would route another client's traffic to this one.
    if (!free_port) {
        ++stats_.refused;
        return nullptr;
    }
    c.vport = vp;
    by_vport_[vp] = token;
    if (re) ++stats_.reregistered;
    else ++stats_.registered;
    if (note) {
        *note = "relay: " + addr_text(addr, port_nbo) + " is relay port " + std::to_string(vp) +
                (re ? " (re-registered a token this relay did not know)" : "");
    }
    return &by_token_.emplace(token, c).first->second;
}

std::size_t RelayServer::answer_stun(const std::uint8_t* d, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                                     bool offer_relay, std::uint8_t* out, std::string* note) {
    std::uint8_t txid[net::stun::kTxid];
    std::uint8_t token[net::stun::kTokenLen];
    bool hello = false;
    if (!net::stun::parse_binding_request(d, n, txid, &hello, token)) return 0;
    if (!admitted(addr)) {  // not a party member: no answer (no reflection, no relay port)
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.refused;
        return 0;
    }
    net::stun::Relay relay;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.answered;
        if (hello && offer_relay) {
            std::uint64_t key = token_key(token);
            auto it = by_token_.find(key);
            if (it == by_token_.end()) {
                Client* fresh = nullptr;
                if (key == 0) {
                    do {
                        key = rng_();
                    } while (key == 0 || by_token_.count(key));
                    fresh = register_locked(key, addr, port_nbo, note, false);
                } else {
                    // A token from before a restart of ours: the client keeps it (and, unless
                    // taken, its port).
                    fresh = register_locked(key, addr, port_nbo, note, true);
                }
                it = fresh ? by_token_.find(key) : by_token_.end();
            }
            if (it != by_token_.end()) {  // else: the table is full, a plain STUN answer
                Client& c = it->second;
                c.addr = addr;
                c.port = port_nbo;
                c.seen = Clock::now();
                relay.present = true;
                key_bytes(c.token, relay.token);
                relay.vport = c.vport;
                relay.observed_addr = addr;
                relay.observed_port = bswap16(port_nbo);
            }
        }
    }
    return net::stun::build_binding_response(out, txid, addr, bswap16(port_nbo), relay.present ? &relay : nullptr);
}

bool RelayServer::forward(std::uint8_t* buf, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                          std::uint8_t** out, std::size_t* out_len, std::uint32_t* to_addr, std::uint16_t* to_port_nbo,
                          std::string* note) {
    if (!is_relay_request(buf, n)) return false;
    const std::uint64_t src_key = token_key(buf + 2);
    const std::uint16_t dst_vport = static_cast<std::uint16_t>((buf[10] << 8) | buf[11]);
    std::uint16_t src_vport = 0;
    bool known = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_key == 0) return false;
        known = by_token_.count(src_key) != 0;
    }
    // An unknown token registers its sender: only an admitted address may (the admit filter
    // takes other locks, so it runs outside ours).
    if (!known && !admitted(addr)) {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.refused;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto src = by_token_.find(src_key);
        if (src == by_token_.end()) {
            Client* fresh = register_locked(src_key, addr, port_nbo, note, true);
            if (!fresh) return false;
            src = by_token_.find(src_key);
        }
        src->second.addr = addr;  // follows the client's NAT rebinding
        src->second.port = port_nbo;
        src->second.seen = Clock::now();
        src_vport = src->second.vport;
        auto dv = by_vport_.find(dst_vport);
        if (dv == by_vport_.end()) {
            ++stats_.unknown_dst;
            return false;
        }
        const Client& dst = by_token_[dv->second];
        *to_addr = dst.addr;
        *to_port_nbo = dst.port;
        ++stats_.forwarded;
        stats_.forwarded_bytes += n - kRelayHeader;
    }
    std::uint8_t* frame = buf + kRelayHeader - kDeliveryHeader;
    frame[0] = kRelayMagic;
    frame[1] = kRelayToClient;
    frame[2] = static_cast<std::uint8_t>(src_vport >> 8);
    frame[3] = static_cast<std::uint8_t>(src_vport);
    *out = frame;
    *out_len = n - (kRelayHeader - kDeliveryHeader);
    return true;
}

bool RelayServer::vport_for(std::uint32_t addr, std::uint16_t port_host, std::uint16_t* vport) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [key, c] : by_token_) {
        (void)key;
        if (c.addr == addr && c.port == bswap16(port_host)) {
            if (vport) *vport = c.vport;
            return true;
        }
    }
    return false;
}

void RelayServer::expire(Clock::time_point now, Clock::duration idle) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = by_token_.begin(); it != by_token_.end();) {
        if (now - it->second.seen > idle) {
            by_vport_.erase(it->second.vport);
            it = by_token_.erase(it);
            ++stats_.expired;
        } else {
            ++it;
        }
    }
}

std::size_t RelayServer::clients() const {
    std::lock_guard<std::mutex> lk(mu_);
    return by_token_.size();
}

RelayServer::Stats RelayServer::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

// ---- RelayClient ----

bool RelayClient::on_answer(std::uint32_t server, std::uint16_t stun_port_nbo, const net::stun::Relay& relay) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool was = on_;
    const std::uint16_t was_vport = vport_;
    std::uint8_t was_token[net::stun::kTokenLen];
    std::memcpy(was_token, token_, sizeof was_token);
    if (relay.present) {
        on_ = true;
        server_ = server;
        stun_port_ = stun_port_nbo;
        std::memcpy(token_, relay.token, sizeof token_);
        vport_ = relay.vport;
    } else {
        on_ = false;  // a server without the relay: datagrams go as addressed
    }
    return on_ != was || vport_ != was_vport || std::memcmp(was_token, token_, sizeof was_token) != 0;
}

bool RelayClient::token_for(std::uint32_t server, std::uint16_t stun_port_nbo, std::uint8_t* token) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on_ || server_ != server || stun_port_ != stun_port_nbo) return false;
    std::memcpy(token, token_, net::stun::kTokenLen);
    return true;
}

void RelayClient::add_relay_port(std::uint16_t port_host) const {
    std::lock_guard<std::mutex> lk(mu_);
    relay_ports_.insert(port_host);
}

bool RelayClient::frame_for(std::uint32_t addr, std::uint16_t port_nbo, std::uint8_t* hdr, std::uint32_t* stun_addr,
                            std::uint16_t* stun_port_nbo) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on_ || addr != server_ || port_nbo == stun_port_) return false;
    if (!relay_ports_.count(bswap16(port_nbo))) return false;  // a player sharing the host's address
    hdr[0] = kRelayMagic;
    hdr[1] = kRelayToServer;
    std::memcpy(hdr + 2, token_, net::stun::kTokenLen);
    std::memcpy(hdr + 10, &port_nbo, 2);  // network order is the frame's big-endian
    *stun_addr = server_;
    *stun_port_nbo = stun_port_;
    return true;
}

bool RelayClient::unwrap(const std::uint8_t* d, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                         std::uint16_t* src_port_nbo) const {
    if (!is_relay_delivery(d, n)) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!on_ || addr != server_ || port_nbo != stun_port_) return false;
    }
    const std::uint16_t from = static_cast<std::uint16_t>((d[2] << 8) | d[3]);
    *src_port_nbo = bswap16(from);
    return true;
}

bool RelayClient::get(std::uint32_t* server, std::uint16_t* vport) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (server) *server = server_;
    if (vport) *vport = vport_;
    return on_;
}

bool RelayClient::is_server(std::uint32_t addr, std::uint16_t port_nbo) const {
    std::lock_guard<std::mutex> lk(mu_);
    return on_ && addr == server_ && port_nbo == stun_port_;
}

bool RelayClient::on() const {
    std::lock_guard<std::mutex> lk(mu_);
    return on_;
}

void RelayClient::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    on_ = false;
    server_ = 0;
    stun_port_ = 0;
    relay_ports_.clear();
    std::memset(token_, 0, sizeof token_);
    vport_ = 0;
}

// ---- PeerPaths ----

void PeerPaths::configure(const Config& c) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = c;
    if (c.force)
        for (auto& [k, p] : peers_) {
            (void)k;
            if (p.v.relay_port) p.v.relayed = true;
        }
}

void PeerPaths::add(std::uint32_t addr, std::uint16_t port_nbo, std::uint16_t relay_port, Clock::time_point now) {
    if (!addr || !port_nbo) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto [it, fresh] = peers_.try_emplace(key(addr, port_nbo));
    Peer& p = it->second;
    if (fresh) {
        p.v.addr = addr;
        p.v.port = port_nbo;
        p.added = now;
        p.next_probe = now;
    }
    p.v.relay_port = relay_port;
    if (!relay_port) p.v.relayed = false;
    else if (cfg_.force) p.v.relayed = true;
}

void PeerPaths::remove(std::uint32_t addr, std::uint16_t port_nbo) {
    std::lock_guard<std::mutex> lk(mu_);
    peers_.erase(key(addr, port_nbo));
}

void PeerPaths::on_ack(std::uint32_t addr, std::uint16_t port_nbo, Clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(key(addr, port_nbo));
    if (it == peers_.end()) return;
    it->second.last_ack = now;
    it->second.v.answered = true;
}

bool PeerPaths::note_gap(std::uint32_t addr, std::uint16_t port_nbo, Clock::time_point now, std::string* note) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cfg_.gaps_to_relay <= 0 || cfg_.force) return false;
    auto it = peers_.find(key(addr, port_nbo));
    if (it == peers_.end() || !it->second.v.relay_port) return false;
    Peer& p = it->second;
    if (p.v.relayed && p.by_gaps) {
        // The gaps go on over the relay: the direct path was not the cause.
        p.by_gaps = false;
        p.v.relayed = false;
        ++p.v.switches;
        p.gaps.clear();
        p.gap_off_until = now + std::chrono::minutes(5);
        if (note) *note = "path to " + addr_text(addr, port_nbo) + ": game traffic gaps over the relay too, direct again";
        return false;
    }
    if (p.v.relayed || now < p.gap_off_until) return false;
    const auto window = std::chrono::milliseconds(cfg_.gap_window_ms);
    p.gaps.erase(std::remove_if(p.gaps.begin(), p.gaps.end(), [&](Clock::time_point t) { return now - t > window; }),
                 p.gaps.end());
    p.gaps.push_back(now);
    if (static_cast<int>(p.gaps.size()) < cfg_.gaps_to_relay) return false;
    p.gaps.clear();
    p.v.relayed = true;
    p.by_gaps = true;
    p.relay_since = now;
    p.stable_since = Clock::time_point{};
    ++p.v.switches;
    if (note)
        *note = "path to " + addr_text(addr, port_nbo) + ": repeated game traffic gaps, through the host relay (port " +
                std::to_string(p.v.relay_port) + ")";
    return true;
}

void PeerPaths::set_v6(std::uint32_t addr, std::uint16_t port_nbo, bool has, Clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(key(addr, port_nbo));
    if (it == peers_.end() || it->second.v.v6 == has) return;
    it->second.v.v6 = has;
    it->second.next_probe6 = now;
    if (!has) it->second.v.answered6 = it->second.v.via6 = false;
}

void PeerPaths::on_ack6(std::uint32_t addr, std::uint16_t port_nbo, Clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(key(addr, port_nbo));
    if (it == peers_.end() || !it->second.v.v6) return;
    it->second.last_ack6 = now;
    it->second.v.answered6 = true;
}

void PeerPaths::tick(Clock::time_point now, std::vector<std::pair<std::uint32_t, std::uint16_t>>* probes,
                     std::vector<std::string>* notes, std::vector<std::pair<std::uint32_t, std::uint16_t>>* probes6) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto ms = [](int v) { return std::chrono::milliseconds(v); };
    for (auto& [k, p] : peers_) {
        (void)k;
        if (now >= p.next_probe) {
            if (probes) probes->emplace_back(p.v.addr, p.v.port);
            p.next_probe = now + ms(cfg_.probe_interval_ms);
        }
        const bool fresh4 = p.v.answered && now - p.last_ack <= ms(cfg_.probe_interval_ms * 3 / 2);
        const bool fresh6 = p.v.v6 && p.v.answered6 && now - p.last_ack6 <= ms(cfg_.probe_interval_ms * 3 / 2);
        if (p.v.v6) {
            if (now >= p.next_probe6) {
                if (probes6) probes6->emplace_back(p.v.addr, p.v.port);
                p.next_probe6 = now + ms(cfg_.probe_interval_ms);
            }
            // IPv4 while it answers (no change from before), IPv6 once IPv4 is down and IPv6 answers,
            // back to IPv4 as soon as it answers again (or IPv6 stops answering).
            const bool v4_down = !p.v.answered ? now - p.added > ms(cfg_.fallback_ms) : now - p.last_ack > ms(cfg_.lost_ms);
            const bool v6_down = !p.v.answered6 || now - p.last_ack6 > ms(cfg_.lost_ms);
            const bool keep6 = p.v.via6 ? !fresh4 && !v6_down : v4_down && fresh6;
            if (keep6 != p.v.via6) {
                p.v.via6 = keep6;
                if (notes)
                    notes->push_back("path to " + addr_text(p.v.addr, p.v.port) +
                                     (keep6 ? ": IPv4 probes unanswered, IPv6 answers: over IPv6"
                                            : std::string(": IPv6 no longer needed or answered")));
            }
        }
        if (cfg_.force || !p.v.relay_port) continue;
        const bool any_answered = p.v.answered || p.v.answered6;
        const Clock::time_point last_any = std::max(p.last_ack, p.v.answered6 ? p.last_ack6 : p.last_ack);
        const bool fresh_ack = fresh4 || fresh6;
        if (!fresh_ack) p.stable_since = Clock::time_point{};
        else if (p.stable_since == Clock::time_point{}) p.stable_since = now;
        const bool lost = !any_answered ? now - p.added > ms(cfg_.fallback_ms) : now - last_any > ms(cfg_.lost_ms);
        // Back to direct: answered, for stable_ms without a break, once held for hold_ms.
        const bool back = fresh_ack && now - p.relay_since >= ms(cfg_.hold_ms) &&
                          now - p.stable_since >= ms(cfg_.stable_ms);
        const bool want = p.v.relayed ? !back : lost;
        if (want != p.v.relayed) {
            p.v.relayed = want;
            p.by_gaps = false;
            if (want) p.relay_since = now;
            ++p.v.switches;
            if (notes) {
                notes->push_back("path to " + addr_text(p.v.addr, p.v.port) +
                                 (want ? ": no answer to direct probes, through the host relay (port " +
                                             std::to_string(p.v.relay_port) + ")"
                                       : std::string(": direct again (probes answered)")));
            }
        }
    }
}

bool PeerPaths::relayed(std::uint32_t addr, std::uint16_t port_nbo, std::uint16_t* relay_port) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(key(addr, port_nbo));
    if (it == peers_.end() || !it->second.v.relayed) return false;
    if (relay_port) *relay_port = it->second.v.relay_port;
    return true;
}

bool PeerPaths::direct_of_relay(std::uint16_t relay_port, std::uint32_t* addr, std::uint16_t* port_nbo) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [k, p] : peers_) {
        (void)k;
        if (p.v.relay_port == relay_port) {
            *addr = p.v.addr;
            *port_nbo = p.v.port;
            return true;
        }
    }
    return false;
}

std::vector<PeerPaths::PeerView> PeerPaths::peers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PeerView> v;
    for (const auto& [k, p] : peers_) {
        (void)k;
        v.push_back(p.v);
    }
    return v;
}

// ---- routing ----

void EndpointAliases::set(std::uint32_t pa, std::uint16_t pp, std::uint32_t aa, std::uint16_t ap) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = out_.find(key(pa, pp)); it != out_.end()) in_.erase(it->second);
    if (pa == aa && pp == ap) {
        out_.erase(key(pa, pp));
        return;
    }
    out_[key(pa, pp)] = key(aa, ap);
    in_[key(aa, ap)] = key(pa, pp);
}

void EndpointAliases::clear(std::uint32_t pa, std::uint16_t pp) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = out_.find(key(pa, pp));
    if (it == out_.end()) return;
    in_.erase(it->second);
    out_.erase(it);
}

bool EndpointAliases::outgoing(std::uint32_t* addr, std::uint16_t* port) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (out_.empty()) return false;
    auto it = out_.find(key(*addr, *port));
    if (it == out_.end()) return false;
    *addr = static_cast<std::uint32_t>(it->second);
    *port = static_cast<std::uint16_t>(it->second >> 32);
    return true;
}

bool EndpointAliases::incoming(std::uint32_t* addr, std::uint16_t* port) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (in_.empty()) return false;
    auto it = in_.find(key(*addr, *port));
    if (it == in_.end()) return false;
    *addr = static_cast<std::uint32_t>(it->second);
    *port = static_cast<std::uint16_t>(it->second >> 32);
    return true;
}

std::size_t EndpointAliases::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return out_.size();
}

int punch_choice(bool primary_heard, bool local_heard, bool have_local, bool primary_on_our_net) {
    if (!have_local || !local_heard) return 0;
    // A peer already answering on an address inside one of our own subnets (a VPN such as Radmin /
    // Hamachi, or our LAN) is reached through the path its packets arrive on: its other "local"
    // address is some far-away LAN's number that only looked heard (2026-10-25: a guest's home
    // address 192.168.x was picked over the Radmin address, our replies left by the default route).
    if (primary_heard && primary_on_our_net) return 0;
    return 1;
}

bool on_own_subnet(std::uint32_t ip, const std::vector<OwnIf>& own) {
    if (!ip) return false;
    for (const OwnIf& o : own)
        if (o.mask && o.ip != ip && (o.ip & o.mask) == (ip & o.mask)) return true;
    return false;
}

bool is_own_endpoint(std::uint32_t ip, std::uint16_t port, std::uint16_t own_port, const std::vector<OwnIf>& own) {
    if (!ip || port != own_port) return false;
    for (const OwnIf& o : own)
        if (o.ip == ip) return true;
    return false;
}

bool local_candidate_usable(std::uint32_t cand_ip, std::uint16_t cand_port, std::uint16_t own_port,
                            const std::vector<OwnIf>& own, std::uint32_t own_public, std::uint32_t peer_public) {
    if (!cand_ip) return false;
    if (is_own_endpoint(cand_ip, cand_port, own_port, own)) return false;
    if ((cand_ip & 0xff) == 127) return true;
    if (own_public && own_public == peer_public) return true;
    if (own.empty()) return true;
    for (const OwnIf& o : own)
        if (o.mask && (o.ip & o.mask) == (cand_ip & o.mask)) return true;
    return false;
}

bool force_relay_from_env() {
    const char* v = std::getenv("BB_PARTY_FORCE_RELAY");
    return v && *v && v[0] != '0';
}

PeerRoute route_to_peer(const RelayClient& rc, std::uint32_t direct_addr, std::uint16_t direct_port,
                        std::uint16_t peer_relay_port, bool force_relay) {
    PeerRoute r;
    r.addr = direct_addr;
    r.port = direct_port;
    if (!force_relay || !peer_relay_port) return r;
    std::uint32_t server = 0;
    std::uint16_t own = 0;
    if (!rc.get(&server, &own) || !server) return r;
    if (rc.is_server(direct_addr, bswap16(direct_port))) return r;  // the host: its port is the relay
    if (peer_relay_port == own) return r;  // ourselves
    r.addr = server;
    r.port = peer_relay_port;
    r.relayed = true;
    rc.add_relay_port(peer_relay_port);
    return r;
}

}  // namespace bbnet::udp
