// SPDX-License-Identifier: GPL-3.0-or-later
// The party port's route table (party_route.h): IPv6 bindings and the member relay.
#define _CRT_RAND_S  // rand_s (secure_random fallback)
#include "party_route.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace bbnet::udp {

namespace {

std::uint16_t sw16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
std::uint64_t get64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(get32(p)) << 32 | get32(p + 4);
}
void put_key(std::vector<std::uint8_t>& v, std::uint32_t a, std::uint16_t p) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&a);
    v.insert(v.end(), b, b + 4);
    const auto* q = reinterpret_cast<const std::uint8_t*>(&p);
    v.insert(v.end(), q, q + 2);
}
void get_key(const std::uint8_t* d, std::uint32_t* a, std::uint16_t* p) {
    std::memcpy(a, d, 4);
    std::memcpy(p, d + 4, 2);
}
void append(std::vector<std::uint8_t>& v, const char* label) {
    v.insert(v.end(), label, label + std::strlen(label));
}
bool equal16(const std::uint8_t* a, const std::uint8_t* b) {
    std::uint8_t d = 0;
    for (int i = 0; i < 16; ++i) d |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return d == 0;
}
std::string clean_name(const std::uint8_t* d, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && s.size() < 16; ++i) {
        const char c = static_cast<char>(d[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            s.push_back(c);
    }
    return s;
}

// SipHash-2-4 (Aumasson, Bernstein): the keyed hash of the synthetic keys and /64 groups.
std::uint64_t rotl(std::uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
std::uint64_t le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}
std::uint64_t siphash24(const std::uint8_t key[16], const std::uint8_t* in, std::size_t n) {
    const std::uint64_t k0 = le64(key), k1 = le64(key + 8);
    std::uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1, v2 = 0x6c7967656e657261ull ^ k0,
                  v3 = 0x7465646279746573ull ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    const std::size_t full = n / 8 * 8;
    for (std::size_t i = 0; i < full; i += 8) {
        const std::uint64_t m = le64(in + i);
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    std::uint64_t b = static_cast<std::uint64_t>(n) << 56;
    for (std::size_t i = 0; i < n - full; ++i) b |= static_cast<std::uint64_t>(in[full + i]) << (8 * i);
    v3 ^= b;
    round();
    round();
    v0 ^= b;
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// V / v frames: [header up to the MAC][mac 16][payload]; the MAC covers everything but itself.
constexpr std::size_t kVHeaderMemberToHost = 37;  // fc 'V' flags id4 ctr8 ga4 gp2 mac16
constexpr std::size_t kVHeaderHostToMember = 36;  // fc 'v' id4 ctr8 ga4 gp2 mac16
constexpr std::uint8_t kFlagStart = 1;            // the member's session with that guest has just started
void mac_input(const std::vector<std::uint8_t>& f, std::size_t hdr, std::vector<std::uint8_t>* m) {
    m->assign(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(hdr - 16));
    m->insert(m->end(), f.begin() + static_cast<std::ptrdiff_t>(hdr), f.end());
}
void seal(const Router::KeyedMac& kmac, const std::uint8_t* key, std::vector<std::uint8_t>& f, std::size_t hdr) {
    std::vector<std::uint8_t> m;
    mac_input(f, hdr, &m);
    kmac(key, m.data(), m.size(), f.data() + hdr - 16);
}
bool open(const Router::KeyedMac& kmac, const std::uint8_t* key, const std::vector<std::uint8_t>& f, std::size_t hdr) {
    std::vector<std::uint8_t> m;
    mac_input(f, hdr, &m);
    std::uint8_t want[16];
    kmac(key, m.data(), m.size(), want);
    return equal16(want, f.data() + hdr - 16);
}

constexpr std::size_t kQ2 = 67, kQ3 = 115, kQ5 = 22;  // frame sizes (Q1: Router::kQ1Size; Q4: 40 + name)
constexpr std::size_t kTHeader = 30;                  // T / t: fc type session4 ctr8 mac16
constexpr const char* kLabelCookie = "bbport relay cookie v1";
constexpr const char* kLabelUp = "bbport relay up v1";
constexpr const char* kLabelDown = "bbport relay down v1";
constexpr const char* kLabelNonceMac = "bbport net6 nonce v1";
constexpr const char* kLabelScalar = "bbport cpace member scalar v1";
constexpr const char* kLabelIsk = "bbport cpace isk v1";
constexpr const char* kLabelConfirmA = "bbport cpace confirm guest v1";
constexpr const char* kLabelConfirmB = "bbport cpace confirm member v1";

}  // namespace

// ---- addresses ----

bool parse_ip6(const std::string& text_in, Ip6* out) {
    std::string t = text_in;
    if (!t.empty() && t.front() == '[') {
        const auto e = t.find(']');
        if (e == std::string::npos) return false;
        t = t.substr(1, e - 1);
    }
    if (const auto pct = t.find('%'); pct != std::string::npos) t = t.substr(0, pct);  // a zone id
    if (t.size() < 2 || t.size() > 39) return false;
    std::vector<std::uint16_t> head, tail;
    bool gap = false;
    std::size_t i = 0;
    if (t.compare(0, 2, "::") == 0) {
        gap = true;
        i = 2;
    } else if (t[0] == ':') {
        return false;
    }
    while (i < t.size()) {
        std::size_t j = i;
        unsigned v = 0;
        while (j < t.size() && j - i < 4 && std::isxdigit(static_cast<unsigned char>(t[j]))) {
            const char c = t[j];
            v = v * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            ++j;
        }
        if (j == i) return false;
        (gap ? tail : head).push_back(static_cast<std::uint16_t>(v));
        if (j == t.size()) break;
        if (t[j] != ':') return false;
        if (j + 1 < t.size() && t[j + 1] == ':') {
            if (gap) return false;
            gap = true;
            i = j + 2;
            if (i == t.size()) break;
        } else {
            i = j + 1;
            if (i == t.size()) return false;
        }
    }
    const std::size_t groups = head.size() + tail.size();
    if (groups > 8 || (!gap && groups != 8) || (gap && groups == 8)) return false;
    std::array<std::uint16_t, 8> g{};
    for (std::size_t h = 0; h < head.size(); ++h) g[h] = head[h];
    for (std::size_t q = 0; q < tail.size(); ++q) g[8 - tail.size() + q] = tail[q];
    for (int q = 0; q < 8; ++q) {
        out->b[2 * q] = static_cast<std::uint8_t>(g[q] >> 8);
        out->b[2 * q + 1] = static_cast<std::uint8_t>(g[q]);
    }
    return true;
}

std::string ip6_text(const Ip6& a) {
    std::uint16_t g[8];
    for (int q = 0; q < 8; ++q) g[q] = static_cast<std::uint16_t>(a.b[2 * q] << 8 | a.b[2 * q + 1]);
    int best = -1, best_len = 0;
    for (int q = 0; q < 8;) {
        if (g[q]) {
            ++q;
            continue;
        }
        int e = q;
        while (e < 8 && !g[e]) ++e;
        if (e - q > best_len && e - q > 1) {
            best = q;
            best_len = e - q;
        }
        q = e;
    }
    std::string s;
    char buf[8];
    for (int q = 0; q < 8; ++q) {
        if (q == best) {
            s += "::";
            q += best_len - 1;
            continue;
        }
        if (!s.empty() && s.back() != ':') s += ':';
        std::snprintf(buf, sizeof buf, "%x", g[q]);
        s += buf;
    }
    return s.empty() ? "::" : s;
}

bool ip6_global(const Ip6& a) { return (a.b[0] & 0xe0) == 0x20; }
bool ip6_loopback(const Ip6& a) {
    for (int i = 0; i < 15; ++i)
        if (a.b[i]) return false;
    return a.b[15] == 1;
}
bool ip6_zero(const Ip6& a) {
    for (std::uint8_t c : a.b)
        if (c) return false;
    return true;
}

std::string key_text(std::uint32_t addr_nbo, std::uint16_t port_nbo) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&addr_nbo);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], sw16(port_nbo));
    return buf;
}

