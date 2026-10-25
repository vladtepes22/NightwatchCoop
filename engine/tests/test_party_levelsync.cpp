// SPDX-License-Identifier: GPL-3.0-or-later
// Level sync and phantom health (gpu/shim/party/party_levelsync_rules.h): the game's range test, what a
// rule does to SpEffect 9200 and to 9006 / 9026.
// Build and run: ninja -C out/gpu party-levelsync-test && out/party-levelsync-test.exe
#include "party/party_levelsync_rules.h"

#include <cassert>
#include <cstdio>

using namespace coop::levelsync;

int main() {
    // The default range (no matching parameters): host * 1.2 + 20.
    const float m = RangeMultiplier(false, 0), a = RangeAdd(false, 0);
    assert(m > 1.199f && m < 1.201f && a == 20.0f);
    // The matching parameters' own: +10 % and +5 (pct, add).
    assert(RangeMultiplier(true, 10) > 1.099f && RangeMultiplier(true, 10) < 1.101f && RangeAdd(true, 5) == 5.0f);
    // Host level 37: 37 * 1.2 + 20 = 64.4; 64 is within, 65 above.
    assert(!AboveRange(64, 37, m, a) && AboveRange(65, 37, m, a));
    assert(!AboveRange(10, 37, m, a) && !AboveRange(37, 37, m, a));
    assert(AboveRange(120, 37, m, a));

    // Rule on: 9200 follows the range for a player other than the host, never on the host, never twice.
    assert(Decide(true, true, true, 0) == Action::Apply);
    assert(Decide(true, true, true, 1) == Action::None);
    assert(Decide(true, true, true, 2) == Action::Remove);
    assert(Decide(true, true, false, 1) == Action::Remove);
    assert(Decide(true, true, false, 0) == Action::None);
    assert(Decide(true, false, true, 0) == Action::None);   // the host's own chr
    assert(Decide(true, false, true, 1) == Action::Remove);
    // Rule off: it always goes, and is never added.
    assert(Decide(false, true, true, 1) == Action::Remove);
    assert(Decide(false, true, true, 2) == Action::Remove);
    assert(Decide(false, true, true, 0) == Action::None);
    assert(Decide(false, true, false, 0) == Action::None);
    // The party's range is the game's normal one.
    assert(RangeMultiplier(true, kPartyRangePercent) == m && RangeAdd(true, kPartyRangeAdd) == a);

    // Phantom health: the game's 0.7 on, full off.
    assert(PhantomHpRate(true) == 0.7f && PhantomHpRate(false) == 1.0f);
    assert(kLevelSyncEffect == 0x23f0);
    std::printf("party-levelsync-test: ok\n");
    return 0;
}
