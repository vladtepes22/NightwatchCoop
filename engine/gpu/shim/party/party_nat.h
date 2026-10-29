// SPDX-License-Identifier: GPL-3.0-or-later
// NAT traversal decisions for the party (no sockets, no threads; unit-tested by party-nat-test):
//  - classify(): the NAT type from two STUN answers taken from the same local UDP port towards
//    two different servers (same external port: an endpoint-independent mapping, which hole
//    punching needs; another port per server: symmetric). Plain STUN Binding cannot tell how a
//    cone NAT filters, so a cone is reported as Cone (filtering unknown).
//  - predict(): which path a guest's party link takes, the fallback chain
//      TCP to the host's open port (UPnP / forward / no NAT)
//      -> UDP straight in (the host's NAT lets unsolicited UDP in: full cone)
//      -> UDP hole punch (the host learns the guest's endpoint: guest code or the rendezvous)
//      -> none (symmetric NAT in the way: port forward, another host or a VPN).
//    Guest-to-guest game traffic then punches too and falls back to the host relay (PeerPaths).
//  - advice(): the one-line explanation and suggestion shown to the player.
//  - Box: a simulated NAT (mapping + filtering per RFC 4787) for the tests' wire, so the chain is
//    checked end to end with the real reliable-UDP streams (party_rudp) for every pair of types.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "party/party_rudp.h"

namespace party::nat {

enum class Type : std::uint8_t {
    Unknown,         // no STUN answer (UDP blocked, STUN unreachable or off)
    Open,            // the STUN-mapped address is our own: no NAT (public IP)
    FullCone,        // endpoint-independent mapping and filtering (simulation / UPnP-like)
    Restricted,      // address-dependent filtering (simulation)
    PortRestricted,  // address-and-port-dependent filtering (simulation; the usual home router)
    Cone,            // detected: endpoint-independent mapping, filtering unknown (assume PortRestricted)
    Symmetric,       // endpoint-dependent mapping: each destination sees another external port
};

const char* type_name(Type t);  // "unknown", "open", "full cone", ... (stable, for logs)
// "open", "full", "addr", "port", "cone", "sym" (and the type_name spellings) -> Type; Unknown otherwise.
Type parse_type(const std::string& s);

// Where the router's own outside address (UPnP's external IP) sits. A UPnP mapping only opens the
// port to the internet when that address is the one the internet sees (STUN); a router behind the
// provider's NAT (CGNAT, 100.64.0.0/10) or behind another router (a private address) maps the port
// on itself and nothing comes in (Nightwatch beta 8 report: "port open" behind CGNAT).
enum class Outside : std::uint8_t {
    Unknown,      // no UPnP external address
    Public,       // public, and STUN agrees (or did not answer)
    ProviderNat,  // 100.64.0.0/10: the provider's shared NAT
    PrivateNat,   // 10/8, 172.16/12, 192.168/16: another router in front of this one
    Mismatch,     // public, but the internet sees another address (another NAT in between)
};
// Dotted IPv4 text; empty when unknown.
Outside classify_outside(const std::string& upnp_external, const std::string& stun_public);
const char* outside_name(Outside o);
// The host's line when its UPnP mapping does not reach the internet ("" for Unknown / Public).
std::string outside_advice(Outside o, const std::string& upnp_external, const std::string& stun_public,
                           std::uint16_t port);

struct StunSample {
    bool ok = false;
    std::uint32_t addr = 0;  // network order
    std::uint16_t port = 0;  // host order
};

// `local_addrs`: this machine's IPv4 addresses (network order); `local_port`: the port the STUN
// requests left from (host order). `a`, `b`: answers from two different STUN servers. No `a`:
// Unknown; `a` at a local address: Open; the same external endpoint in both: Cone; another port
// (or address) per server: Symmetric; no `b`: Cone (cannot tell, the common case assumed).
Type classify(const std::vector<std::uint32_t>& local_addrs, std::uint16_t local_port, const StunSample& a,
              const StunSample& b);

struct Side {
    Type type = Type::Unknown;
    bool port_open = false;  // host: UPnP mapped, port forwarded or a public address (TCP+UDP in)
};

enum class Path : std::uint8_t {
    Tcp,         // TCP to the host's open port
    UdpDirect,   // the host's NAT lets the guest's UDP in without a punch (full cone)
    UdpPunched,  // the host punched the guest's endpoint (guest code / rendezvous) first
    None,        // cannot connect directly: port forward, another host or a VPN
};
const char* path_name(Path p);  // "tcp", "udp-direct", "udp-punched", "none"

// The path a guest's party link takes. `guest_endpoint_known`: the host got the guest's public
// UDP endpoint (guest code or the rendezvous). Unknown types are treated as port-restricted cones.
Path predict(const Side& host, const Side& guest, bool guest_endpoint_known);

// One line for the log / Party page: what the NAT means for this player and what to do.
// `host`: we host; `port_open`: (host) UPnP / forward worked; `rendezvous`: the automatic
// endpoint exchange through the DHT (party_rdv.h): On, Off (the player's opt-in setting is off:
// the line says to turn it on on both PCs or use the guest code), or NotApplicable (no public
// endpoint, a LAN / plain address code: the guest code is the only way).
enum class Rendezvous : std::uint8_t { On, Off, NotApplicable };
std::string advice(Type self, bool host, bool port_open, Rendezvous rendezvous, std::uint16_t port);
// A guest's TCP connection that reached the party port from outside proves it is open (UPnP, a
// manual forward or a public address; a STUN probe cannot see a manual forward). `peer_nbo`: the
// connection's source address, network order. Loopback, LAN, VPN and CGNAT ranges prove nothing.
// Inline (header only): party_link.cpp calls it and the link-only test targets do not build party_nat.cpp.
inline bool proves_open_port(bool direct_tcp, std::uint32_t peer_nbo) {
    if (!direct_tcp || peer_nbo == 0) return false;
    const std::uint8_t a = peer_nbo & 0xff, b = (peer_nbo >> 8) & 0xff;
    if (a == 0 || a == 127 || a >= 224) return false;
    if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127) ||
        (a == 169 && b == 254))
        return false;
    return true;
}
// The hint when automatic punching is off, for the guest's failure message and the Party page.
inline constexpr const char* kRendezvousOffHint =
    "automatic punching is off: send the host your guest code, or turn on the DHT option on both PCs";

