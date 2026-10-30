// SPDX-License-Identifier: GPL-3.0-or-later
// The open party: a host with no password that lets players join by plain address, without a party code. The pure
// rule, header only (unit test: tests/test_party_open.cpp, ninja -C out/gpu party-open-test).
//
// A plain address has no secret to prove, so the only thing that keeps the internet out is where the peer is. An open
// join is accepted only from a peer on the subnet of one of this PC's private or VPN adapters:
//   - the Nightwatch VPN lobby on: only the tunnel's subnet (Scope::Tunnel);
//   - party_open on: any such adapter, so also the router's LAN, Radmin, Tailscale (Scope::Local);
//   - a host with a password is never open: the password decides.
// Refused whatever the scope: a public address on any adapter (a PC plugged straight into the modem shares its subnet
// with strangers), the adapter's own gateway (a router that forwards a port can present internet players as itself),
// IPv6 peers and guests a member relays (they arrive as a 240/4 key, party_route.h, which is not a private address).
// A loopback peer counts only in a loopback test: a tunnel program on this PC can present the internet as 127.0.0.1.
// The link's encryption does not protect an open party from others on that network: they know the same key.
#pragma once

#include <cstdint>
#include <vector>

namespace party::open_party {

enum class Scope { None, Tunnel, Local };

/// All addresses in host order.
struct Adapter {
    std::uint32_t ip = 0;
    std::uint32_t mask = 0;     // 0 unknown: the adapter is not used
    std::uint32_t gateway = 0;  // 0 none
    bool tunnel = false;        // holds a Nightwatch VPN address
};

inline Scope ScopeOf(bool has_password, bool lobby_on, bool open_on) {
    if (has_password) return Scope::None;
    if (open_on) return Scope::Local;
    return lobby_on ? Scope::Tunnel : Scope::None;
}

/// RFC 1918, CGNAT (Tailscale), link-local, and Radmin VPN's 26/8 (not routed on the internet).
inline bool PrivateAddress(std::uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == 0xac1u || (a >> 16) == 0xc0a8u || (a >> 22) == 0x191u ||
           (a >> 16) == 0xa9feu || (a >> 24) == 26;
}

inline bool PeerAllowed(Scope scope, std::uint32_t peer, const std::vector<Adapter>& adapters, bool loopback_test) {
    if (scope == Scope::None) return false;
    if ((peer >> 24) == 127) return loopback_test;
    if (!PrivateAddress(peer)) return false;
    for (const Adapter& a : adapters) {
        if (!a.mask || !PrivateAddress(a.ip) || (scope == Scope::Tunnel && !a.tunnel)) continue;
        if ((peer & a.mask) == (a.ip & a.mask) && peer != a.gateway && peer != a.ip) return true;
    }
    return false;
}

}  // namespace party::open_party
