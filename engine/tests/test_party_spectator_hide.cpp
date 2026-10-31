// SPDX-License-Identifier: GPL-3.0-or-later
// Spectator hide rule (gpu/shim/party/party_spectator_hide.h).
// Build and run: ninja -C out/gpu party-spectator-hide-test && out/gpu/party-spectator-hide-test.exe
#include "party/party_spectator_hide.h"

#include <cstdio>

using coop::spectator::Act;
using coop::spectator::HideTracker;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

int main() {
    // A real spectator body: HP 0 for longer than the dwell -> hidden once, then left alone.
    {
        HideTracker t;
        CHECK(t.Step(0x10, 0, 100.0, true) == Act::None);
        CHECK(t.Step(0x10, 0, 100.5, true) == Act::None);
        CHECK(t.Step(0x10, 0, 101.6, true) == Act::Hide);
        CHECK(t.Step(0x10, 0, 102.0, true) == Act::None);
        CHECK(t.hidden.count(0x10) == 1);
    }
    // A copy built a moment ago reads HP 0 for a frame, then its state arrives: never hidden.
    {
        HideTracker t;
        CHECK(t.Step(0x20, 0, 100.0, true) == Act::None);
        CHECK(t.Step(0x20, 594, 100.1, true) == Act::None);
        CHECK(t.Step(0x20, 0, 100.2, true) == Act::None);  // a second blip restarts the dwell
        CHECK(t.Step(0x20, 0, 101.5, true) == Act::None);
        CHECK(t.hidden.empty());
    }
    // A hidden copy that reads HP > 0 (revived, or hidden by mistake) is shown again.
    {
        HideTracker t;
        t.Step(0x30, 0, 0.0, true);
        CHECK(t.Step(0x30, 0, 2.0, true) == Act::Hide);
        CHECK(t.Step(0x30, 500, 3.0, true) == Act::Show);
        CHECK(t.hidden.empty());
        CHECK(t.Step(0x30, 500, 4.0, true) == Act::None);
    }
    // No limbo window: nothing is hidden, and a dwell started earlier does not carry over.
    {
        HideTracker t;
        CHECK(t.Step(0x40, 0, 0.0, true) == Act::None);
        CHECK(t.Step(0x40, 0, 5.0, false) == Act::None);
        CHECK(t.Step(0x40, 0, 5.1, true) == Act::None);  // starts again at 5.1
        CHECK(t.Step(0x40, 0, 6.0, true) == Act::None);
        CHECK(t.Step(0x40, 0, 6.7, true) == Act::Hide);
    }
    // A load frees the copies; the address may come back as a living member's new chr.
    {
        HideTracker t;
        t.Step(0x50, 0, 0.0, true);
        t.Step(0x50, 0, 2.0, true);
        CHECK(t.hidden.count(0x50) == 1);
        t.Prune({0x99});
        CHECK(t.hidden.empty() && t.dead_since.empty());
        CHECK(t.Step(0x50, 0, 3.0, true) == Act::None);  // new chr at the same address at HP 0
        CHECK(t.Step(0x50, 594, 3.2, true) == Act::None);
    }
    {
        HideTracker t;
        t.Step(0x60, 0, 0.0, true);
        t.Step(0x60, 0, 2.0, true);
        t.Prune({0x60});
        CHECK(t.hidden.count(0x60) == 1);  // still there: kept
        t.Clear();
        CHECK(t.hidden.empty());
    }
    if (g_failures == 0) std::puts("party-spectator-hide-test: ok");
    return g_failures ? 1 : 0;
}
