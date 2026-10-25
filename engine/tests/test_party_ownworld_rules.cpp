// SPDX-License-Identifier: GPL-3.0-or-later
// cmake --build out/gpu --target party-ownworld-rules-test && out/gpu/party-ownworld-rules-test
#include "party/party_ownworld_rules.h"

#include <cstdio>

using namespace coop::ownworld;

static int g_fail = 0;
#define CHECK(x)                                                     \
    do {                                                             \
        if (!(x)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

int main() {
    // The reports: an own-world load (flow 0) with a phantom's remembered type 1 is cleared.
    CHECK(ClearRememberedType(0, 1, -1));
    CHECK(ClearRememberedType(0, 2, -1));
    CHECK(ClearRememberedType(4, 1, -1));  // any non-summon flow state
    CHECK(ClearRememberedType(-1, 1, -1));  // offline: no network flow object (unreadable -> -1)
    // A summoned load keeps the guest's type (the host's world is built for a phantom).
    CHECK(!ClearRememberedType(kSummonFlow, 1, -1));
    // Already a normal character: nothing to do.
    CHECK(!ClearRememberedType(0, 0, -1));
    // The game's own pending type (copied over +0xA4 by the step) is left alone.
    CHECK(!ClearRememberedType(0, 1, 0));
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
