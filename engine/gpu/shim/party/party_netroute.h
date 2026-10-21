// SPDX-License-Identifier: GPL-3.0-or-later
// F2 (0.7.1): the party runtime's side of IPv6 and the member relay (docs/party/network_robustness.md
// 4a). The party port's route table (net/party_route.h) carries the datagrams; this module decides:
//  - host: IPv6 on the party port and in the party code (only while its IPv4 port is not open),
//    accepting guests a member relays (members' addresses only), the members' IPv6 table
//    (net6 -> net6_table), the guest endpoints it punches passed to the members (relay_punch);
//  - guest: the path of each link attempt (party_netpath.h: IPv6, direct, member relays), the
//    member relay handshake keyed by the party key, relaying for others while its own link is up
//    and direct, its IPv6 endpoint for the other guests, and the path in the log / Network line.
// Security (F2 review, 0.7.1): a member's V / v frames are MACed under its own random key, given
// to the host over its party link (relay_key); a member's IPv6 endpoint enters net6_table only once a
// nonce the host sent there came back over its link (net6_echo); password / relay lockouts count an
// IPv6 /64 as one address (LinkConfig throttle_key).
// Switches: BbSettings party_member_relay (BB_PARTY_MEMBER_RELAY) and party_ipv6 (BB_PARTY_IPV6),
// both on by default; BB_PARTY_RELAY_VIA (comma list of a.b.c.d:port or BBG1 guest codes: members
// to relay through) and BB_PARTY_IPV6_ADDR (this PC's IPv6 address, ::1 for loopback tests).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "party/party_code.h"
#include "party/party_link.h"

namespace party::netroute {

constexpr const char* kNet6Event = "net6";            // guest -> host {Addr6, Port6}
constexpr const char* kNet6TableEvent = "net6_table";  // host -> guests {Members:[{Name, Addr, Port, Addr6, Port6}]}
constexpr const char* kRelayPunchEvent = "relay_punch";  // host -> guests {Addr, Port}
constexpr const char* kRelayKeyEvent = "relay_key";      // member -> host {Id, Key}: its V / v frame key
constexpr const char* kNet6EchoEvent = "net6_echo";      // guest -> host {Nonce}: the IPv6 nonce it got

struct Env {
    std::function<PartyLink*()> link;
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> network;  // the Party page's Network line (and status)
    std::function<bool()> stopping;
};
void Configure(Env env);

// ---- host ----
// After the link listens: IPv6 on the party port (setting), accepting relayed guests (setting).
void HostStart(bool loopback, const std::string& password, const std::array<std::uint8_t, 8>& secret);
// The IPv6 address for the party code's tail: only with IPv6 on, a global address (or the test
// override) and an IPv4 port that is not open. False: the code stays IPv4 only.
bool HostCodeIpv6(bool port_open, std::array<std::uint8_t, 16>* out);
// The host punched a guest endpoint (guest code / DHT): the members open their NAT to it too.
void HostPunched(std::uint32_t addr_nbo, std::uint16_t port_nbo);
// The NAT advice line the host shows; relayed members are appended to it.
void HostNetworkLine(const std::string& advice);

// ---- guest ----
// Before link->start_guest: the route hint, IPv6, the relay key, BB_PARTY_RELAY_VIA.
void GuestStart(PartyLink* link, const LinkConfig& cfg, const PartyCode& code, std::uint32_t host_addr_nbo,
                std::uint16_t host_port, bool loopback);
// The guest link's state changes (party_runtime's on_state): attempts, connects, losses.
void GuestLinkState(LinkState s);
// "Join through a member": BBG1 guest codes / a.b.c.d:port (comma / newline separated); how many
// were new and valid.
int AddRelayText(const std::string& text, const std::string& label);
// The path the link took, for "network path: party link over ..." and the Network line.
std::string GuestPathText(bool udp);
// The guest's own IPv6 party endpoint as a BBG6 code (empty without IPv6).
std::string GuestCode6();
// The guest's Network line ("Connected over ... <advice>"); "relaying for N" is appended to it.
void GuestNetworkLine(const std::string& line);
// With the DHT option on (opt-in): our relay is announced while we serve, and members that relay
// are looked up while our own link is down (traversal start_relay_*). `addr` / `port`: our party
// port's public mapping (the guest code's), network order; never taken as a member.
void GuestRendezvous(std::uint32_t addr_nbo, std::uint16_t port_nbo);

// ---- both ----
// A party event this module handles (net6 / net6_table / relay_punch); true when consumed.
bool OnEvent(bool host, int slot, const std::string& name, const std::string& body);
void Stop();

}  // namespace party::netroute
