// SPDX-License-Identifier: GPL-3.0-or-later
// When the port shows a remote player chr itself (gpu/shim/party/party_remote_show_rules.h): never
// before its member's first data (the game draws it then).
// Build and run: ninja -C out/gpu party-remote-show-test && out/gpu/party-remote-show-test.exe
#include "party/party_remote_show_rules.h"

#include <cstdio>

using namespace coop::remote_show;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

int main() {
    // As logged live (restartdraw run1 / boldmark run2): STEP_Create's chr before the data.
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const float origin_up[3] = {-0.2f, 0.3f, 0.2f};
    const float origin_high[3] = {0.1f, 0.6f, -0.4f};
    CHECK(AwaitsFirstData(1, origin));
    CHECK(AwaitsFirstData(1, origin_up));
    CHECK(AwaitsFirstData(1, origin_high));
    CHECK(AwaitsFirstData(-1, origin));  // HP unread
    // After the data: the member's HP and position.
    const float there[3] = {17.1f, -9.5f, 101.8f};
    CHECK(!AwaitsFirstData(719, there));
    CHECK(!AwaitsFirstData(719, origin));  // a healthy member at a map origin
    CHECK(!AwaitsFirstData(1, there));     // a member at 1 HP somewhere else
    const float near_x[3] = {1.2f, 0.0f, 0.0f}, near_z[3] = {0.0f, 0.0f, -1.5f};
    CHECK(!AwaitsFirstData(1, near_x));
    CHECK(!AwaitsFirstData(1, near_z));

    // The safety net: only with the data, without the draw bit, after 4 s.
    CHECK(!SafetyNetShows(true, false, 100.0));  // before the data: never
    CHECK(!SafetyNetShows(false, true, 100.0));
    CHECK(!SafetyNetShows(false, false, 3.9));
    CHECK(SafetyNetShows(false, false, kSafetyNetSeconds));
    CHECK(SafetyNetShows(false, false, 10.0));

    if (g_failures) {
        std::fprintf(stderr, "party-remote-show-test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-remote-show-test: ok\n");
    return 0;
}
