// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance rules (gpu/shim/party/party_balance_rules.h): scaling rows, rot, JSON.
// cmake --build out/gpu --target party-balance-test && out/gpu/party-balance-test
#include "party/party_balance_rules.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace coop::balance;

static bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-4f;
}

int main() {
    // Vanilla rules reproduce the game's own rows 7500 / 7501 / 7502 / (7503 unused).
    const Rules v = Vanilla();
    assert(!v.rot);
    for (int n = 1; n <= 3; ++n) {
        const Doping d = Boss(v, n);
        assert(Near(d.hp_rate, 1.0f + 0.5f * n) && Near(d.resist_rate, d.hp_rate));
        assert(Near(d.attack_rate, 1.0f) && d.super_armor == 30 * n);
        assert(Enemy(v, n) == Doping{});
    }
    assert(!EnemyScalingActive(v));
    assert(Boss(v, 0) == Doping{} && Enemy(Rules{}, 0) == Doping{});

    // The defaults: harder than vanilla (user decision 2026-10-11); rot off (2026-10-22).
    const Rules d;
    assert(!d.rot && d.boss_hp == 150 && d.boss_damage == 10 && d.enemy_hp == 50);
    const Doping b1 = Boss(d, 1), b2 = Boss(d, 2), e2 = Enemy(d, 2);
    assert(Near(b1.hp_rate, 1.75f) && Near(b2.hp_rate, 2.5f) && Near(b1.attack_rate, 1.1f) && b1.super_armor == 30);
    assert(Near(e2.hp_rate, 1.5f) && Near(e2.attack_rate, 1.0f) && e2.super_armor == 0);
    assert(EnemyScalingActive(d));
    // Poise starts at 0 (user decision 2026-10-21): below zero enemies staggered less, not more
    // (live: 0 of 20 hits at -100 % against 18 of 40 at 0 %), so old negative values read as 0.
    Rules p;
    p.enemy_poise = -50;
    p.boss_poise = -100;
    assert(Enemy(p, 3).super_armor == 0 && Boss(p, 2).super_armor == 0);
    assert(Clamped(p).enemy_poise == 0 && Clamped(p).boss_poise == 0);
    p.enemy_poise = 50;
    assert(Enemy(p, 3).super_armor == 45);
    // Counts past 3 count as 3 (the game's slots).
    assert(Boss(d, 9) == Boss(d, 3));

    // Slider tables.
    assert(AtPosition(kHpTable, Position(kHpTable, 150)) == 150 && Position(kHpTable, 100) == 4);
    assert(kPoiseTable[0] == 0 && kPoiseTable[10] == 300 && Position(kPoiseTable, 0) == 0);
    assert(Position(kPoiseTable, -100) == 0 && AtPosition(kPoiseTable, Position(kPoiseTable, 100)) == 100);
    assert(AtPosition(kDamageTable, 99) == 100 && AtPosition(kDamageTable, -3) == 0);

    // JSON round trip, clamping, malformed input.
    Rules r;
    r.rot = false;
    r.boss_hp = 300;
    r.enemy_poise = 75;
    Rules back;
    assert(FromJson(ToJson(r), &back) && back == r);
    // 0.7.1: the kill record, the NPC mirror and Gather the party go with the rules; a host from
    // before them sends none: on.
    r.kill_record = false;
    r.gather = false;
    assert(FromJson(ToJson(r), &back) && back == r && !back.kill_record && back.npc_mirror && !back.gather);
    r.npc_mirror = false;
    assert(FromJson(ToJson(r), &back) && !back.npc_mirror && back != Rules{});
    {
        Rules old;
        assert(FromJson("{\"rot\":true,\"host_revive\":false}", &old) && old.kill_record && old.npc_mirror &&
               old.gather && !old.host_revive && old.npc_fights);
        assert(old.covenant_per_player);  // 0.7.3: a host from before it sends none: on
    }
    // 0.7.3: covenant outcomes per player go with the rules, on by default.
    {
        Rules c;
        assert(c.covenant_per_player);
        c.covenant_per_player = false;
        Rules cb;
        assert(FromJson(ToJson(c), &cb) && cb == c && !cb.covenant_per_player && cb != Rules{});
        assert(ToJson(c).find("\"covenant_per_player\":false") != std::string::npos);
    }
    // 0.7.3 (D11): NPC hunters fight the whole party goes with the rules, on by default.
    {
        Rules f;
        f.npc_fights = false;
        assert(FromJson(ToJson(f), &back) && !back.npc_fights && back == f && back != Rules{});
    }
    // 0.7.3 (kidnapall): Anyone can be abducted goes with the rules, on by default; a host from before it: on.
    {
        Rules k;
        k.any_kidnap = false;
        assert(FromJson(ToJson(k), &back) && !back.any_kidnap && back == k && back != Rules{});
        Rules old;
        assert(FromJson("{\"rot\":false}", &old) && old.any_kidnap);
        // 0.7.5 partyrules: the session rules travel in the JSON and default on for a host from before them.
        Rules sr;
        sr.open_world = false;
        sr.guest_refill = false;
        sr.full_rewards = false;
        Rules sback;
        assert(FromJson(ToJson(sr), &sback) && !sback.open_world && !sback.guest_refill && !sback.full_rewards &&
               sback.guest_respawn && sback == sr && sback != Rules{});
        assert(old.open_world && old.guest_lamps && old.story_mirror);
        StartupRules st;
        st.guest_lamps = false;
        st.guest_insight = 1;
        st.start_immediate = true;
        const std::string tag = JoinRulesTag(st);
        assert(tag.find("Guests use lamps=off") != std::string::npos && tag.find("Insight for bosses=Full bonus") != std::string::npos &&
               tag.find("Party starts=Right away") != std::string::npos);
    }
    // 0.7.5 (levelsync): Level sync and Phantom health go with the rules, on by default; a host from before them: on.
    {
        Rules k;
        k.level_sync = false;
        assert(FromJson(ToJson(k), &back) && !back.level_sync && back.phantom_hp && back == k && back != Rules{});
        Rules h;
        h.phantom_hp = false;
        assert(FromJson(ToJson(h), &back) && back.level_sync && !back.phantom_hp && back == h && back != Rules{});
        Rules old;
        assert(FromJson("{\"rot\":false}", &old) && old.level_sync && old.phantom_hp);
        // 0.7.6 (pvphp): Full health in PvP goes with the rules, on by default; a host from before it: on.
        assert(old.pvp_full_hp && Rules{}.pvp_full_hp);
        Rules p;
        p.pvp_full_hp = false;
        assert(FromJson(ToJson(p), &back) && !back.pvp_full_hp && back.phantom_hp && back == p && back != Rules{});
        assert(ToJson(p).find("\"pvp_full_hp\":false") != std::string::npos);
    }
    // 0.7.2: Guests bring their lamps goes with the rules, off by default; a host from before it: off.
    {
        Rules g;
        assert(!g.guests_bring_lamps && g.share_lamps);
        g.guests_bring_lamps = true;
        Rules gb;
        assert(FromJson(ToJson(g), &gb) && gb == g && gb.guests_bring_lamps && gb != Rules{});
        Rules old;
        assert(FromJson("{\"share_lamps\":true}", &old) && !old.guests_bring_lamps);
    }
    Rules c;
    assert(FromJson("{\"boss_hp\":100000,\"enemy_damage\":-5,\"rot\":7}", &c));
    assert(c.boss_hp == 400 && c.enemy_damage == 0 && !c.rot);  // not a bool: the default (off)
    // A host still sending negative poise (an older build): 0 on the guest.
    assert(FromJson("{\"boss_poise\":-100,\"enemy_poise\":-25}", &c) && c.boss_poise == 0 && c.enemy_poise == 0);
    std::string err;
    assert(!FromJson("[1,2]", &c, &err) && !err.empty());
    assert(!FromJson("{", &c, &err));
    assert(!FromJson(std::string(5000, ' '), &c, &err));

    // Rot.
    assert(Near(RotHpFactor(0), 1.0f) && Near(RotHpFactor(2), 0.8f) && Near(RotHpFactor(5), 0.5f));
    assert(Near(RotHpFactor(9), 0.5f) && Near(RotHpFactor(-1), 1.0f));
    // What cleanses the rot: a lamp warp is the respawn only while the player is down from a death.
    assert(LampWarpIsRespawn(false, 3.0, false));        // dead at the warp: the respawn
    assert(LampWarpIsRespawn(true, 5.0, false));         // just respawned, not up yet
    assert(!LampWarpIsRespawn(true, 19.0, true));        // playtest 1 H5:6072: revived in place, then a lamp
    assert(!LampWarpIsRespawn(true, 60.0, false));       // long after
    assert(!LampWarpIsRespawn(true, -1.0, false));       // no death yet
    // #21: the lamp menu opened beside a lamp is a rest.
    // Rot belongs to the character: stacks in a prologue save are another character's.
    assert(RotFromAnotherCharacter(2, true, kStaleRotPrologueS));
    assert(!RotFromAnotherCharacter(2, true, 1.0));   // a moment's reading, not yet
    assert(!RotFromAnotherCharacter(2, false, 60.0)); // past the prologue: its own
    assert(!RotFromAnotherCharacter(0, true, 60.0));  // nothing to drop
    // Save audit S4: party_rot.txt per save slot.
    {
        // The old unkeyed file: dropped (whose it was is unknown), the count kept for the log.
        const RotFile old = ParseRotFile("stacks 2\nkind0 1\nkind1 1\nkind2 0\nkind3 0\nkind4 0\n");
        assert(old.legacy_stacks == 2 && old.slots.empty());
        assert(old.Get(0).Stacks() == 0 && old.Get(1).Stacks() == 0);
        assert(ParseRotFile("stacks 9\n").legacy_stacks == kRotMaxStacks);
        assert(FormatRotFile(old).empty());
        // Two characters, each its own.
        RotFile f;
        SlotRot a;
        a.slot = 1;
        a.kind[0] = 1;
        a.kind[3] = 1;
        f.Set(a);
        SlotRot b;
        b.slot = 0;
        b.kind[2] = 3;
        f.Set(b);
        assert(f.Get(1).Stacks() == 2 && f.Get(0).Stacks() == 3 && f.Get(4).Stacks() == 0 && f.Get(4).slot == 4);
        const RotFile g = ParseRotFile(FormatRotFile(f));
        assert(g.legacy_stacks == 0 && g.slots.size() == 2);
        assert(g.Get(1).kind[0] == 1 && g.Get(1).kind[3] == 1 && g.Get(0).kind[2] == 3);
        // A cleansed character's entry goes.
        SlotRot clean;
        clean.slot = 1;
        f.Set(clean);
        assert(f.slots.size() == 1 && f.Get(1).Stacks() == 0);
        assert(FormatRotFile(f) == "slot 0 kinds 0 0 3 0 0\n");
        // Clamped to kRotMaxStacks in all; junk and out-of-range slots ignored.
        const RotFile c = ParseRotFile("slot 2 kinds 4 4 4 0 0\nslot 11 kinds 1 0 0 0 0\nslot x\n");
        assert(c.slots.size() == 1 && c.Get(2).Stacks() == kRotMaxStacks && c.Get(2).kind[1] == 1);
        // The active slot switches only to a known, different one.
        assert(RotSlotChanged(-1, 0) && RotSlotChanged(0, 1));
        assert(!RotSlotChanged(1, 1) && !RotSlotChanged(1, -1) && !RotSlotChanged(-1, -1));
    }
    assert(LampMenuRest(true, true, 1.5f, 0.2f, -1.0f));
    assert(!LampMenuRest(false, true, 1.5f, 0.2f, -1.0f));  // no dialog
    assert(!LampMenuRest(true, false, 1.5f, 0.2f, -1.0f));  // dead
    assert(!LampMenuRest(true, true, 6.0f, 0.0f, 0.0f));    // an item box elsewhere
    assert(!LampMenuRest(true, true, 1.0f, 5.0f, 0.0f));    // a floor above

    // C4 stamina cost: +10 % a stack, charged on each drop, fraction carried.
    {
        float carry = 0;
        assert(RotExtraStaminaDrain(100, 90, 100, 100, 1.1f, &carry) == 1 && carry < 0.01f);  // roll 10 -> +1
        carry = 0;
        assert(RotExtraStaminaDrain(100, 95, 100, 100, 1.1f, &carry) == 0 && carry > 0.49f);  // 0.5 carried
        assert(RotExtraStaminaDrain(95, 90, 100, 100, 1.1f, &carry) == 1 && carry < 0.01f);
        carry = 0;
        assert(RotExtraStaminaDrain(100, 70, 100, 100, 1.2f, &carry) == 6);   // 2 stacks: 30 -> +6
        assert(RotExtraStaminaDrain(90, 95, 100, 100, 1.1f, &carry) == 0);    // regen
        assert(RotExtraStaminaDrain(100, 90, 100, 90, 1.1f, &carry) == 0);    // max changed (stamina rot)
        assert(RotExtraStaminaDrain(100, 90, 100, 100, 1.0f, &carry) == 0);   // no stack
        assert(RotExtraStaminaDrain(-1, 90, 100, 100, 1.5f, &carry) == 0);    // unknown
        carry = 0;
        assert(RotExtraStaminaDrain(10, 1, 100, 100, 2.0f, &carry) == 1);     // never below 0
    }

    // C4 rally: -15 % a stack of each non-vial HP gain, fraction carried.
    {
        float carry = 0;
        assert(RotRallyTakeBack(400, 500, 719, 719, 0.85f, false, &carry) == 15);   // +100 -> keep 85
        assert(RotRallyTakeBack(400, 500, 719, 719, 0.85f, true, &carry) == 0);     // a Blood Vial
        assert(RotRallyTakeBack(0, 500, 719, 719, 0.85f, false, &carry) == 0);      // a revive
        assert(RotRallyTakeBack(400, 500, 647, 719, 0.85f, false, &carry) == 0);    // the max changed
        assert(RotRallyTakeBack(500, 400, 719, 719, 0.85f, false, &carry) == 0);    // damage
        assert(RotRallyTakeBack(400, 500, 719, 719, 1.0f, false, &carry) == 0);     // no stack
        carry = 0;
        assert(RotRallyTakeBack(400, 410, 719, 719, 0.85f, false, &carry) == 1 && carry > 0.49f);  // 1.5
        assert(RotRallyTakeBack(410, 420, 719, 719, 0.85f, false, &carry) == 2);   // 1.5 + 0.5
        carry = 0;
        assert(RotRallyTakeBack(400, 500, 719, 719, -1.0f, false, &carry) == 100);  // clamped
        assert(RotRallyGainInWindow(0.5));     // a hit back right after damage
        assert(RotRallyGainInWindow(kRotRallyWindowS));
        assert(!RotRallyGainInWindow(kRotRallyWindowS + 0.1));  // a regeneration tick later
        assert(!RotRallyGainInWindow(-1.0));   // no damage seen
        // Item discovery: -10 points (0.1) a stack, floor 0.1, clamped stacks, unknown left alone.
        assert(Near(RotItemDiscovery(1.0f, 0), 1.0f) && Near(RotItemDiscovery(1.0f, 2), 0.8f));
        assert(Near(RotItemDiscovery(1.7f, 5), 1.2f) && Near(RotItemDiscovery(1.0f, 9), 0.5f));
        assert(Near(RotItemDiscovery(0.3f, 5), kRotDiscoveryFloor) && Near(RotItemDiscovery(0.0f, 3), 0.0f));
        assert(Near(RotItemDiscovery(2.1f, -1), 2.1f));
    }

    // Menu texts.
    {   // The rot as the menu and the party HUD show it.
        const int none[kRotFileKinds] = {}, mixed[kRotFileKinds] = {2, 0, 1, 0, 0}, rally[kRotFileKinds] = {0, 0, 0, 5, 0};
        assert(DescribeRot(none, false).empty());
        assert(DescribeRot(mixed, false) == "Rot 3/5: max HP -20 %, stamina costs +10 %");
        assert(DescribeRot(rally, false) == "Rot 5/5: rally -75 %");
        assert(DescribeRot(mixed, true).find("Гниль 3/5") == 0);
    }
    assert(DescribeHp(150, false).find("+75 %") != std::string::npos);
    assert(DescribePoise(300, false).find("+90") != std::string::npos);
    std::printf("party balance rules: ok\n");
    return 0;
}
