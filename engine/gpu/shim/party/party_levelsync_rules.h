// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.5 level sync and phantom health (the pure part, tests/test_party_levelsync.cpp; the game side is
// party_balance.cpp LevelSyncTick and seamless_rules.cpp ApplyPhantomHp; docs/party/balance.md section 5).
// 0.7.6 "Full health in PvP" (PhantomCutOn, HpRatioKeeper; docs/party/pvp.md).
//
// Bloodborne's own rules for a hunter who joins another's world:
//  - Level sync. A per-frame ChrIns step (0x15077c0) puts SpEffect 9200 (0x23f0, the blue icon by the
//    name) on a player chr whose level is above `hostLevel * mult + add`, and takes it off when it is
//    not; the rates then multiply max HP (0x17e46e0, CalcCorrectGraph 120), max stamina (0x17e4a20, 124),
//    defence (0x17e76b0, 121) and the poison / blood / curse resistances (0x17e8110, 122 / 123) by
//    curve(host level * mult + add) / curve(own level) (0x1582940). `mult` is 1 + pct / 100 and `add` the
//    matching ranges of SprjNetworkClientMan +0x18 (+0x8e4 / +0x8e0), 20 % and 20 without it.
//  - Phantom health. SpEffects 9006 (cooperator) / 9026 (invader phantom) carry maxHpRate 0.7.
#pragma once

#include <cstdint>

namespace coop::levelsync {

constexpr int kLevelSyncEffect = 9200;
constexpr float kDefaultRangePercent = 20.0f;  ///< 0x492622c, used while SprjNetworkClientMan +0x18 is null
constexpr float kDefaultRangeAdd = 20.0f;

/// 1 + pct / 100 (the game's `pct / 100 + 1`); `have` false: the default range.
inline float RangeMultiplier(bool have, std::uint32_t pct) {
    return (have ? static_cast<float>(pct) : kDefaultRangePercent) / 100.0f + 1.0f;
}
inline float RangeAdd(bool have, std::uint32_t add) {
    return have ? static_cast<float>(add) : kDefaultRangeAdd;
}
/// The game's test (0x15077c0): `(float)host * mult + add < (float)level`.
inline bool AboveRange(int level, int host_level, float mult, float add) {
    return static_cast<float>(host_level) * mult + add < static_cast<float>(level);
}

enum class Action { None, Apply, Remove };
/// What to do with SpEffect 9200 on one chr. `rule_on` the host's Level sync rule; `eligible` the chr is a
/// player other than the host (the game's step only syncs those); `above` AboveRange.
/// Rule off: the effect always goes. Rule on: it follows `above` (the game's own step, made certain).
/// `count` is how many 9200 effects the chr carries: the game's own step and ours may both have added
/// one, and two would square the scaling, so extras go too.
inline Action Decide(bool rule_on, bool eligible, bool above, int count) {
    if (!rule_on) return count > 0 ? Action::Remove : Action::None;
    const bool want = eligible && above;
    if (want && count == 0) return Action::Apply;
    if ((!want && count > 0) || (want && count > 1)) return Action::Remove;
    return Action::None;
}

/// SpEffect 9006 / 9026 maxHpRate: the game's 0.7 with the rule on, 1.0 off.
/// The matching range this port runs the sync with: its game has no matching server, so
/// SprjNetworkClientMan +0x18 holds an unlimited placeholder (+0 %, +1000 levels, live 2026-10-24) and
/// no hunter would ever be above the range. A party uses the game's own normal range.
constexpr std::uint32_t kPartyRangePercent = 20, kPartyRangeAdd = 20;

inline float PhantomHpRate(bool rule_on) { return rule_on ? 0.7f : 1.0f; }

/// 0.7.6 (pvphp, owner's decision): the host's rule "Full health in PvP" (pvp_full_hp, default on). While
/// friendly fire / PvP is on, a joined hunter keeps its full maximum health: a 70 % hunter against a 100 %
/// host is no duel. The cut in force on every peer: the Phantom health rule, unless PvP is on and this is.
inline bool PhantomCutOn(bool phantom_hp, bool friendly_fire, bool pvp_full_hp) {
    return phantom_hp && !(friendly_fire && pvp_full_hp);
}

/// The HP that is the same fraction of `new_max` as `hp` of `max`, to the nearest point (half up): a hunter
/// at 245/490 goes to 350/700 and back, full stays full (490/490 -> 700/700, no 490/700), 0 stays 0 (no
/// free heal for a dead hunter). A `max` of 0 or less, or `hp` above it, counts as full.
inline int HpAtRatio(int hp, int max, int new_max) {
    if (new_max <= 0 || hp <= 0) return 0;
    if (max <= 0 || hp >= max) return new_max;
    const long long scaled = (static_cast<long long>(hp) * new_max * 2 + max) / (2LL * max);
    return static_cast<int>(scaled > new_max ? new_max : scaled);
}

/// Keeps the local hunter's HP fraction across the max-HP change a maxHpRate write brings. The game itself
/// adds the max difference when the max grows (245/490 becomes 455/700, 65 %; live 2026-10-25: 260/503 ->
/// 476/719) and keeps the points when it shrinks (700/700 is clamped to 490/490, 350/700 stays 350/490, 70 %):
/// a switch would heal or hurt. Arm with the HP before the write; Track with the HP
/// every frame after it: when the max changes, Track gives the HP to write (the last HP seen with the old
/// max, at the same fraction of the new one) and disarms; a max that never changes (the host's own chr has
/// no phantom effect; a reload) disarms after `kWindow` s. Pure (tests/test_party_levelsync.cpp).
struct HpRatioKeeper {
    static constexpr double kWindow = 5.0;
    bool armed = false;
    int hp = -1, max = -1;
    double since = 0.0;
    void Arm(int cur_hp, int cur_max, double now) {
        armed = cur_max > 0;
        hp = cur_hp;
        max = cur_max;
        since = now;
    }
    /// -1: nothing to write (not armed, no change yet, the window passed); else the HP for the new max.
    int Track(int cur_hp, int cur_max, double now) {
        if (!armed) return -1;
        if (now - since > kWindow) {
            armed = false;
            return -1;
        }
        if (cur_max <= 0) return -1;
        if (cur_max != max) {
            armed = false;
            return HpAtRatio(hp, max, cur_max);
        }
        hp = cur_hp;
        return -1;
    }
};

}  // namespace coop::levelsync
