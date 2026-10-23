// SPDX-License-Identifier: GPL-3.0-or-later
// Build and run: ninja -C out/gpu party-collision-test && out/gpu/party-collision-test.exe
#include "party/party_collision.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    std::vector<std::uint64_t> queries;
    int classifications = 0;
    bool remote = false;
    auto is_remote = [&](std::uint64_t) { ++classifications; return remote; };
    auto is_live = [&](std::uint64_t hit) { queries.push_back(hit); return hit == 0x12340000; };

    assert(!coop::collision::ClearStaleHits(0, is_remote, is_live));
    assert(classifications == 0 && queries.empty());
    // Even an unreadable character address must be ignored before accessing collision fields.
    assert(!coop::collision::ClearStaleHits(1, is_remote, is_live));
    assert(classifications == 1 && queries.empty());

    std::array<std::uint64_t, 0x358 / 8> chr{};
    const auto address = reinterpret_cast<std::uint64_t>(chr.data());
    auto& primary = chr[0x288 / 8];
    auto& secondary = chr[0x350 / 8];
    primary = 1;
    secondary = 2;
    assert(!coop::collision::ClearStaleHits(address, is_remote, is_live));
    assert(primary == 1 && secondary == 2 && queries.empty());

    remote = true;
    primary = 0;
    secondary = 0;
    assert(!coop::collision::ClearStaleHits(address, is_remote, is_live));
    assert(queries.empty());
    primary = 0x12340000;
    secondary = 0x12340000;
    assert(!coop::collision::ClearStaleHits(address, is_remote, is_live));
    assert(primary == 0x12340000 && secondary == 0x12340000 && queries.size() == 2);

    queries.clear();
    secondary = 1; // an intentionally unreadable stale record
    assert(coop::collision::ClearStaleHits(address, is_remote, is_live));
    assert(primary == 0x12340000 && secondary == 0 && queries.size() == 2);
    queries.clear();
    primary = 2;
    secondary = 3;
    assert(coop::collision::ClearStaleHits(address, is_remote, is_live));
    assert(primary == 0 && secondary == 0 && queries.size() == 2);
    assert(queries[0] == 2 && queries[1] == 3);
    std::puts("PASS: local/NPC exclusion, empty/live hits, stale remote collision safety, one query per hit");
}
