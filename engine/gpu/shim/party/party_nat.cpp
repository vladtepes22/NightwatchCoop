// SPDX-License-Identifier: GPL-3.0-or-later
#include "party/party_nat.h"

#include <algorithm>
#include <cctype>

namespace party::nat {

namespace {
std::uint16_t sw16(std::uint16_t v) { return static_cast<std::uint16_t>((v >> 8) | (v << 8)); }
std::uint64_t key_of(const rudp::Endpoint& e) { return static_cast<std::uint64_t>(e.addr) << 16 | e.port; }
}  // namespace

const char* type_name(Type t) {
    switch (t) {
    case Type::Unknown: return "unknown";
    case Type::Open: return "open";
    case Type::FullCone: return "full cone";
    case Type::Restricted: return "restricted cone";
    case Type::PortRestricted: return "port-restricted cone";
    case Type::Cone: return "cone";
    case Type::Symmetric: return "symmetric";
    }
    return "unknown";
}

Type parse_type(const std::string& in) {
    std::string s;
    for (char c : in) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (s == "open" || s == "none") return Type::Open;
    if (s == "full" || s == "full cone" || s == "fullcone") return Type::FullCone;
    if (s == "addr" || s == "restricted" || s == "restricted cone") return Type::Restricted;
    if (s == "port" || s == "port-restricted cone" || s == "portrestricted") return Type::PortRestricted;
    if (s == "cone") return Type::Cone;
    if (s == "sym" || s == "symmetric") return Type::Symmetric;
    return Type::Unknown;
}

Type classify(const std::vector<std::uint32_t>& local_addrs, std::uint16_t local_port, const StunSample& a,
              const StunSample& b) {
    (void)local_port;
    if (!a.ok) return Type::Unknown;
    if (std::find(local_addrs.begin(), local_addrs.end(), a.addr) != local_addrs.end()) return Type::Open;
    if (!b.ok) return Type::Cone;
    if (a.addr != b.addr || a.port != b.port) return Type::Symmetric;
    return Type::Cone;
}

const char* path_name(Path p) {
    switch (p) {
    case Path::Tcp: return "tcp";
    case Path::UdpDirect: return "udp-direct";
    case Path::UdpPunched: return "udp-punched";
    case Path::None: return "none";
    }
    return "none";
}

Path predict(const Side& host, const Side& guest, bool guest_endpoint_known) {
    if (host.port_open || host.type == Type::Open) return Path::Tcp;
    switch (host.type) {
    case Type::Symmetric:
        // The host's code carries the port its STUN server saw; a guest's datagrams to it hit a
        // mapping made for that server only.
        return Path::None;
    case Type::FullCone:
        // Anything gets in once the mapping exists (the party port's STUN made it); the answer
        // leaves from the same port, which the guest's own NAT expects.
        return Path::UdpDirect;
    default: break;
    }
    if (!guest_endpoint_known) return Path::None;
    if (guest.type != Type::Symmetric) return Path::UdpPunched;
    // A symmetric guest's advertised port is wrong; only a host NAT that filters by address
    // alone lets its datagrams (from another port of the same address) in after the punch.
    return host.type == Type::Restricted ? Path::UdpPunched : Path::None;
}

std::string advice(Type self, bool host, bool port_open, Rendezvous rendezvous, std::uint16_t port) {
    const std::string p = std::to_string(port);
    if (host) {
        if (port_open) return "port " + p + " open: guests connect directly";
        switch (self) {
        case Type::Open: return "public address, no NAT: guests connect directly (allow bbport in the firewall)";
        case Type::Symmetric:
            return "symmetric NAT and no open port: guests cannot get through. Forward UDP+TCP " + p +
                   " to this PC, let a friend host, or use a VPN (Tailscale, ZeroTier) and share its address";
        case Type::Unknown:
            return "no STUN answer (UDP to the internet blocked?) and no open port: forward UDP+TCP " + p +
                   " or use a VPN (Tailscale, ZeroTier)";
        default:
            return std::string("no open port (") + type_name(self) + " NAT): guests join by UDP hole punching" +
                   (rendezvous == Rendezvous::On ? ", found automatically (or with their guest code)"
                    : rendezvous == Rendezvous::Off
                        ? ", with their guest code (Add a guest code; automatic punching is off: turn on the "
                          "DHT option on both PCs to skip the code)"
                        : ", with their guest code") +
                   "; a guest behind a symmetric NAT needs UDP+TCP " + p + " forwarded";
        }
    }
    switch (self) {
    case Type::Open: return "public address, no NAT: any host is reachable";
    case Type::Symmetric:
        return "symmetric NAT (mobile / carrier NAT): hole punching cannot reach you. Join a host with an "
               "open port (UPnP or port forward), host yourself only with a forwarded port, or use a VPN "
               "(Tailscale, ZeroTier)";
    case Type::Unknown: return "no STUN answer: UDP to the internet may be blocked; a host with an open port still works";
    default:
        return std::string(type_name(self)) + " NAT: hole punching works" +
               (rendezvous == Rendezvous::On  ? " (endpoint exchanged automatically when the host has the DHT option on too)"
                : rendezvous == Rendezvous::Off ? std::string(" (") + kRendezvousOffHint + ")"
                                                : std::string(" (give the host your guest code)"));
    }
}

Box::Box(Type type, std::uint32_t public_addr_nbo, std::uint16_t first_port)
    : type_(type), addr_(public_addr_nbo), next_(first_port) {}

rudp::Endpoint Box::out(std::uint16_t local, const rudp::Endpoint& to) {
    if (type_ == Type::Open) return {addr_, sw16(local)};
    const std::uint64_t flow =
        type_ == Type::Symmetric ? (static_cast<std::uint64_t>(local) << 48) ^ key_of(to) ^ (1ull << 63)
                                 : static_cast<std::uint64_t>(local);
    auto it = by_flow_.find(flow);
    std::uint16_t ext;
    if (it == by_flow_.end()) {
        ext = next_++;
        by_flow_[flow] = ext;
        by_ext_[ext].local = local;
    } else {
        ext = it->second;
    }
    Mapping& m = by_ext_[ext];
    m.sent_to.insert(key_of(to));
    m.sent_addr.insert(to.addr);
    return {addr_, sw16(ext)};
}

std::uint16_t Box::in(std::uint16_t ext_nbo, const rudp::Endpoint& from) {
    const std::uint16_t ext = sw16(ext_nbo);
    if (type_ == Type::Open) return ext;
    auto it = by_ext_.find(ext);
    if (it == by_ext_.end()) return 0;  // no mapping: nothing gets in (no port forward)
    const Mapping& m = it->second;
    switch (type_) {
    case Type::FullCone: return m.local;
    case Type::Restricted: return m.sent_addr.count(from.addr) ? m.local : 0;
    default: return m.sent_to.count(key_of(from)) ? m.local : 0;
    }
}

}  // namespace party::nat
