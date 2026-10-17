// C6 crash rejoin: which of the host game's transport connections belong to a crashed member.
// Pure logic (no game memory), unit-tested in tests/test_party_nr_drop.cpp.
//
// The host's game keeps its NexusRevolution connection to a crashed guest until the
// connection's own idle timeout (~25 s), and a restarted guest summoned before that never gets
// the summon data (docs/party/join_timing.md). The transport (0x138 bytes, vtable 0x52c51e0 for
// udpp2p) has a connection pool at +0xd0 (count +0x6c, 0xc0 bytes each); a connection is in use
// while its byte +0x2c is set and keys on the peer's address (FUN_0271a420): live (2026-10-12),
// +0x10..+0x1f zero, +0x20 the IPv4 address (host order), +0x28 the port (host order), +0x2a the
// vport.
// The transport's release (vtable +0x28, FUN_02716a40) unhashes it, resets it and calls the
// owner's close callback: the game's own path for a connection that ended.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace coop::nrdrop {

struct Source {
    std::uint32_t addr = 0;     // network order
    std::uint16_t port_nbo = 0; // network order
};

// Sources heard within `recent_ms` before `now_ms` but not in the last `silent_ms`
// (heard: addr | port << 32 -> ms, as bbnet::p2p_heard returns it).
inline std::vector<Source> SilentSources(const std::map<std::uint64_t, std::int64_t>& heard, std::int64_t now_ms,
                                         std::int64_t silent_ms, std::int64_t recent_ms) {
    std::vector<Source> out;
    for (const auto& [key, at] : heard) {
        const std::int64_t age = now_ms - at;
        if (age < silent_ms || age > recent_ms) continue;
        Source s;
        s.addr = static_cast<std::uint32_t>(key & 0xffffffffu);
        s.port_nbo = static_cast<std::uint16_t>(key >> 32);
        out.push_back(s);
    }
    return out;
}

// A connection's key: the kKeySize bytes from its +0x10 (addr at key +0x10, port at key +0x18,
// both host order) names `s`.
constexpr std::size_t kKeySize = 0x1c;
inline bool KeyMatches(const std::uint8_t* key, const Source& s) {
    std::uint16_t port = 0;
    std::uint32_t addr = 0;
    std::memcpy(&port, key + 0x18, 2);
    std::memcpy(&addr, key + 0x10, 4);
    const std::uint32_t want_addr = ((s.addr & 0xffu) << 24) | ((s.addr & 0xff00u) << 8) | ((s.addr >> 8) & 0xff00u) |
                                    (s.addr >> 24);
    const std::uint16_t want_port = static_cast<std::uint16_t>((s.port_nbo >> 8) | ((s.port_nbo & 0xff) << 8));
    return port == want_port && addr == want_addr && (s.addr != 0 || s.port_nbo != 0);
}

inline bool KeyMatchesAny(const std::uint8_t* key16, const std::vector<Source>& v) {
    for (const Source& s : v)
        if (KeyMatches(key16, s)) return true;
    return false;
}

}  // namespace coop::nrdrop
