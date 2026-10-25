// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.5 level sync and phantom health (the pure part, tests/test_party_levelsync.cpp; the game side is
// party_balance.cpp LevelSyncTick and seamless_rules.cpp PhantomHpTick; docs/party/balance.md section 5).
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

}  // namespace coop::levelsync
