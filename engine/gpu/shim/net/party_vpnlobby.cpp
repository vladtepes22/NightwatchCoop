// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_vpnlobby.h"

#include "bbnet_internal.h"
#include "party_discovery.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace bbnet::lobby {
namespace {

// Decodes one UTF-8 sequence at s[*i]; false for invalid / overlong / surrogate / > U+10FFFF.
bool next_cp(const std::string& s, std::size_t* i, std::uint32_t* cp) {
    const auto b = [&](std::size_t k) { return static_cast<std::uint8_t>(s[k]); };
    const std::uint8_t c = b(*i);
    std::size_t n = 0;
    std::uint32_t v = 0;
    if (c < 0x80) {
        *cp = c;
        *i += 1;
        return true;
    }
    if (c >= 0xc2 && c <= 0xdf) {
        n = 1;
        v = c & 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
        n = 2;
        v = c & 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        n = 3;
        v = c & 0x07;
    } else {
        return false;
    }
    for (std::size_t k = 1; k <= n; ++k) {
        if (*i + k >= s.size() || (b(*i + k) & 0xc0) != 0x80) return false;
        v = (v << 6) | (b(*i + k) & 0x3f);
    }
    if ((n == 2 && v < 0x800) || (n == 3 && (v < 0x10000 || v > 0x10ffff)) || (v >= 0xd800 && v <= 0xdfff)) return false;
    *cp = v;
    *i += n + 1;
    return true;
}

void append_utf8(std::string* o, std::uint32_t cp) {
    if (cp < 0x80) {
        *o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *o += static_cast<char>(0xc0 | (cp >> 6));
        *o += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        *o += static_cast<char>(0xe0 | (cp >> 12));
        *o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        *o += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        *o += static_cast<char>(0xf0 | (cp >> 18));
        *o += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        *o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        *o += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

// The code points the lobby would drop anyway (its clean_name: Unicode categories C* and Z* except the
// plain space, and combining marks): controls, soft hyphen, zero width and bidi controls, line / paragraph
// separators, no-break and other spaces, tags, variation selectors, private use. The lobby is the
// authority; this keeps the game from sending what would be cut.
bool dropped(std::uint32_t c) {
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) return true;
    if (c == 0xad || c == 0x34f || c == 0x61c || c == 0x115f || c == 0x1160 || c == 0x17b4 || c == 0x17b5) return true;
    if (c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200f) || (c >= 0x2028 && c <= 0x202f) ||
        (c >= 0x205f && c <= 0x206f) || c == 0x3000 || c == 0xfeff || c == 0x3164 || c == 0xffa0)
        return true;
    if ((c >= 0xe000 && c <= 0xf8ff) || (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfff0 && c <= 0xfffb)) return true;
    if (c >= 0xe0000) return true;
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) ||
        (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe20 && c <= 0xfe2f))
        return true;
    return false;
}

}  // namespace

std::string clean_name(const std::string& utf8) {
    std::string out;
    std::size_t chars = 0, i = 0;
    bool space = false;
    while (i < utf8.size()) {
        std::uint32_t cp;
        if (!next_cp(utf8, &i, &cp)) return "";
        if (cp == ' ') {
            space = !out.empty();
            continue;
        }
        if (dropped(cp)) continue;
        std::string one;
        append_utf8(&one, cp);
        const std::size_t extra = space ? 1 : 0;
        if (chars + 1 + extra > kMaxNameChars || out.size() + one.size() + extra > kMaxNameBytes) break;
        if (space) out += ' ';
        space = false;
        out += one;
        chars += 1 + extra;
    }
    return out;
}

std::string utf16_to_utf8(const char16_t* s, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; s && i < n && s[i]; ++i) {
        std::uint32_t c = s[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            ++i;
        } else if (c >= 0xd800 && c <= 0xdfff) {
            continue;  // a lone surrogate
        }
        append_utf8(&out, c);
    }
    return out;
}

std::vector<std::uint8_t> encode_announce(const Announce& a) {
    const std::string name = clean_name(a.name);
    std::string build;
    for (char c : a.build) {
        if (build.size() >= kMaxBuild) break;
        const auto u = static_cast<unsigned char>(c);
        build += (u >= 0x20 && u < 0x7f) ? c : '_';
    }
    if (name.empty() || a.port < 1024 || a.max_players < 1 || a.max_players > 8 || a.players > a.max_players) return {};
    std::vector<std::uint8_t> v = {0xfa, 'L', 1, 1, static_cast<std::uint8_t>(a.password ? 1 : 0),
                                   static_cast<std::uint8_t>(a.port >> 8), static_cast<std::uint8_t>(a.port & 255),
                                   a.players, a.max_players, static_cast<std::uint8_t>(build.size())};
    v.insert(v.end(), build.begin(), build.end());
    v.push_back(static_cast<std::uint8_t>(name.size()));
    v.insert(v.end(), name.begin(), name.end());
    return v;
}

std::vector<std::uint8_t> encode_bye() { return {0xfa, 'L', 1, 4}; }

