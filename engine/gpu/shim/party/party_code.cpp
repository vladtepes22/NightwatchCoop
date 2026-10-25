// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_code.h"
#include "party_crypto.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace party {

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t kRawSize = 18;
constexpr std::size_t kSymbols = (kRawSize * 8 + 4) / 5;  // 29
constexpr char kPrefix[] = "BBP3";
constexpr char kPrefixV2[] = "BBP2";
constexpr char kPrefixV1[] = "BBP1";

int symbol_value(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'I' || c == 'L') c = '1';
    if (c == 'O') c = '0';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

bool parse_port(std::string_view s, std::uint16_t* port) {
    if (s.empty() || s.size() > 5) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v == 0 || v > 65535) return false;
    *port = static_cast<std::uint16_t>(v);
    return true;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

}  // namespace

std::uint16_t crc16_ccitt(const std::uint8_t* d, std::size_t n) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<std::uint16_t>(d[i] << 8);
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
    }
    return crc;
}

bool parse_ipv4(std::string_view s, std::array<std::uint8_t, 4>* out) {
    std::array<std::uint8_t, 4> ip{};
    int part = 0;
    unsigned v = 0;
    int digits = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (digits == 0 || v > 255 || part > 3) return false;
            ip[part++] = static_cast<std::uint8_t>(v);
            v = 0;
            digits = 0;
        } else if (s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + static_cast<unsigned>(s[i] - '0');
            if (++digits > 3) return false;
        } else {
            return false;
        }
    }
    if (part != 4) return false;
    *out = ip;
    return true;
}

std::string format_ipv4(const std::array<std::uint8_t, 4>& ip) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return buf;
}

std::string PartyCode::address() const {
    std::string h = !host.empty() ? host : lan_partial ? "*.*.*." + std::to_string(ipv4[3]) : format_ipv4(ipv4);
    return h + ":" + std::to_string(port);
}

namespace {

std::string to_symbols(const std::uint8_t* raw, std::size_t n, const char* prefix) {
    std::string sym;
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        acc = (acc << 8) | raw[i];
        bits += 8;
        while (bits >= 5) {
            sym += kAlphabet[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) sym += kAlphabet[(acc << (5 - bits)) & 31];
    std::string out = prefix;
    for (std::size_t i = 0; i < sym.size(); ++i) {
        if (i % 4 == 0) out += '-';
        out += sym[i];
    }
    return out;
}

// Symbols back to bytes: false unless exactly `n` bytes with zero padding bits.
bool from_symbols(const std::vector<int>& vals, std::uint8_t* raw, std::size_t n) {
    std::size_t got = 0;
    std::uint32_t acc = 0;
    int bits = 0;
    for (int v : vals) {
        acc = (acc << 5) | static_cast<std::uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            if (got >= n) return false;
            raw[got++] = static_cast<std::uint8_t>(acc >> (bits - 8));
            bits -= 8;
        }
    }
    return got == n && (acc & ((1u << bits) - 1)) == 0;
}

std::size_t symbols_for(std::size_t bytes) { return (bytes * 8 + 4) / 5; }

// BBP2 payload size (with the crc) for these wire flags.
std::size_t v2_size(std::uint8_t wire_flags) {
    return 1 + ((wire_flags & kCodeFlagLan) ? 1 : 4) + ((wire_flags & kCodeFlagDefaultPort) ? 0 : 2) + 6 + 1;
}

// BBP3: payload bits (flags nibble, address, port, secret) for these wire flags, and the symbol
// count: the smallest that leaves at least 8 check bits.
std::size_t v3_payload_bits(std::uint8_t wire_flags) {
    return 4 + ((wire_flags & kCodeFlagLan) ? 8 : 32) + ((wire_flags & kCodeFlagDefaultPort) ? 0 : 16) +
           8 * ((wire_flags & kCodeFlagPasswordRequired) ? kSecretBytesWithPassword : kSecretBytes);
}
std::size_t v3_symbols(std::uint8_t wire_flags) { return (v3_payload_bits(wire_flags) + 8 + 4) / 5; }

// A CRC of `k` (8..12) bits over a bit string, MSB first, init all ones. Every polynomial has
// the x^0 term, so every burst of up to k bits is caught: any one mistyped symbol, and with
// k >= 10 any two neighbouring ones (a swapped pair too).
std::uint32_t crc_bits(const std::vector<std::uint8_t>& bits, std::size_t n, int k) {
    static const std::uint32_t kPoly[] = {0x07, 0x119, 0x233, 0x385, 0x80F};  // k = 8 .. 12
    const std::uint32_t poly = kPoly[k - 8], mask = (1u << k) - 1;
    std::uint32_t reg = mask;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t top = ((reg >> (k - 1)) & 1u) ^ bits[i];
        reg = (reg << 1) & mask;
        if (top) reg ^= poly;
    }
    return reg;
}

