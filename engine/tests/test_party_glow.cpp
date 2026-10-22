// SPDX-License-Identifier: GPL-3.0-or-later
// Companions without the phantom glow (gpu/shim/party/party_glow_rules.h): when the glow is off and
// which SFX id each glow row gets.
// Build and run: cmake --build out/gpu --target party-glow-test && out/gpu/party-glow-test.exe
#include "party/party_glow_rules.h"

#include <cassert>
#include <cstdio>

using namespace coop::glow;

static void Setting() {
    // The setting decides in party mode; on by default in BbSettings (glow removed).
    assert(GlowOff(true, nullptr, true));
    assert(!GlowOff(false, nullptr, true));
    assert(GlowOff(true, "", true));
    // BB_PARTY_GLOW_OFF overrides it (tests).
    assert(!GlowOff(true, "0", true));
    assert(GlowOff(false, "1", true));
    // Never outside party mode: single-player and the game's own co-op stay vanilla.
    assert(!GlowOff(true, nullptr, false));
    assert(!GlowOff(true, "1", false));
}

static void Rows() {
    // The two cooperator rows, with the 1.09 ids (checked against gameparam.parambnd.dcx).
    static_assert(sizeof(kGlowRows) / sizeof(kGlowRows[0]) == 2, "two glow rows");
    assert(kGlowRows[0].row == 272 && kGlowRows[0].sfx == 211);
    assert(kGlowRows[1].row == 276 && kGlowRows[1].sfx == 212);
    // Off: the game's id becomes "none"; on: put back; nothing to do when already so.
    assert(WantedSfx(211, 211, true) == kNoSfx);
    assert(WantedSfx(kNoSfx, 211, true) == kNoSfx);
    assert(WantedSfx(kNoSfx, 211, false) == 211);
    assert(WantedSfx(211, 211, false) == 211);
    // Another value (a mod's): never touched, either way.
    assert(WantedSfx(9999, 211, true) == 9999);
    assert(WantedSfx(9999, 211, false) == 9999);
}

static void LiveSwitch() {
    Refresh r;
    // The first state (the game's start): learned, no pause.
    assert(r.Update(true, 0.0) == kEffectSfx);
    assert(r.Update(true, 5.0) == kEffectSfx);
    // A switch: the rows pause for kRefreshS, then play again (from the new id).
    assert(r.Update(false, 10.0) == kEffectPaused);
    assert(r.Update(false, 10.0 + kRefreshS / 2) == kEffectPaused);
    assert(r.Update(false, 10.0 + kRefreshS) == kEffectSfx);
    assert(r.Update(false, 20.0) == kEffectSfx);
    // And back.
    assert(r.Update(true, 30.0) == kEffectPaused);
    assert(r.Update(true, 31.0) == kEffectSfx);
    // A switch during a pause starts it again.
    assert(r.Update(false, 40.0) == kEffectPaused);
    assert(r.Update(true, 40.2) == kEffectPaused);
    assert(r.Update(true, 40.2 + kRefreshS - 0.01) == kEffectPaused);
    assert(r.Update(true, 40.2 + kRefreshS) == kEffectSfx);
    // The paused value is none of the player's cases that do something (0 SFX, 1 / 2, 3 a stop).
    static_assert(kEffectPaused > 3 && kEffectSfx == 0, "effect types");
}

int main() {
    Setting();
    Rows();
    LiveSwitch();
    std::printf("party glow: ok\n");
    return 0;
}
