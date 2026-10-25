// SPDX-License-Identifier: GPL-3.0-or-later
// Party codes: what a host reads out to friends. "BBP3-" + Crockford base32 symbols (groups of 4,
// '-' separated) of a bit string (MSB first, not byte aligned):
//   flags 4 bits (kCodeFlagLan | kCodeFlagPasswordRequired | kCodeFlagRelayOnly | kCodeFlagDefaultPort)
//   ipv4 32 bits (LAN code: only the last octet, 8 bits)
//   port 16 bits (absent with kCodeFlagDefaultPort = 9307)
//   secret 32 bits with kCodeFlagPasswordRequired, else 48 bits
//   check: the bits left in the last symbol, at least 8 (8..12), a CRC of that width (crc_bits in
//   party_code.cpp: polynomials 0x07 / 0x119 / 0x233 / 0x385 / 0x80F, init all ones). Each
//   polynomial has the x^0 term, so every burst of up to that many bits is caught: any one wrong
//   symbol always, two neighbouring ones (or a swapped pair) whenever the check has 10+ bits.
// Lengths (default port; another port adds 3 symbols):
//   Internet + password 16 symbols  BBP3-XXXX-XXXX-XXXX-XXXX        (BBP2: 20)
//   Internet, no password 19        BBP3-XXXX-XXXX-XXXX-XXXX-XXX    (BBP2: 20)
//   LAN + password 11               BBP3-XXXX-XXXX-XXX              (BBP2: 15)
//   LAN, no password 14             BBP3-XXXX-XXXX-XXXX-XX          (BBP2: 15)
//
// Security trade-off (party key = Argon2i(password || secret), party_crypto.h). The guest proves
// the key to the host (keyed BLAKE2b over both nonces) and the host never sends a proof to an
// unproven guest, so a stranger who finds the port can only guess online, one TCP handshake per
// guess. Someone who records a real guest's handshake (on path, or on the same LAN) can guess
// offline, at one Argon2i (8 MiB, ~30 ms) per guess.
//  - No password: the 48-bit secret is all that keeps strangers out: 2^48 online handshakes, or
//    ~2^48 Argon2i offline (~270,000 CPU-core years). It stays 48 bits; never shortened.
//  - With a password: the password already carries the secrecy, because a host with a password
//    also admits a guest that knows only the password (a plain host:port, party_link.cpp
//    password_key). The secret therefore adds nothing against an online attacker either way, and
//    its 32 bits still cost an offline attacker 2^32 Argon2i (~4 core years) per password guess.
//    So the party is as strong as its password, exactly as with the 48-bit secret: pick a real one.
//    The host keeps a 4-byte secret while a password is set and a 6-byte one without (it
//    shortens / lengthens the stored secret, and the code changes once, party_runtime.cpp
//    start_host).
// The secret sits in the 8-byte slot zero padded, so the party key is the same as for the BBP2 /
// BBP1 code of that secret. A LAN code's guest takes the first three octets from its own best
// local IPv4 (fill_lan_prefix), or asks the LAN (party_lan.h) when it has none.
// Older forms still decode: "BBP2-" (flags u8 | ipv4[4] (LAN: last octet) | port u16 BE (absent
// with kCodeFlagDefaultPort) | secret[6] | crc8, poly 0x07, init 0) and "BBP1-" (29 symbols of
// ver u8 = 1 | flags u8 | ipv4[4] | port u16 BE | secret[8] | crc16-CCITT BE).
// Decoding is case-insensitive, ignores '-' and blanks, reads I/L as 1 and O as 0; the check
// catches any other typo. A plain "host:port" (or "host" = default port) is accepted too, with
// no secret: the password alone then keys the party.
// IPv6 tail (F2, 0.7.1): "<code>+6-XXXX-..." carries the host's global IPv6 address (16 bytes +
// crc8: 28 symbols; the port is the code's). The host adds it only while its IPv4 port is not open
// (network_robustness.md 4a); a code without it decodes as before.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace party {

constexpr std::uint16_t kDefaultPartyPort = 9307;
constexpr std::size_t kSecretBytes = 6;              // a BBP3 / BBP2 code's secret without a password
constexpr std::size_t kSecretBytesWithPassword = 4;  // BBP3 with kCodeFlagPasswordRequired

// What every prompt, tooltip and error shows as a party code: a real BBP3 Internet code with a
// password (203.0.113.7 is a documentation address, TEST-NET-3), and a LAN one.
#define PARTY_CODE_EXAMPLE "BBP3-NJR0-E43N-WWN9-2C2P"
#define PARTY_LAN_CODE_EXAMPLE "BBP3-PAJY-EAMH-7TD"

enum : std::uint8_t {
    kCodeFlagLan = 1,               // a LAN address (make_lan_code)
    kCodeFlagPasswordRequired = 2,  // the host also set BB_PARTY_PASSWORD
    kCodeFlagRelayOnly = 4,         // guests must route game P2P through the host relay
    kCodeFlagDefaultPort = 8,       // BBP3 / BBP2 wire only: no port, 9307 (never in PartyCode::flags)
};

struct PartyCode {
    std::uint8_t version = 3;  // the form encode_party_code writes (3, or 2 / 1 for an old secret)
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 4> ipv4{};  // a.b.c.d in order
    std::uint16_t port = kDefaultPartyPort;
    std::array<std::uint8_t, 8> secret{};
    bool plain = false;     // decoded from "host:port": no secret, flags unknown
    bool lan_partial = false;  // a BBP3 / BBP2 LAN code: only ipv4[3] came with it (fill_lan_prefix)
    std::string host;       // dotted ipv4 (or the hostname given in plain form)
    bool has_ipv6 = false;  // the "+6" tail: the host's IPv6 address (same port)
    std::array<std::uint8_t, 16> ipv6{};