bool is_synthetic(std::uint32_t addr_nbo) { return (reinterpret_cast<const std::uint8_t*>(&addr_nbo)[0] & 0xf8) == 0xf0; }

void secure_random(void* out, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(out);
#if defined(_WIN32)
    using GenFn = long(__stdcall*)(void*, unsigned char*, unsigned long, unsigned long);
    static const GenFn gen = [] {
        HMODULE m = ::LoadLibraryA("bcrypt.dll");
        return m ? reinterpret_cast<GenFn>(reinterpret_cast<void*>(::GetProcAddress(m, "BCryptGenRandom"))) : nullptr;
    }();
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG = 2
    if (gen && gen(nullptr, p, static_cast<unsigned long>(n), 2) >= 0) return;
    while (n) {
        unsigned int v = 0;
        rand_s(&v);
        const std::size_t k = n < 4 ? n : 4;
        std::memcpy(p, &v, k);
        p += k;
        n -= k;
    }
#else
    if (std::FILE* f = std::fopen("/dev/urandom", "rb")) {
        const std::size_t got = std::fread(p, 1, n, f);
        std::fclose(f);
        if (got == n) return;
    }
    std::random_device rd;
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(rd());
#endif
}

// ---- Router ----

Router::Router() {
    secure_random(hash_key_.data(), hash_key_.size());
    secure_random(sv_salt_.data(), sv_salt_.size());
    secure_random(sv_cookie_key_.data(), sv_cookie_key_.size());
}

Router::MemberKey Router::new_member_key() {
    MemberKey k;
    while (!k.id) secure_random(&k.id, sizeof k.id);
    secure_random(k.key.data(), k.key.size());
    return k;
}

bool Router::Bucket::take(double n, RClock::time_point now) {
    if (at == RClock::time_point{}) {
        tokens = burst;
        at = now;
    }
    const double dt = std::max(0.0, std::chrono::duration<double>(now - at).count());
    if (now > at) at = now;
    tokens = std::min(burst, tokens + dt * rate);
    if (tokens < n) return false;
    tokens -= n;
    return true;
}

bool Router::Replay::accept(std::uint64_t c) {
    if (!seen_any) {
        seen_any = true;
        top = c;
        mask = 1;
        return true;
    }
    if (c > top) {
        const std::uint64_t shift = c - top;
        mask = shift >= 64 ? 0 : mask << shift;
        mask |= 1;
        top = c;
        return true;
    }
    const std::uint64_t back = top - c;
    if (back >= 64) return false;
    const std::uint64_t bit = 1ull << back;
    if (mask & bit) return false;
    mask |= bit;
    return true;
}

void Router::note(std::string s) {
    if (notes_.size() < 256) notes_.push_back(std::move(s));
}

std::vector<std::string> Router::take_notes() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> v;
    v.swap(notes_);
    return v;
}

void Router::set_keyed_mac(KeyedMac mac) {
    std::lock_guard<std::mutex> lk(mu_);
    kmac_ = std::move(mac);
}

void Router::set_ipv6(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    ipv6_ = on;
}
bool Router::ipv6() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ipv6_;
}

void Router::set_key_hash_for_tests(std::function<std::uint32_t(const Ip6&)> h) {
    std::lock_guard<std::mutex> lk(mu_);
    test_hash_ = std::move(h);
}

std::uint32_t Router::hash27(const Ip6& a) const {
    if (test_hash_) return test_hash_(a) & 0x07ffffffu;
    return static_cast<std::uint32_t>(siphash24(hash_key_.data(), a.b.data(), 16)) & 0x07ffffffu;
}

namespace {
std::uint32_t addr_of_host(std::uint32_t host_order) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(host_order >> 24), static_cast<std::uint8_t>(host_order >> 16),
                               static_cast<std::uint8_t>(host_order >> 8), static_cast<std::uint8_t>(host_order)};
    std::uint32_t a;
    std::memcpy(&a, b, 4);
    return a;
}
bool same_prefix(const Ip6& a, const Ip6& b, std::size_t bytes) { return std::memcmp(a.b.data(), b.b.data(), bytes) == 0; }
}  // namespace

std::uint32_t Router::group_of_locked(std::uint32_t addr, std::uint16_t port) const {
    if (!is_synthetic(addr)) return addr;
    auto it = v6_by_key_.find(k(addr, port));
    if (it == v6_by_key_.end()) return addr;
    std::uint8_t in[8];
    in[0] = 'g';
    std::memcpy(in + 1, it->second.e6.addr.b.data(), 7);  // the /56: a home or a small site
    const auto h = static_cast<std::uint32_t>(siphash24(hash_key_.data(), in, sizeof in)) & 0x07ffffffu;
    return addr_of_host(0xf8000000u | h);  // 248/5: never a key
}

std::uint32_t Router::throttle_key(std::uint32_t addr, std::uint16_t port) const {
    std::lock_guard<std::mutex> lk(mu_);
    return group_of_locked(addr, port);
}

void Router::mark_trusted(std::uint32_t addr, std::uint16_t port) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = v6_by_key_.find(k(addr, port)); it != v6_by_key_.end()) it->second.trusted = true;
}

bool Router::bind_v6(std::uint32_t ka, std::uint16_t kp, const Ep6& e6, bool route) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::uint64_t key = k(ka, kp);
    if (auto prev = key_by_v6_.find(e6); prev != key_by_v6_.end() && prev->second != key) {
        auto pv = v6_by_key_.find(prev->second);
        if (pv != v6_by_key_.end() && !pv->second.synthetic) {
            note("route: [" + ip6_text(e6.addr) + "]:" + std::to_string(sw16(e6.port)) + " is bound to " +
                 key_text(static_cast<std::uint32_t>(prev->second), static_cast<std::uint16_t>(prev->second >> 32)) +
                 " already: not rebound to " + key_text(ka, kp));
            return false;
        }
        if (pv != v6_by_key_.end()) v6_by_key_.erase(pv);  // its synthetic key: the binding wins
    }
    if (auto old = v6_by_key_.find(key); old != v6_by_key_.end() && !(old->second.e6 == e6))
        key_by_v6_.erase(old->second.e6);
    V6& v = v6_by_key_[key];
    const bool changed = !(v.e6 == e6) || v.route != route || v.synthetic;
    v.e6 = e6;
    v.route = route;
    v.synthetic = false;
    v.seen = RClock::now();
    key_by_v6_[e6] = key;
    if (changed)
        note("route: " + key_text(ka, kp) + " is [" + ip6_text(e6.addr) + "]:" + std::to_string(sw16(e6.port)) +
             (route ? " (IPv6, in use)" : " (IPv6 candidate)"));
    return true;
}

