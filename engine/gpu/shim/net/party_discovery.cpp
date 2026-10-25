// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_discovery.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

namespace bbnet::disc {
namespace {

std::uint32_t host_order(std::uint32_t nbo) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&nbo);
    return (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
}

std::string clean(const std::string& s, std::size_t max) {
    std::string o;
    for (char c : s) {
        if (o.size() >= max) break;
        const auto u = static_cast<unsigned char>(c);
        o += (u >= 0x20 && u < 0x7f) ? c : '?';
    }
    return o;
}

void put_str(std::vector<std::uint8_t>* v, const std::string& s) {
    v->push_back(static_cast<std::uint8_t>(s.size()));
    v->insert(v->end(), s.begin(), s.end());
}

bool get_str(const std::uint8_t* d, std::size_t n, std::size_t* pos, std::size_t max, std::string* out) {
    if (*pos >= n) return false;
    const std::size_t len = d[(*pos)++];
    if (len > max || *pos + len > n) return false;
    for (std::size_t i = 0; i < len; ++i) {
        if (d[*pos + i] < 0x20 || d[*pos + i] >= 0x7f) return false;
    }
    out->assign(reinterpret_cast<const char*>(d + *pos), len);
    *pos += len;
    return true;
}

}  // namespace

std::vector<std::uint8_t> encode(const Announce& a) {
    std::vector<std::uint8_t> v{0xfa, 'A', kVersion, static_cast<std::uint8_t>(a.password ? 1 : 0)};
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(a.id >> s));
    v.push_back(static_cast<std::uint8_t>(a.port >> 8));
    v.push_back(static_cast<std::uint8_t>(a.port));
    v.push_back(static_cast<std::uint8_t>(a.level >> 8));
    v.push_back(static_cast<std::uint8_t>(a.level));
    v.push_back(a.players);
    v.push_back(a.max_players);
    put_str(&v, clean(a.build, kMaxBuild));
    put_str(&v, clean(a.name, 16));
    put_str(&v, clean(a.area, 32));
    return v;
}

bool parse(const std::uint8_t* d, std::size_t n, Announce* out) {
    if (!d || !out || n < 17 || n > kMaxPacket) return false;
    if (d[0] != 0xfa || d[1] != 'A' || d[2] != kVersion || (d[3] & ~1) != 0) return false;
    Announce a;
    a.password = (d[3] & 1) != 0;
    a.id = (std::uint32_t(d[4]) << 24) | (std::uint32_t(d[5]) << 16) | (std::uint32_t(d[6]) << 8) | d[7];
    a.port = static_cast<std::uint16_t>((d[8] << 8) | d[9]);
    a.level = static_cast<std::uint16_t>((d[10] << 8) | d[11]);
    a.players = d[12];
    a.max_players = d[13];
    if (a.port == 0 || a.max_players == 0 || a.max_players > 8 || a.players > a.max_players) return false;
    std::size_t pos = 14;
    if (!get_str(d, n, &pos, kMaxBuild, &a.build) || !get_str(d, n, &pos, 16, &a.name) ||
        !get_str(d, n, &pos, 32, &a.area) || pos != n)
        return false;
    if (a.name.empty()) return false;
    *out = std::move(a);
    return true;
}

bool adapter_discovery_ok(const LanAdapter& a) {
    if (!a.ip || a.loopback || a.tunnel) return false;
    const std::uint32_t h = host_order(a.ip);
    const bool priv = (h >> 24) == 10 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8;
    switch (vpn_kind(a)) {
        case VpnKind::Radmin: return (h >> 24) == 26 || priv;
        case VpnKind::Hamachi: return (h >> 24) == 25 || priv;
        case VpnKind::ZeroTier: return priv;
        case VpnKind::None: return priv && !adapter_is_virtual(a.name);
        default: return false;  // Tailscale (no broadcasts), commercial VPNs
    }
}

std::vector<Target> targets_for(const std::vector<LanAdapter>& adapters) {
    std::vector<Target> out;
    for (const LanAdapter& a : adapters) {
        if (!adapter_discovery_ok(a) || a.mask == 0 || a.mask == 0xffffffffu) continue;
        Target t;
        t.source = a.ip;
        t.broadcast = a.ip | ~a.mask;
        t.name = a.name;
        const bool dup = std::any_of(out.begin(), out.end(), [&](const Target& o) {
            return o.source == t.source && o.broadcast == t.broadcast;
        });
        if (!dup) out.push_back(std::move(t));
    }
    return out;
}

