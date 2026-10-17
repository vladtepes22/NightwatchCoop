// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_lan.h"

#include "party_sock.h"

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include <algorithm>
#include <cstring>
#include <thread>

namespace party::lan {

namespace {

constexpr char kProbeLabel[] = "bbp-lan-probe";
constexpr char kReplyLabel[] = "bbp-lan-reply";

std::array<std::uint8_t, kTagSize> tag_of(const crypto::Key& key, const char* label, const std::uint8_t* body,
                                         std::size_t n) {
    std::vector<std::uint8_t> buf(key.begin(), key.end());
    buf.insert(buf.end(), label, label + std::strlen(label));
    buf.insert(buf.end(), body, body + n);
    const crypto::Key h = crypto::blake2b256(buf.data(), buf.size());
    crypto::wipe(buf.data(), buf.size());
    std::array<std::uint8_t, kTagSize> t{};
    std::memcpy(t.data(), h.data(), kTagSize);
    return t;
}

bool equal_tag(const std::uint8_t* a, const std::uint8_t* b) {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < kTagSize; ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

void header(std::uint8_t* d, std::uint8_t type) {
    d[0] = kMagic0;
    d[1] = kMagic1;
    d[2] = kVersion;
    d[3] = type;
}

bool has_header(const std::uint8_t* d, std::size_t n, std::size_t size, std::uint8_t type) {
    return d && n == size && d[0] == kMagic0 && d[1] == kMagic1 && d[2] == kVersion && d[3] == type;
}

}  // namespace

std::array<std::uint8_t, kProbeSize> encode_probe(const crypto::Key& key, const Nonce& nonce) {
    std::array<std::uint8_t, kProbeSize> d{};
    header(d.data(), kTypeProbe);
    std::memcpy(d.data() + 4, nonce.data(), 8);
    const auto t = tag_of(key, kProbeLabel, nonce.data(), 8);
    std::memcpy(d.data() + 12, t.data(), kTagSize);
    return d;
}

bool verify_probe(const crypto::Key& key, const std::uint8_t* d, std::size_t n, Nonce* nonce) {
    if (!has_header(d, n, kProbeSize, kTypeProbe)) return false;
    const auto t = tag_of(key, kProbeLabel, d + 4, 8);
    if (!equal_tag(t.data(), d + 12)) return false;
    if (nonce) std::memcpy(nonce->data(), d + 4, 8);
    return true;
}

std::array<std::uint8_t, kReplySize> encode_reply(const crypto::Key& key, const Nonce& nonce,
                                                  const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port) {
    std::array<std::uint8_t, kReplySize> d{};
    header(d.data(), kTypeReply);
    std::memcpy(d.data() + 4, nonce.data(), 8);
    std::memcpy(d.data() + 12, ipv4.data(), 4);
    d[16] = static_cast<std::uint8_t>(port >> 8);
    d[17] = static_cast<std::uint8_t>(port);
    const auto t = tag_of(key, kReplyLabel, d.data() + 4, 14);
    std::memcpy(d.data() + 18, t.data(), kTagSize);
    return d;
}

bool parse_reply(const crypto::Key& key, const Nonce& nonce, const std::uint8_t* d, std::size_t n,
                 std::array<std::uint8_t, 4>* ipv4, std::uint16_t* port) {
    if (!has_header(d, n, kReplySize, kTypeReply)) return false;
    if (std::memcmp(d + 4, nonce.data(), 8) != 0) return false;
    const auto t = tag_of(key, kReplyLabel, d + 4, 14);
    if (!equal_tag(t.data(), d + 18)) return false;
    const std::uint16_t p = static_cast<std::uint16_t>((d[16] << 8) | d[17]);
    if (p == 0) return false;
    if (ipv4) std::memcpy(ipv4->data(), d + 12, 4);
    if (port) *port = p;
    return true;
}

bool ReplyLimiter::allow(Clock::time_point now) {
    if (!started_) {
        started_ = true;
        last_ = now;
    }
    const double dt = std::chrono::duration<double>(now - last_).count();
    if (dt > 0) {
        tokens_ = std::min<double>(kRepliesPerSecond, tokens_ + dt * kRepliesPerSecond);
        last_ = now;
    }
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
}

std::vector<std::uint8_t> answer_probe(const std::vector<crypto::Key>& keys, const std::uint8_t* d, std::size_t n,
                                       const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port,
                                       ReplyLimiter* limiter, Clock::time_point now) {
    if (!has_header(d, n, kProbeSize, kTypeProbe)) return {};
    for (const crypto::Key& k : keys) {
        Nonce nonce{};
        if (!verify_probe(k, d, n, &nonce)) continue;
        if (limiter && !limiter->allow(now)) return {};
        const auto r = encode_reply(k, nonce, ipv4, port);
        return std::vector<std::uint8_t>(r.begin(), r.end());
    }
    return {};
}

std::vector<std::uint32_t> broadcast_targets() {
    std::vector<std::uint32_t> out{0xffffffffu};
    auto add = [&](std::uint32_t addr_nbo, std::uint32_t mask_nbo) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(&addr_nbo);
        if (b[0] == 127 || b[0] == 0 || (b[0] == 169 && b[1] == 254)) return;
        if (mask_nbo == 0 || mask_nbo == 0xffffffffu) return;
        const std::uint32_t bc = addr_nbo | ~mask_nbo;
        if (std::find(out.begin(), out.end(), bc) == out.end()) out.push_back(bc);
    };
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const unsigned bits = u->OnLinkPrefixLength;
            if (bits == 0 || bits >= 32) continue;
            const std::uint32_t mask = htonl(0xffffffffu << (32 - bits));
            add(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr, mask);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            add(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr,
                reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
        }
        freeifaddrs(ifs);
    }
#endif
    return out;
}

