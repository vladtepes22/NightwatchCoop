// SPDX-License-Identifier: GPL-3.0-or-later
// 0.8.1 (CL-4a20f7): opt-in direct path between two Nightwatch VPN party members. The rules, pure (no network,
// no game access), unit-tested in tests/test_nw_direct.cpp.
//
// Since 0.7.8 (CL-79fe9c) a Nightwatch-mode pair is reached on the tunnel only (one path per pair, always: the
// mixed-path 4-5 s gaps, NW-80b250f). This adds ONE sticky exception: when BOTH members opted in
// (party_nw_direct), the party link is up, and a short probe of the peer's direct (WAN) candidate answered
// every time and was clearly faster than the tunnel, the pair switches to the direct path once, at a load
// screen, both sides together. A direct path that then loses packets for kLostMs goes back to the tunnel for
// the rest of the session. Never per packet, never back and forth.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bbnet::nwd {

constexpr int kProbes = 20;           // probes per path in the test window
constexpr int kProbeIntervalMs = 150; // 20 x 150 ms = 3 s
constexpr int kReplyGraceMs = 700;    // the last probe's answer may take this long
constexpr int kKeepaliveMs = 250;     // direct path keepalive probes after the switch
constexpr int kLostMs = 2000;         // direct path silent this long: back to the tunnel (sticky)
constexpr int kMarginMs = 20;         // direct must be faster by this many ms ...
constexpr double kMarginFrac = 0.30;  // ... or by this fraction of the tunnel RTT

// How one path did in the probe window.
struct PathStats {
    int sent = 0;
    int answered = 0;
    std::vector<double> rtt_ms;  // one per answered probe
    int lost() const { return sent - answered; }
};

inline double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

enum class Verdict : std::uint8_t {
    Direct,       // switch
    SlowerTunnel, // the tunnel is as fast or faster than the margin allows
    Loss,         // a probe on the direct path went unanswered
    NoProbes,     // nothing was measured (no tunnel answer to compare with)
    PeerDeclined, // our probes were fine, the other side's were not (it stays on the tunnel, so do we)
    NoCandidate,  // a side could not learn its public address (no STUN / UPnP answer)
};

// Every direct probe must have answered (a lossy direct path is never taken), and the direct RTT must beat the
// tunnel RTT by >= kMarginMs or >= kMarginFrac of it. The tunnel must have answered at least once (else there
// is nothing to compare; a tunnel that answers nothing is not a reason to leave it unmeasured).
inline Verdict judge(const PathStats& tunnel, const PathStats& direct, double* tunnel_ms, double* direct_ms) {
    const double t = median(tunnel.rtt_ms), d = median(direct.rtt_ms);
    if (tunnel_ms) *tunnel_ms = t;
    if (direct_ms) *direct_ms = d;
    if (direct.sent < kProbes || tunnel.sent < kProbes) return Verdict::NoProbes;
    if (direct.answered < direct.sent) return Verdict::Loss;
    if (tunnel.answered == 0) return Verdict::NoProbes;
    const double gain = t - d;
    if (gain >= kMarginMs || gain >= kMarginFrac * t) return Verdict::Direct;
    return Verdict::SlowerTunnel;
}

// The one log line per pair. `peer` is a slot label (never an IP).
inline std::string log_line(Verdict v, double tunnel_ms, double direct_ms, int direct_lost, int direct_sent) {
    char b[160];
    switch (v) {
    case Verdict::Direct:
        std::snprintf(b, sizeof(b), "direct path: tunnel %.0f ms, direct %.0f ms -> direct", tunnel_ms, direct_ms);
        break;
    case Verdict::SlowerTunnel:
        std::snprintf(b, sizeof(b), "direct path: tunnel %.0f ms, direct %.0f ms -> tunnel", tunnel_ms, direct_ms);
        break;
    case Verdict::Loss:
        if (direct_sent > 0 && direct_lost >= direct_sent) {  // nothing came back: there is no direct time to show
            std::snprintf(b, sizeof(b), "direct path: tunnel %.0f ms, direct no answer -> tunnel (loss %d/%d)",
                          tunnel_ms, direct_lost, direct_sent);
            break;
        }
        std::snprintf(b, sizeof(b), "direct path: tunnel %.0f ms, direct %.0f ms -> tunnel (loss %d/%d)", tunnel_ms,
                      direct_ms, direct_lost, direct_sent);
        break;
    case Verdict::PeerDeclined:
        std::snprintf(b, sizeof(b), "direct path: tunnel %.0f ms, direct %.0f ms -> tunnel (peer declined)", tunnel_ms,
                      direct_ms);
        break;
    case Verdict::NoCandidate:
        std::snprintf(b, sizeof(b), "direct path: -> tunnel (no public address)");
        break;
    default:
        std::snprintf(b, sizeof(b), "direct path: not measured -> tunnel");
        break;
    }
    return b;
}
inline std::string not_opted_line() { return "direct path: -> tunnel (not opted in)"; }

// Pair opt-in: both members must have it on.
inline bool both_opted(bool local_on, bool peer_on) { return local_on && peer_on; }

// Where a pair is. Terminal states never change within a session: Tunnel (decided or fell back) is final,
// Direct only ends in Tunnel (never back).
enum class Phase : std::uint8_t {
    Idle,      // nothing known yet
    Wait,      // opted in here, waiting for the peer's flag / candidate
    Probing,   // both candidates known, the 3 s window is running
    Decided,   // verdict reached, waiting for the other side / a safe moment
    Direct,    // switched
    Tunnel,    // final: stays on the tunnel (not opted in, verdict, loss, fell back)
};

