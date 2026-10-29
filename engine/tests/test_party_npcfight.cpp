// SPDX-License-Identifier: GPL-3.0-or-later
// NPC hunter fights with the whole party (gpu/shim/party/party_npcfight_rules.h, ERSC parity D11).
#include "party/party_npcfight_rules.h"

#include <cassert>
#include <cstdio>
#include <iterator>
#include <set>

using namespace coop::npcfight;

int main() {
    // Slots: only the AI-player summons use them. The party's default (3 players) leaves one; a
    // 4-player party fills the game's four-player limit; the 5-entry table never overflows.
    assert(AiSummonFits(1, 0) && AiSummonFits(2, 1) && AiSummonFits(3, 0));
    assert(!AiSummonFits(3, 1) && !AiSummonFits(4, 0) && !AiSummonFits(5, 0) && !AiSummonFits(0, 0));
    for (int h = 1; h <= kSlotEntries; ++h)
        for (int n = 0; n <= kSlotEntries; ++n)
            if (AiSummonFits(h, n)) assert(h + n + 1 <= kVanillaMaxPlayers && h + n + 1 <= kSlotEntries);

    // The table: one gate per event, sorted out of the scan; every map one the party plays.
    std::set<int> seen;
    for (const GuestRun& g : kGuestRuns) {
        assert(seen.insert(g.event).second && g.index >= 0 && g.who && g.area_block >= 2100);
        assert(GuestRunFor(g.event) == &g);
    }
    assert(!GuestRunFor(12410811) && !GuestRunFor(12410806));  // Eileen's ally / hostile at the Tomb run everywhere

    // Alfred in Cainhurst (12500053: 1003[6] "end if client" at 0, team 27 at 7, flags at 8 / 9, save at 10).
    assert(Decide(12500053, 0, 1003, 6, true, true) == Act::pass_gate);
    assert(Decide(12500053, 0, 1003, 6, false, true) == Act::restore_gate);  // the host: the data's own
    assert(Decide(12500053, 0, 1003, 6, true, false) == Act::restore_gate);  // the rule off: vanilla
    assert(Decide(12500053, 7, 2004, 2, true, true) == Act::foe_team);       // the team switch runs, as a foe
    assert(Decide(12500053, 6, 2003, 2, true, true) == Act::skip);           // its flag writes do not
    assert(Decide(12500053, 8, 2003, 22, true, true) == Act::skip);
    assert(Decide(12500053, 10, 2000, 5, true, true) == Act::skip);
    assert(Decide(12500053, 6, 2003, 2, false, true) == Act::none);           // the host writes them
    assert(Decide(12500053, 6, 2003, 2, true, false) == Act::none);
    // The blood-drunk Eileen (12400656: 4[3] at 1): only that 4[3] is the gate.
    assert(Decide(12400656, 1, 4, 3, true, true) == Act::pass_gate);
    assert(Decide(12400656, 2, 4, 3, true, true) == Act::none);
    assert(Decide(12400656, 1, 3, 0, true, true) == Act::none);
    assert(Decide(12400656, 9, 2004, 2, true, true) == Act::foe_team);
    // Another event is never touched, whatever it holds.
    assert(Decide(12410803, 0, 4, 3, true, true) == Act::none && Decide(12410803, 5, 2003, 22, true, true) == Act::none);
    // The hostile team on a guest's game (live nfa run 2: a phantom cannot lock on to / hit team 27):
    // a guest's copy writes 23, every other dispatch puts the data's 27 back; other teams untouched.
    assert(TeamFor(Act::foe_team, kHostileTeam) == kGuestFoeTeam);
    assert(TeamFor(Act::foe_team, kGuestFoeTeam) == kGuestFoeTeam);  // already written
    assert(TeamFor(Act::foe_team, 8) == 8);                           // Valtr's friendly team
    assert(TeamFor(Act::restore_team, kGuestFoeTeam) == kHostileTeam);
    assert(TeamFor(Act::restore_team, kHostileTeam) == kHostileTeam && TeamFor(Act::restore_team, 8) == 8);
    assert(TeamFor(Act::none, kHostileTeam) == kHostileTeam && TeamFor(Act::skip, kGuestFoeTeam) == kGuestFoeTeam);
    for (const GuestRun& g : kGuestRuns) {
        assert(TeamEvent(g.event));
        assert(Decide(g.event, 99, 2004, 2, true, true) == Act::foe_team);
        assert(Decide(g.event, 99, 2004, 2, false, true) == Act::restore_team);  // the host
        assert(Decide(g.event, 99, 2004, 2, true, false) == Act::restore_team);  // the rule off
    }
    for (std::int32_t e : kTeamEvents) {
        assert(TeamEvent(e) && !GuestRunFor(e));
        assert(Decide(e, 30, 2004, 2, true, true) == Act::foe_team && Decide(e, 30, 2004, 2, false, false) == Act::restore_team);
        assert(Decide(e, 0, 4, 3, true, true) == Act::none && Decide(e, 5, 2003, 22, true, true) == Act::none);
    }
    // Eileen turned hostile by the host's attack (12400658 [11]: 2400900 team 27): a guest can hit her
    assert(Decide(12400658, 11, 2004, 2, true, true) == Act::foe_team && TeamFor(Act::foe_team, kHostileTeam) == kGuestFoeTeam);
    assert(Decide(12400590, 46, 2004, 2, true, true) == Act::foe_team);  // Alfred hostile at a load
    assert(!TeamEvent(12410811) && Decide(12410811, 3, 2004, 2, true, true) == Act::none);  // Eileen's ally (28)
    assert(Decide(12500053, 7, 2004, 8, true, true) == Act::none);                           // not a team switch
    // the relation: the host's body (19) and a phantom (20) both treat 23 as an enemy (pvp.md);
    // 27 is one only for 19 - the reason for the switch
    static_assert(kGuestFoeTeam == 23 && kHostileTeam == 27 && kTeamArg == 4, "2004[2] layout / teams");
    assert(ProgressWrite(2003, 2) && ProgressWrite(2000, 5) && !ProgressWrite(2004, 2) && !ProgressWrite(2004, 8) &&
           !ProgressWrite(2003, 4));

    // Assassins: shown while the host's appear flag is on, hidden again only when we showed it.
    assert(PlanAssassin(true, true, false, true, false).show);
    assert(!PlanAssassin(true, true, false, false, true).show && !PlanAssassin(true, true, false, false, true).hide);
    assert(PlanAssassin(true, false, false, false, true).hide);
    assert(!PlanAssassin(true, false, false, false, false).hide);  // the guest's own state: left alone
    assert(!PlanAssassin(true, true, true, true, false).show);     // repelled / dead: nothing
    assert(PlanAssassin(false, true, false, false, true).hide && !PlanAssassin(false, true, false, true, false).show);
    for (const Assassin& a : kAssassins) assert(a.appeared != a.done && a.entity / 100000 >= 35);

    // Covenant outcomes are per player: Annalise's flesh (1042), Alfred in the castle (1348), Valtr.
    assert(CovenantOutcomeFlag(1042) && CovenantOutcomeFlag(1348) && CovenantOutcomeFlag(1805));
    assert(!CovenantOutcomeFlag(1367) && !CovenantOutcomeFlag(12410810) && !CovenantOutcomeFlag(1039));
    std::puts("party-npcfight-test: ok");

    // Bell helpers and the boss fog (npcfog2077): a helper still outside the arena after the grace is put at
    // the entry spot; one inside, or with the fight off / not joined / left / dead, is left alone.
    {
        std::set<std::uint32_t> npcs;
        for (const FogHelper& h : kFogHelpers) {
            assert(npcs.insert(h.npc).second && h.join && h.leave && h.host_flag && h.region);
            assert(h.join != h.leave);
        }
        assert(kFogHelpers[0].npc == 2300740 && kFogHelpers[0].host_flag == 12304800 && kFogHelpers[0].region == 2302800);
        assert(PlanFogFollow(true, true, true, false, true, 0.0, 40.0f, false) == FogFollow::wait);
        assert(PlanFogFollow(true, true, true, false, true, kFogFollowGrace, 40.0f, false) == FogFollow::warp);
        assert(PlanFogFollow(true, true, true, false, true, 30.0, 3.0f, false) == FogFollow::done);
        assert(PlanFogFollow(false, true, true, false, true, 30.0, 40.0f, false) == FogFollow::none);   // the rule off
        assert(PlanFogFollow(true, false, true, false, true, 30.0, 40.0f, false) == FogFollow::none);   // no fight
        assert(PlanFogFollow(true, true, false, false, true, 30.0, 40.0f, false) == FogFollow::none);   // never joined
        assert(PlanFogFollow(true, true, true, true, true, 30.0, 40.0f, false) == FogFollow::none);     // it left
        assert(PlanFogFollow(true, true, true, false, false, 30.0, 40.0f, false) == FogFollow::none);   // dead / off
        assert(PlanFogFollow(true, true, true, false, true, 30.0, 40.0f, true) == FogFollow::none);     // once per fight
    }

    return 0;
}
