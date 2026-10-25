// SPDX-License-Identifier: GPL-3.0-or-later
// Party discovery (0.7.5, bbport.ini party_discovery): a hosting game announces itself by UDP
// broadcast so friends on the same VPN (Radmin, Hamachi, ZeroTier) or LAN find it in the
// launcher's Join list without typing an address.
//
//   announce (host -> <adapter's directed broadcast>:9308, every 1.5 s), version 1, big endian:
//     [0xfa]['A'][ver 1][flags]  id u32  port u16  level u16  players u8  max u8
//     build: u8 len + bytes   name: u8 len + bytes   area: u8 len + bytes
//     flags bit 0: the party has a password. Printable ASCII only; at most kMaxPacket bytes.
//   Never the password, the party secret, a party code or an address (the receiver reads the host's
//   address from the datagram's source). Parsing is strict: any length, version or character
//   violation drops the packet. A listener ignores its own id and rate-limits per source.
// Only the adapters of adapter_discovery_ok() are used: a private LAN address on a real adapter, or
// a Radmin / Hamachi / ZeroTier address. Never the Internet, a commercial VPN or a virtual switch.
#pragma once

#include "bbnet_internal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bbnet::disc {

constexpr std::uint16_t kDiscoveryPort = 9308;
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kMaxPacket = 160;
// The build id field: ours is "nightwatch-coop-<version>-<16 hex>" (about 45 characters); visy's 32
// would cut the hash off, so two different builds would look alike in the Join list.
constexpr std::size_t kMaxBuild = 64;
constexpr int kAnnounceEveryMs = 1500;

struct Announce {
    std::uint32_t id = 0;     // random per game process (own-packet filter, de-duplication)
    std::uint16_t port = 0;   // the party port (TCP link and UDP)
    std::uint16_t level = 0;  // the host's character level; 0 unknown
    std::uint8_t players = 0, max_players = 0;
    bool password = false;
    std::string build;        // party build id (at most kMaxBuild characters)
    std::string name;         // host name (at most 16)
    std::string area;         // area name (at most 32)
};

// Printable ASCII only; longer strings are cut, other bytes become '?'. The bytes to broadcast.
std::vector<std::uint8_t> encode(const Announce& a);
// Strict: false (and *out untouched) for a wrong magic / version, a short or long packet, a length
// that overruns, trailing bytes, a non-printable character, port 0 or players > max.
bool parse(const std::uint8_t* d, std::size_t n, Announce* out);

// May an announcement go out on this adapter? A private LAN address (10/8, 172.16/12, 192.168/16) on a
// non-virtual adapter, or the address of a Radmin / Hamachi / ZeroTier adapter. Tailscale passes no
// broadcasts (the launcher asks `tailscale status`), a commercial VPN is never used.
bool adapter_discovery_ok(const LanAdapter& a);
struct Target {
    std::uint32_t source = 0;     // the adapter's address (network order): the socket binds to it
    std::uint32_t broadcast = 0;  // directed broadcast (network order)
    std::string name;
};
// The targets for the adapters that pass, no duplicates (an adapter without a mask is skipped).
std::vector<Target> targets_for(const std::vector<LanAdapter>& adapters);
// The up adapters now.
std::vector<LanAdapter> enumerate_adapters();

// Host side. The provider fills the live state each round and returns false while not hosting /
// switched off (nothing is sent). Idempotent start; stop joins the thread (bounded, < 1 s).
using Provider = std::function<bool(Announce*)>;
void start(Provider provider);
void stop();
// BB_PARTY_DISCOVERY_LOOPBACK=1 (tests): also announce to 127.0.0.1.
}  // namespace bbnet::disc
