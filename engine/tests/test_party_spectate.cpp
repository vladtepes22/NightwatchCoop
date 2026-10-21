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

    // The carried spot: the first step jumps there, then it glides (no hop per tick).
    SpectateFollow f;
    bool snapped = false;
    const float a[3] = {10.0f, 0.0f, 0.0f};
    CHECK(f.Step(a, 1.0f / 60, 0.15f, 8.0f, &snapped) == 10.0f && snapped && f.at[0] == 10.0f);
    // The member rolls 3 m in one tick: the spot moves a fraction of it per 60 Hz frame.
    const float b[3] = {13.0f, 0.0f, 0.0f};
    const float step = f.Step(b, 1.0f / 60, 0.15f, 8.0f, &snapped);
    CHECK(!snapped && step > 0.2f && step < 0.4f);
    // ... and reaches it (within 2 cm) in about a second, whatever the frame rate.
    for (int i = 0; i < 60; ++i) f.Step(b, 1.0f / 60, 0.15f, 8.0f, &snapped);
    CHECK(std::fabs(f.at[0] - 13.0f) < 0.02f);
    SpectateFollow g;
    g.Step(a, 0.1f, 0.15f, 8.0f, &snapped);
    for (int i = 0; i < 30; ++i) g.Step(b, 1.0f / 30, 0.15f, 8.0f, &snapped);
    CHECK(std::fabs(g.at[0] - f.at[0]) < 0.02f);
    // A warp of the member (more than the snap distance): straight there.
    const float far[3] = {40.0f, 0.0f, 0.0f};
    CHECK(f.Step(far, 1.0f / 60, 0.15f, 8.0f, &snapped) > 8.0f && snapped && f.at[0] == 40.0f);
    // A zero time step does not move it.
    const float near[3] = {41.0f, 0.0f, 0.0f};
    CHECK(f.Step(near, 0.0f, 0.15f, 8.0f, &snapped) == 0.0f && !snapped && f.at[0] == 40.0f);

    // Diagnostics counters.
    SpectateCamStats st;
    st.Step(10.0f, true);   // a jump is not a step
    st.Step(0.3f, false);
    st.Step(0.1f, false);
    st.resets = 3;
    st.Camera(0.5f, 0.2f, 4.0f);
    st.Camera(0.1f, 0.4f, 2.0f);
    CHECK(st.ticks == 3 && st.snaps == 1 && st.max_step == 0.3f);
    CHECK(st.max_cam_move == 0.5f && st.max_cam_turn == 0.4f && st.min_cam_dist == 2.0f);
    CHECK(st.ResetsPerMinute(30.0) == 6.0f && st.ResetsPerMinute(0.0) == 0.0f);
    st.Reset();
    CHECK(st.ticks == 0 && st.resets == 0 && st.max_step == 0.0f);
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-spectate-test: ok\n");
    return 0;
}
