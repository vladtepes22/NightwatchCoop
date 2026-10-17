// SPDX-License-Identifier: GPL-3.0-or-later
// C5 spectate target selection (gpu/shim/party/party_spectate.h).
// Build and run: ninja -C out/gpu party-spectate-test && out/gpu/party-spectate-test.exe
#include "party/party_spectate.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

int main() {
    const int none[1] = {0};
    CHECK(SpectateTarget(none, 0, -1, false) == -1);   // nobody alive
    CHECK(SpectateTarget(nullptr, 3, 2, true) == -1);
    const int one[] = {0};                              // a guest following the host
    CHECK(SpectateTarget(one, 1, -1, false) == 0);     // first pick
    CHECK(SpectateTarget(one, 1, 0, false) == 0);      // stays
    CHECK(SpectateTarget(one, 1, 0, true) == 0);       // a switch with one member keeps it
    const int three[] = {0, 2, 3};
    CHECK(SpectateTarget(three, 3, -1, false) == 0);
    CHECK(SpectateTarget(three, 3, 0, false) == 0);
    CHECK(SpectateTarget(three, 3, 0, true) == 2);     // Cross: next member
    CHECK(SpectateTarget(three, 3, 2, true) == 3);
    CHECK(SpectateTarget(three, 3, 3, true) == 0);     // wraps
    // The followed member died (slot 1 no longer living): the next one after it, no Cross needed.
    CHECK(SpectateTarget(three, 3, 1, false) == 2);
    CHECK(SpectateTarget(three, 3, 1, true) == 2);
    // Followed slot 4 left; nothing after it: wrap to the first.
    CHECK(SpectateTarget(three, 3, 4, false) == 0);
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-spectate-test: ok\n");
    return 0;
}