void Router::route_v6(std::uint32_t ka, std::uint16_t kp, bool route) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(ka, kp));
    if (it == v6_by_key_.end() || it->second.synthetic || it->second.route == route) return;
    it->second.route = route;
    note("route: " + key_text(ka, kp) + (route ? " over IPv6 now" : " over IPv4 again"));
}

void Router::unbind_v6(std::uint32_t ka, std::uint16_t kp) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(ka, kp));
    if (it == v6_by_key_.end()) return;
    key_by_v6_.erase(it->second.e6);
    v6_by_key_.erase(it);
}

bool Router::v6_of(std::uint32_t ka, std::uint16_t kp, Ep6* e6, bool* route) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(ka, kp));
    if (it == v6_by_key_.end()) return false;
    if (e6) *e6 = it->second.e6;
    if (route) *route = it->second.route || it->second.synthetic;
    return true;
}

bool Router::key_of(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port) {
    std::lock_guard<std::mutex> lk(mu_);
    return key_of_locked(e6, addr, port);
}

bool Router::known_key(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = key_by_v6_.find(e6);
    if (it == key_by_v6_.end()) return false;
    *addr = static_cast<std::uint32_t>(it->second);
    *port = static_cast<std::uint16_t>(it->second >> 32);
    return true;
}

bool Router::key_of_locked(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port) {
    const RClock::time_point now = RClock::now();
    if (auto it = key_by_v6_.find(e6); it != key_by_v6_.end()) {
        *addr = static_cast<std::uint32_t>(it->second);
        *port = static_cast<std::uint16_t>(it->second >> 32);
        if (auto v = v6_by_key_.find(it->second); v != v6_by_key_.end()) v->second.seen = now;
        return true;
    }
    // Caps (N2): per /64 and per /48 (an attacker may own a whole prefix) and in all. A full prefix
    // or table evicts its oldest untrusted synthetic entry (a stranger's, never a party member's or a
    // bound one), else the newcomer is dropped.
    auto make_room = [&](std::size_t prefix_bytes, std::size_t cap) {
        std::size_t count = 0;
        auto oldest = v6_by_key_.end();
        for (auto i = v6_by_key_.begin(); i != v6_by_key_.end(); ++i) {
            if (!i->second.synthetic) continue;
            if (prefix_bytes && !same_prefix(i->second.e6.addr, e6.addr, prefix_bytes)) continue;
            ++count;
            if (!i->second.trusted && (oldest == v6_by_key_.end() || i->second.seen < oldest->second.seen)) oldest = i;
        }
        if (count < cap) return true;
        if (oldest == v6_by_key_.end()) return false;
        key_by_v6_.erase(oldest->second.e6);
        v6_by_key_.erase(oldest);
        return true;
    };
    if (!make_room(8, kMaxV6Per64) || !make_room(6, kMaxV6Per48)) return false;
    if (v6_by_key_.size() >= kMaxV6 && !make_room(0, kMaxV6)) return false;
    // A clash with any entry is never resolved by eviction: the newcomer probes for the next free key.
    const std::uint32_t h = hash27(e6.addr);
    for (std::uint32_t i = 0; i < 32; ++i) {
        const std::uint32_t a = addr_of_host(0xf0000000u | ((h + i) & 0x07ffffffu));
        const std::uint64_t key = k(a, e6.port);
        if (v6_by_key_.count(key)) continue;
        V6& v = v6_by_key_[key];
        v.e6 = e6;
        v.synthetic = true;
        v.route = true;
        v.seen = now;
        key_by_v6_[e6] = key;
        *addr = a;
        *port = e6.port;
        return true;
    }
    return false;
}

void Router::set_nonce_key(const std::array<std::uint8_t, 32>& key) {
    std::lock_guard<std::mutex> lk(mu_);
    nonce_key_ = key;
    have_nonce_key_ = true;
}

std::uint64_t Router::curve_ops() const {
    std::lock_guard<std::mutex> lk(mu_);
    return curve_ops_;
}

Wire Router::nonce_wire(const Ep6& to, const std::array<std::uint8_t, 16>& nonce) const {
    Wire w;
    w.v6 = true;
    w.addr6 = to.addr;
    w.port = to.port;
    w.data = {kRouteMagic, 'N'};
    w.data.insert(w.data.end(), nonce.begin(), nonce.end());
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::uint8_t> m;
    append(m, kLabelNonceMac);
    m.insert(m.end(), nonce.begin(), nonce.end());
    std::uint8_t tag[16] = {};
    if (kmac_ && have_nonce_key_) kmac_(nonce_key_.data(), m.data(), m.size(), tag);
    w.data.insert(w.data.end(), tag, tag + 16);
    return w;
}

std::vector<std::array<std::uint8_t, 16>> Router::take_nonces() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::array<std::uint8_t, 16>> v;
    for (const auto& [g, nonce] : nonces_) {
        (void)g;
        v.push_back(nonce);
    }
    nonces_.clear();
    return v;
}

void Router::wire_to(std::uint32_t addr, std::uint16_t port, std::vector<std::uint8_t> data, std::vector<Wire>* out) const {
    auto it = v6_by_key_.find(k(addr, port));
    Wire w;
    if (it != v6_by_key_.end() && ipv6_ && (it->second.route || it->second.synthetic)) {
        w.v6 = true;
        w.addr6 = it->second.e6.addr;
        w.port = it->second.e6.port;
    } else if (is_synthetic(addr)) {
        return;  // an IPv6 peer we have no route to (or IPv6 is off): never put on the IPv4 wire
    } else {
        w.addr = addr;
        w.port = port;
    }
    w.data = std::move(data);
    out->push_back(std::move(w));
}

void Router::outgoing(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                      RClock::time_point now, std::vector<Wire>* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st != ClientState::Off && to_addr == cl_.h_addr && to_port == cl_.h_port) {
        if (cl_.st != ClientState::Ready || !kmac_) return;  // the handshake first; the link dials after it
        std::vector<std::uint8_t> f{kRouteMagic, 'T'};
        put32(f, cl_.session);
        put64(f, ++cl_.up_ctr);
        f.resize(f.size() + 16);
        f.insert(f.end(), d, d + n);
        seal(kmac_, cl_.k_up.data(), f, kTHeader);
        cl_.last_sent = now;
        wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
        return;
    }
    if (accept_) {
        if (auto it = host_routes_.find(k(to_addr, to_port)); it != host_routes_.end()) {
            auto m = members_.find(it->second.member_id);
            if (m == members_.end() || !m->second.bound_addr || !kmac_) return;
            Member& mb = m->second;
            std::vector<std::uint8_t> f{kRouteMagic, 'v'};
            put32(f, it->second.member_id);
            put64(f, ++mb.out_ctr);
            put_key(f, to_addr, to_port);
            f.resize(f.size() + 16);
            f.insert(f.end(), d, d + n);
            seal(kmac_, mb.key.data(), f, kVHeaderHostToMember);
            wire_to(mb.bound_addr, mb.bound_port, std::move(f), out);
            return;
        }
    }
    wire_to(to_addr, to_port, std::vector<std::uint8_t>(d, d + n), out);
}