void put_bits(std::vector<std::uint8_t>* bits, std::uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bits->push_back(static_cast<std::uint8_t>((v >> i) & 1u));
}

std::uint32_t get_bits(const std::vector<std::uint8_t>& bits, std::size_t* at, int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | bits[(*at)++];
    return v;
}

std::uint8_t wire_flags_of(const PartyCode& code) {
    std::uint8_t wire =
        static_cast<std::uint8_t>(code.flags & (kCodeFlagLan | kCodeFlagPasswordRequired | kCodeFlagRelayOnly));
    if (code.port == kDefaultPartyPort) wire |= kCodeFlagDefaultPort;
    return wire;
}

std::string encode_v3(const PartyCode& code) {
    const std::uint8_t wire = wire_flags_of(code);
    std::vector<std::uint8_t> bits;
    put_bits(&bits, wire, 4);
    if (wire & kCodeFlagLan) {
        put_bits(&bits, code.ipv4[3], 8);
    } else {
        for (std::uint8_t b : code.ipv4) put_bits(&bits, b, 8);
    }
    if (!(wire & kCodeFlagDefaultPort)) put_bits(&bits, code.port, 16);
    const std::size_t secret_bytes = (wire & kCodeFlagPasswordRequired) ? kSecretBytesWithPassword : kSecretBytes;
    for (std::size_t i = 0; i < secret_bytes; ++i) put_bits(&bits, code.secret[i], 8);
    const std::size_t payload = bits.size();
    const std::size_t symbols = v3_symbols(wire);
    const int k = static_cast<int>(symbols * 5 - payload);
    put_bits(&bits, crc_bits(bits, payload, k), k);
    std::string out = kPrefix;
    std::size_t at = 0;
    for (std::size_t i = 0; i < symbols; ++i) {
        if (i % 4 == 0) out += '-';
        out += kAlphabet[get_bits(bits, &at, 5)];
    }
    return out;
}

}  // namespace

bool code_fits_v3(const PartyCode& code) {
    const std::size_t used = (code.flags & kCodeFlagPasswordRequired) ? kSecretBytesWithPassword : kSecretBytes;
    for (std::size_t i = used; i < code.secret.size(); ++i)
        if (code.secret[i]) return false;
    return true;
}

namespace {

}  // namespace

std::uint8_t crc8(const std::uint8_t* d, std::size_t n) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x80) ? static_cast<std::uint8_t>((crc << 1) ^ 0x07) : static_cast<std::uint8_t>(crc << 1);
    }
    return crc;
}

void fill_lan_prefix(PartyCode* code, const std::array<std::uint8_t, 4>& local) {
    code->ipv4[0] = local[0];
    code->ipv4[1] = local[1];
    code->ipv4[2] = local[2];
    code->host = format_ipv4(code->ipv4);
}

namespace {
// The "+6" tail: 16 address bytes and their crc8, 28 symbols.
std::string ipv6_tail(const std::array<std::uint8_t, 16>& a) {
    std::uint8_t raw[17];
    std::memcpy(raw, a.data(), 16);
    raw[16] = crc8(raw, 16);
    return to_symbols(raw, sizeof raw, "+6");
}
constexpr std::size_t kIpv6TailSymbols = (17 * 8 + 4) / 5;  // 28
}  // namespace

