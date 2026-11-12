// SPDX-License-Identifier: GPL-3.0-or-later
// 0.8.1 (CL-4a20f7): the opt-in direct path between two Nightwatch VPN party members (docs/party/nightwatch_vpn.md,
// "Allow direct connection"). The rules are net/party_nw_direct_rules.h (pure, unit tested); this module drives them:
//  - both members of a pair must have the setting on (bbport.ini party_nw_direct, env BB_PARTY_NW_DIRECT=0/1 over it,
//    off by default); the flag, the public candidate and every decision travel as the party link's "nwd" EVENT
//    (never the lobby); a guest's message for another guest is forwarded by the host, which stamps the sender's tunnel
//    address from the link itself;
//  - only once the link is up over the tunnel: the candidate is gathered (UPnP first, as a mapping opens the port;
//    STUN when UPnP gives no public address; existing code) when the first
//    opted-in peer is known, ~20 probes run over BOTH the tunnel and the candidate for ~3 s (the game stays on the
//    tunnel), and the pair goes direct only if every direct probe answered and it beats the tunnel by 20 ms / 30 %;
//  - the switch happens once, on both sides, at a loading screen or before the world (rules safe_moment), as an
//    endpoint alias in the net layer (the game keeps the peer's tunnel address; PeerPaths pins the pair, so no relay
//    switching): from then on that one path only. A direct path silent for 2 s goes back to the tunnel for good.
// One log line per pair: "direct path: tunnel 92 ms, direct 18 ms -> direct" / "-> tunnel (loss 3/20)" /
// "-> tunnel (not opted in)". Never an IP in the log.
#pragma once

#include <functional>
#include <string>

#include "party/party_link.h"

namespace party::nwdirect {

constexpr const char* kEventName = "nwd";

struct Env {
    std::function<PartyLink*()> link;
    std::function<void(const std::string&)> log;
    std::function<bool()> stopping;
    std::function<int()> local_state;  // party::MemberState as int
};
// Starts the worker (idle until a link is up and the setting is on).
void Configure(Env env);
// A party event: true when it was ours ("nwd").
bool OnEvent(bool host, int slot, const std::string& name, const std::string& body);
void Stop();
// The setting (ini + env), for the tests and the status.
bool Enabled();

}  // namespace party::nwdirect
