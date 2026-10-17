// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_addr.h"

#include "../net/net_stun.h"
#include "party_code.h"
#include "party_sock.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace party {

bool resolve_ipv4(const std::string& host_port, std::uint16_t default_port, std::array<std::uint8_t, 4>* ip,
                  std::uint16_t* port) {
    std::string host = host_port;
    std::uint16_t p = default_port;
    std::size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        long v = std::strtol(host_port.c_str() + colon + 1, nullptr, 10);
        if (v <= 0 || v > 65535) return false;
        p = static_cast<std::uint16_t>(v);
    }
    if (host.empty()) return false;
    if (parse_ipv4(host, ip)) {
        *port = p;
        return true;
    }
    if (!sock::startup()) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    bool ok = false;
    for (addrinfo* r = res; r; r = r->ai_next) {
        if (r->ai_family != AF_INET) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(r->ai_addr);
        std::memcpy(ip->data(), &sin->sin_addr, 4);
        ok = true;
        break;
    }
    freeaddrinfo(res);
    if (ok) *port = p;
    return ok;
}

bool stun_query(std::intptr_t sock_in, const std::string& server, std::uint16_t local_port,
                std::array<std::uint8_t, 4>* ip, std::uint16_t* port, std::string* error, int timeout_ms, int tries) {
    auto fail = [&](std::string m) {
        if (error) *error = std::move(m);
        return false;
    };
    if (!sock::startup()) return fail("winsock unavailable");
    std::array<std::uint8_t, 4> sip{};
    std::uint16_t sport = 0;
    if (!resolve_ipv4(server, 3478, &sip, &sport)) return fail("cannot resolve STUN server " + server);

    sock::Socket s = sock_in == -1 ? sock::kInvalid : static_cast<sock::Socket>(sock_in);
    bool own = false;
    if (sock_in == -1) {
        s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == sock::kInvalid) return fail("cannot create a UDP socket");
        own = true;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(local_port);
        if (sock::loopback_only()) a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            a.sin_port = 0;  // the party port is taken (the game's socket): any port tells the address
            ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        }
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(sport);
    std::memcpy(&to.sin_addr, sip.data(), 4);

    bool ok = false;
    for (int t = 0; t < tries && !ok; ++t) {
        std::uint8_t req[net::stun::kMaxRequest];
        std::uint8_t txid[net::stun::kTxid];
        std::size_t n = net::stun::build_binding_request(req, txid);
        if (::sendto(s, reinterpret_cast<const char*>(req), static_cast<int>(n), 0, reinterpret_cast<sockaddr*>(&to),
                     sizeof to) < 0)
            continue;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                            .count();
            if (left <= 0) break;
            sock::PollFd pfd{};
            pfd.fd = s;
            pfd.events = POLLIN;
            if (sock::poll(&pfd, 1, static_cast<int>(left)) <= 0) break;
            std::uint8_t buf[1500];
            sockaddr_in from{};
            sock::socklen fl = sizeof from;
            long r = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (r <= 0) continue;
            std::uint32_t addr = 0;
            std::uint16_t p = 0;
            if (net::stun::parse_binding_response(buf, static_cast<std::size_t>(r), txid, &addr, &p)) {
                std::memcpy(ip->data(), &addr, 4);
                *port = p;
                ok = true;
                break;
            }
        }
    }
    if (own) sock::close(s);
    if (!ok) return fail("no STUN answer from " + server + " (" + std::to_string(tries) + " tries)");
    return true;
}

PublicAddress resolve_public_address(std::uint16_t party_port, std::intptr_t udp_sock,
                                     const std::string& upnp_external_ip) {
    PublicAddress out;
    if (const char* ov = std::getenv("BB_PARTY_PUBLIC_ADDR"); ov && *ov) {
        std::array<std::uint8_t, 4> ip{};
        std::uint16_t port = party_port;
        if (resolve_ipv4(ov, party_port, &ip, &port)) {
            out.ok = true;
            out.ip = ip;
            out.port = port;
            out.source = "override";
            out.message = std::string("public address from BB_PARTY_PUBLIC_ADDR: ") + format_ipv4(ip) + ":" +
                          std::to_string(port);
            return out;
        }
        out.message = std::string("BB_PARTY_PUBLIC_ADDR is not an address: ") + ov + "; ";
    }
    const char* env = std::getenv("BB_PARTY_STUN");
    std::string server = env && *env ? env : "stun.l.google.com:19302";
    if (server != "off" && server != "0") {
        std::array<std::uint8_t, 4> ip{};
        std::uint16_t port = 0;
        std::string err;
        if (stun_query(udp_sock, server, party_port, &ip, &port, &err)) {
            out.ok = true;
            out.ip = ip;
            out.port = port;
            out.source = "stun";
            out.message += "public address from STUN " + server + ": " + format_ipv4(ip) + ":" + std::to_string(port);
            if (udp_sock != -1 && port != party_port)
                out.message += " (the NAT remaps ports: guests may need the host relay or a port forward)";
            if (udp_sock == -1) out.port = party_port;
            return out;
        }
        out.message += err + "; ";
    } else {
        out.message += "STUN disabled (BB_PARTY_STUN=off); ";
    }
    std::array<std::uint8_t, 4> ip{};
    if (!upnp_external_ip.empty() && parse_ipv4(upnp_external_ip, &ip)) {
        out.ok = true;
        out.ip = ip;
        out.port = party_port;
        out.source = "upnp";
        out.message += "public address from the UPnP router: " + upnp_external_ip;
        return out;
    }
    out.message += "no public address: set BB_PARTY_PUBLIC_ADDR or share the LAN code";
    return out;
}

}  // namespace party
