// SPDX-License-Identifier: GPL-3.0-or-later
// F2 (0.7.1): which path the guest's next party link attempt takes (docs/party/network_robustness.md
// 4a). No sockets: party_runtime applies the step to the party port's route table (party_route.h)
// and tells the link; unit-tested by party-nat-test.
//
// A round tries, in order: IPv6 (when both PCs have a global IPv6 address: TCP to the host's IPv4
// port first, then the reliable UDP stream over IPv6) and Direct (TCP, then UDP over IPv4: straight
// in or hole punched). Member relays (a member's endpoint we reach) join the round once the link
// has been down for relay_after_ms (the host may still find and punch us meanwhile), after the
// direct steps. A step that connected is kept for the link's reconnects; stick_failures failed
// reconnects on it start the rounds again.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace party::netpath {

using Clock = std::chrono::steady_clock;

enum class Route : std::uint8_t { Direct, Ipv6, Relay };
const char* route_name(Route r);  // "direct", "ipv6", "relay"

struct Step {
    Route route = Route::Direct;
    int relay = -1;  // Relay: the candidate's index
    bool operator==(const Step& o) const { return route == o.route && relay == o.relay; }
};

struct Candidate {
    std::uint32_t addr = 0;  // network order
    std::uint16_t port = 0;  // network order
    std::string label;       // where it came from ("guest code", "DHT", "BB_PARTY_RELAY_VIA")
    int failures = 0;
};

class Chooser {
public:
    struct Config {
        int relay_after_ms = 20000;
        int stick_failures = 2;
        std::size_t max_relays = 8;
    };
    Chooser() = default;
    explicit Chooser(Config c) : cfg_(c) {}

    // The link starts trying (or tries again after a Leave / Rejoin): the rounds from the top.
    void start(Clock::time_point now);
    // Both PCs have a global IPv6 address (ours and the party code's): the IPv6 step is in the round.
    void set_ipv6(bool available);
    bool ipv6() const { return ipv6_; }
    // A member endpoint we may relay through; true when it is new. Our own host is never one.
    bool add_relay(std::uint32_t addr, std::uint16_t port, const std::string& label);
    const std::vector<Candidate>& relays() const { return relays_; }

    Step current() const { return cur_; }
    // The current step's attempt failed (the link is reconnecting without having connected):
    // the next step.
    Step failed(Clock::time_point now);
    // The link connected on the current step: kept for reconnects.
    void connected();
    bool stuck() const { return stuck_; }
    // A connected link was lost: reconnects stay on its step for stick_failures attempts.
    void lost();

    // "IPv6", "UDP over IPv4 (straight in or punched)", "member relay via 1.2.3.4:9307 (guest code)".
    std::string describe(const Step& s) const;

private:
    std::vector<Step> round(Clock::time_point now) const;
    Config cfg_;
    bool ipv6_ = false;
    std::vector<Candidate> relays_;
    Step cur_;
    std::size_t pos_ = 0;
    Clock::time_point start_{};
    bool stuck_ = false;
    int stick_left_ = 0;
};

// The host's Internet code carries its IPv6 address ("+6" tail) only with IPv6 on, an IPv6 socket
// on the party port, a global address and an IPv4 port that is not open (UPnP, forward or public
// IP: then the code is exactly the 0.7.0 one).
inline bool code_carries_ipv6(bool ipv6_on, bool ipv6_socket, bool have_address, bool v4_port_open) {
    return ipv6_on && ipv6_socket && have_address && !v4_port_open;
}

}  // namespace party::netpath