    std::string address() const;  // "a.b.c.d:port" (or host:port for a plain hostname)
};

// "BBP3-XXXX-XXXX-...". Normalised upper case. A secret that does not fit BBP3 (code_fits_v3)
// gives the BBP2 form, one that does not fit BBP2 either (secret[6] or [7] set) the BBP1 form.
std::string encode_party_code(const PartyCode& code);
// The secret's bytes past what BBP3 carries (4 with kCodeFlagPasswordRequired, else 6) are zero.
bool code_fits_v3(const PartyCode& code);
// A BBP3 / BBP2 / BBP1 code or "host:port"; on failure `error` says what is wrong (bad character, wrong
// length, checksum mismatch = typo, unknown version).
bool decode_party_code(std::string_view text, PartyCode* out, std::string* error);

std::uint16_t crc16_ccitt(const std::uint8_t* d, std::size_t n);  // poly 0x1021, init 0xFFFF
std::uint8_t crc8(const std::uint8_t* d, std::size_t n);           // poly 0x07, init 0 ("123456789" = 0xF4)

// A LAN code's (lan_partial) first three octets from the guest's own address: ipv4 and host set.
void fill_lan_prefix(PartyCode* code, const std::array<std::uint8_t, 4>& local);

// The best local IPv4 for a LAN code (network order bytes in out): an up, non-loopback adapter,
// preferring one with a default gateway and skipping virtual/tunnel adapters (Hyper-V, VPN,
// VirtualBox, VMware, WSL) when a physical one exists. False when none.
// The LAN code, the LAN-discovery reply and the UPnP mapping all go through this, so they carry
// the address the game itself uses: the net layer installs a provider (set_local_ipv4_provider)
// that answers BB_PARTY_LOCAL_IP, then the chosen VPN adapter, then the best adapter. Without a
// provider (unit tests, tools) it is adapter_best_local_ipv4().
bool best_local_ipv4(std::array<std::uint8_t, 4>* out);
using LocalIpv4Provider = bool (*)(std::array<std::uint8_t, 4>* out);
void set_local_ipv4_provider(LocalIpv4Provider fn);
// The plain adapter scan best_local_ipv4() falls back to (no override, no VPN choice).
bool adapter_best_local_ipv4(std::array<std::uint8_t, 4>* out);

// Codes for the host: the Internet code carries the public address (STUN / UPnP / override,
// see party_addr.h), the LAN code the best local address with kCodeFlagLan.
std::string make_party_code(const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port,
                            const std::array<std::uint8_t, 8>& secret, std::uint8_t flags,
                            const std::array<std::uint8_t, 16>* ipv6 = nullptr);
bool make_lan_code(std::uint16_t port, const std::array<std::uint8_t, 8>& secret, std::uint8_t flags,
                   std::string* code);

// A guest joining with this code uses no password: a party code whose host set none
// (no kCodeFlagPasswordRequired). A plain host:port carries no flags: the password is kept.
inline bool code_ignores_password(const PartyCode& code) {
    return !code.plain && !(code.flags & kCodeFlagPasswordRequired);
}

bool parse_ipv4(std::string_view s, std::array<std::uint8_t, 4>* out);
std::string format_ipv4(const std::array<std::uint8_t, 4>& ip);

// ---- The host's code secret from one launch to the next (party_runtime.cpp start_host) ----
// The party code stays the same across game launches until the host asks for a new one
// (launcher Party tab "New code", System > Party mode > New party code). Where it comes from:
//   CrashRestart  the crash marker (party_state.json) of a restarted host: with its member slots
//   Menu          a party game started from the game's menus: party.json's host_secret
//   Saved         a normal launch: <user>/party_secret.txt (never the marker's member slots)
//   New           none of them (first host, or after a revoke): fit_secret_to_code makes one
enum class HostSecretFrom { CrashRestart, Menu, Saved, New };
const char* host_secret_from_name(HostSecretFrom from);
// 16 hex digits (blanks / line ends around them ignored) -> the 8-byte slot. False otherwise.
bool parse_secret_hex(std::string_view text, std::array<std::uint8_t, 8>* out);
// The choice above. `restart_secret`: the crash restart's secret (null: not a restart, or none
// found); `menu_hex`: party.json's host_secret ("" when this run is not a menu party game);
// `saved_text`: party_secret.txt's contents ("" when there is none). A source that does not parse
// is skipped. New leaves *secret all zero.
HostSecretFrom pick_host_secret(const std::array<std::uint8_t, 8>* restart_secret, const std::string& menu_hex,
                                const std::string& saved_text, std::array<std::uint8_t, 8>* secret);

// The secret as the BBP3 code carries it: 4 bytes while a password is set (the password keeps
// strangers out), 6 without (the secret alone does). A stored secret of the other size is
// shortened / lengthened, a missing one made: the party key changes with it, so friends need the
// new code once (setting or removing a password therefore still gives a new code).
enum class SecretFit { Kept, Made, Shortened, Lengthened };
SecretFit fit_secret_to_code(std::array<std::uint8_t, 8>* secret, bool password);
// A revoke: a fresh random secret of the right size for `password`, never equal to `old`.
std::array<std::uint8_t, 8> new_host_secret(const std::array<std::uint8_t, 8>& old, bool password);

}  // namespace party