std::string encode_party_code(const PartyCode& code) {
    if (code.has_ipv6) {
        PartyCode v4 = code;
        v4.has_ipv6 = false;
        return encode_party_code(v4) + ipv6_tail(code.ipv6);
    }
    if (code.version >= 3 && code_fits_v3(code)) return encode_v3(code);
    const bool fits_v2 = code.secret[6] == 0 && code.secret[7] == 0;
    if (code.version >= 2 && fits_v2) {
        const std::uint8_t wire = wire_flags_of(code);
        std::uint8_t raw[16];
        std::size_t n = 0;
        raw[n++] = wire;
        if (wire & kCodeFlagLan) {
            raw[n++] = code.ipv4[3];
        } else {
            std::memcpy(raw + n, code.ipv4.data(), 4);
            n += 4;
        }
        if (!(wire & kCodeFlagDefaultPort)) {
            raw[n++] = static_cast<std::uint8_t>(code.port >> 8);
            raw[n++] = static_cast<std::uint8_t>(code.port);
        }
        std::memcpy(raw + n, code.secret.data(), 6);
        n += 6;
        raw[n] = crc8(raw, n);
        ++n;
        return to_symbols(raw, n, kPrefixV2);
    }
    std::uint8_t raw[kRawSize];
    raw[0] = 1;
    raw[1] = static_cast<std::uint8_t>(code.flags & ~kCodeFlagDefaultPort);
    std::memcpy(raw + 2, code.ipv4.data(), 4);
    raw[6] = static_cast<std::uint8_t>(code.port >> 8);
    raw[7] = static_cast<std::uint8_t>(code.port);
    std::memcpy(raw + 8, code.secret.data(), 8);
    std::uint16_t crc = crc16_ccitt(raw, 16);
    raw[16] = static_cast<std::uint8_t>(crc >> 8);
    raw[17] = static_cast<std::uint8_t>(crc);
    return to_symbols(raw, kRawSize, kPrefixV1);
}

