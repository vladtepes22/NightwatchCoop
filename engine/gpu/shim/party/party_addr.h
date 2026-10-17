// SPDX-License-Identifier: GPL-3.0-or-later
// The host's public address for the Internet party code:
//   1. BB_PARTY_PUBLIC_ADDR ("a.b.c.d" or "a.b.c.d:port") overrides everything;
//   2. a STUN Binding request (RFC 5389, encode/decode from shim/net/net_stun) to BB_PARTY_STUN
//      (default "stun.l.google.com:19302"; "off" disables), 2 s timeout, 3 tries, sent from the
//      caller's UDP socket (the game's party-port socket, so the reflexive mapping is that port's)
//      or from a temporary socket bound to the party port (ephemeral port if that is taken);
//   3. the external IP the UPnP router reported, if any.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace party {

struct PublicAddress {
    bool ok = false;
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0;   // the port the world sees (STUN mapped port; the party port otherwise)
    std::string source;       // "override", "stun", "upnp"
    std::string message;      // what happened, for the log / UI
};

// One STUN exchange. `sock` is a bound UDP socket (SOCKET / fd cast to intptr_t) or -1 for a
// temporary one bound to local_port (0 = any). False with `error` on timeout / bad server.
// Note: when `sock` is the game's live socket, the caller must not be reading it concurrently
// (or must hand STUN responses over); the temporary-socket form has no such constraint.
bool stun_query(std::intptr_t sock, const std::string& server, std::uint16_t local_port,
                std::array<std::uint8_t, 4>* ip, std::uint16_t* port, std::string* error, int timeout_ms = 2000,
                int tries = 3);

// The whole chain above. `upnp_external_ip` is UpnpResult::external_ip (may be empty).
PublicAddress resolve_public_address(std::uint16_t party_port, std::intptr_t udp_sock = -1,
                                     const std::string& upnp_external_ip = {});

// "host:port" / "host" (default_port) -> IPv4 (DNS allowed). False when it does not resolve.
bool resolve_ipv4(const std::string& host_port, std::uint16_t default_port, std::array<std::uint8_t, 4>* ip,
                  std::uint16_t* port);

}  // namespace party
