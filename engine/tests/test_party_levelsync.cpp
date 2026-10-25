// SPDX-License-Identifier: GPL-3.0-or-later
// Level sync and phantom health (gpu/shim/party/party_levelsync_rules.h): the game's range test, what a
// rule does to SpEffect 9200 and to 9006 / 9026; 0.7.6 Full health in PvP (the cut in force, the HP fraction kept).
// Build and run: ninja -C out/gpu party-levelsync-test && out/party-levelsync-test.exe
#include "party/party_levelsync_rules.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

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

    // 0.7.6 Full health in PvP: the cut goes while friendly fire is on and the rule is; the rule alone or
    // friendly fire alone change nothing; the Phantom health rule off is off whatever PvP does.
    assert(PhantomCutOn(true, false, true) && PhantomCutOn(true, false, false) && PhantomCutOn(true, true, false));
    assert(!PhantomCutOn(true, true, true));
    assert(!PhantomCutOn(false, false, true) && !PhantomCutOn(false, true, true) && !PhantomCutOn(false, true, false));

    // The HP fraction across a max change: full stays full, half stays half, 0 stays 0, both ways.
    assert(HpAtRatio(490, 490, 700) == 700 && HpAtRatio(700, 700, 490) == 490);
    assert(HpAtRatio(245, 490, 700) == 350 && HpAtRatio(350, 700, 490) == 245);
    assert(HpAtRatio(0, 490, 700) == 0 && HpAtRatio(0, 700, 490) == 0);
    assert(HpAtRatio(1, 490, 700) == 1 && HpAtRatio(1, 700, 490) == 1);  // 1.43 -> 1; 0.7 -> 1 (alive stays alive)
    // Rounding: to the nearest point, half up; never above the new max.
    assert(HpAtRatio(233, 700, 300) == 100 && HpAtRatio(234, 700, 300) == 100 && HpAtRatio(235, 700, 300) == 101);  // 99.86, 100.29, 100.71
    assert(HpAtRatio(1, 2, 3) == 2);          // 1.5 -> 2
    assert(HpAtRatio(1, 4, 2) == 1);          // 0.5 -> 1
    assert(HpAtRatio(237, 474, 677) == 339);  // level sync + cut (474) -> level sync alone (677): 338.5
    assert(HpAtRatio(339, 677, 474) == 237);  // and back: 237.3
    assert(HpAtRatio(600, 500, 700) == 700);  // above the max: full
    assert(HpAtRatio(300, 0, 700) == 700 && HpAtRatio(300, -1, 700) == 700);  // no old max: full
    assert(HpAtRatio(300, 500, 0) == 0);      // no new max: nothing to give
    for (int max : {490, 700, 719, 1}) {  // the identity: the same max keeps every HP
        for (int hp = 0; hp <= max; hp += 7) assert(HpAtRatio(hp, max, max) == hp);
    }
    // A round trip is lossless at the ends and within a point in the middle.
    for (int hp = 0; hp <= 490; ++hp) {
        const int up = HpAtRatio(hp, 490, 700), back = HpAtRatio(up, 700, 490);
        assert(back >= hp - 1 && back <= hp + 1 && (hp != 0 || up == 0) && (hp != 490 || up == 700));
    }

    // The keeper: armed with the HP before the rate write; the game first keeps the points (490/700), the
    // keeper gives the HP at the old fraction and disarms.
    {
        HpRatioKeeper k;
        assert(k.Track(490, 490, 0.0) == -1);  // not armed: nothing
        k.Arm(490, 490, 1.0);
        assert(k.armed);
        assert(k.Track(490, 490, 1.02) == -1);  // the max has not changed yet
        assert(k.Track(490, 700, 1.05) == 700 && !k.armed);
        assert(k.Track(700, 700, 1.1) == -1);
        // PvP off again: the game clamps 700/700 to 490/490; the keeper's answer is the same 490.
        k.Arm(700, 700, 2.0);
        assert(k.Track(490, 490, 2.03) == 490 && !k.armed);
        // Half health, both ways; damage taken between the arm and the change counts (the last HP seen).
        k.Arm(350, 700, 3.0);
        assert(k.Track(340, 700, 3.01) == -1);
        assert(k.Track(340, 490, 3.03) == 238);  // 340 / 700 of 490 = 238
        k.Arm(238, 490, 4.0);
        assert(k.Track(238, 700, 4.02) == 340);
        // Dead: stays dead.
        k.Arm(0, 490, 5.0);
        assert(k.Track(0, 700, 5.02) == 0);
        // A max that never changes (the host's own chr): disarmed after the window, nothing written.
        k.Arm(600, 719, 6.0);
        assert(k.Track(600, 719, 8.0) == -1 && k.armed);
        assert(k.Track(600, 719, 6.0 + HpRatioKeeper::kWindow + 0.1) == -1 && !k.armed);
        assert(k.Track(600, 800, 12.0) == -1);  // a later change is the game's own business
        // No chr yet (a load): a max of 0 is skipped, the window still runs.
        k.Arm(490, 490, 20.0);
        assert(k.Track(0, 0, 20.5) == -1 && k.armed);
        assert(k.Track(490, 700, 21.0) == 700);
        // Armed with no max (no chr): never writes.
        k.Arm(0, 0, 30.0);
        assert(!k.armed && k.Track(490, 700, 30.1) == -1);
        // Level sync coming on in the same window (474 = 677 * 0.7): the fraction is kept across whatever
        // the max became.
        k.Arm(503, 503, 40.0);
        assert(k.Track(503, 474, 40.02) == 474);
        k.Arm(474, 474, 41.0);
        assert(k.Track(474, 677, 41.02) == 677);
    }
    std::printf("party-levelsync-test: ok\n");
    return 0;
}