bool decode_party_code(std::string_view text_in, PartyCode* out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    std::string text = trim(text_in);
    if (text.empty()) return fail("empty party code");
    if (const auto plus = text.find('+'); plus != std::string::npos) {
        // The IPv6 tail ("+6-XXXX-..."): the code before it decodes as usual.
        std::vector<int> vals;
        std::size_t i = plus + 1;
        while (i < text.size() && (text[i] == ' ' || text[i] == '-')) ++i;
        if (i >= text.size() || text[i] != '6') return fail("unknown part after '+' in the party code");
        for (++i; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '-' || c == ' ' || c == '\t') continue;
            const int v = symbol_value(c);
            if (v < 0) return fail("invalid character in the party code's IPv6 part");
            vals.push_back(v);
        }
        std::uint8_t raw[17];
        if (vals.size() != kIpv6TailSymbols || !from_symbols(vals, raw, sizeof raw) || crc8(raw, 16) != raw[16])
            return fail("party code IPv6 part checksum mismatch (typo?)");
        PartyCode pc;
        if (!decode_party_code(std::string_view(text).substr(0, plus), &pc, error)) return false;
        if (pc.plain) return fail("an IPv6 part needs a BBP party code before it");
        pc.has_ipv6 = true;
        std::memcpy(pc.ipv6.data(), raw, 16);
        *out = pc;
        return true;
    }

    std::string upper;
    for (char c : text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    // "BBP3", "BBP2", or "BBP1" / "BBPI" / "BBPL" (1 typed as I or L).
    const int ver = upper.size() >= 4 && upper.compare(0, 3, "BBP") == 0 ? symbol_value(upper[3]) : -1;
    bool coded = (ver >= 1 && ver <= 3) && (upper.size() == 4 || upper[4] == '-' || upper[4] == ' ');
    if (!coded) {
        // host:port, or a bare host (default port).
        PartyCode pc;
        pc.plain = true;
        std::string host = text;
        std::size_t colon = text.rfind(':');
        if (colon != std::string::npos) {
            host = text.substr(0, colon);
            if (!parse_port(std::string_view(text).substr(colon + 1), &pc.port))
                return fail("bad port in host:port (expected 1-65535)");
        }
        if (host.empty()) return fail("missing host in host:port");
        for (char c : host)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'))
                return fail("not a party code (like " PARTY_CODE_EXAMPLE ") or host:port");
        pc.host = host;
        if (parse_ipv4(host, &pc.ipv4)) pc.host = format_ipv4(pc.ipv4);
        *out = pc;
        return true;
    }

    std::vector<int> vals;
    for (std::size_t i = 4; i < upper.size(); ++i) {
        char c = upper[i];
        if (c == '-' || c == ' ' || c == '\t') continue;
        int v = symbol_value(c);
        if (v < 0) {
            if (error) *error = std::string("invalid character '") + c + "' in party code";
            return false;
        }
        vals.push_back(v);
    }
    auto wrong_length = [&](std::size_t want) {
        if (error)
            *error = "party code has " + std::to_string(vals.size()) + " characters after BBP" + std::to_string(ver) +
                     ", expected " + std::to_string(want);
        return false;
    };

    PartyCode pc;
    pc.version = static_cast<std::uint8_t>(ver);
    if (ver == 3) {
        if (vals.empty()) return wrong_length(v3_symbols(kCodeFlagDefaultPort | kCodeFlagPasswordRequired));
        const std::uint8_t wire = static_cast<std::uint8_t>(vals[0] >> 1);
        const std::size_t symbols = v3_symbols(wire);
        if (vals.size() != symbols) {
            // A typo in the flags changes the expected length: a code of another valid length is a typo.
            for (std::uint8_t f = 0; f < 16; ++f)
                if (vals.size() == v3_symbols(f)) return fail("party code checksum mismatch (typo?)");
            return wrong_length(symbols);
        }
        std::vector<std::uint8_t> bits;
        for (int v : vals) put_bits(&bits, static_cast<std::uint32_t>(v), 5);
        const std::size_t payload = v3_payload_bits(wire);
        const int k = static_cast<int>(bits.size() - payload);
        std::size_t at = payload;
        if (get_bits(bits, &at, k) != crc_bits(bits, payload, k)) return fail("party code checksum mismatch (typo?)");
        at = 4;
        pc.flags = static_cast<std::uint8_t>(wire & ~kCodeFlagDefaultPort);
        if (wire & kCodeFlagLan) {
            pc.ipv4 = {0, 0, 0, static_cast<std::uint8_t>(get_bits(bits, &at, 8))};
            pc.lan_partial = true;
        } else {
            for (auto& b : pc.ipv4) b = static_cast<std::uint8_t>(get_bits(bits, &at, 8));
        }
        pc.port = (wire & kCodeFlagDefaultPort) ? kDefaultPartyPort
                                                : static_cast<std::uint16_t>(get_bits(bits, &at, 16));
        pc.secret = {};
        const std::size_t secret_bytes = (wire & kCodeFlagPasswordRequired) ? kSecretBytesWithPassword : kSecretBytes;
        for (std::size_t i = 0; i < secret_bytes; ++i) pc.secret[i] = static_cast<std::uint8_t>(get_bits(bits, &at, 8));
        if (!pc.lan_partial) pc.host = format_ipv4(pc.ipv4);
        if (pc.port == 0) return fail("party code has port 0");
        *out = pc;
        return true;
    }
    if (ver == 2) {
        if (vals.size() < 2) return wrong_length(symbols_for(v2_size(kCodeFlagDefaultPort)));
        const std::uint8_t wire = static_cast<std::uint8_t>((vals[0] << 3) | (vals[1] >> 2));
        const std::size_t size = v2_size(wire);
        if (vals.size() != symbols_for(size)) {
            // A typo in the flags changes the expected length: a code of another valid length is a typo.
            for (std::uint8_t f : {0, 1, 8, 9})
                if (vals.size() == symbols_for(v2_size(f))) return fail("party code checksum mismatch (typo?)");
            return wrong_length(size);
        }
        std::uint8_t raw[16];
        if (!from_symbols(vals, raw, size) || crc8(raw, size - 1) != raw[size - 1])
            return fail("party code checksum mismatch (typo?)");
        if (wire & ~(kCodeFlagLan | kCodeFlagPasswordRequired | kCodeFlagRelayOnly | kCodeFlagDefaultPort))
            return fail("party code from a newer version of the port");
        std::size_t n = 1;
        pc.flags = static_cast<std::uint8_t>(wire & ~kCodeFlagDefaultPort);
        if (wire & kCodeFlagLan) {
            pc.ipv4 = {0, 0, 0, raw[n++]};
            pc.lan_partial = true;
        } else {
            std::memcpy(pc.ipv4.data(), raw + n, 4);
            n += 4;
        }
        if (wire & kCodeFlagDefaultPort) {
            pc.port = kDefaultPartyPort;
        } else {
            pc.port = static_cast<std::uint16_t>((raw[n] << 8) | raw[n + 1]);
            n += 2;
        }
        pc.secret = {};
        std::memcpy(pc.secret.data(), raw + n, 6);
        if (!pc.lan_partial) pc.host = format_ipv4(pc.ipv4);
        if (pc.port == 0) return fail("party code has port 0");
        *out = pc;
        return true;
    }

    if (vals.size() != kSymbols) return wrong_length(kSymbols);
    std::uint8_t raw[kRawSize];
    if (!from_symbols(vals, raw, kRawSize)) return fail("party code checksum mismatch (typo?)");
    std::uint16_t crc = static_cast<std::uint16_t>((raw[16] << 8) | raw[17]);
    if (crc != crc16_ccitt(raw, 16)) return fail("party code checksum mismatch (typo?)");
    if (raw[0] != 1) return fail("party code from a newer version of the port");

    pc.version = raw[0];
    pc.flags = static_cast<std::uint8_t>(raw[1] & ~kCodeFlagDefaultPort);
    std::memcpy(pc.ipv4.data(), raw + 2, 4);
    pc.port = static_cast<std::uint16_t>((raw[6] << 8) | raw[7]);
    std::memcpy(pc.secret.data(), raw + 8, 8);
    pc.host = format_ipv4(pc.ipv4);
    if (pc.port == 0) return fail("party code has port 0");
    *out = pc;
    return true;
}

