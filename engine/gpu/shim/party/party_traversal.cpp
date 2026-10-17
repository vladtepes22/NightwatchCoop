// SPDX-License-Identifier: GPL-3.0-or-later
#include "party/party_traversal.h"

#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

#include "net/netsim.h"
#include "party/party_rdv.h"
#include "bbport_settings.h"

namespace party::traversal {

namespace {
std::mutex g_mu;
std::unique_ptr<rdv::Rendezvous> g_host, g_guest;

const char* env(const char* n) {
    const char* v = std::getenv(n);
    return v && *v ? v : nullptr;
}

std::vector<std::string> bootstrap() {
    const char* b = env("BB_PARTY_DHT_BOOTSTRAP");
    if (!b) return rdv::default_bootstrap();
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = b;; ++p) {
        if (*p == ',' || !*p) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else if (*p != ' ') {
            cur.push_back(*p);
        }
    }
    return out;
}

std::uint16_t sw16(std::uint16_t v) { return static_cast<std::uint16_t>((v >> 8) | (v << 8)); }
}  // namespace

void split_host_port(const std::string& s, std::uint16_t def_port, std::string* host, std::uint16_t* port) {
    *host = s;
    *port = def_port;
    if (const auto c = s.rfind(':'); c != std::string::npos) {
        *host = s.substr(0, c);
        *port = static_cast<std::uint16_t>(std::strtoul(s.c_str() + c + 1, nullptr, 10));
    }
}

std::uint16_t simulated_mapped_port(std::uint16_t party_port) {
    return sw16(bbnet::netsim::nat_advertised_port(sw16(party_port), 0));
}

nat::Type detect(const Stun& stun, const nat::StunSample& first, std::uint32_t local_addr, std::uint16_t local_port,
                 bool loopback) {
    nat::StunSample second;
    if (loopback) {
        const bbnet::netsim::Config c = bbnet::netsim::config();
        if (!c.enabled || !c.nat) return nat::Type::Open;  // plain loopback: no NAT at all
        // The simulator stands in for both STUN servers; the address is "public" (not ours).
        nat::StunSample a{true, 0x0100007fu, sw16(bbnet::netsim::nat_advertised_port(sw16(local_port), 0))};
        second = {true, 0x0100007fu, sw16(bbnet::netsim::nat_advertised_port(sw16(local_port), 1))};
        return nat::classify({}, local_port, a, second);
    }
    const char* s2 = env("BB_PARTY_STUN2");
    const std::string server = s2 ? s2 : "stun.cloudflare.com:3478";
    if (first.ok && server != "off" && server != "0") {
        std::string h;
        std::uint16_t p = 3478;
        split_host_port(server, 3478, &h, &p);
        for (int i = 0; i < 2 && !second.ok; ++i) second.ok = stun(h, p, &second.addr, &second.port);
    }
    return nat::classify({local_addr}, local_port, first, second);
}

bool rendezvous_enabled(bool loopback) {
    if (!BbSettings::Get().party_rendezvous.load()) return false;  // opt-in (default off)
    return !loopback || env("BB_PARTY_DHT_BOOTSTRAP");
}

void start_host(const std::uint8_t* key, bool loopback,
                std::function<void(std::uint32_t, std::uint16_t)> punch, Log log) {
    auto last = std::make_shared<std::map<std::uint64_t, std::chrono::steady_clock::time_point>>();
    auto r = std::make_unique<rdv::Rendezvous>(rdv::info_hash(key, 32), bootstrap(), loopback, log);
    const bool ok = r->start_host(
        [punch, last](const rdv::Endpoint& e) {
            const std::uint64_t k = static_cast<std::uint64_t>(e.addr) << 16 | e.port;
            const auto now = std::chrono::steady_clock::now();
            auto it = last->find(k);
            if (it != last->end() && now - it->second < std::chrono::seconds(50)) return;
            if (last->size() > 256) last->clear();
            (*last)[k] = now;
            punch(e.addr, e.port);
        },
        5000);
    if (!ok) {
        if (log) log("rendezvous: no UDP socket for the DHT");
        return;
    }
    if (log) log("rendezvous: looking for guests in the DHT every 5 s (no open port; the DHT option is on)");
    std::lock_guard<std::mutex> lk(g_mu);
    g_host = std::move(r);
}

void start_guest(const std::uint8_t* key, bool loopback, std::uint16_t port, std::function<bool()> want, Log log) {
    auto r = std::make_unique<rdv::Rendezvous>(rdv::info_hash(key, 32), bootstrap(), loopback, log);
    if (!r->start_guest(port, std::move(want), 45000)) {
        if (log) log("rendezvous: no UDP socket for the DHT");
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_guest = std::move(r);
}

void stop() {
    std::unique_ptr<rdv::Rendezvous> h, g;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        h = std::move(g_host);
        g = std::move(g_guest);
    }
    if (h) h->stop();
    if (g) g->stop();
}

}  // namespace party::traversal
