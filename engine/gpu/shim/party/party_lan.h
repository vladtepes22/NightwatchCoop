// SPDX-License-Identifier: GPL-3.0-or-later
// Same-network fallback for an Internet party code. A guest on the host's own LAN usually cannot
// reach the host's public address (most home routers lack hairpin NAT: "Failed to acquire
// Bloodborne server information"). When the direct connect does not answer quickly, or the guest's
// own public IP (STUN) is the code's IP, the guest asks its LAN for the host:
//
//   probe  (guest -> 255.255.255.255 and each adapter's directed broadcast, the code's port, UDP)
//          [0xfa]['D'][ver 1][type 1] nonce[8] tag[16]                                   28 bytes
//   reply  (host -> the probe's source, from the party UDP port)
//          [0xfa]['D'][ver 1][type 2] nonce[8] ipv4[4] port u16 BE tag[16]               34 bytes
//
//   key = the party key (party_crypto.h derive_party_key(password, code secret); a plain host:port
//         guest: the password with a zero secret, so only with a password)
//   tag = BLAKE2b-256(key || "bbp-lan-probe" || nonce)[0..16]               (probe)
//         BLAKE2b-256(key || "bbp-lan-reply" || nonce || ipv4 || port)[0..16]  (reply)
// The secret never leaves the process; only a host holding the same key answers, and the guest
// accepts only a reply that echoes its fresh nonce under the same key. The guest connects to the
// reply's source address (the host's address on the guest's network) at the reply's port.
// Amplification: the reply is 34 bytes for a 28-byte probe, only for valid probes, and the host
// answers at most kRepliesPerSecond per second (ReplyLimiter).
#pragma once

#include "party_crypto.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace party::lan {

using Clock = std::chrono::steady_clock;
using Nonce = std::array<std::uint8_t, 8>;

constexpr std::uint8_t kMagic0 = 0xfa, kMagic1 = 'D', kVersion = 1;
constexpr std::uint8_t kTypeProbe = 1, kTypeReply = 2;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kProbeSize = 4 + 8 + kTagSize;          // 28
constexpr std::size_t kReplySize = 4 + 8 + 4 + 2 + kTagSize;  // 34
constexpr int kRepliesPerSecond = 5;

// A LAN discovery datagram (probe or reply) by its first two bytes.
inline bool is_lan(const std::uint8_t* d, std::size_t n) { return n >= 4 && d[0] == kMagic0 && d[1] == kMagic1; }

std::array<std::uint8_t, kProbeSize> encode_probe(const crypto::Key& key, const Nonce& nonce);
// True when `d` is a probe made with `key` (constant-time tag compare); its nonce in *nonce.
bool verify_probe(const crypto::Key& key, const std::uint8_t* d, std::size_t n, Nonce* nonce);

// ipv4 in order a.b.c.d, port host order.
std::array<std::uint8_t, kReplySize> encode_reply(const crypto::Key& key, const Nonce& nonce,
                                                  const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port);
// True when `d` is a reply to our probe (`nonce`) under `key`.
bool parse_reply(const crypto::Key& key, const Nonce& nonce, const std::uint8_t* d, std::size_t n,
                 std::array<std::uint8_t, 4>* ipv4, std::uint16_t* port);

// Token bucket: at most kRepliesPerSecond replies per second (burst of the same size).
class ReplyLimiter {
public:
    bool allow(Clock::time_point now);

private:
    double tokens_ = kRepliesPerSecond;
    Clock::time_point last_{};
    bool started_ = false;
};

// Host side: the answer to one datagram, or empty when it is not a valid probe for any of `keys`
// (the party key, and the password-only key of a host with a password) or the limiter says no.
std::vector<std::uint8_t> answer_probe(const std::vector<crypto::Key>& keys, const std::uint8_t* d, std::size_t n,
                                       const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port,
                                       ReplyLimiter* limiter, Clock::time_point now);

// The broadcast targets (network order IPv4): 255.255.255.255 and every up adapter's directed
// broadcast (address | ~mask), loopback and link-local skipped, no duplicates.
std::vector<std::uint32_t> broadcast_targets();

// Guest: a non-blocking TCP connect to ip:port (network order addr, host order port) that
// answers within timeout_ms. The socket is closed at once (the host ignores a connection that
// never sends HELLO).
bool tcp_reachable(std::uint32_t addr_nbo, std::uint16_t port, int timeout_ms);

struct Found {
    std::uint32_t addr_nbo = 0;           // the reply's source address
    std::uint16_t port = 0;               // host order: the host's party port
    std::array<std::uint8_t, 4> said{};   // the address the host reported (its best LAN address)
};
// Guest: sends a probe to each of `targets` (network order) at `port` up to three times within
// budget_ms and waits for a valid reply. `loopback`: binds 127.0.0.1 (local tests; targets are
// then normally just 127.0.0.1). False when nothing answered.
bool discover(const crypto::Key& key, const std::vector<std::uint32_t>& targets, std::uint16_t port, int budget_ms,
              bool loopback, Found* out, std::string* note);

}  // namespace party::lan
