// SPDX-License-Identifier: GPL-3.0-or-later
// The open party's rule: who may join a passwordless host by plain address (party_open_rules.h).
#include "party/party_open_rules.h"

#include <cassert>
#include <cstdio>

using namespace party::open_party;

static constexpr std::uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}
static constexpr std::uint32_t mask(int bits) { return bits ? 0xffffffffu << (32 - bits) : 0; }

int main() {
    // A password always decides; no switch means code only.
    assert(ScopeOf(true, true, true) == Scope::None);
    assert(ScopeOf(false, false, false) == Scope::None);
    assert(ScopeOf(false, true, false) == Scope::Tunnel);
    assert(ScopeOf(false, false, true) == Scope::Local);
    assert(ScopeOf(false, true, true) == Scope::Local);

    const Adapter wifi{ip(192, 168, 1, 20), mask(24), ip(192, 168, 1, 1), false};
    const Adapter vpn{ip(10, 8, 0, 7), mask(24), 0, true};
    const Adapter radmin{ip(26, 10, 20, 30), mask(8), ip(26, 0, 0, 1), false};
    const Adapter tailscale{ip(100, 70, 1, 2), mask(10), 0, false};
    const std::vector<Adapter> all{wifi, vpn, radmin, tailscale};

    // Local: every private or VPN subnet of this PC.
    assert(PeerAllowed(Scope::Local, ip(192, 168, 1, 50), all, false));
    assert(PeerAllowed(Scope::Local, ip(10, 8, 0, 9), all, false));
    assert(PeerAllowed(Scope::Local, ip(26, 99, 1, 1), all, false));
    assert(PeerAllowed(Scope::Local, ip(100, 90, 3, 4), all, false));
    // Tunnel: only the Nightwatch VPN's subnet.
    assert(PeerAllowed(Scope::Tunnel, ip(10, 8, 0, 9), all, false));
    assert(!PeerAllowed(Scope::Tunnel, ip(192, 168, 1, 50), all, false));
    assert(!PeerAllowed(Scope::Tunnel, ip(26, 99, 1, 1), all, false));
    // No scope: nobody.
    assert(!PeerAllowed(Scope::None, ip(192, 168, 1, 50), all, false));

    // Private but on no adapter of ours, and the internet.
    assert(!PeerAllowed(Scope::Local, ip(192, 168, 2, 50), all, false));
    assert(!PeerAllowed(Scope::Local, ip(10, 8, 1, 9), all, false));
    assert(!PeerAllowed(Scope::Local, ip(8, 8, 8, 8), all, false));
    assert(!PeerAllowed(Scope::Local, ip(172, 32, 0, 1), all, false));  // just past 172.16/12
    // The gateway can present forwarded internet players as itself; our own address is not a player.
    assert(!PeerAllowed(Scope::Local, ip(192, 168, 1, 1), all, false));
    assert(!PeerAllowed(Scope::Local, ip(26, 0, 0, 1), all, false));
    assert(!PeerAllowed(Scope::Local, ip(192, 168, 1, 20), all, false));

    // A PC with a public address on the adapter shares that subnet with strangers.
    const std::vector<Adapter> modem{{ip(84, 10, 20, 30), mask(24), ip(84, 10, 20, 1), false}};
    assert(!PeerAllowed(Scope::Local, ip(84, 10, 20, 31), modem, false));
    // A private peer does not get in through a public adapter that happens to cover it.
    const std::vector<Adapter> wide{{ip(84, 10, 20, 30), mask(1), 0, false}};
    assert(!PeerAllowed(Scope::Local, ip(10, 1, 2, 3), wide, false));
    // An unknown mask is not a subnet.
    const std::vector<Adapter> no_mask{{ip(192, 168, 1, 20), 0, 0, false}};
    assert(!PeerAllowed(Scope::Local, ip(192, 168, 1, 50), no_mask, false));

    // IPv6 peers and relayed guests arrive as a 240/4 key.
    assert(!PeerAllowed(Scope::Local, ip(240, 1, 2, 3), all, false));
    assert(!PeerAllowed(Scope::Local, ip(255, 255, 255, 255), all, false));

    // Loopback: only in a loopback test.
    assert(!PeerAllowed(Scope::Local, ip(127, 0, 0, 1), all, false));
    assert(PeerAllowed(Scope::Local, ip(127, 0, 0, 1), all, true));
    assert(!PeerAllowed(Scope::None, ip(127, 0, 0, 1), all, true));

    std::printf("party-open-test: ok\n");
    return 0;
}
