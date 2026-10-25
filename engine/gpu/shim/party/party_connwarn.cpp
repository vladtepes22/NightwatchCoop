// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_connwarn.h"

#include "party_status.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace party::connwarn {

// ---- rules ----

bool PortListHas(const std::string& ports, int port) {
    if (ports.empty() || ports == "*") return true;
    std::size_t i = 0;
    while (i <= ports.size()) {
        std::size_t j = ports.find(',', i);
        if (j == std::string::npos) j = ports.size();
        std::string item = ports.substr(i, j - i);
        item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char c) { return std::isspace(c); }), item.end());
        if (!item.empty()) {
            const std::size_t dash = item.find('-');
            char* end = nullptr;
            if (dash == std::string::npos) {
                const long v = std::strtol(item.c_str(), &end, 10);
                if (end != item.c_str() && *end == 0 && v == port) return true;
            } else {
                const long lo = std::strtol(item.substr(0, dash).c_str(), nullptr, 10);
                const long hi = std::strtol(item.substr(dash + 1).c_str(), nullptr, 10);
                if (lo <= port && port <= hi && hi > 0) return true;
            }
        }
        i = j + 1;
    }
    return false;
}

bool SamePath(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (std::tolower(static_cast<unsigned char>(x)) != std::tolower(static_cast<unsigned char>(y))) return false;
    }
    return true;
}

namespace {
bool Matches(const FwRule& r, int proto, int profile_bit, const std::string& exe, int port) {
    if (!r.enabled || !r.inbound) return false;
    if (r.protocol != 256 && r.protocol != proto) return false;
    if (!(r.profiles & profile_bit)) return false;
    if (!r.app.empty() && !SamePath(r.app, exe)) return false;
    return PortListHas(r.local_ports, port);
}
}  // namespace

FwReport Classify(const std::vector<FwRule>& rules, const std::vector<FwProfile>& active, const std::string& exe,
                  int port) {
    FwReport out;
    bool any_on = false;
    for (const FwProfile& p : active) {
        if (!p.enabled) continue;
        any_on = true;
        for (const int proto : {6, 17}) {
            bool blocked = false, allowed = false;
            for (const FwRule& r : rules) {
                if (!Matches(r, proto, p.bit, exe, port)) continue;
                (r.allow ? allowed : blocked) = true;
            }
            const char* pn = proto == 6 ? "TCP" : "UDP";
            if (blocked) {
                out.state = FwState::Blocked;
                out.detail = std::string("a block rule matches inbound ") + pn + " " + std::to_string(port);
                return out;
            }
            if (!allowed && p.inbound_blocked) {
                out.state = FwState::Missing;
                out.detail = std::string("no allow rule for inbound ") + pn + " " + std::to_string(port) + " (profile " +
                             std::to_string(p.bit) + ")";
                return out;
            }
        }
    }
    if (!any_on) {
        out.state = active.empty() ? FwState::Unknown : FwState::Off;
        out.detail = active.empty() ? "no active profile read" : "the Windows Firewall is off on the active profiles";
        return out;
    }
    out.state = FwState::Allowed;
    out.detail = "inbound UDP and TCP " + std::to_string(port) + " allowed";
    return out;
}

// ---- the seam ----
namespace {
std::mutex g_mu;
FirewallQuery g_query;
bool (*g_enabled)() = nullptr;
void (*g_log)(const char*) = nullptr;

// Test aids: BB_PARTY_CONNWARN_UDP_N (failures in a row per guest), BB_PARTY_CONNWARN_NOGUEST_S (seconds).
Thresholds EnvThresholds() {
    Thresholds t;
    if (const char* v = std::getenv("BB_PARTY_CONNWARN_UDP_N")) t.guest_udp_n = std::max(1, std::atoi(v));
    if (const char* v = std::getenv("BB_PARTY_CONNWARN_NOGUEST_S")) t.no_guest_s = std::max(1, std::atoi(v));
    return t;
}
Tracker g_tracker(EnvThresholds());
}  // namespace

void SetFirewallQuery(FirewallQuery q) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_query = std::move(q);
}