bool tcp_reachable(std::uint32_t addr_nbo, std::uint16_t port, int timeout_ms) {
    if (!sock::startup()) return false;
    sock::Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == sock::kInvalid) return false;
    sock::set_nonblocking(s);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addr_nbo;
    a.sin_port = htons(port);
    bool ok = false;
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        ok = true;
    } else if (sock::would_block(sock::last_error())) {
        sock::PollFd p{};
        p.fd = s;
        p.events = POLLOUT;
        if (sock::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLOUT) && !(p.revents & (POLLERR | POLLHUP))) {
            int err = 0;
            sock::socklen el = sizeof err;
            ok = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el) == 0 && err == 0;
        }
    }
    sock::close(s);
    return ok;
}

bool discover(const crypto::Key& key, const std::vector<std::uint32_t>& targets, std::uint16_t port, int budget_ms,
              bool loopback, Found* out, std::string* note) {
    auto say = [&](const std::string& m) {
        if (note) *note = m;
    };
    if (!sock::startup()) return say("winsock unavailable"), false;
    sock::Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == sock::kInvalid) return say("no UDP socket"), false;
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = loopback ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) {
        sock::close(s);
        return say("cannot bind a UDP socket"), false;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    sock::set_nonblocking(s);
    Nonce nonce{};
    crypto::random_bytes(nonce.data(), nonce.size());
    const auto probe = encode_probe(key, nonce);
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(budget_ms);
    // Three rounds: at once, after 1/4 and after 1/2 of the budget (a lost broadcast is common on Wi-Fi).
    const std::chrono::milliseconds rounds[] = {std::chrono::milliseconds(0), std::chrono::milliseconds(budget_ms / 4),
                                                std::chrono::milliseconds(budget_ms / 2)};
    int sent_rounds = 0, sent = 0;
    bool found = false;
    while (!found) {
        const auto now = Clock::now();
        if (now >= deadline) break;
        while (sent_rounds < 3 && now - start >= rounds[sent_rounds]) {
            for (std::uint32_t t : targets) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = t;
                to.sin_port = htons(port);
                if (::sendto(s, reinterpret_cast<const char*>(probe.data()), static_cast<int>(probe.size()), 0,
                             reinterpret_cast<sockaddr*>(&to), sizeof to) > 0)
                    ++sent;
            }
            ++sent_rounds;
        }
        sock::PollFd p{};
        p.fd = s;
        p.events = POLLIN;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (sock::poll(&p, 1, static_cast<int>(std::max<long long>(1, std::min<long long>(left, 50)))) <= 0) continue;
        for (;;) {
            std::uint8_t buf[256];
            sockaddr_in from{};
            sock::socklen fl = sizeof from;
            const long r = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                      reinterpret_cast<sockaddr*>(&from), &fl);
            if (r <= 0) break;
            std::array<std::uint8_t, 4> said{};
            std::uint16_t hp = 0;
            if (!parse_reply(key, nonce, buf, static_cast<std::size_t>(r), &said, &hp)) continue;
            out->addr_nbo = from.sin_addr.s_addr;
            out->port = hp;
            out->said = said;
            found = true;
            break;
        }
    }
    sock::close(s);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    say(std::to_string(sent) + " probe(s) to " + std::to_string(targets.size()) + " address(es), " +
        std::to_string(ms) + " ms");
    return found;
}

}  // namespace party::lan