// A pair's sticky machine. Pure: the caller feeds events and reads the phase.
class Pair {
public:
    Phase phase() const { return phase_; }
    bool direct() const { return phase_ == Phase::Direct; }
    bool final_tunnel() const { return phase_ == Phase::Tunnel; }

    // Both flags are known (the peer's arrived). Not both on -> final tunnel.
    void flags(bool local_on, bool peer_on) {
        if (phase_ == Phase::Tunnel || phase_ == Phase::Direct) return;
        if (!both_opted(local_on, peer_on)) {
            phase_ = Phase::Tunnel;
            return;
        }
        if (phase_ == Phase::Idle) phase_ = Phase::Wait;
    }
    void start_probing() {
        if (phase_ == Phase::Wait) phase_ = Phase::Probing;
    }
    // The probe window ended with this verdict; `peer_ok`: the other side's own verdict was Direct.
    // Returns true when both agree on Direct (then Decided, awaiting the safe moment).
    bool decide(Verdict mine, bool peer_ok) {
        if (phase_ != Phase::Probing) return false;
        if (mine == Verdict::Direct && peer_ok) {
            phase_ = Phase::Decided;
            return true;
        }
        phase_ = Phase::Tunnel;
        return false;
    }
    // The safe moment came and both sides confirmed: switch (once).
    bool switch_now(bool safe_moment) {
        if (phase_ != Phase::Decided || !safe_moment) return false;
        phase_ = Phase::Direct;
        last_heard_ms_ = 0;
        return true;
    }
    // The direct path answered (a keepalive ack) at `now_ms`.
    void heard(std::int64_t now_ms) { last_heard_ms_ = now_ms; }
    // Called while Direct: silent for kLostMs -> back to the tunnel for good. `since_ms`: when we switched.
    bool lost(std::int64_t now_ms, std::int64_t since_ms) {
        if (phase_ != Phase::Direct) return false;
        const std::int64_t ref = last_heard_ms_ > since_ms ? last_heard_ms_ : since_ms;
        if (now_ms - ref < kLostMs) return false;
        phase_ = Phase::Tunnel;
        return true;
    }
    // The peer said it fell back / refused: follow, final.
    void peer_fell_back() {
        phase_ = Phase::Tunnel;
    }

private:
    Phase phase_ = Phase::Idle;
    std::int64_t last_heard_ms_ = 0;
};

// The pair is at a moment where changing its path is safe: either side is on a loading screen, or no guest of
// the pair is playing in the host's world (a guest in its own world / title / joining has no game traffic with
// the host yet; the host itself is "in the world" whenever it plays, which says nothing about the pair).
// State values follow party::MemberState: Title 0, Home 1, Joining 2, InHostWorld 3, Dead 4, Loading 5, Prologue 6.
// A member in the host's world: InHostWorld or Dead, and not the host (slot 0).
inline bool in_play(int state, bool is_host) { return !is_host && (state == 3 || state == 4); }
inline bool safe_pair(int local_state, bool local_is_host, int peer_state, bool peer_is_host) {
    if (local_state == 5 || peer_state == 5) return true;
    return !in_play(local_state, local_is_host) && !in_play(peer_state, peer_is_host);
}

// Switch probe / ack datagram: fe 'bbnd' type tag seq(4). type 0 request, 1 ack. tag 0 = tunnel path, 1 = direct.
constexpr std::size_t kProbeLen = 12;
inline bool encode_probe(std::uint8_t* out, std::size_t cap, std::uint8_t type, std::uint8_t tag, std::uint32_t seq) {
    if (cap < kProbeLen) return false;
    out[0] = 0xfe; out[1] = 'b'; out[2] = 'b'; out[3] = 'n'; out[4] = 'd';
    out[5] = type; out[6] = tag; out[7] = 0;
    out[8] = static_cast<std::uint8_t>(seq >> 24); out[9] = static_cast<std::uint8_t>(seq >> 16);
    out[10] = static_cast<std::uint8_t>(seq >> 8); out[11] = static_cast<std::uint8_t>(seq);
    return true;
}
inline bool decode_probe(const std::uint8_t* d, std::size_t n, std::uint8_t* type, std::uint8_t* tag,
                         std::uint32_t* seq) {
    if (n != kProbeLen || d[0] != 0xfe || d[1] != 'b' || d[2] != 'b' || d[3] != 'n' || d[4] != 'd') return false;
    if (d[5] > 1 || d[6] > 1) return false;
    *type = d[5];
    *tag = d[6];
    *seq = (static_cast<std::uint32_t>(d[8]) << 24) | (static_cast<std::uint32_t>(d[9]) << 16) |
           (static_cast<std::uint32_t>(d[10]) << 8) | d[11];
    return true;
}

// A candidate the peer sent is acceptable only when it is a public-looking IPv4 (network order irrelevant here:
// host order a.b.c.d): not 0/8, 10/8, 127/8, 169.254/16, 172.16/12, 192.168/16, 100.64/10, multicast or above.
// `allow_loopback`: test runs only.
inline bool candidate_ok(std::uint32_t ip, std::uint16_t port, bool allow_loopback) {
    if (!ip || port < 1024) return false;
    const std::uint32_t a = ip >> 24, b = (ip >> 16) & 0xff;
    if (a == 127) return allow_loopback;
    if (a == 0 || a == 10 || a >= 224) return false;
    if (a == 169 && b == 254) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && b == 168) return false;
    if (a == 100 && b >= 64 && b <= 127) return false;
    return true;
}

}  // namespace bbnet::nwd