std::vector<LanAdapter> enumerate_adapters() {
    std::vector<LanAdapter> out;
#if defined(_WIN32)
    ULONG size = 32 * 1024;
    std::vector<std::uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    auto narrow = [](const wchar_t* w) {
        std::string s;
        if (!w) return s;
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            s.resize(static_cast<std::size_t>(n - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        }
        return s;
    };
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        LanAdapter info;
        info.name = narrow(a->FriendlyName);
        const std::string desc = narrow(a->Description);
        if (!desc.empty() && desc != info.name) info.name += " / " + desc;
        info.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        info.tunnel = a->IfType == IF_TYPE_TUNNEL;
        info.physical = a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            LanAdapter one = info;
            one.ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            const unsigned bits = u->OnLinkPrefixLength;
            one.mask = bits == 0 ? 0 : htonl(bits >= 32 ? 0xffffffffu : 0xffffffffu << (32 - bits));
            out.push_back(std::move(one));
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            LanAdapter one;
            one.name = i->ifa_name ? i->ifa_name : "";
            one.loopback = (i->ifa_flags & IFF_LOOPBACK) != 0;
            one.ip = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
            one.mask = reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr;
            one.physical = one.name.rfind("en", 0) == 0 || one.name.rfind("eth", 0) == 0 || one.name.rfind("wl", 0) == 0;
            out.push_back(std::move(one));
        }
        freeifaddrs(ifs);
    }
#endif
    return out;
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

// One UDP socket bound to the adapter's address (so the datagram leaves that adapter only).
Sock open_bound(std::uint32_t source_nbo) {
    Sock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return kBad;
    int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = source_nbo;
    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        close_sock(s);
        return kBad;
    }
    return s;
}

struct Out {
    Sock sock = kBad;
    std::uint32_t source = 0, broadcast = 0;
};

void thread_main(Provider provider) {
    std::vector<Out> outs;
    auto close_all = [&] {
        for (Out& o : outs) close_sock(o.sock);
        outs.clear();
    };
    std::random_device rd;
    const std::uint32_t id = (std::uint32_t(rd()) << 16) ^ std::uint32_t(rd()) ^ 0x42425044u;
    const char* lb_env = std::getenv("BB_PARTY_DISCOVERY_LOOPBACK");
    const bool loopback_test = lb_env && lb_env[0] == '1';
    auto last_scan = std::chrono::steady_clock::time_point{};
    bool logged = false;
    State& st = S();
    for (;;) {
        Announce a;
        const bool on = provider(&a);
        const auto now = std::chrono::steady_clock::now();
        if (!on) {
            close_all();
            last_scan = {};
            logged = false;
        } else {
            if (last_scan == std::chrono::steady_clock::time_point{} || now - last_scan > std::chrono::seconds(10)) {
                last_scan = now;
                close_all();
                std::vector<Target> targets = targets_for(enumerate_adapters());
                if (loopback_test) {
                    Target t;
                    t.source = t.broadcast = htonl(0x7f000001u);
                    t.name = "loopback (test)";
                    targets.push_back(t);
                }
                std::string names;
                for (const Target& t : targets) {
                    Sock s = open_bound(t.source);
                    if (s == kBad) continue;
                    outs.push_back({s, t.source, t.broadcast});
                    names += (names.empty() ? "" : "; ") + t.name;
                }
                if (!logged) {
                    log("party discovery: announcing on %zu adapter(s)%s%s", outs.size(), names.empty() ? "" : ": ",
                        names.c_str());
                    logged = true;
                }
            }
            a.id = id;
            const std::vector<std::uint8_t> pkt = encode(a);
            for (const Out& o : outs) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(kDiscoveryPort);
                to.sin_addr.s_addr = o.broadcast;
                ::sendto(o.sock, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
                         reinterpret_cast<sockaddr*>(&to), sizeof(to));
            }
        }
        std::unique_lock<std::mutex> lk(st.mu);
        if (st.cv.wait_for(lk, std::chrono::milliseconds(kAnnounceEveryMs), [&] { return st.stop; })) break;
    }
    close_all();
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

}  // namespace bbnet::disc