bool Router::outgoing_v6(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                         std::vector<Wire>* out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(to_addr, to_port));
    if (!ipv6_ || it == v6_by_key_.end()) return false;
    Wire w;
    w.v6 = true;
    w.addr6 = it->second.e6.addr;
    w.port = it->second.e6.port;
    w.data.assign(d, d + n);
    out->push_back(std::move(w));
    return true;
}

bool Router::incoming(const Wire& from, RClock::time_point now, std::vector<std::uint8_t>* data, std::uint32_t* src_addr,
                      std::uint16_t* src_port, std::vector<Wire>* out, bool* via_v6, bool* via_relay) {
    if (via_v6) *via_v6 = from.v6;
    if (via_relay) *via_relay = false;
    std::uint32_t sa = from.addr;
    std::uint16_t sp = from.port;
    std::lock_guard<std::mutex> lk(mu_);
    if (from.v6) {
        if (!ipv6_) return false;
        if (!key_of_locked(Ep6{from.addr6, from.port}, &sa, &sp)) return false;  // its /64 is full
    }
    const std::vector<std::uint8_t>& d = from.data;
    if (!is_route_frame(d.data(), d.size())) {
        if (accept_) {
            direct_[k(sa, sp)] = now;
            if (direct_.size() > 4096 || now - direct_pruned_ > std::chrono::seconds(30)) {
                direct_pruned_ = now;
                for (auto i = direct_.begin(); i != direct_.end();)
                    i = now - i->second > kDirectGuard ? direct_.erase(i) : std::next(i);
                if (direct_.size() > 4096) direct_.clear();
            }
        }
        *data = d;
        *src_addr = sa;
        *src_port = sp;
        return true;
    }
    *data = d;
    return on_frame(sa, sp, from.v6, *data, now, src_addr, src_port, out, via_relay);
}

// The member's per-guest secret scalar (stateless: recomputed at Q3 from the salt, the guest's
// endpoint, its nonce and point, and the minute).
void Router::member_scalar(std::uint32_t a, std::uint16_t p, const std::uint8_t* cn, const std::uint8_t* ya_pub,
                           std::int64_t minute, std::uint8_t out[32]) const {
    std::vector<std::uint8_t> m;
    append(m, kLabelScalar);
    m.insert(m.end(), sv_salt_.begin(), sv_salt_.end());
    put_key(m, a, p);
    m.insert(m.end(), cn, cn + 16);
    m.insert(m.end(), ya_pub, ya_pub + 32);
    put32(m, static_cast<std::uint32_t>(minute));
    sv_pake_.hash(m.data(), m.size(), out);
}

// ISK = H(label || cn || Ya || Yb || K): the session's key for both confirmations.
void Router::session_key(const Pake& pk, const std::uint8_t* cn, const std::uint8_t* ya, const std::uint8_t* yb,
                         const std::uint8_t* kk, std::uint8_t out[32]) {
    std::vector<std::uint8_t> m;
    append(m, kLabelIsk);
    m.insert(m.end(), cn, cn + 16);
    m.insert(m.end(), ya, ya + 32);
    m.insert(m.end(), yb, yb + 32);
    m.insert(m.end(), kk, kk + 32);
    pk.hash(m.data(), m.size(), out);
}

// The member's stateless cookie for a guest's handshake (Q2, checked at Q3 before any curve work).
void Router::cookie(std::uint32_t a, std::uint16_t p, const std::uint8_t* cn, const std::uint8_t* ya,
                    const std::uint8_t* yb, std::int64_t minute, std::uint8_t out[16]) const {
    std::vector<std::uint8_t> m;
    append(m, kLabelCookie);
    put_key(m, a, p);
    m.insert(m.end(), cn, cn + 16);
    m.insert(m.end(), ya, ya + 32);
    m.insert(m.end(), yb, yb + 32);
    put32(m, static_cast<std::uint32_t>(minute));
    kmac_(sv_cookie_key_.data(), m.data(), m.size(), out);
}

// Handshake budgets: per /56 group (5 a second each for Q1 and Q3) and for everyone (200).
bool Router::budget(bool confirm, std::uint32_t group, bool party, RClock::time_point now) {
    if (group_budget_.size() > 4096) {
        for (auto i = group_budget_.begin(); i != group_budget_.end();)
            i = now - i->second.first.at > std::chrono::minutes(1) && now - i->second.second.at > std::chrono::minutes(1)
                    ? group_budget_.erase(i)
                    : std::next(i);
        if (group_budget_.size() > 4096) group_budget_.clear();
    }
    auto& gb = group_budget_[group];
    Bucket& b = confirm ? gb.second : gb.first;
    if (b.rate == 0) {
        b.rate = kHandshakeRatePerGroup;
        b.burst = 2 * kHandshakeRatePerGroup;
    }
    if (!b.take(1, now)) return false;
    if ((confirm ? confirm_budget_ : hello_budget_).take(1, now)) return true;
    // The general budget is spent (a flood): the reserved part for the party's own sources.
    if (!party) {
        auto g = good_groups_.find(group);
        party = g != good_groups_.end() && now - g->second < std::chrono::hours(1);
    }
    return party && (confirm ? confirm_reserved_ : hello_reserved_).take(1, now);
}

// The guest's side: the member's confirmation failed (an impostor or another party): not asked
// again for a while, doubling (R3).
void Router::member_failed(RClock::time_point now, bool authenticated) {
    Backoff& b = backoff_[k(cl_.m_addr, cl_.m_port)];
    if (!authenticated) {
        // An unauthenticated refusal (anyone may forge one): this handshake ends, the wait is at most
        // kBackoffFirst and the failure count does not grow (L1).
        if (b.until < now + kBackoffFirst) b.until = now + kBackoffFirst;
        if (backoff_.size() > 256) backoff_.erase(backoff_.begin());
        return;
    }
    ++b.failures;
    auto wait = kBackoffFirst;
    for (int i = 1; i < b.failures && wait < kBackoffMax; ++i) wait *= 2;
    if (wait > kBackoffMax) wait = kBackoffMax;
    b.until = now + wait;
    if (backoff_.size() > 256) backoff_.erase(backoff_.begin());
}

bool Router::valid_guest_key(std::uint32_t ga, std::uint16_t gp, std::uint32_t member_src) const {
    if (!ga || !gp) return false;
    const std::uint8_t a0 = reinterpret_cast<const std::uint8_t*>(&ga)[0];
    const bool member_lb = reinterpret_cast<const std::uint8_t*>(&member_src)[0] == 127;
    if (a0 == 0 || (a0 >= 224 && a0 < 240) || a0 >= 248) return false;  // unspecified, multicast, reserved, broadcast
    if (a0 == 127 && !member_lb) return false;                            // loopback: a local test only
    if (own_addrs_.count(ga) && !member_lb) return false;                 // never one of our own addresses
    // A key of our own IPv6 table (a peer we reach directly, or another one's synthetic key): a
    // member's guest never speaks for it (N3).
    if (v6_by_key_.count(k(ga, gp))) return false;
    return true;
}