std::string make_party_code(const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port,
                            const std::array<std::uint8_t, 8>& secret, std::uint8_t flags,
                            const std::array<std::uint8_t, 16>* ipv6) {
    PartyCode pc;
    pc.flags = flags;
    pc.ipv4 = ipv4;
    pc.port = port;
    pc.secret = secret;
    if (ipv6) {
        pc.has_ipv6 = true;
        pc.ipv6 = *ipv6;
    }
    return encode_party_code(pc);
}

bool make_lan_code(std::uint16_t port, const std::array<std::uint8_t, 8>& secret, std::uint8_t flags,
                   std::string* code) {
    std::array<std::uint8_t, 4> ip{};
    if (!best_local_ipv4(&ip)) return false;
    *code = make_party_code(ip, port, secret, static_cast<std::uint8_t>(flags | kCodeFlagLan));
    return true;
}

#if defined(_WIN32)

static bool contains_ci(const wchar_t* hay, const wchar_t* needle) {
    if (!hay) return false;
    std::wstring h(hay), n(needle);
    for (auto& c : h) c = static_cast<wchar_t>(towlower(c));
    for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
    return h.find(n) != std::wstring::npos;
}

static LocalIpv4Provider g_local_ipv4_provider = nullptr;

void set_local_ipv4_provider(LocalIpv4Provider fn) { g_local_ipv4_provider = fn; }

bool best_local_ipv4(std::array<std::uint8_t, 4>* out) {
    // The net layer's choice (BB_PARTY_LOCAL_IP, then the VPN pick, then the best adapter) wins
    // when it is wired in; the adapter scan below is the fallback (and the unit tests' default).
    if (LocalIpv4Provider fn = g_local_ipv4_provider) {
        std::array<std::uint8_t, 4> v{};
        if (fn(&v)) {
            *out = v;
            return true;
        }
    }
    return adapter_best_local_ipv4(out);
}

