// visy 0.8.1 loading speed-up rules (gpu/shim/bbport_load_policy.h; his tests/test_fast_loading.cpp without the
// unpaced loading screen, which we do not take). ninja -C out/gpu load-policy-test
#include "bbport_load_policy.h"

#include <cassert>
#include <cstdio>

int main() {
    using namespace bbport::loadpolicy;
    // Load threads: min(4, max(3, CPUs - 2)), never below the game's 3, 0 when switched off.
    assert(DefaultWorkers(16, true) == 4 && DefaultWorkers(8, true) == 4 && DefaultWorkers(6, true) == 4);
    assert(DefaultWorkers(5, true) == 3 && DefaultWorkers(4, true) == 3 && DefaultWorkers(2, true) == 3);
    assert(DefaultWorkers(1, true) == 3 && DefaultWorkers(0, true) == 3);
    assert(DefaultWorkers(16, false) == 0);
    assert(ParseWorkers(nullptr, 4) == 4 && ParseWorkers("", 4) == 4);
    assert(ParseWorkers("0", 4) == 0 && ParseWorkers("off", 4) == 0 && ParseWorkers("false", 4) == 0);
    assert(ParseWorkers("6", 4) == 6 && ParseWorkers("99", 4) == 8 && ParseWorkers("-2", 4) == 0);

    // Fades twice as fast: the extra countdown after the game's own step, never to 0, never from a jump.
    assert(FadeExtra(0.5f, 0.4f, 2.0f, 0.25f) > 0.099f && FadeExtra(0.5f, 0.4f, 2.0f, 0.25f) < 0.101f);
    assert(FadeExtra(0.5f, 0.5f, 2.0f, 0.25f) == 0.0f);      // not moving
    assert(FadeExtra(0.0f, 0.5f, 2.0f, 0.25f) == 0.0f);      // a new fade started (prev 0)
    assert(FadeExtra(0.2f, 0.5f, 2.0f, 0.25f) == 0.0f);      // counting up
    assert(FadeExtra(0.9f, 0.3f, 2.0f, 0.25f) == 0.0f);      // a jump, not the game's step
    assert(FadeExtra(0.05f, 0.04f, 2.0f, 0.25f) <= 0.04f * 0.9f + 1e-6f && FadeExtra(0.05f, 0.04f, 2.0f, 0.25f) > 0.0f);
    assert(FadeExtra(0.5f, 0.4f, 1.0f, 0.25f) == 0.0f);      // factor 1 = off
    assert(FadeExtra(0.5f, 0.4f, 3.0f, 0.25f) > 0.19f);      // other factors work the same way
    std::puts("load-policy-test: ok");
    return 0;
}
