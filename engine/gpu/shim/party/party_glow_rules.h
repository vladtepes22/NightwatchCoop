// SPDX-License-Identifier: GPL-3.0-or-later
// Companions without the phantom glow (0.7.2): the pure rules, header only, no game dependency
// (unit test: tests/test_party_glow.cpp, ninja -C out/gpu party-glow-test).
//
// A summoned guest is a white phantom (ChrIns +0x78 = 1) and carries SpEffect 9006 (maxHpRate 0.7;
// 9026 in the other cooperator descriptor). That row has useSpEffectEffect set, so the chr's effect
// update (0x1A50CC0 -> 0x1A520B0, every frame) looks its stateInfo up as a SpEffectVfxParam row and
// plays that row's midst SFX on the chr: row 272 (9006) = SFX 211 on dummy poly 203, row 276
// (9026) = SFX 212: the blue, glowing phantom look, on every screen the guest is seen on (its own
// too). RE: run 1 of uinotice (2026-10-22): the guest's chr type set to 0 or the host's map hit
// applied to it changed nothing; the SpEffect lists showed 9006 on the guest only.
//
// The fix edits only the two SFX ids of this game's SpEffectVfxParam (-1: no SFX), never the
// SpEffect rows: the guest keeps its type, team, 9006 and its HP rule, so the game still counts it
// as a cooperator. It is a display setting of each player (bbport.ini party_glow_off, default on):
// it changes only what this game draws, for every phantom it shows. Off puts the game's ids back.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace coop::glow {

/// A SpEffectVfxParam row that draws the phantom glow: its id and the game's (1.09) midstSfxId.
struct GlowRow {
    std::uint32_t row;
    std::int32_t sfx;
};
inline constexpr GlowRow kGlowRows[] = {
    {272, 211},  // SpEffect 9006 (cooperator: maxHpRate 0.7, stateInfo 272)
    {276, 212},  // SpEffect 9026 (the other cooperator descriptor: stateInfo 276)
};
inline constexpr std::int32_t kNoSfx = -1;  ///< the param's "none" (the vfx player skips < 0)

/// Whether the glow is removed now: the setting, unless BB_PARTY_GLOW_OFF says 0 / 1 (tests), and
/// only in party mode (single-player and the game's own co-op stay as in the original game).
inline bool GlowOff(bool setting, const char* env, bool party_mode) {
    if (!party_mode) return false;
    if (env && *env) return env[0] != '0';
    return setting;
}

/// The midstSfxId to write to a glow row holding `current`: `vanilla` or kNoSfx as `off` wants;
/// `current` itself (nothing to write) when it already is that, or when it is neither (another
/// mod's value: left alone).
inline std::int32_t WantedSfx(std::int32_t current, std::int32_t vanilla, bool off) {
    if (current != vanilla && current != kNoSfx) return current;
    return off ? kNoSfx : vanilla;
}

// A live switch. The vfx player keeps one effect entry per row on each chr (0x18998F0, chr +0x2A0,
// keyed by the stateInfo) and only marks it alive each frame: an entry made with "no SFX" never
// starts the SFX when the id comes back, and one made with the SFX keeps it (run 2 of uinotice:
// the switch back on showed no glow). So for a moment after a switch the rows' effectType (+0x2A,
// the game's 0: the midst SFX) is kEffectPaused, which the player skips: the game ends the entries
// it no longer marks, and the next frames make them again from the new id.
inline constexpr std::uint8_t kEffectSfx = 0;
inline constexpr std::uint8_t kEffectPaused = 4;  ///< no case in 0x1A520B0's switch (3 is a stop)
inline constexpr double kRefreshS = 0.4;

class Refresh {
public:
    /// Each tick: the effectType the glow rows hold now (kEffectPaused for kRefreshS after `off`
    /// changed; the first call only learns the state: nothing plays before it).
    std::uint8_t Update(bool off, double now) {
        if (!seen_) {
            seen_ = true;
            off_ = off;
        } else if (off != off_) {
            off_ = off;
            until_ = now + kRefreshS;
        }
        return now < until_ ? kEffectPaused : kEffectSfx;
    }
private:
    bool seen_ = false, off_ = false;
    double until_ = -1e9;
};

}  // namespace coop::glow