bool adapter_best_local_ipv4(std::array<std::uint8_t, 4>* out) {
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return false;
    int best_score = -1;
    static const wchar_t* kVirtual[] = {L"virtual", L"hyper-v", L"vmware", L"virtualbox", L"vethernet",
                                        L"wsl", L"tap-", L"wireguard", L"tunnel", L"loopback", L"vpn"};
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        bool is_virtual = false;
        for (const wchar_t* v : kVirtual)
            if (contains_ci(a->Description, v) || contains_ci(a->FriendlyName, v)) is_virtual = true;
        int score = 0;
        if (a->FirstGatewayAddress) score += 4;
        if (!is_virtual) score += 2;
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211) score += 1;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            const auto* b = reinterpret_cast<const std::uint8_t*>(&sin->sin_addr);
            if (b[0] == 127 || (b[0] == 169 && b[1] == 254) || b[0] == 0) continue;
            if (score > best_score) {
                best_score = score;
                std::memcpy(out->data(), b, 4);
            }
        }
    }
    return best_score >= 0;
}

#else

static LocalIpv4Provider g_local_ipv4_provider = nullptr;
void set_local_ipv4_provider(LocalIpv4Provider fn) { g_local_ipv4_provider = fn; }
bool adapter_best_local_ipv4(std::array<std::uint8_t, 4>*) { return false; }  // stub: not needed off Windows yet
bool best_local_ipv4(std::array<std::uint8_t, 4>* out) {
    std::array<std::uint8_t, 4> v{};
    if (g_local_ipv4_provider && g_local_ipv4_provider(&v)) {
        *out = v;
        return true;
    }
    return false;
}

#endif

// ---- the host's code secret across launches ----

const char* host_secret_from_name(HostSecretFrom from) {
    switch (from) {
    case HostSecretFrom::CrashRestart: return "the crash marker";
    case HostSecretFrom::Menu: return "party.json";
    case HostSecretFrom::Saved: return "party_secret.txt";
    case HostSecretFrom::New: return "new";
    }
    return "?";
}

bool parse_secret_hex(std::string_view text, std::array<std::uint8_t, 8>* out) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.size() != 2 * out->size()) return false;
    std::array<std::uint8_t, 8> v{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v[i / 2] = static_cast<std::uint8_t>(v[i / 2] << 4 | d);
    }
    if (std::all_of(v.begin(), v.end(), [](std::uint8_t b) { return b == 0; })) return false;
    *out = v;
    return true;
}

HostSecretFrom pick_host_secret(const std::array<std::uint8_t, 8>* restart_secret, const std::string& menu_hex,
                                const std::string& saved_text, std::array<std::uint8_t, 8>* secret) {
    *secret = {};
    if (restart_secret && std::any_of(restart_secret->begin(), restart_secret->end(), [](std::uint8_t b) { return b != 0; })) {
        *secret = *restart_secret;
        return HostSecretFrom::CrashRestart;
    }
    if (!menu_hex.empty() && parse_secret_hex(menu_hex, secret)) return HostSecretFrom::Menu;
    if (!saved_text.empty() && parse_secret_hex(saved_text, secret)) return HostSecretFrom::Saved;
    *secret = {};
    return HostSecretFrom::New;
}

SecretFit fit_secret_to_code(std::array<std::uint8_t, 8>* secret, bool password) {
    auto& s = *secret;
    const std::size_t want = password ? kSecretBytesWithPassword : kSecretBytes;
    const bool empty = std::all_of(s.begin(), s.end(), [](std::uint8_t b) { return b == 0; });
    if (empty) {
        do {
            crypto::random_bytes(s.data(), want);
        } while (std::all_of(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(want), [](std::uint8_t b) { return b == 0; }));
        return SecretFit::Made;
    }
    if (s[6] || s[7] || (password && (s[4] || s[5]))) {
        // An 8-byte secret from before BBP2, or a 6-byte one now that a password is set.
        for (std::size_t i = want; i < s.size(); ++i) s[i] = 0;
        return SecretFit::Shortened;
    }
    if (!password && !s[4] && !s[5]) {
        // A 4-byte secret from a party with a password: without one the code is all that keeps
        // strangers out, so it gets its 48 bits back.
        do {
            crypto::random_bytes(s.data() + 4, 2);
        } while (!s[4] && !s[5]);
        return SecretFit::Lengthened;
    }
    return SecretFit::Kept;
}

std::array<std::uint8_t, 8> new_host_secret(const std::array<std::uint8_t, 8>& old, bool password) {
    std::array<std::uint8_t, 8> s{};
    do {
        s = {};
        fit_secret_to_code(&s, password);
    } while (s == old);
    return s;
}

}  // namespace party
