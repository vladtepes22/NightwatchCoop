// SPDX-License-Identifier: GPL-3.0-or-later
#include "party/party_netpath.h"

#include <cstdio>

namespace party::netpath {

const char* route_name(Route r) {
    switch (r) {
    case Route::Direct: return "direct";
    case Route::Ipv6: return "ipv6";
    case Route::Relay: return "relay";
    }
    return "?";
}

void Chooser::start(Clock::time_point now) {
    start_ = now;
    pos_ = 0;
    stuck_ = false;
    stick_left_ = 0;
    const std::vector<Step> r = round(now);
    cur_ = r.empty() ? Step{} : r.front();
}

void Chooser::set_ipv6(bool available) {
    if (ipv6_ == available) return;
    ipv6_ = available;
    if (!stuck_ && pos_ == 0) cur_ = round(start_).front();  // not tried yet: the round's first step
}

bool Chooser::add_relay(std::uint32_t addr, std::uint16_t port, const std::string& label) {
    if (!addr || !port) return false;
    for (const Candidate& c : relays_)
        if (c.addr == addr && c.port == port) return false;
    if (relays_.size() >= cfg_.max_relays) return false;
    relays_.push_back({addr, port, label, 0});
    return true;
}

std::vector<Step> Chooser::round(Clock::time_point now) const {
    std::vector<Step> r;
    if (ipv6_) r.push_back({Route::Ipv6, -1});
    r.push_back({Route::Direct, -1});
    if (now - start_ >= std::chrono::milliseconds(cfg_.relay_after_ms))
        for (std::size_t i = 0; i < relays_.size(); ++i) r.push_back({Route::Relay, static_cast<int>(i)});
    return r;
}

Step Chooser::failed(Clock::time_point now) {
    if (cur_.route == Route::Relay && cur_.relay >= 0 && cur_.relay < static_cast<int>(relays_.size()))
        ++relays_[static_cast<std::size_t>(cur_.relay)].failures;
    if (stuck_) {
        if (--stick_left_ > 0) return cur_;
        stuck_ = false;  // the step that worked does not any more: the rounds again
        pos_ = 0;
        start_ = now;
        cur_ = round(now).front();
        return cur_;
    }
    const std::vector<Step> r = round(now);
    std::size_t at = 0;
    for (std::size_t i = 0; i < r.size(); ++i)
        if (r[i] == cur_) at = i;
    pos_ = (at + 1) % r.size();
    cur_ = r[pos_];
    return cur_;
}

void Chooser::connected() {
    stuck_ = true;
    stick_left_ = cfg_.stick_failures;
    if (cur_.route == Route::Relay && cur_.relay >= 0 && cur_.relay < static_cast<int>(relays_.size()))
        relays_[static_cast<std::size_t>(cur_.relay)].failures = 0;
}

void Chooser::lost() {
    if (stuck_) stick_left_ = cfg_.stick_failures;
}

std::string Chooser::describe(const Step& s) const {
    switch (s.route) {
    case Route::Direct: return "TCP / UDP over IPv4";
    case Route::Ipv6: return "IPv6";
    case Route::Relay: {
        if (s.relay < 0 || s.relay >= static_cast<int>(relays_.size())) return "member relay";
        const Candidate& c = relays_[static_cast<std::size_t>(s.relay)];
        const auto* b = reinterpret_cast<const std::uint8_t*>(&c.addr);
        char buf[96];
        std::snprintf(buf, sizeof buf, "member relay via %u.%u.%u.%u:%u (%s)", b[0], b[1], b[2], b[3],
                      static_cast<unsigned>(static_cast<std::uint16_t>((c.port << 8) | (c.port >> 8))), c.label.c_str());
        return buf;
    }
    }
    return "?";
}

}  // namespace party::netpath
