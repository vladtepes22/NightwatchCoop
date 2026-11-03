// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-hide HUD rules (gpu/shim/party/auto_hud_rules.h): what shows the HUD, how long, the fades.
// Build and run: cmake --build out/gpu --target auto-hud-test && out/gpu/auto-hud-test.exe
#include "party/auto_hud_rules.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace coop::autohud;

static Inputs Idle(double now) {
    Inputs i;
    i.valid = true;
    i.hp = i.max_hp = 600;
    i.stamina = i.max_stamina = 100;
    i.echoes = 1234;
    i.insight = 5;
    i.equip_hash = 42;
    i.now = now;
    return i;
}

// Steps the machine at 60 Hz from t0 to t1 with the input `base` (now filled in).
static double Run(Machine& m, Inputs base, double t0, double t1) {
    double a = m.alpha();
    for (double t = t0; t <= t1 + 1e-9; t += 1.0 / 60.0) {
        base.now = t;
        a = m.Update(base);
    }
    return a;
}

int main() {
    // Entering the world shows the HUD for hold_enter, then it fades out and stays hidden.
    {
        Machine m;
        assert(Run(m, Idle(0), 0, 1.0) == 1.0 && (m.reason() & kEntered));
        assert(Run(m, Idle(0), 1.0, 3.9) == 1.0);
        const double mid = Run(m, Idle(0), 4.0, 4.5);
        assert(mid < 1.0 && mid > 0.0);  // fading out
        assert(Run(m, Idle(0), 4.5, 7.0) == 0.0 && m.reason() == kNone);
        // Idle stays hidden for a long time.
        assert(Run(m, Idle(0), 7.0, 120.0) == 0.0);
    }
    // Damage: HP changes -> shown, fades after hold_hp + fade_out.
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        assert(m.alpha() == 0.0);
        Inputs hit = Idle(10.0);
        hit.hp = 450;
        hit.now = 10.0;
        m.Update(hit);
        assert(m.reason() & kHp);
        double a = Run(m, hit, 10.0 + 1.0 / 60.0, 10.5);
        assert(a == 1.0);  // fade-in done in 0.2 s
        a = Run(m, hit, 10.5, 14.9);
        assert(a == 1.0);  // still inside hold_hp (5 s from the last change)
        a = Run(m, hit, 14.9, 15.4);
        assert(a < 1.0 && a > 0.0);
        a = Run(m, hit, 15.4, 17.0);
        assert(a == 0.0);
        // Healing back to full is a change too.
        Inputs heal = hit;
        heal.hp = 600;
        heal.now = 20.0;
        m.Update(heal);
        assert(m.reason() & kHp);
    }
    // Stamina: spent and regained while it changes; the bars leave 2 s after it stops changing.
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        Inputs s = Idle(10.0);
        for (int k = 0; k < 120; ++k) {  // two seconds of regaining
            s.stamina = 40 + k / 2;
            s.now = 10.0 + k / 60.0;
            m.Update(s);
            assert(m.reason() & kStamina);
        }
        assert(m.alpha() == 1.0);
        s.stamina = 100;
        Run(m, s, 12.0, 13.9);
        assert(m.alpha() == 1.0);
        assert(Run(m, s, 14.0, 16.0) == 0.0);
    }
    // Lock-on and a close enemy hold it while they last, then hold_combat more.
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        Inputs l = Idle(10.0);
        l.lock_on = true;
        assert(Run(m, l, 10.0, 40.0) == 1.0 && (m.reason() & kLock));
        l.lock_on = false;
        assert(Run(m, l, 40.0, 45.9) == 1.0);
        assert(Run(m, l, 46.0, 48.5) == 0.0);
        Inputs e = Idle(50.0);
        e.enemy_near = true;
        assert(Run(m, e, 50.0, 70.0) == 1.0 && (m.reason() & kEnemy));
    }
    // A quick-item switch (equipment hash) shows it for hold_equip.
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        Inputs q = Idle(10.0);
        q.equip_hash = 43;
        m.Update(q);
        assert(m.reason() & kEquip);
        assert(Run(m, q, 10.0, 13.9) == 1.0);
        assert(Run(m, q, 14.0, 16.0) == 0.0);
    }
    // Echoes / Insight changes (a kill, a pickup, the Insight shop).
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        Inputs r = Idle(10.0);
        r.echoes = 2000;
        m.Update(r);
        assert(m.reason() & kRunes);
        Inputs r2 = r;
        r2.now = 10.1;
        r2.insight = 6;
        m.Update(r2);
        assert(m.reason() & kRunes);
    }
    // Menus show it while open (+1 s).
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        Inputs o = Idle(10.0);
        o.menu_open = true;
        assert(Run(m, o, 10.0, 20.0) == 1.0 && (m.reason() & kMenu));
        o.menu_open = false;
        assert(Run(m, o, 20.0, 20.9) == 1.0);
        assert(Run(m, o, 21.0, 23.0) == 0.0);
    }
    // Low HP keeps it up (below half), full-ish HP does not.
    {
        Machine m;
        Inputs l = Idle(0);
        l.hp = 200;
        Run(m, l, 0, 30.0);
        assert(m.alpha() == 1.0 && (m.reason() & kLowHp));
        l.hp = 400;  // healed above half: HP changed once, then it leaves
        Run(m, l, 30.0, 37.0);
        assert(m.alpha() == 0.0);
    }
    // An invalid sample (loading, no world) changes nothing and the next valid one counts as entering.
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        assert(m.alpha() == 0.0);
        Inputs bad;
        bad.valid = false;
        bad.now = 11.0;
        assert(m.Update(bad) == 0.0);
        Inputs back = Idle(12.0);
        m.Update(back);
        assert(m.reason() & kEntered);
    }
    // Peek (a request to look at the HUD).
    {
        Machine m;
        Run(m, Idle(0), 0, 10.0);
        m.Peek(10.0, 3.0);
        assert(Run(m, Idle(0), 10.0, 12.9) == 1.0);
        assert(Run(m, Idle(0), 13.0, 15.0) == 0.0);
    }
    // The fade speeds are wall-clock: the same at 30 and 144 steps a second.
    {
        for (double hz : {30.0, 144.0}) {
            Machine m;
            Inputs i = Idle(0);
            double t = 0;
            for (; t < 10.0; t += 1.0 / hz) { i.now = t; m.Update(i); }
            assert(m.alpha() == 0.0);
            i.hp = 100;
            double reached = -1;
            for (t = 10.0; t < 11.0; t += 1.0 / hz) {
                i.now = t;
                if (m.Update(i) >= 1.0 && reached < 0) reached = t - 10.0;
            }
            assert(reached > 0.15 && reached < 0.3);
        }
    }
    assert(Hash("abc", 3) != Hash("abd", 3) && Hash("abc", 3) == Hash("abc", 3));
    std::puts("auto hud: ok");
    return 0;
}