FwReport QueryFirewall(const std::string& exe, int port) {
    FirewallQuery q;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        q = g_query;
    }
    return q ? q(exe, port) : QueryFirewallWindows(exe, port);
}

// ---- texts ----
std::string FirewallText(FwState s, int port) {
    const std::string p = std::to_string(port);
    const char* why = s == FwState::Blocked ? "Windows Firewall blocks your party" : "Windows Firewall may block your party";
    return std::string(why) +
           "\nGuests cannot join until the game is allowed: Windows Security > Firewall > Allow an app through firewall, "
           "tick the game for Private and Public (UDP and TCP " + p + "), or accept the Windows prompt when it asks.";
}
std::string NoGuestsText(int port) {
    const std::string p = std::to_string(port);
    return "Nobody has reached your party yet\nGuests probably cannot reach this PC: your router has no open port. Turn UPnP "
           "on in the router, or forward UDP and TCP " + p + " to this PC, or play over a VPN (Radmin, ZeroTier). Also check "
           "that Windows Firewall allows the game.";
}
std::string GuestUdpText(const std::string& guest, int port) {
    const std::string p = std::to_string(port);
    return (guest.empty() ? std::string("A guest") : guest) +
           " is in the party but its game is not heard\nAllow the game in Windows Firewall on both PCs (UDP and TCP " + p +
           "). On a VPN both players set BB_PARTY_LOCAL_IP to their own VPN address.";
}
std::string HostGoneText(const std::string& host_addr) {
    return "Cannot reach the host" + (host_addr.empty() ? std::string() : " at " + host_addr) +
           "\nCheck that the host is hosting, that you are on the same VPN network, and that the host's firewall lets the "
           "game through.";
}
std::string OwnSummonText(int port) {
    const std::string p = std::to_string(port);
    return "The host cannot hear your game\nAllow the game in Windows Firewall on both PCs (UDP and TCP " + p +
           "). On a VPN both players set BB_PARTY_LOCAL_IP to their own VPN address.";
}

// ---- tracker ----
bool Tracker::Due(const std::string& key, double now) const {
    for (const Shown& s : shown_)
        if (s.key == key) return now - s.at >= t_.repeat_s;
    return true;
}
void Tracker::Mark(const std::string& key, double now) {
    for (Shown& s : shown_)
        if (s.key == key) {
            s.at = now;
            return;
        }
    shown_.push_back({key, now});
}

std::string Tracker::FirewallVerdict(const FwReport& r, int port, double now) {
    if (r.state != FwState::Blocked && r.state != FwState::Missing) return {};
    if (!Due("fw", now)) return {};
    Mark("fw", now);
    return FirewallText(r.state, port);
}

void Tracker::HostStarted(bool port_open, bool vpn, double now) {
    hosting_ = true;
    port_open_ = port_open;
    vpn_ = vpn;
    host_start_ = now;
}
void Tracker::GuestOnLink() {
    guest_seen_ = true;
}
std::string Tracker::HostTick(double now, int port) {
    if (!hosting_ || guest_seen_ || port_open_ || vpn_) return {};
    if (now - host_start_ < t_.no_guest_s) return {};
    if (!Due("noguests", now)) return {};
    Mark("noguests", now);
    return NoGuestsText(port);
}

std::string Tracker::GuestUdpUnheard(const std::string& guest, int port, double now) {
    Count* c = nullptr;
    for (Count& x : udp_fails_)
        if (x.who == guest) c = &x;
    if (!c) {
        udp_fails_.push_back({guest, 0});
        c = &udp_fails_.back();
    }
    if (++c->n < t_.guest_udp_n) return {};
    const std::string key = "udp:" + guest;
    if (!Due(key, now)) return {};
    c->n = 0;
    Mark(key, now);
    return GuestUdpText(guest, port);
}
void Tracker::GuestUdpFine(const std::string& guest) {
    for (Count& x : udp_fails_)
        if (x.who == guest) x.n = 0;
}

