// SPDX-License-Identifier: GPL-3.0-or-later
// C4 rot, what cleanses part of it (gpu/shim/party/party_rot_rules.h) and its rules / party_rot.txt
// fields (party_balance_rules.h). cmake --build out/gpu --target party-rot-test && out/gpu/party-rot-test
#include "party/party_balance_rules.h"
#include "party/party_rot_rules.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace coop::rot;
namespace bal = coop::balance;

namespace {

enum { H, ST, SC, R, D };  // the kinds: health, stamina, stamina cost, rally, discovery

Toggles AllOn() {
    Toggles t;
    t.rot = t.boss = t.killer = t.echoes = true;
    return t;
}

Killer Enemy(std::uint32_t entity, std::uint64_t chr = 0x1000) {
    Killer k;
    k.entity = entity;
    k.chr = chr;
    k.key = 77;
    return k;
}

/// A death as the game side does it: the stack first, then the ledger.
void Die(Ledger* l, int stacks[kKinds], int kind, const Killer& k = Killer{}) {
    if (kind >= 0 && Stacks(stacks) < kMaxStacks) {
        ++stacks[kind];
    } else {
        kind = -1;
    }
    l->OnDeath(kind, k);
}

}  // namespace

int main() {
    // ---- The rules: rot off by default (2026-10-22), the cleanses on; an older host: off. ----
    {
        const bal::Rules d;
        assert(!d.rot && d.rot_cleanse_boss && d.rot_cleanse_killer && d.rot_cleanse_echoes);
        bal::Rules r;
        r.rot = true;
        r.rot_cleanse_killer = false;
        bal::Rules back;
        assert(bal::FromJson(bal::ToJson(r), &back) && back == r && back.rot && !back.rot_cleanse_killer &&
               back.rot_cleanse_boss && back.rot_cleanse_echoes);
        // A 0.7.1 host's rules (no cleanse keys): its rot as sent, the cleanses off.
        bal::Rules old;
        assert(bal::FromJson("{\"rot\":true,\"boss_hp\":150}", &old) && old.rot && !old.rot_cleanse_boss &&
               !old.rot_cleanse_killer && !old.rot_cleanse_echoes);
        // No "rot" at all: the new default (off).
        bal::Rules none;
        assert(bal::FromJson("{}", &none) && !none.rot);
        r.rot_cleanse_echoes = false;
        assert(r != back);
    }

    // ---- Bookkeeping: one stack per death, each with its links. ----
    {
        Ledger l;
        int s[kKinds] = {};
        Die(&l, s, H, Enemy(2410247));
        assert(l.deaths().size() == 1 && l.deaths()[0].kind == H && l.deaths()[0].killer_link && l.deaths()[0].stain_link);
        Die(&l, s, ST);  // a fall: no killer
        assert(l.deaths().size() == 2 && !l.deaths()[1].killer_link && l.deaths()[1].stain_link);
        assert(!l.deaths()[0].stain_link && l.deaths()[0].killer_link);  // its stain is gone, its killer lives
        assert(l.StainKind() == ST && Stacks(s) == 2);
        // At the maximum a death adds nothing but still replaces the stain.
        Die(&l, s, R);
        Die(&l, s, R);
        Die(&l, s, D);
        assert(Stacks(s) == 5);
        Die(&l, s, H, Enemy(2410202));
        assert(Stacks(s) == 5 && l.StainKind() == -1);
        // The prologue / rot off: no stack, no record (kind -1).
        Ledger p;
        p.OnDeath(-1, Enemy(1));
        assert(p.deaths().empty());
    }

    // ---- Killer: the enemy that killed you dies, its death's stack goes (by entity or chr). ----
    {
        Ledger l;
        int s[kKinds] = {};
        Die(&l, s, SC, Enemy(2410247, 0xAA));
        Die(&l, s, H, Enemy(2410999, 0xBB));
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410111) == 0);       // somebody else's enemy
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 1 && s[SC] == 0 && s[H] == 1);
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 0);       // once
        assert(Apply(Why::Killer, AllOn(), &l, s, 0, 0xBB) == 1 && Stacks(s) == 0);  // by its chr here
        // The same enemy killed you twice: both stacks.
        Die(&l, s, R, Enemy(2400125));
        Die(&l, s, D, Enemy(2400125));
        assert(Apply(Why::Killer, AllOn(), &l, s, 2400125) == 2 && Stacks(s) == 0);
        assert(std::strcmp(NoticeText(Why::Killer, 1), "Rot -1: your killer is dead") == 0);
    }

    // ---- No killer: a fall, the environment, yourself, another player, a late hit. ----
    {
        assert(KillerCounts(0.0, false, true));
        assert(!KillerCounts(-1.0, false, true));          // no kill log call (a fall, a scripted death)
        assert(!KillerCounts(0.1, true, true));            // yourself
        assert(!KillerCounts(0.1, false, false));          // a player (friendly fire)
        assert(!KillerCounts(kKillerWindowS + 1, false, true));  // an older hit
        assert(PartyEntity(2410247) && !PartyEntity(10000) && !PartyEntity(0) && !PartyEntity(0xFFFFFFFFu));
        Ledger l;
        int s[kKinds] = {};
        Die(&l, s, H);
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 0 && s[H] == 1);
    }

    // ---- The killer respawns (a rest, a wipe): the link is gone. A guest's own load: the chr goes. ----
    {
        Ledger l;
        int s[kKinds] = {};
        Die(&l, s, H, Enemy(2410247, 0xAA));
        l.OnWorldReset();
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 0 && s[H] == 1);
        // its bloodstain still gives it back
        assert(Apply(Why::Echoes, AllOn(), &l, s) == 1 && s[H] == 0);

        Die(&l, s, ST, Enemy(2410247, 0xAA));
        Killer nameless = Enemy(0, 0xCC);  // an enemy without an entity id: only its chr
        Die(&l, s, SC, nameless);
        l.OnLocalLoad();
        assert(Apply(Why::Killer, AllOn(), &l, s, 0, 0xAA) == 0);         // the chr is a new one now
        assert(Apply(Why::Killer, AllOn(), &l, s, 0, 0xCC) == 0 && s[SC] == 1);  // nameless: lost
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 1 && s[ST] == 0);  // by its id: still
    }

    // ---- Echoes: the stain's death; killer first then echoes; died again before retrieving. ----
    {
        Ledger l;
        int s[kKinds] = {};
        Die(&l, s, R, Enemy(2410247));
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 1 && s[R] == 0);
        assert(Apply(Why::Echoes, AllOn(), &l, s) == 0 && Stacks(s) == 0);  // removed already: nothing more
        Die(&l, s, H);   // stain 1
        Die(&l, s, ST);  // stain 2 replaces it (stain 1's echoes lost, its stack stays)
        assert(Apply(Why::Echoes, AllOn(), &l, s) == 1 && s[ST] == 0 && s[H] == 1);
        assert(Apply(Why::Echoes, AllOn(), &l, s) == 0 && s[H] == 1);       // the old stain is gone
        assert(std::strcmp(NoticeText(Why::Echoes, 1), "Rot -1: Blood Echoes retrieved") == 0);
        assert(NoticeText(Why::Echoes, 0)[0] == 0);
        // Echoes first, then the killer: once.
        Die(&l, s, D, Enemy(2410300));
        assert(Apply(Why::Echoes, AllOn(), &l, s) == 1 && s[D] == 0);
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410300) == 0 && s[H] == 1);
    }

    // ---- Boss: everything, for everyone; with the rule off a boss is still a killer. ----
    {
        Ledger l;
        int s[kKinds] = {};
        Die(&l, s, H, Enemy(2410247));
        Die(&l, s, ST);
        Die(&l, s, R, Enemy(2410800));  // killed by the boss (Cleric Beast)
        assert(Apply(Why::Boss, AllOn(), &l, s, 2410800) == 3 && Stacks(s) == 0 && l.deaths().empty());
        assert(std::strcmp(NoticeText(Why::Boss, 3), "Rot cleansed: boss defeated") == 0);
        assert(Apply(Why::Echoes, AllOn(), &l, s) == 0);
        Toggles t = AllOn();
        t.boss = false;
        Die(&l, s, H, Enemy(2410800));
        Die(&l, s, ST);
        assert(Apply(Why::Boss, t, &l, s, 2410800) == 1 && s[H] == 0 && s[ST] == 1);
        assert(Apply(Why::Boss, t, &l, s, 0) == 0 && s[ST] == 1);  // a guest's notice: no entity
    }

    // ---- The toggles off: no change (the links are used up all the same). ----
    {
        Ledger l;
        int s[kKinds] = {};
        Toggles off = AllOn();
        off.boss = off.killer = off.echoes = false;
        Die(&l, s, H, Enemy(2410247));
        assert(Apply(Why::Boss, off, &l, s, 2410800) == 0 && s[H] == 1);
        assert(Apply(Why::Killer, off, &l, s, 2410247) == 0 && s[H] == 1);
        assert(Apply(Why::Echoes, off, &l, s) == 0 && s[H] == 1);
        // ... and rot itself off: none of the three does anything.
        Toggles rot_off = AllOn();
        rot_off.rot = false;
        int s2[kKinds] = {};
        Ledger l2;
        Die(&l2, s2, SC, Enemy(2410247));
        assert(Apply(Why::Killer, rot_off, &l2, s2, 2410247) == 0 && s2[SC] == 1);
        assert(Apply(Why::Echoes, rot_off, &l2, s2) == 0 && s2[SC] == 1);
        assert(Apply(Why::Boss, rot_off, &l2, s2, 0) == 0 && s2[SC] == 1);
        // One rule on, the others off.
        Toggles echoes_only = off;
        echoes_only.echoes = true;
        Ledger l3;
        int s3[kKinds] = {};
        Die(&l3, s3, R, Enemy(2410247));
        assert(Apply(Why::Killer, echoes_only, &l3, s3, 2410247) == 0 && s3[R] == 1);
        assert(Apply(Why::Echoes, echoes_only, &l3, s3) == 1 && s3[R] == 0);
    }

    // ---- Persistence per save slot: party_rot.txt keeps the stain's kind ("stain K"). ----
    {
        bal::RotFile f;
        bal::SlotRot a;
        a.slot = 2;
        a.kind[H] = 1;
        a.kind[R] = 2;
        a.stain = R;
        f.Set(a);
        bal::SlotRot b;
        b.slot = 5;
        b.kind[ST] = 1;  // no stain link
        f.Set(b);
        const std::string text = bal::FormatRotFile(f);
        assert(text == "slot 2 kinds 1 0 0 2 0 stain 3\nslot 5 kinds 0 1 0 0 0\n");
        const bal::RotFile g = bal::ParseRotFile(text);
        assert(g.Get(2).stain == R && g.Get(2).Stacks() == 3 && g.Get(5).stain == -1 && g.Get(7).stain == -1);
        // A 0.7.1 file (no stain): read as before; a stain on a kind without stacks is dropped.
        assert(bal::ParseRotFile("slot 1 kinds 0 2 0 0 0\n").Get(1).stain == -1);
        assert(bal::ParseRotFile("slot 1 kinds 0 2 0 0 0 stain 4\n").Get(1).stain == -1);
        assert(bal::ParseRotFile("slot 1 kinds 0 2 0 0 0 stain 9\n").Get(1).Stacks() == 2);
        // After a restart only the stain is known (every killer respawned).
        Ledger l;
        l.Restore(g.Get(2).stain);
        int s[kKinds] = {1, 0, 0, 2, 0};
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 0);
        assert(Apply(Why::Echoes, AllOn(), &l, s) == 1 && s[R] == 1 && s[H] == 1);
        l.Restore(-1);
        assert(l.deaths().empty() && l.StainKind() == -1);
        // Another slot's character: its own stain only.
        l.Restore(g.Get(5).stain);
        assert(l.deaths().empty());
    }

    // ---- Forget: stacks that went another way are never given back twice. ----
    {
        Ledger l;
        int s[kKinds] = {};
        Die(&l, s, H, Enemy(2410247));
        Die(&l, s, H, Enemy(2410202));
        s[H] = 1;  // one went (e.g. a merge capped it)
        l.Forget(s);
        assert(l.deaths().size() == 1 && l.deaths()[0].killer.entity == 2410202);
        assert(Apply(Why::Killer, AllOn(), &l, s, 2410247) == 0 && s[H] == 1);
        assert(TakeStack(s, H) && !TakeStack(s, H) && !TakeStack(s, -1) && !TakeStack(s, 5));
    }

    // The party HUD's short line while a boss bar is up (2+ kinds on one line).
    {
        const int two[kKinds] = {1, 0, 0, 1, 0};
        assert(bal::DescribeRotShort(two, false) == "Rot 2/5: HP, rally");
        const int none[kKinds] = {};
        assert(bal::DescribeRotShort(none, false).empty());
        const int all[kKinds] = {1, 1, 1, 1, 1};
        assert(bal::DescribeRotShort(all, false) == "Rot 5/5: HP, stamina, costs, rally, finds");
    }

    std::printf("party-rot-test: ok\n");
    return 0;
}