// mu_ held. `d` holds the frame; on true it holds the inner datagram for the layers above.
bool Router::on_frame(std::uint32_t sa, std::uint16_t sp, bool v6, std::vector<std::uint8_t>& d, RClock::time_point now,
                      std::uint32_t* src_addr, std::uint16_t* src_port, std::vector<Wire>* out, bool* via_relay) {
    const std::size_t n = d.size();
    const std::uint8_t type = d[1];
    if (type == 'N') {  // the host checks our IPv6 endpoint: echoed over the link (net6_echo)
        if (!v6 || n != 34 || !kmac_ || !have_nonce_key_) return false;
        std::vector<std::uint8_t> m;
        append(m, kLabelNonceMac);
        m.insert(m.end(), d.begin() + 2, d.begin() + 18);
        std::uint8_t want[16];
        kmac_(nonce_key_.data(), m.data(), m.size(), want);
        if (!equal16(want, d.data() + 18)) return false;  // not from a party member (R4)
        auto it = v6_by_key_.find(k(sa, sp));
        if (it == v6_by_key_.end()) return false;
        // One slot per source /48 (N6: a flood from one network keeps one slot; the latest wins), at
        // most kMaxNonceSlots; the guest's tick drains them four times a second.
        std::uint64_t g48 = 0;
        for (int i = 0; i < 6; ++i) g48 = g48 << 8 | it->second.e6.addr.b[static_cast<std::size_t>(i)];
        if (!nonces_.count(g48) && nonces_.size() >= kMaxNonceSlots) return false;
        std::array<std::uint8_t, 16> nonce;
        std::memcpy(nonce.data(), d.data() + 2, 16);
        nonces_[g48] = nonce;
        return false;
    }
    if (type == 'Q' && n >= 3 + 16) {
        const std::uint8_t sub = d[2];
        const std::uint8_t* cn = d.data() + 3;
        const std::int64_t minute = std::chrono::duration_cast<std::chrono::minutes>(now.time_since_epoch()).count();
        const std::uint32_t tkey = group_of_locked(sa, sp);  // an IPv6 /56 counts as one address
        auto refuse = [&](std::uint8_t code, int wait_s) {
            std::vector<std::uint8_t> f{kRouteMagic, 'Q', 5};
            f.insert(f.end(), cn, cn + 16);
            f.push_back(code);
            const int w = std::clamp(wait_s, 0, 65535);
            f.push_back(static_cast<std::uint8_t>(w >> 8));
            f.push_back(static_cast<std::uint8_t>(w));
            wire_to(sa, sp, std::move(f), out);
        };
        if (sub == 1 && n >= kQ1Size) {  // a guest's hello: its point Ya
            if (!serving_) {
                refuse(kNotServing, 0);
                return false;
            }
            // Each hello costs two curve operations: budgets per /56 and for everyone (R1).
            auto src6 = v6_by_key_.find(k(sa, sp));
            const bool party_src = src6 != v6_by_key_.end() && src6->second.trusted;
            if (!kmac_ || !budget(false, tkey, party_src, now)) return false;
            const std::uint8_t* ya = d.data() + 19;
            std::uint8_t g[32], yb_s[32], yb[32], ck[16];
            sv_pake_.generator(cn, g);
            member_scalar(sa, sp, cn, ya, minute, yb_s);
            curve_ops_ += 2;
            if (!sv_pake_.x25519(yb_s, g, yb)) return false;
            cookie(sa, sp, cn, ya, yb, minute, ck);
            // Q2 is the member's point and its cookie: nothing in it can be checked against a password guess.
            std::vector<std::uint8_t> f{kRouteMagic, 'Q', 2};
            f.insert(f.end(), cn, cn + 16);
            f.insert(f.end(), yb, yb + 32);
            f.insert(f.end(), ck, ck + 16);
            wire_to(sa, sp, std::move(f), out);
            return false;
        }
        if (sub == 3 && n == kQ3) {  // the guest's confirmation: proves it holds the party key
            if (!serving_) {
                refuse(kNotServing, 0);
                return false;
            }
            const std::uint8_t* ya = d.data() + 19;
            const std::uint8_t* yb = d.data() + 51;
            // The cookie first: one BLAKE2b, no curve work for a forged or stale Q3 (R1).
            if (!kmac_) return false;
            std::int64_t at = 0;
            bool fresh = false;
            for (std::int64_t m = minute; m >= minute - 1 && !fresh; --m) {
                std::uint8_t ck[16];
                cookie(sa, sp, cn, ya, yb, m, ck);
                if (equal16(ck, d.data() + 83)) {
                    fresh = true;
                    at = m;
                }
            }
            if (!fresh) return false;  // stale or not ours: the guest says hello again
            auto src6 = v6_by_key_.find(k(sa, sp));
            if (!budget(true, tkey, src6 != v6_by_key_.end() && src6->second.trusted, now)) return false;
            std::uint8_t yb_s[32];
            member_scalar(sa, sp, cn, ya, at, yb_s);
            std::int64_t remaining = 0;
            if (sv_throttle_.locked && sv_throttle_.locked(tkey, &remaining)) {
                refuse(kLockedOut, static_cast<int>((remaining + 999) / 1000));
                return false;
            }
            std::uint8_t kk[32], isk[32], confirm[16];
            ++curve_ops_;
            const bool point_ok = sv_pake_.x25519(yb_s, ya, kk);  // a low-order Ya gives zero: refused
            if (point_ok) {
                session_key(sv_pake_, cn, ya, yb, kk, isk);
                std::vector<std::uint8_t> m;
                append(m, kLabelConfirmA);
                m.insert(m.end(), cn, cn + 16);
                kmac_(isk, m.data(), m.size(), confirm);
            }
            if (!point_ok || !equal16(confirm, d.data() + 99)) {
                if (sv_throttle_.fail) sv_throttle_.fail(tkey);
                note("member relay: a wrong party key from " + key_text(sa, sp) + " (refused)");
                refuse(kWrongKey, 0);
                return false;
            }
            if (sv_throttle_.success) sv_throttle_.success(tkey);
            if (good_groups_.size() > 4096) good_groups_.clear();
            good_groups_[tkey] = now;  // its /56 may use the reserved budget during a flood (L2)
            std::array<std::uint8_t, 16> cna;
            std::memcpy(cna.data(), cn, 16);
            auto it = sessions_.find(k(sa, sp));
            if (it == sessions_.end() || it->second.cn != cna) {
                // A handshake seen before (a replayed Q3) never makes a session; a live session is
                // only replaced once it has gone quiet (a restarted guest's new handshake).
                if (std::find(recent_cn_.begin(), recent_cn_.end(), cna) != recent_cn_.end()) return false;
                if (it != sessions_.end() && now - it->second.last < kSessionReplaceIdle) return false;
                if (it == sessions_.end()) {
                    for (auto i = sessions_.begin(); i != sessions_.end();)
                        i = now - i->second.last > kSessionIdle ? sessions_.erase(i) : std::next(i);
                    if (sessions_.size() >= kMaxSessions) {
                        refuse(kFull, 0);
                        note("member relay: refused " + key_text(sa, sp) + " (already relaying for " +
                             std::to_string(kMaxSessions) + ")");
                        return false;
                    }
                }
                Session s;
                s.addr = sa;
                s.port = sp;
                while (!s.id) secure_random(&s.id, sizeof s.id);
                s.cn = cna;
                s.created = s.last = now;
                std::vector<std::uint8_t> ku(isk, isk + 32), kd(isk, isk + 32);
                append(ku, kLabelUp);
                append(kd, kLabelDown);
                sv_pake_.hash(ku.data(), ku.size(), s.k_up.data());
                sv_pake_.hash(kd.data(), kd.size(), s.k_down.data());
                // A guest we relay over IPv6 is a party member's: its key is never evicted.
                if (auto v = v6_by_key_.find(k(sa, sp)); v != v6_by_key_.end()) v->second.trusted = true;
                s.frames.rate = kSessionFrameRate;
                s.frames.burst = 2 * kSessionFrameRate;
                s.bytes.rate = kSessionByteRate;
                s.bytes.burst = 2 * kSessionByteRate;
                const bool again = it != sessions_.end();
                sessions_[k(sa, sp)] = s;
                it = sessions_.find(k(sa, sp));
                recent_cn_.push_back(cna);
                if (recent_cn_.size() > 64) recent_cn_.pop_front();
                note("member relay: relaying for a guest at " + key_text(sa, sp) +
                     (again ? " (a new session)" : "") + ": it cannot reach the host directly");
            }
            // (a repeated Q3 of the live session gets its welcome again, but does not keep it alive)
            std::vector<std::uint8_t> f{kRouteMagic, 'Q', 4};
            f.insert(f.end(), cn, cn + 16);
            put32(f, it->second.id);
            const std::string name = sv_name_.substr(0, 16);
            f.push_back(static_cast<std::uint8_t>(name.size()));
            f.insert(f.end(), name.begin(), name.end());
            std::vector<std::uint8_t> ok;
            append(ok, kLabelConfirmB);
            ok.insert(ok.end(), cn, cn + 16);
            put32(ok, it->second.id);
            ok.insert(ok.end(), name.begin(), name.end());
            std::uint8_t tag[16];
            kmac_(isk, ok.data(), ok.size(), tag);
            f.insert(f.end(), tag, tag + 16);
            wire_to(sa, sp, std::move(f), out);
            return false;
        }
        // The guest's side: answers from our member only, for our hello.
        if (cl_.st != ClientState::Handshake || sa != cl_.m_addr || sp != cl_.m_port ||
            std::memcmp(cn, cl_.cn.data(), 16) != 0)
            return false;
        if (sub == 2 && n == kQ2 && !cl_.have_isk) {
            std::memcpy(cl_.yb.data(), d.data() + 19, 32);
            std::memcpy(cl_.cookie.data(), d.data() + 51, 16);
            std::uint8_t kk[32];
            if (!cl_.pake.x25519(cl_.ya_s.data(), cl_.yb.data(), kk)) return false;  // a low-order point
            session_key(cl_.pake, cl_.cn.data(), cl_.ya.data(), cl_.yb.data(), kk, cl_.isk.data());
            cl_.have_isk = true;
            client_send_auth(now, out);
        } else if (sub == 4 && n >= 3 + 16 + 4 + 1 + 16 && cl_.have_isk) {
            const std::uint32_t session = get32(d.data() + 19);
            const std::size_t nl = d[23];
            if (n != 24 + nl + 16 || nl > 16) return false;
            std::vector<std::uint8_t> ok;
            append(ok, kLabelConfirmB);
            ok.insert(ok.end(), cl_.cn.begin(), cl_.cn.end());
            put32(ok, session);
            ok.insert(ok.end(), d.begin() + 24, d.begin() + 24 + static_cast<std::ptrdiff_t>(nl));
            std::uint8_t tag[16];
            kmac_(cl_.isk.data(), ok.data(), ok.size(), tag);
            if (!equal16(tag, d.data() + 24 + nl)) {  // not a party member: its welcome is ignored
                if (!cl_.impostor_noted) {
                    note("member relay: " + key_text(cl_.m_addr, cl_.m_port) +
                         " does not know our party key (not a party member): not asked again for a while");
                    member_failed(now, true);  // its confirmation failed: an impostor
                    cl_.st = ClientState::Refused;
                    cl_.code = kWrongKey;
                }
                cl_.impostor_noted = true;
                return false;
            }
            std::vector<std::uint8_t> ku(cl_.isk.begin(), cl_.isk.end()), kd(cl_.isk.begin(), cl_.isk.end());
            append(ku, kLabelUp);
            append(kd, kLabelDown);
            cl_.pake.hash(ku.data(), ku.size(), cl_.k_up.data());
            cl_.pake.hash(kd.data(), kd.size(), cl_.k_down.data());
            backoff_.erase(k(cl_.m_addr, cl_.m_port));
            cl_.session = session;
            cl_.name = clean_name(d.data() + 24, nl);
            cl_.st = ClientState::Ready;
            cl_.last_sent = now;
            note("member relay: " + (cl_.name.empty() ? std::string("the member") : cl_.name) + " at " +
                 key_text(cl_.m_addr, cl_.m_port) + " relays for us to the host " + key_text(cl_.h_addr, cl_.h_port));
        } else if (sub == 5 && n == kQ5) {
            cl_.code = d[19];
            cl_.wait_s = d[20] << 8 | d[21];
            cl_.st = ClientState::Refused;
            if (cl_.code == kWrongKey) member_failed(now, false);  // a hint only (unauthenticated)
            static const char* why[] = {"", "it has another party key (a different code or password)",
                                        "too many wrong attempts from here", "it does not relay now",
                                        "it relays for others already"};
            note("member relay: the member at " + key_text(cl_.m_addr, cl_.m_port) + " refused: " +
                 (cl_.code >= 1 && cl_.code <= 4 ? why[cl_.code] : "?") +
                 (cl_.wait_s ? " (" + std::to_string(cl_.wait_s) + " s)" : ""));
        }
        return false;
    }
    if (type == 'T' && n >= kTHeader && serving_ && kmac_) {  // a guest's datagram for the host
        auto it = sessions_.find(k(sa, sp));
        if (it == sessions_.end() || it->second.id != get32(d.data() + 2)) return false;
        Session& s = it->second;
        if (!open(kmac_, s.k_up.data(), d, kTHeader)) return false;  // forged or altered (R2)
        if (!s.up_in.accept(get64(d.data() + 6))) return false;      // replayed
        s.last = now;
        if (n == kTHeader) return false;  // keepalive
        if (is_route_frame(d.data() + kTHeader, n - kTHeader) || !sv_key_.id) return false;
        if (!s.frames.take(1, now) || !s.bytes.take(static_cast<double>(n), now)) {
            if (s.dropped++ % 1000 == 0)
                note("member relay: " + key_text(sa, sp) + " is over its rate; datagrams dropped");
            return false;
        }
        std::vector<std::uint8_t> f{kRouteMagic, 'V',
                                    static_cast<std::uint8_t>(now - s.created < kSessionStart ? kFlagStart : 0)};
        put32(f, sv_key_.id);
        put64(f, ++sv_out_ctr_);
        put_key(f, sa, sp);
        f.resize(f.size() + 16);
        f.insert(f.end(), d.begin() + kTHeader, d.end());
        seal(kmac_, sv_key_.key.data(), f, kVHeaderMemberToHost);
        ++s.up;
        wire_to(sv_h_addr_, sv_h_port_, std::move(f), out);
        return false;
    }
    if (type == 'v' && n > kVHeaderHostToMember && serving_) {  // the host's datagram for a guest we relay for
        if (sa != sv_h_addr_ || sp != sv_h_port_ || !kmac_ || get32(d.data() + 2) != sv_key_.id) return false;
        if (!open(kmac_, sv_key_.key.data(), d, kVHeaderHostToMember)) return false;
        if (!sv_in_.accept(get64(d.data() + 6))) return false;  // a replay
        std::uint32_t ga;
        std::uint16_t gp;
        get_key(d.data() + 14, &ga, &gp);
        auto it = sessions_.find(k(ga, gp));
        if (it == sessions_.end() || is_route_frame(d.data() + kVHeaderHostToMember, n - kVHeaderHostToMember)) return false;
        std::vector<std::uint8_t> f{kRouteMagic, 't'};
        put32(f, it->second.id);
        put64(f, ++it->second.down_ctr);
        f.resize(f.size() + 16);
        f.insert(f.end(), d.begin() + kVHeaderHostToMember, d.end());
        seal(kmac_, it->second.k_down.data(), f, kTHeader);
        ++it->second.down;
        wire_to(ga, gp, std::move(f), out);
        return false;
    }
    if (type == 't' && n > kTHeader && kmac_) {  // the host's datagram, through our member
        if (cl_.st != ClientState::Ready || sa != cl_.m_addr || sp != cl_.m_port || get32(d.data() + 2) != cl_.session)
            return false;
        if (!open(kmac_, cl_.k_down.data(), d, kTHeader)) return false;  // forged or altered (R2)
        if (!cl_.down_in.accept(get64(d.data() + 6))) return false;      // replayed
        if (is_route_frame(d.data() + kTHeader, n - kTHeader)) return false;
        d.erase(d.begin(), d.begin() + kTHeader);
        *src_addr = cl_.h_addr;
        *src_port = cl_.h_port;
        if (via_relay) *via_relay = true;
        return true;
    }
    if (type == 'V' && n > kVHeaderMemberToHost && accept_ && kmac_) {  // a member's frame from a guest it relays
        const std::uint8_t flags = d[2];
        const std::uint32_t id = get32(d.data() + 3);
        auto m = members_.find(id);
        if (m == members_.end()) return false;  // no member gave us that key
        Member& mb = m->second;
        if (sa != mb.ip) return false;          // not from that member's address
        const bool bound_here = mb.bound_addr == sa && mb.bound_port == sp;
        if (mb.bound_addr && !bound_here && now - mb.last < std::chrono::seconds(30)) return false;
        if (!open(kmac_, mb.key.data(), d, kVHeaderMemberToHost)) return false;  // forged
        if (!mb.in.accept(get64(d.data() + 7))) return false;                     // replayed
        if (!bound_here) {
            if (mb.bound_addr)
                note("member relay: " + mb.name + "'s frames now come from " + key_text(sa, sp));
            mb.bound_addr = sa;
            mb.bound_port = sp;
        }
        mb.last = now;
        std::uint32_t ga;
        std::uint16_t gp;
        get_key(d.data() + 15, &ga, &gp);
        if ((ga == sa && gp == sp) || !valid_guest_key(ga, gp, sa) ||
            is_route_frame(d.data() + kVHeaderMemberToHost, n - kVHeaderMemberToHost))
            return false;
        auto it = host_routes_.find(k(ga, gp));
        if (it != host_routes_.end() && it->second.member_id != id) {
            // Another member's guest: only once that route went quiet and this member's session
            // with the guest has just started (the guest re-handshook with it).
            if (!(flags & kFlagStart) || now - it->second.last < kRouteMoveIdle) return false;
            note("member relay: the guest at " + key_text(ga, gp) + " now comes through " + mb.name);
            it->second.member_id = id;
        } else if (it == host_routes_.end()) {
            if (auto dir = direct_.find(k(ga, gp)); dir != direct_.end() && now - dir->second < kDirectGuard)
                return false;  // heard directly: a member cannot pose as it
            std::size_t mine = 0;
            for (const auto& [key, r] : host_routes_) mine += r.member_id == id;
            if (mine >= kMaxRoutesPerMember) return false;
            if (host_routes_.size() >= kMaxHostRoutes) {
                auto old = host_routes_.begin();
                for (auto i = host_routes_.begin(); i != host_routes_.end(); ++i)
                    if (i->second.last < old->second.last) old = i;
                if (now - old->second.last < kHostRouteIdle) return false;
                host_routes_.erase(old);
            }
            HostRouteState st;
            st.guest_addr = ga;
            st.guest_port = gp;
            st.member_id = id;
            it = host_routes_.emplace(k(ga, gp), st).first;
            note("member relay: a guest at " + key_text(ga, gp) + " reaches us through " + mb.name + " at " +
                 key_text(sa, sp));
        }
        it->second.last = now;
        d.erase(d.begin(), d.begin() + kVHeaderMemberToHost);
        *src_addr = ga;
        *src_port = gp;
        if (via_relay) *via_relay = true;
        return true;
    }
    return false;
}