std::string Tracker::HostLost(const std::string& host_addr, double now) {
    if (ever_connected_) return {};
    if (++lost_n_ < t_.host_lost_n) return {};
    if (!Due("hostgone", now)) return {};
    lost_n_ = 0;
    Mark("hostgone", now);
    return HostGoneText(host_addr);
}
void Tracker::HostConnected() {
    ever_connected_ = true;
    lost_n_ = 0;
}

std::string Tracker::OwnSummonFailed(int port, double now) {
    if (++own_n_ < t_.own_summon_n) return {};
    if (!Due("ownsummon", now)) return {};
    own_n_ = 0;
    Mark("ownsummon", now);
    return OwnSummonText(port);
}
void Tracker::OwnSummonJoined() {
    own_n_ = 0;
}

// ---- glue ----
namespace {
double NowS() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
bool On() {
    bool (*e)();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        e = g_enabled;
    }
    return !e || e();
}
void Raise(const std::string& text) {
    if (text.empty()) return;
    void (*lg)(const char*);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        lg = g_log;
    }
    if (lg) {
        std::string line = "connection warning: " + text;
        std::replace(line.begin(), line.end(), '\n', ' ');
        lg(line.c_str());
    }
    status::SetAlert(text);
}
template <class F>
std::string Locked(F f) {
    std::lock_guard<std::mutex> lk(g_mu);
    return f(g_tracker);
}
}  // namespace

void SetEnabledProvider(bool (*enabled)()) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_enabled = enabled;
}
void SetLogger(void (*log)(const char*)) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_log = log;
}

void HostCheckFirewall(int port) {
    if (!On()) return;
    std::thread([port] {
        std::string exe;
#ifdef _WIN32
        char buf[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
        exe.assign(buf, n);
#endif
        const FwReport r = QueryFirewall(exe, port);
        void (*lg)(const char*);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            lg = g_log;
        }
        if (lg) {
            const char* st = r.state == FwState::Allowed   ? "allowed"
                             : r.state == FwState::Blocked ? "blocked"
                             : r.state == FwState::Missing ? "no allow rule"
                             : r.state == FwState::Off     ? "firewall off (another product may manage it)"
                                                           : "unknown";
            const std::string line = std::string("firewall check: ") + st + " (" + r.detail + ")";
            lg(line.c_str());
        }
        if (!On()) return;
        Raise(Locked([&](Tracker& t) { return t.FirewallVerdict(r, port, NowS()); }));
    }).detach();
}
void HostStarted(bool port_open, bool vpn) {
    Locked([&](Tracker& t) {
        t.HostStarted(port_open, vpn, NowS());
        return std::string();
    });
}
void HostTick(int port) {
    if (!On()) return;
    Raise(Locked([&](Tracker& t) { return t.HostTick(NowS(), port); }));
}
void HostGuestOnLink() {
    Locked([&](Tracker& t) {
        t.GuestOnLink();
        return std::string();
    });
}
void HostGuestUdpUnheard(const std::string& guest, int port) {
    if (!On()) return;
    Raise(Locked([&](Tracker& t) { return t.GuestUdpUnheard(guest, port, NowS()); }));
}
void HostGuestUdpFine(const std::string& guest) {
    Locked([&](Tracker& t) {
        t.GuestUdpFine(guest);
        return std::string();
    });
}
void GuestHostLost(const std::string& host_addr) {
    if (!On()) return;
    Raise(Locked([&](Tracker& t) { return t.HostLost(host_addr, NowS()); }));
}
void GuestHostConnected() {
    Locked([&](Tracker& t) {
        t.HostConnected();
        return std::string();
    });
}
void GuestOwnSummonFailed(int port) {
    if (!On()) return;
    Raise(Locked([&](Tracker& t) { return t.OwnSummonFailed(port, NowS()); }));
}
void GuestOwnSummonJoined() {
    Locked([&](Tracker& t) {
        t.OwnSummonJoined();
        return std::string();
    });
}
void ResetForTest() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_tracker = Tracker(EnvThresholds());
    g_query = nullptr;
    g_enabled = nullptr;
    g_log = nullptr;
}

}  // namespace party::connwarn
