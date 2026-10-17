// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by the party host service and the NP/HTTP layer: lenient JSON reads
// (bbhost's net::int_of / net::str_of), base64 and dotted IPv4 text.
#pragma once

#include "json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace bbnet::party {

inline long long int_of(const json::Value& v, const char* key, long long def = 0) {
    const json::Value* m = v.type == json::Value::Type::Object ? v.find(key) : nullptr;
    if (!m) return def;
    if (m->type == json::Value::Type::Number) {
        // A peer's 1e300 (or NaN): converting it to an integer is undefined; clamp instead.
        const double d = m->number;
        if (!(d == d)) return def;
        if (d >= 9.2e18) return 9200000000000000000LL;
        if (d <= -9.2e18) return -9200000000000000000LL;
        return static_cast<long long>(d);
    }
    if (m->type == json::Value::Type::Bool) return m->boolean ? 1 : 0;
    if (m->type == json::Value::Type::String && !m->string.empty()) {
        char* end = nullptr;
        const long long x = std::strtoll(m->string.c_str(), &end, 0);
        return end && *end == 0 ? x : def;
    }
    return def;
}

inline std::string str_of(const json::Value& v, const char* key) {
    const json::Value* m = v.type == json::Value::Type::Object ? v.find(key) : nullptr;
    if (!m) return {};
    if (m->type == json::Value::Type::String) return m->string;
    if (m->type == json::Value::Type::Number) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", m->number);
        return buf;
    }
    return {};
}

inline std::string b64_encode(const std::uint8_t* p, std::size_t n) {
    static const char* const t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(p[i]) << 16) |
                                (i + 1 < n ? static_cast<std::uint32_t>(p[i + 1]) << 8 : 0u) |
                                (i + 2 < n ? static_cast<std::uint32_t>(p[i + 2]) : 0u);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < n ? t[(v >> 6) & 63] : '=';
        out += i + 2 < n ? t[v & 63] : '=';
    }
    return out;
}
inline std::string b64_encode(const std::string& s) {
    return b64_encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

inline std::vector<std::uint8_t> b64_decode(const std::string& in) {
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else continue;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return out;
}

// Strict base64 (standard alphabet, '=' padding only at the end, no other characters): false on
// anything else. For peer data whose size matters (the game's SummonData).
inline bool b64_decode_strict(const std::string& in, std::vector<std::uint8_t>* out) {
    out->clear();
    if (in.size() % 4 != 0) return false;
    std::uint32_t acc = 0;
    int bits = 0;
    std::size_t pad = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char ch = in[i];
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else if (ch == '=' && i + 2 >= in.size()) {
            ++pad;
            continue;
        } else return false;
        if (pad) return false;  // data after '='
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return pad <= 2;
}

// "a.b.c.d" -> network order (0 when not a dotted IPv4 address).
inline std::uint32_t ip_parse(const std::string& text) {
    unsigned a, b, c, d;
    char tail;
    if (text.empty() || std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return a | (b << 8) | (c << 16) | (d << 24);
}
// Network order -> "a.b.c.d" ("" for 0).
inline std::string ip_text(std::uint32_t nbo) {
    if (!nbo) return {};
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", nbo & 0xff, (nbo >> 8) & 0xff, (nbo >> 16) & 0xff, nbo >> 24);
    return buf;
}
inline bool ip_is_loopback(std::uint32_t nbo) { return (nbo & 0xff) == 127; }
// A private / LAN address (network order): 10/8, 172.16/12, 192.168/16, 100.64/10 (CGNAT, VPNs
// such as Tailscale), 169.254/16 (link-local). Not loopback.
inline bool ip_is_private(std::uint32_t nbo) {
    const std::uint8_t a = nbo & 0xff, b = (nbo >> 8) & 0xff;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127) ||
           (a == 169 && b == 254);
}

}  // namespace bbnet::party