void Router::client_send_hello(RClock::time_point now, std::vector<Wire>* out) {
    std::vector<std::uint8_t> f{kRouteMagic, 'Q', 1};
    f.insert(f.end(), cl_.cn.begin(), cl_.cn.end());
    f.insert(f.end(), cl_.ya.begin(), cl_.ya.end());
    f.resize(kQ1Size, 0);
    cl_.next_send = now + std::chrono::milliseconds(500);
    wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
}

void Router::client_send_auth(RClock::time_point now, std::vector<Wire>* out) {
    std::vector<std::uint8_t> m;
    append(m, kLabelConfirmA);
    m.insert(m.end(), cl_.cn.begin(), cl_.cn.end());
    std::uint8_t confirm[16];
    kmac_(cl_.isk.data(), m.data(), m.size(), confirm);
    std::vector<std::uint8_t> f{kRouteMagic, 'Q', 3};
    f.insert(f.end(), cl_.cn.begin(), cl_.cn.end());
    f.insert(f.end(), cl_.ya.begin(), cl_.ya.end());
    f.insert(f.end(), cl_.yb.begin(), cl_.yb.end());
    f.insert(f.end(), cl_.cookie.begin(), cl_.cookie.end());
    f.insert(f.end(), confirm, confirm + 16);
    cl_.next_send = now + std::chrono::milliseconds(500);
    wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
}