bool is_tunnel_player_address(std::uint32_t ip) {
    // 10.8.0.0/22 (0.7.8: room for 1021 players once the server widens its pool from /24); the network,
    // the server (10.8.0.1) and the broadcast address are not players
    return (ip & 0xfffffc00u) == 0x0a080000u && ip != 0x0a080000u && ip != 0x0a080001u && ip != 0x0a0803ffu;
}

// ---- the announcer thread ------------------------------------------------------------------------

namespace {
#if defined(_WIN32)
using Sock = SOCKET;
const Sock kBad = INVALID_SOCKET;
void close_sock(Sock s) { ::closesocket(s); }
#else
using Sock = int;
const Sock kBad = -1;
void close_sock(Sock s) { ::close(s); }
#endif

struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::thread th;
    bool running = false, stop = false;
};
State& S() {
    static State s;
    return s;
}

// A UDP socket bound to the tunnel address (so the datagram leaves through the tunnel adapter) and the
// destination. Test override: BB_PARTY_VPNLOBBY_ADDR=ip:port, sent from 127.0.0.1.
struct Route {
    Sock sock = kBad;
    sockaddr_in to{};
};

bool find_route(Route* r) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    std::uint32_t source = 0;
    const char* t = std::getenv("BB_PARTY_VPNLOBBY_ADDR");
    if (t && *t) {
        const std::string s = t;
        const auto colon = s.rfind(':');
        in_addr ia{};
        const long port = colon == std::string::npos ? kLobbyPort : std::strtol(s.c_str() + colon + 1, nullptr, 10);
        if (inet_pton(AF_INET, s.substr(0, colon).c_str(), &ia) != 1 || port < 1 || port > 65535) return false;
        to.sin_addr = ia;
        to.sin_port = htons(static_cast<std::uint16_t>(port));
        source = htonl(0x7f000001u);
    } else {
        for (const LanAdapter& a : disc::enumerate_adapters()) {
            if (!is_tunnel_player_address(ntohl(a.ip))) continue;
            source = a.ip;
            to.sin_addr.s_addr = htonl(0x0a080001u);  // 10.8.0.1
            to.sin_port = htons(kLobbyPort);
            break;
        }
        if (!source) return false;
    }
    Sock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = source;
    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        close_sock(s);
        return false;
    }
    r->sock = s;
    r->to = to;
    return true;
}

void send_to(const Route& r, const std::vector<std::uint8_t>& pkt) {
    if (r.sock == kBad || pkt.empty()) return;
    ::sendto(r.sock, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
             reinterpret_cast<const sockaddr*>(&r.to), sizeof(r.to));
}

void thread_main(Provider provider) {
    State& st = S();
    Route route;
    bool announced = false, logged = false;
    auto last_scan = std::chrono::steady_clock::time_point{}, last_sent = std::chrono::steady_clock::time_point{};
    std::vector<std::uint8_t> last_pkt;
    auto drop_route = [&] {
        if (route.sock != kBad) close_sock(route.sock);
        route = Route{};
    };
    for (;;) {
        Announce a;
        const bool on = provider(&a);
        const auto now = std::chrono::steady_clock::now();
        if (!on) {
            if (announced) send_to(route, encode_bye());
            announced = false;
            drop_route();
            last_pkt.clear();
            last_scan = {};
            logged = false;
        } else {
            // Without a route the adapters are rescanned every round (the tunnel may come up later);
            // once found the route is kept for as long as the game announces.
            if (route.sock == kBad && (last_scan == std::chrono::steady_clock::time_point{} ||
                                       now - last_scan >= std::chrono::seconds(5))) {
                last_scan = now;
                if (!find_route(&route)) route = Route{};
                if (route.sock != kBad && !logged) {
                    log("VPN lobby: announcing to the Nightwatch lobby");
                    logged = true;
                }
            }
            // Every kAnnounceEveryMs, and at once when the state changed (a player joined, a new name).
            const std::vector<std::uint8_t> pkt = encode_announce(a);
            if (route.sock != kBad && !pkt.empty() &&
                (pkt != last_pkt || now - last_sent >= std::chrono::milliseconds(kAnnounceEveryMs))) {
                send_to(route, pkt);
                last_pkt = pkt;
                last_sent = now;
                announced = true;
            }
        }
        std::unique_lock<std::mutex> lk(st.mu);
        if (st.cv.wait_for(lk, std::chrono::milliseconds(2000), [&] { return st.stop; })) break;
    }
    if (announced) send_to(route, encode_bye());
    drop_route();
}
}  // namespace

void start(Provider provider) {
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.running || !provider) return;
    st.running = true;
    st.stop = false;
    st.th = std::thread(thread_main, std::move(provider));
}

void stop() {
    State& st = S();
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (!st.running) return;
        st.stop = true;
        st.running = false;
        th = std::move(st.th);
    }
    st.cv.notify_all();
    if (th.joinable()) th.join();
}

}  // namespace bbnet::lobby