// ---- a simulated NAT for tests ----
// One public address; internal endpoints are local ports. out() maps an outbound datagram (and
// opens the filter for its destination), in() decides whether an inbound one passes and to which
// local port. Open = no NAT (identity, no filtering).
class Box {
public:
    Box(Type type, std::uint32_t public_addr_nbo, std::uint16_t first_port = 40000);
    // An outbound datagram from local port `local` to `to`: the external endpoint it leaves from.
    rudp::Endpoint out(std::uint16_t local, const rudp::Endpoint& to);
    // An inbound datagram from `from` to external port `ext` (network order): the local port, or
    // 0 when the NAT drops it.
    std::uint16_t in(std::uint16_t ext, const rudp::Endpoint& from);
    // What a STUN server at `server` reports for local port `local` (sends the request: maps it).
    rudp::Endpoint stun(std::uint16_t local, const rudp::Endpoint& server) { return out(local, server); }
    std::uint32_t public_addr() const { return addr_; }
    Type type() const { return type_; }

private:
    struct Mapping {
        std::uint16_t local = 0;
        std::set<std::uint64_t> sent_to;  // endpoint keys
        std::set<std::uint32_t> sent_addr;
    };
    Type type_;
    std::uint32_t addr_;
    std::uint16_t next_;
    std::map<std::uint64_t, std::uint16_t> by_flow_;  // (local, dest key) or (local) -> ext port (nbo)
    std::map<std::uint16_t, Mapping> by_ext_;
};

}  // namespace party::nat
