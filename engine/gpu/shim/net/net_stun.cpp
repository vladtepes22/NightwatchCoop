// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/net/stun.cpp @8f2746c
#include "net_stun.h"

#include <chrono>
#include <cstring>
#include <random>

namespace net::stun {

namespace {

constexpr std::uint16_t kBindingRequest = 0x0001, kBindingResponse = 0x0101;
constexpr std::uint16_t kMappedAddress = 0x0001, kXorMappedAddress = 0x0020, kXorMappedAddressOld = 0x8020;
constexpr std::uint16_t kBbhostHello = 0x8100, kBbhostRelay = 0x8101;
constexpr std::uint8_t kRelayVersion[4] = {'b', 'b', 'r', '1'};
constexpr std::uint32_t kCookie = 0x2112A442u;
constexpr std::uint8_t kFamilyIpv4 = 0x01;

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t rd32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
void wr16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void wr32(std::uint8_t* p, std::uint32_t v) {
    wr16(p, static_cast<std::uint16_t>(v >> 16));
    wr16(p + 2, static_cast<std::uint16_t>(v));
}
std::uint32_t to_nbo(std::uint32_t host) {
    std::uint8_t b[4];
    wr32(b, host);
    std::uint32_t v;
    std::memcpy(&v, b, 4);
    return v;
}

}  // namespace

std::size_t build_binding_request(std::uint8_t* out, std::uint8_t txid[kTxid], bool hello, const std::uint8_t* token) {
    static thread_local std::mt19937_64 rng(
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x5bd1e995u);
    wr32(txid, kCookie);
    for (std::size_t i = 4; i < kTxid; i += 4) wr32(txid + i, static_cast<std::uint32_t>(rng()));
    wr16(out, kBindingRequest);
    std::memcpy(out + 4, txid, kTxid);
    std::size_t n = kHeader;
    if (hello) {
        wr16(out + n, kBbhostHello);
        wr16(out + n + 2, 12);
        std::memcpy(out + n + 4, kRelayVersion, 4);
        if (token) std::memcpy(out + n + 8, token, kTokenLen);
        else std::memset(out + n + 8, 0, kTokenLen);
        n += 16;
    }
    wr16(out + 2, static_cast<std::uint16_t>(n - kHeader));
    return n;
}

bool parse_binding_response(const std::uint8_t* d, std::size_t n, const std::uint8_t txid[kTxid], std::uint32_t* addr,
                            std::uint16_t* port, Relay* relay) {
    if (relay) *relay = Relay{};
    if (n < kHeader || rd16(d) != kBindingResponse || std::memcmp(d + 4, txid, kTxid) != 0) return false;
    const std::size_t len = rd16(d + 2);
    if (kHeader + len > n) return false;
    // The response's own first four id bytes: the cookie when it followed
    // RFC 5389, else whatever the request carried (the same bytes here).
    const std::uint32_t xor_addr_key = rd32(d + 4);
    const std::uint16_t xor_port_key = rd16(d + 4);
    std::uint32_t mapped = 0, xored = 0;
    std::uint16_t mapped_port = 0, xored_port = 0;
    bool have_mapped = false, have_xored = false;
    for (std::size_t at = kHeader; at + 4 <= kHeader + len;) {
        const std::uint16_t type = rd16(d + at), alen = rd16(d + at + 2);
        const std::uint8_t* v = d + at + 4;
        if (at + 4 + alen > kHeader + len) break;
        if (type == kBbhostRelay && relay && alen >= 20 && std::memcmp(v, kRelayVersion, 4) == 0) {
            relay->present = true;
            std::memcpy(relay->token, v + 4, kTokenLen);
            relay->vport = rd16(v + 12);
            relay->observed_addr = to_nbo(rd32(v + 14));
            relay->observed_port = rd16(v + 18);
        } else if (alen >= 8 && v[1] == kFamilyIpv4) {
            if (type == kMappedAddress) {
                mapped_port = rd16(v + 2);
                mapped = rd32(v + 4);
                have_mapped = true;
            } else if (type == kXorMappedAddress || type == kXorMappedAddressOld) {
                xored_port = static_cast<std::uint16_t>(rd16(v + 2) ^ xor_port_key);
                xored = rd32(v + 4) ^ xor_addr_key;
                have_xored = true;
            }
        }
        at += 4 + ((alen + 3u) & ~3u);
    }
    if (!have_mapped && !have_xored) return false;
    const std::uint32_t a = have_mapped ? mapped : xored;
    const std::uint16_t p = have_mapped ? mapped_port : xored_port;
    if (!a || !p) return false;
    if (addr) *addr = to_nbo(a);
    if (port) *port = p;
    return true;
}

// --- bbport: the server side ---

bool parse_binding_request(const std::uint8_t* d, std::size_t n, std::uint8_t txid[kTxid], bool* hello,
                           std::uint8_t token[kTokenLen]) {
    if (hello) *hello = false;
    if (token) std::memset(token, 0, kTokenLen);
    if (n < kHeader || rd16(d) != kBindingRequest) return false;
    const std::size_t len = rd16(d + 2);
    if (kHeader + len > n || (len & 3u)) return false;
    std::memcpy(txid, d + 4, kTxid);
    for (std::size_t at = kHeader; at + 4 <= kHeader + len;) {
        const std::uint16_t type = rd16(d + at), alen = rd16(d + at + 2);
        const std::uint8_t* v = d + at + 4;
        if (at + 4 + alen > kHeader + len) return false;
        if (type == kBbhostHello && alen >= 12 && std::memcmp(v, kRelayVersion, 4) == 0) {
            if (hello) *hello = true;
            if (token) std::memcpy(token, v + 4, kTokenLen);
        }
        at += 4 + ((alen + 3u) & ~3u);
    }
    return true;
}

std::size_t build_binding_response(std::uint8_t* out, const std::uint8_t txid[kTxid], std::uint32_t addr,
                                   std::uint16_t port, const Relay* relay) {
    std::uint8_t a[4];
    std::memcpy(a, &addr, 4);  // network order: the wire's
    const std::uint32_t host_addr = rd32(a);
    wr16(out, kBindingResponse);
    std::memcpy(out + 4, txid, kTxid);
    std::size_t n = kHeader;
    // MAPPED-ADDRESS {0, family, port, addr}
    wr16(out + n, kMappedAddress);
    wr16(out + n + 2, 8);
    out[n + 4] = 0;
    out[n + 5] = kFamilyIpv4;
    wr16(out + n + 6, port);
    wr32(out + n + 8, host_addr);
    n += 12;
    // XOR-MAPPED-ADDRESS, keyed with the id's first bytes (parse_binding_response).
    wr16(out + n, kXorMappedAddress);
    wr16(out + n + 2, 8);
    out[n + 4] = 0;
    out[n + 5] = kFamilyIpv4;
    wr16(out + n + 6, static_cast<std::uint16_t>(port ^ rd16(txid)));
    wr32(out + n + 8, host_addr ^ rd32(txid));
    n += 12;
    if (relay && relay->present) {
        // BBHOST-RELAY {"bbr1", token, port u16, observed addr u32, observed port u16}
        wr16(out + n, kBbhostRelay);
        wr16(out + n + 2, 20);
        std::memcpy(out + n + 4, kRelayVersion, 4);
        std::memcpy(out + n + 8, relay->token, kTokenLen);
        wr16(out + n + 16, relay->vport);
        std::uint8_t o[4];
        std::memcpy(o, &relay->observed_addr, 4);
        wr32(out + n + 18, rd32(o));
        wr16(out + n + 22, relay->observed_port);
        n += 24;
    }
    wr16(out + 2, static_cast<std::uint16_t>(n - kHeader));
    return n;
}

}  // namespace net::stun
