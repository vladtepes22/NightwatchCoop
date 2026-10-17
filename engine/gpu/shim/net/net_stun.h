// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/net/stun.h @8f2746c
// The STUN Binding exchange, as bytes: a request the host sends
// from its P2P UDP socket to the private server's STUN listener (or any
// public STUN server), and the response that carries the address and port
// the world sees that socket at - the reflexive address the server records
// for other players to reach us behind NAT. No sockets here; hle/net.cpp
// sends and receives on the P2P port so the mapping is that port's.
//
// The request is RFC 5389's shape (the magic cookie leads the transaction
// id), which an RFC 3489 server such as the private server's reads as a
// 16-byte id. The response is taken from MAPPED-ADDRESS (0x0001)
// when present, else XOR-MAPPED-ADDRESS (0x8020 or 0x0020), un-XORed with
// the cookie when the response carries it and with the transaction id's
// own first bytes otherwise (what the private server's does).
#pragma once

#include <cstddef>
#include <cstdint>

namespace net::stun {

constexpr std::size_t kHeader = 20;
constexpr std::size_t kTxid = 16;
constexpr std::size_t kTokenLen = 8;
// The longest request: the header and the relay HELLO (4 + 12).
constexpr std::size_t kMaxRequest = kHeader + 16;

// The private server's relay: a request with
// BBHOST-HELLO (0x8100: "bbr1" + the token it was given, zeros before the
// first) gets a relay port bound to a token, and BBHOST-RELAY (0x8101:
// "bbr1" + token + port + the address the request came from) in the answer.
// The client then sends its datagrams for other relay ports to the STUN
// port framed [0xfb]['R'][token][port u16] and receives the relay's framed
// [0xfb]['r'][source port u16] (hle/net.cpp). A server without the relay
// ignores the attribute.
struct Relay {
    bool present = false;
    std::uint8_t token[kTokenLen] = {};
    std::uint16_t vport = 0;           // host order
    std::uint32_t observed_addr = 0;   // network order: where the request came from
    std::uint16_t observed_port = 0;   // host order
};

// Writes a Binding Request into `out` (kMaxRequest bytes room) and its
// transaction id (the cookie plus 12 random bytes) into `txid`; returns its
// length. `hello` adds BBHOST-HELLO with `token` (zeros when null).
std::size_t build_binding_request(std::uint8_t* out, std::uint8_t txid[kTxid], bool hello = false,
                                  const std::uint8_t* token = nullptr);

// True when [d, d+n) is a Binding Response to `txid` carrying a mapped IPv4
// address: `addr` in network byte order, `port` in host order. `relay`, when
// given, is filled from BBHOST-RELAY (present = false without it).
bool parse_binding_response(const std::uint8_t* d, std::size_t n, const std::uint8_t txid[kTxid], std::uint32_t* addr,
                            std::uint16_t* port, Relay* relay = nullptr);

// --- bbport: the server side, answered by the party host on its party UDP port ---

// The longest response: the header, MAPPED-ADDRESS (12), XOR-MAPPED-ADDRESS
// (12) and BBHOST-RELAY (4 + 20).
constexpr std::size_t kMaxResponse = kHeader + 12 + 12 + 24;

// True when [d, d+n) is a Binding Request: its transaction id lands in
// `txid`; `hello` says whether it carried BBHOST-HELLO, whose token (zeros
// for a first request) lands in `token`.
bool parse_binding_request(const std::uint8_t* d, std::size_t n, std::uint8_t txid[kTxid], bool* hello,
                           std::uint8_t token[kTokenLen]);

// Writes the Binding Response to `txid` (kMaxResponse bytes room) telling
// the requester it was seen at addr (network order) : port (host order),
// as MAPPED-ADDRESS and XOR-MAPPED-ADDRESS, the latter XORed with the
// transaction id's first bytes (the cookie for an RFC 5389 request), which
// is what parse_binding_response undoes. `relay`, when present, adds
// BBHOST-RELAY. Returns the length.
std::size_t build_binding_response(std::uint8_t* out, const std::uint8_t txid[kTxid], std::uint32_t addr,
                                   std::uint16_t port, const Relay* relay = nullptr);

}  // namespace net::stun
