// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch VPN lobby, host side (0.7.7, bbport.ini party_vpn_lobby, off by default). LAN discovery
// (party_discovery.h) does not cross an OpenVPN tunnel, so a hosting game on the Nightwatch VPN announces
// itself to a tiny lobby service on the VPN server (10.8.0.1, UDP 9310; tools/vpn/lobby/nwlobby.py). The
// launcher's Join page asks the lobby for the list and joins with the host's password.
//
//   announce (host -> 10.8.0.1:9310, every 10 s), big endian, at most 512 bytes:
//     [0xfa]['L'][ver 1][type 1]  flags u8 (bit 0: password; none: an open party, party_open_rules.h)  port u16  players u8  max u8
//     build: u8 len + printable ASCII (<= 32)   name: u8 len + UTF-8 (<= 48 bytes, <= 16 characters)
//   bye (host -> lobby, on leaving): [0xfa]['L'][ver 1][type 4]
//   The lobby learns the host's tunnel address from the datagram's source; the game never sends an
//   address, the password, the party secret or a code. The name is the player's character name.
//   announce extension (0.7.8, type 7, sent right after every announce; a lobby that does not know it drops it, so the
//   old list keeps working): flags u8 (bit 0: the host allows invaders, bit 1: a boss fight is on)  area u32 (the host's
//   map id 0xAABBCCDD, 0 unknown)  phase u8 (0 unknown, 1 day, 2 evening, 3 night, 4 Blood Moon)  ng u8 (0 = NG, 1..7 =
//   NG+n, 255 unknown). No coordinates, nothing that names a player. A client that wants the extended list asks with
//   type 8 (same body as type 2) and gets type 9; a lobby that does not answer type 8 is asked with type 2 as before.
//   mods label (0.7.8, type 10, sent right after type 7 while gameplay mods are loaded): label u8 len + ASCII, 1..32 of
//   a-z 0-9 @ . _ : + - ("nocturne@1.0" from the launcher's verified curated mod, else "mods:" + 8 hex of the party mod
//   hash). The lobby relays it in type 12 (list type 11); a client uses it only as a key, never as a path or address.
// Only a machine with a 10.8.0.0/22 tunnel address announces (no other adapter, never the Internet).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bbnet::lobby {

constexpr std::uint16_t kLobbyPort = 9310;
constexpr int kAnnounceEveryMs = 10000;
constexpr std::size_t kMaxNameChars = 16, kMaxNameBytes = 48, kMaxBuild = 32, kMaxModsLabel = 32;

struct Announce {
    std::string name;  // UTF-8 character name
    std::uint16_t port = 0;
    std::uint8_t players = 0, max_players = 0;
    bool password = false;
    std::string build;
    // 0.7.8 extension (above)
    bool invaders = false, boss = false;
    std::uint32_t area = 0;
    std::uint8_t phase = 0, ng = 255;
    std::string mods;  // the mods label (type 10), "" = none
};
// The extension datagram (type 7); always 12 bytes.
std::vector<std::uint8_t> encode_announce_ext(const Announce& a);
// 1..32 characters, each of a-z 0-9 @ . _ : + - (the lobby's rule).
bool valid_mods_label(const std::string& label);
// The mods label datagram (type 10), or empty when a.mods is empty or not a valid label.
std::vector<std::uint8_t> encode_announce_mods(const Announce& a);
// The label this host announces: none without gameplay mods; else `launcher_label` (BB_PARTY_MODS_LABEL, set by the
// launcher only for a curated mod it verified) when valid, else "mods:" + the first 4 bytes of the party mod hash in hex.
std::string mods_label(const char* launcher_label, bool gameplay_mods, const std::array<std::uint8_t, 32>& mods_hash);

// The name as the lobby accepts it: control, format, separator and surrogate characters dropped, runs of
// spaces folded, at most kMaxNameChars characters (and kMaxNameBytes bytes). Invalid UTF-8 gives "".
std::string clean_name(const std::string& utf8);
// UTF-16 (the game's character name, NUL terminated or `n` code units) to UTF-8; stops at the first NUL.
std::string utf16_to_utf8(const char16_t* s, std::size_t n);
// The datagram, or empty when the announce cannot be valid (empty name after cleaning, port < 1024,
// max_players outside 1..8, players > max_players).
std::vector<std::uint8_t> encode_announce(const Announce& a);
std::vector<std::uint8_t> encode_bye();

// Is `ip` (host order) a player address of the Nightwatch tunnel: 10.8.0.0/22 minus the network, the
// server (10.8.0.1) and the broadcast address, i.e. 10.8.0.2 .. 10.8.3.254 (0.7.8; was 10.8.0.0/24)?
// Inline (0.7.8, CL-79fe9c): the address choice and the punch's candidate filter use it too.
inline bool is_tunnel_player_address(std::uint32_t ip) {
    return (ip & 0xfffffc00u) == 0x0a080000u && ip != 0x0a080000u && ip != 0x0a080001u && ip != 0x0a0803ffu;
}

// The provider fills the live state each round and returns false while not hosting / switched off /
// without a name (nothing is sent; a bye goes out when it turns false after announcing).
using Provider = std::function<bool(Announce*)>;
void start(Provider provider);
void stop();  // sends a bye, joins the thread (bounded)
// Tests: BB_PARTY_VPNLOBBY_ADDR=<ip>:<port> sends from 127.0.0.1 to that lobby (no 10.8.0.x adapter needed).
}  // namespace bbnet::lobby
