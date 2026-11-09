// SPDX-License-Identifier: GPL-3.0-or-later
// Ported idea: droogie/bbhost 8ebaf88 (GPL-3.0-or-later): a connection that died is never
// announced ESTABLISHED / ACTIVE to the game afterwards (DEAD always is). bbport adds the link:
// a connection whose peer has been silent on the party link longer than a threshold is dead too.
// Our own code. Pure logic, unit-tested in tests/test_np_sig_alive.cpp.
#pragma once

#include <cstdint>
#include <map>

namespace bbnet::np::sigalive {

constexpr std::uint16_t kEvDead = 0x00;

// A queued signaling event: ESTABLISHED / ACTIVE only for a connection that still exists and is
// live; DEAD always.
inline bool Deliver(std::uint16_t event, bool exists, bool dead) { return event == kEvDead || (exists && !dead); }

// ms of silence at which a connection counts as dead. The F5 outage replay hears a silent peer's
// keepalive for up to 120 s and the member slot is kept 130 s, so while the party link says
// "outage" (replay on) the limit is 3 minutes (the C6 outage window); outside it 60 s (the C6
// crash window), well past the transport's own 25 s idle timeout.
constexpr std::int64_t kSilentMs = 60000;
constexpr std::int64_t kSilentOutageMs = 180000;

inline std::int64_t LimitMs(bool outage, std::int64_t override_ms) {
    if (override_ms > 0) return outage ? override_ms * 3 : override_ms;
    return outage ? kSilentOutageMs : kSilentMs;
}

// age_ms < 0: never heard (a peer that just joined): alive.
inline bool LinkDead(std::int64_t age_ms, bool outage, std::int64_t override_ms = 0) {
    return age_ms >= 0 && age_ms > LimitMs(outage, override_ms);
}

// Age of the peer's source among `ages` (key addr | port_nbo << 32, ms since last heard):
// the exact addr+port; else the only source of that addr; else -1 (unknown, alive).
inline std::int64_t SourceAge(const std::map<std::uint64_t, std::int64_t>& ages, std::uint32_t addr_nbo,
                              std::uint16_t port_nbo) {
    const std::uint64_t key = addr_nbo | (static_cast<std::uint64_t>(port_nbo) << 32);
    if (auto it = ages.find(key); it != ages.end()) return it->second;
    std::int64_t age = -1;
    int n = 0;
    for (const auto& [k, a] : ages) {
        if (static_cast<std::uint32_t>(k & 0xffffffffu) != addr_nbo) continue;
        age = a;
        ++n;
    }
    return n == 1 ? age : -1;
}

}  // namespace bbnet::np::sigalive
