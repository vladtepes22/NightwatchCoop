// SPDX-License-Identifier: GPL-3.0-or-later
// C6 crash rejoin: which host game connections belong to a crashed member (party_nr_drop.h).
// Build and run: ninja -C out/gpu party-nr-drop-test && out/gpu/party-nr-drop-test.exe
#include "party/party_nr_drop.h"

#include <cstdio>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using namespace coop::nrdrop;

static std::uint64_t Key(std::uint32_t addr, std::uint16_t port_nbo) {
    return addr | (static_cast<std::uint64_t>(port_nbo) << 32);
}

int main() {
    const std::uint32_t lo = 0x0100007f;  // 127.0.0.1, network order
    const std::uint16_t dead = 0x29bb, alive = 0x2abb, old = 0x2bbb;
    std::map<std::uint64_t, std::int64_t> heard = {
        {Key(lo, dead), 10000},   // silent 2 s at 12000
        {Key(lo, alive), 11900},  // heard 0.1 s ago
        {Key(lo, old), -50000},  // a minute+ ago: not a connection any more
    };
    std::vector<Source> s = SilentSources(heard, 12000, 1500, 60000);
    CHECK(s.size() == 1);
    CHECK(s.size() == 1 && s[0].addr == lo && s[0].port_nbo == dead);
    CHECK(SilentSources(heard, 11000, 1500, 60000).empty());  // too soon after the crash
    CHECK(SilentSources({}, 12000, 1500, 60000).empty());

    // The connection's key as seen live (conn +0x10): zero, then 127.0.0.1 and port 0xbb29 in
    // host order, vport 30.
    std::uint8_t key[kKeySize] = {};
    const std::uint32_t lo_host = 0x7f000001;
    const std::uint16_t dead_host = 0xbb29, vport = 30;
    std::memcpy(key + 0x10, &lo_host, 4);
    std::memcpy(key + 0x18, &dead_host, 2);
    std::memcpy(key + 0x1a, &vport, 2);
    CHECK(KeyMatches(key, s[0]));
    CHECK(KeyMatchesAny(key, s));
    Source other{lo, alive};
    CHECK(!KeyMatches(key, other));
    Source other_ip{0x0200007f, dead};
    CHECK(!KeyMatches(key, other_ip));
    std::uint8_t empty[kKeySize] = {};
    CHECK(!KeyMatches(empty, Source{}));  // a free connection never matches
    CHECK(!KeyMatchesAny(key, {}));

    {  // 0.8.0 (08c): silent sources without a connection are the orphans
        std::vector<Source> sil{Source{0x0100007f, 1}, Source{0x0100007f, 2}, Source{0x0100007f, 3}};
        CHECK(Orphans(sil, {true, true, true}).empty());
        CHECK(Orphans(sil, {true, false, true}).size() == 1 && Orphans(sil, {true, false, true})[0].port_nbo == 2);
        CHECK(Orphans(sil, {}).size() == 3 && Orphans({}, {}).empty());
    }
    {  // 0.8.0 (08c): the host's address is never in the set to release
        std::vector<Source> sil{Source{0x0100007f, 1}, Source{0x0100007f, 2}}, keep{Source{0x0100007f, 1}};
        CHECK(Without(sil, keep).size() == 1 && Without(sil, keep)[0].port_nbo == 2);
        CHECK(Without(sil, {}).size() == 2 && Without({}, keep).empty());
        CHECK(Without(sil, {Source{0x0200007f, 2}}).size() == 2);  // same port, other address: kept in the set
    }
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
