// SPDX-License-Identifier: GPL-3.0-or-later
// F2 NAT traversal in the running game (party_runtime calls these): the NAT type from a second
// STUN server, the advice line, and the automatic endpoint exchange through the DHT
// (party_rdv.h) that replaces the guest code when the host has no open port. No server of ours.
//
// Env: BB_PARTY_STUN2 (second STUN server, default stun.cloudflare.com:3478; "off" skips the
// type check), BB_PARTY_RENDEZVOUS=1 (the DHT rendezvous: OPT-IN, off unless set to 1 / on / yes /
// true; it posts the guest's address on the public DHT, so the player decides: launcher Party tab,
// System > Party rules, bbport.ini party_rendezvous; the env wins over the ini),
// BB_PARTY_DHT_BOOTSTRAP (comma list host:port; loopback tests only use the DHT with it, against
// a fake node: tools/mp/fake_dht.py).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "party/party_nat.h"

namespace party::traversal {

using Log = std::function<void(const std::string&)>;
// One STUN exchange from the party UDP port: host:port -> mapped address (network order) and
// port (host order).
using Stun = std::function<bool(const std::string& host, std::uint16_t port, std::uint32_t* addr, std::uint16_t* mport)>;

// "host:port" split (default port when missing).
void split_host_port(const std::string& s, std::uint16_t def_port, std::string* host, std::uint16_t* port);

// The NAT type: `first` is the answer of the first STUN server (already asked); this asks the
// second one. Loopback with a simulated NAT (BB_NET_SIM nat=...): both answers come from the
// simulator (nat_advertised_port), so a simulated symmetric NAT is detected as one.
nat::Type detect(const Stun& stun, const nat::StunSample& first, std::uint32_t local_addr, std::uint16_t local_port,
                 bool loopback);

// The guest code's port under a simulated NAT (loopback tests): the simulator's view of what a
// STUN server would report for the party port (host order in and out).
std::uint16_t simulated_mapped_port(std::uint16_t party_port);

// BB_PARTY_RENDEZVOUS's value: 1 / on / yes / true -> true, 0 / off / no / false -> false,
// unset / empty / anything else -> `fallback` (the bbport.ini setting, default off).
inline bool rendezvous_switch(const char* v, bool fallback) {
    if (!v || !*v) return fallback;
    std::string s = v;
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (s == "1" || s == "on" || s == "yes" || s == "true") return true;
    if (s == "0" || s == "off" || s == "no" || s == "false") return false;
    return fallback;
}
// The player's opt-in (BbSettings party_rendezvous, which Load() set from bbport.ini and
// BB_PARTY_RENDEZVOUS) and, in loopback, a fake DHT node (BB_PARTY_DHT_BOOTSTRAP).
bool rendezvous_enabled(bool loopback);

// Host: look the party up in the DHT every 5 s and call `punch` for each guest endpoint found
// (an endpoint at most once per 50 s). `key`: the party key (32 bytes).
void start_host(const std::uint8_t* key, bool loopback,
                std::function<void(std::uint32_t addr_nbo, std::uint16_t port_nbo)> punch, Log log);
// Guest: announce `port` (host order: the party port's public mapping) while `want` says so.
void start_guest(const std::uint8_t* key, bool loopback, std::uint16_t port, std::function<bool()> want, Log log);
void stop();

}  // namespace party::traversal