void Router::relay_use(std::uint32_t ma, std::uint16_t mp, std::uint32_t ha, std::uint16_t hp, Pake pake,
                       RClock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    cl_ = Client{};
    cl_.st = ClientState::Handshake;
    cl_.m_addr = ma;
    cl_.m_port = mp;
    cl_.h_addr = ha;
    cl_.h_port = hp;
    cl_.pake = std::move(pake);
    if (auto b = backoff_.find(k(ma, mp)); b != backoff_.end() && now < b->second.until) {
        cl_.st = ClientState::Refused;
        cl_.code = kBackoff;
        cl_.wait_s = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(b->second.until - now).count());
        note("member relay: not asking the member at " + key_text(ma, mp) + " again yet (its confirmation failed " +
             std::to_string(b->second.failures) + " time(s); " + std::to_string(cl_.wait_s) + " s left)");
        return;
    }
    secure_random(cl_.cn.data(), 16);
    // Our PAKE point Ya = ya * g(relay key, cn).
    std::uint8_t g[32];
    secure_random(cl_.ya_s.data(), 32);
    bool ok = static_cast<bool>(kmac_) && cl_.pake.generator && cl_.pake.x25519 && cl_.pake.hash;
    if (ok) {
        cl_.pake.generator(cl_.cn.data(), g);
        ok = cl_.pake.x25519(cl_.ya_s.data(), g, cl_.ya.data());
    }
    if (!ok) {
        cl_.st = ClientState::Refused;
        cl_.code = kTimeout;
        note("member relay: no PAKE here (no keys): not asking the member");
        return;
    }
    cl_.started = now;
    cl_.next_send = now;
    note("member relay: asking the member at " + key_text(ma, mp) + " to relay for us to the host " + key_text(ha, hp));
}

