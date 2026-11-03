// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-hide HUD (0.7.8, CL-7d51fe): the rules, pure (no game access), unit-tested in tests/test_auto_hud.cpp.
//
// Modelled on Elden Ring's "HUD: Auto" (docs/auto_hud.md): the bars and equipment slots are hidden while nothing
// happens and come back, fading in quickly and out slowly, when something about the player changes or a fight is
// on. Elden Ring's documented behaviour is: Auto hides the HUD except the compass; the Y / triangle button shows
// it for a moment; the HP and FP bars return when damage is taken or spells are used, and in combat or locked on
// (the exact timers are not documented; the values below are ours). Mapped to Bloodborne:
//   - HP, stamina, quick-item slots, bullets, the Blood Echoes / Insight counters  = the game's "PlayerHUD" group;
//   - the boss bar, the lock-on cursor, prompts, subtitles, item notices, damage / "Prey Slaughtered" texts are not
//     part of it and are never touched.
#pragma once

#include <algorithm>
#include <cstdint>

namespace coop::autohud {

struct Inputs {
    bool valid = false;  // the world is up, the player is there, the HUD object was found
    int hp = 0, max_hp = 0;
    int stamina = 0, max_stamina = 0;
    int echoes = 0, insight = 0;
    std::uint64_t equip_hash = 0;  // a hash of the equipment block (weapons, armor, quick items, selected slots)
    bool lock_on = false;
    bool enemy_near = false;   // a living enemy within kEnemyRadiusM (not in the Hunter's Dream: its dummy is no threat)
    bool menu_open = false;    // an in-game menu / dialog is up
    double now = 0;            // seconds, monotonic
};

// What kept the HUD up (bit set), for logs and tests.
enum Reason : unsigned {
    kNone = 0,
    kEntered = 1u << 0,   // the world was just entered
    kHp = 1u << 1,        // HP changed (damage, healing, a level-up)
    kStamina = 1u << 2,   // stamina spent or regaining
    kRunes = 1u << 3,     // Blood Echoes / Insight changed
    kEquip = 1u << 4,     // equipment or the selected quick item changed
    kLock = 1u << 5,      // locked on
    kEnemy = 1u << 6,     // an enemy is close
    kMenu = 1u << 7,      // a menu is open
    kLowHp = 1u << 8,     // HP is low
    kPeek = 1u << 9,      // the player asked to see it (test / hotkey)
};

struct Tuning {
    double hold_hp = 5.0;        // s kept after HP changed
    double hold_stamina = 2.0;   // s after stamina last changed (it changes every frame while regaining)
    double hold_runes = 4.0;
    double hold_equip = 4.0;
    double hold_combat = 6.0;    // s after the lock-on / the nearest enemy goes
    double hold_menu = 1.0;
    double hold_enter = 4.0;     // after entering the world
    double fade_in_s = 0.2;      // hidden -> shown
    double fade_out_s = 1.0;     // shown -> hidden
    double low_hp_fraction = 0.5;
    double min_alpha = 0.0;      // the hidden level (0 = gone)
};

inline constexpr double kEnemyRadiusM = 8.0;

class Machine {
public:
    explicit Machine(const Tuning& t = Tuning()) : t_(t) {}
    void Reset() { *this = Machine(t_); }
    /// One step. Returns the HUD's alpha (min_alpha .. 1); reason() says what is holding it up.
    double Update(const Inputs& in) {
        if (!in.valid) {  // nothing to judge: leave it as it is and start over when the world is back
            have_prev_ = false;
            return alpha_;
        }
        unsigned why = kNone;
        double until = until_;
        const auto hold = [&](unsigned r, double secs) {
            why |= r;
            until = std::max(until, in.now + secs);
        };
        if (!have_prev_) {
            hold(kEntered, t_.hold_enter);
        } else {
            if (in.hp != prev_.hp || in.max_hp != prev_.max_hp) hold(kHp, t_.hold_hp);
            if (in.stamina != prev_.stamina || in.max_stamina != prev_.max_stamina) hold(kStamina, t_.hold_stamina);
            if (in.echoes != prev_.echoes || in.insight != prev_.insight) hold(kRunes, t_.hold_runes);
            if (in.equip_hash != prev_.equip_hash) hold(kEquip, t_.hold_equip);
        }
        if (in.lock_on) hold(kLock, t_.hold_combat);
        if (in.enemy_near) hold(kEnemy, t_.hold_combat);
        if (in.menu_open) hold(kMenu, t_.hold_menu);
        if (in.max_hp > 0 && double(in.hp) < t_.low_hp_fraction * double(in.max_hp)) hold(kLowHp, 0.5);
        if (peek_until_ > in.now) hold(kPeek, peek_until_ - in.now);
        prev_ = in;
        have_prev_ = true;
        until_ = until;
        const bool shown = in.now < until_;
        reason_ = shown ? (why ? why : lastWhy_) : kNone;
        if (why) lastWhy_ = why;
        const double dt = std::clamp(in.now - last_t_, 0.0, 0.5);
        last_t_ = in.now;
        const double target = shown ? 1.0 : t_.min_alpha;
        const double span = shown ? t_.fade_in_s : t_.fade_out_s;
        const double step = span > 0 ? dt / span : 1.0;
        if (alpha_ < target) alpha_ = std::min(target, alpha_ + step);
        else if (alpha_ > target) alpha_ = std::max(target, alpha_ - step * (1.0 - t_.min_alpha));
        return alpha_;
    }
    /// Shows the HUD for `secs` from `now` (the player's "look at it" request).
    void Peek(double now, double secs) { peek_until_ = now + secs; }
    double alpha() const { return alpha_; }
    unsigned reason() const { return reason_; }
    const Tuning& tuning() const { return t_; }

private:
    Tuning t_;
    Inputs prev_;
    bool have_prev_ = false;
    double until_ = 0, last_t_ = 0, peek_until_ = 0;
    double alpha_ = 1.0;
    unsigned reason_ = kNone, lastWhy_ = kNone;
};

/// A FNV-1a hash over `n` bytes (the equipment block).
inline std::uint64_t Hash(const void* data, std::size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

}  // namespace coop::autohud