void Router::relay_stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st == ClientState::Off) return;
    note("member relay: no longer through the member at " + key_text(cl_.m_addr, cl_.m_port));
    cl_ = Client{};
}

Router::ClientState Router::relay_state(std::string* name, int* code, int* wait_s) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (name) *name = cl_.name;
    if (code) *code = cl_.code;
    if (wait_s) *wait_s = cl_.wait_s;
    return cl_.st;
}

bool Router::relay_member(std::uint32_t* addr, std::uint16_t* port) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st == ClientState::Off) return false;
    *addr = cl_.m_addr;
    *port = cl_.m_port;
    return true;
}

void Router::serve(bool on, std::uint32_t ha, std::uint16_t hp, Pake pake, std::string own_name, Throttle throttle,
                   const MemberKey& key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on) {
        if (serving_) note("member relay: not relaying for others any more");
        serving_ = false;
        sessions_.clear();
        sv_key_ = {};
        return;
    }
    if (!serving_ || ha != sv_h_addr_ || hp != sv_h_port_ || key.id != sv_key_.id) {
        sessions_.clear();
        secure_random(sv_salt_.data(), 16);
        secure_random(sv_cookie_key_.data(), 32);
        sv_out_ctr_ = 0;
        sv_in_ = Replay{};
        note("member relay: ready to relay to the host " + key_text(ha, hp) +
             " for party members that cannot reach it directly");
    }
    serving_ = true;
    sv_h_addr_ = ha;
    sv_h_port_ = hp;
    sv_pake_ = std::move(pake);
    if (!sv_pake_.generator || !sv_pake_.x25519 || !sv_pake_.hash) serving_ = false;
    sv_name_ = std::move(own_name);
    sv_throttle_ = std::move(throttle);
    sv_key_ = key;
}

bool Router::serving() const {
    std::lock_guard<std::mutex> lk(mu_);
    return serving_;
}

std::vector<Router::Served> Router::served() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Served> v;
    for (const auto& [key, s] : sessions_) {
        (void)key;
        v.push_back({s.addr, s.port, s.up, s.down});
    }
    return v;
}

void Router::accept_relayed(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    accept_ = on;
    if (!on) host_routes_.clear();
}

void Router::set_member_key(const MemberKey& key, std::uint32_t ip, const std::string& member) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!key.id || !ip) return;
    // An id another member registered: refused (N5: no take-over by a colliding or copied id).
    if (auto same = members_.find(key.id); same != members_.end() && same->second.name != member) {
        note("member relay: " + member + "'s relay key id is another member's: refused");
        return;
    }
    for (auto i = members_.begin(); i != members_.end();) {  // its earlier key goes, with its routes
        if (i->second.name == member && i->first != key.id) {
            for (auto r = host_routes_.begin(); r != host_routes_.end();)
                r = r->second.member_id == i->first ? host_routes_.erase(r) : std::next(r);
            i = members_.erase(i);
        } else {
            ++i;
        }
    }
    if (!members_.count(key.id) && members_.size() >= kMaxMemberKeys) return;
    Member& m = members_[key.id];
    if (m.key != key.key || m.ip != ip) {
        m = Member{};
        m.key = key.key;
        m.ip = ip;
    }
    m.name = member;
}

void Router::drop_member(const std::string& member) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto i = members_.begin(); i != members_.end();) {
        if (i->second.name == member) {
            for (auto r = host_routes_.begin(); r != host_routes_.end();)
                r = r->second.member_id == i->first ? host_routes_.erase(r) : std::next(r);
            i = members_.erase(i);
        } else {
            ++i;
        }
    }
}

void Router::set_own_addrs(std::vector<std::uint32_t> addrs) {
    std::lock_guard<std::mutex> lk(mu_);
    own_addrs_ = std::set<std::uint32_t>(addrs.begin(), addrs.end());
}

std::vector<Router::HostRoute> Router::host_routes() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<HostRoute> v;
    for (const auto& [key, r] : host_routes_) {
        (void)key;
        HostRoute h;
        h.guest_addr = r.guest_addr;
        h.guest_port = r.guest_port;
        if (auto m = members_.find(r.member_id); m != members_.end()) {
            h.member_addr = m->second.bound_addr;
            h.member_port = m->second.bound_port;
            h.member = m->second.name;
        }
        v.push_back(h);
    }
    return v;
}

bool Router::relayed_guest(std::uint32_t addr, std::uint16_t port, std::uint32_t* ma, std::uint16_t* mp) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = host_routes_.find(k(addr, port));
    if (it == host_routes_.end()) return false;
    auto m = members_.find(it->second.member_id);
    if (ma) *ma = m != members_.end() ? m->second.bound_addr : 0;
    if (mp) *mp = m != members_.end() ? m->second.bound_port : 0;
    return true;
}

void Router::poll(RClock::time_point now, std::vector<Wire>* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st == ClientState::Handshake) {
        if (now - cl_.started > kHandshakeTimeout) {
            cl_.st = ClientState::Refused;
            cl_.code = kTimeout;
            note("member relay: no answer from the member at " + key_text(cl_.m_addr, cl_.m_port));
        } else if (now >= cl_.next_send) {
            if (cl_.have_isk) client_send_auth(now, out);
            else client_send_hello(now, out);
        }
    } else if (cl_.st == ClientState::Ready && now - cl_.last_sent > kKeepalive && kmac_) {
        std::vector<std::uint8_t> f{kRouteMagic, 'T'};
        put32(f, cl_.session);
        put64(f, ++cl_.up_ctr);
        f.resize(f.size() + 16);
        seal(kmac_, cl_.k_up.data(), f, kTHeader);
        cl_.last_sent = now;
        wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
    }
    for (auto i = sessions_.begin(); i != sessions_.end();) {
        if (now - i->second.last > kSessionIdle) {
            note("member relay: the guest at " + key_text(i->second.addr, i->second.port) + " went quiet (" +
                 std::to_string(i->second.up) + " datagrams to the host, " + std::to_string(i->second.down) +
                 " back): session closed");
            i = sessions_.erase(i);
        } else {
            ++i;
        }
    }
    for (auto i = host_routes_.begin(); i != host_routes_.end();) {
        if (now - i->second.last > kHostRouteIdle) {
            note("member relay: the guest at " + key_text(i->second.guest_addr, i->second.guest_port) +
                 " is no longer relayed");
            i = host_routes_.erase(i);
        } else {
            ++i;
        }
    }
}

}  // namespace bbnet::udp
