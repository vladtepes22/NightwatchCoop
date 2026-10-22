// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.3 "Anyone can be abducted" (gpu/shim/party/party_kidnap_rules.h): a Snatcher's grab of a
// guest takes the whole party to the Hypogean Gaol, as the host's own grab does.
#include "party/party_kidnap_rules.h"

#include <cassert>
#include <cstdio>

using namespace coop::kidnap;

int main() {
    // The grab events of the game data (m24_00 12400780, m27 12700500); nothing else.
    assert(KidnapEvent(12400780) && KidnapEvent(12700500));
    assert(!KidnapEvent(12400791) && !KidnapEvent(9421) && !KidnapEvent(9422) && !KidnapEvent(0));

    // A summoned guest with the rule on: its copy runs past 4[3] (its own chr type), and its 9420
    // write is skipped (the host is told instead).
    assert(PlanInstr(12400780, 4, 3, true, true) == GuestAct::PassGate);
    assert(PlanInstr(12700500, 4, 3, true, true) == GuestAct::PassGate);
    assert(PlanInstr(12400780, 2003, 2, true, true, 9420) == GuestAct::SkipTrigger);
    // The rule off (the old behaviour): the gate is the game's own (a guest's copy ends there), and a
    // 9420 that came anyway is still never written on a guest (quest_audit F1: never into its save).
    assert(PlanInstr(12400780, 4, 3, true, false) == GuestAct::RestoreGate);
    assert(PlanInstr(12400780, 2003, 2, true, false, 9420) == GuestAct::SkipTrigger);
    // The host and a player in its own world: the game's own event (the host's grab as before).
    assert(PlanInstr(12400780, 4, 3, false, true) == GuestAct::RestoreGate);
    assert(PlanInstr(12400780, 2003, 2, false, true, 9420) == GuestAct::None);
    // The other instructions (death, damage, HP, flags) run as they are; other events untouched.
    assert(PlanInstr(12400780, 4, 0, true, true) == GuestAct::None);
    assert(PlanInstr(12400780, 4, 2, true, true) == GuestAct::None);
    assert(PlanInstr(12400780, 3, 0, true, true) == GuestAct::None);
    assert(PlanInstr(12400780, 2003, 2, true, true, 9401) == GuestAct::None);
    assert(PlanInstr(12401802, 4, 3, true, true) == GuestAct::None);
    assert(PlanInstr(9421, 2003, 2, true, true, 9420) == GuestAct::None);

    // One request per grab: the copy restarts every frame while the body lies at HP 0.
    assert(SendRequest(-1.0) && SendRequest(kRequestRepeat) && SendRequest(60.0));
    assert(!SendRequest(0.0) && !SendRequest(0.5) && !SendRequest(kRequestRepeat - 0.1));

    // The host's answer.
    HostView v;
    v.in_world = true;
    v.members = 1;
    v.host_map = 0x18000000;   // m24_00 Cathedral Ward
    v.guest_map = 0x18000000;
    v.age_s = 0.1;
    assert(PlanHost(v) == HostPlan::Start);
    v.host_map = 0x1b000000;   // m27 Forbidden Woods
    v.guest_map = 0x1b000000;
    assert(PlanHost(v) == HostPlan::Start);
    v.guest_map = 0;           // the guest's map unknown: the host's area decides
    assert(PlanHost(v) == HostPlan::Start);
    {
        HostView o = v;
        o.rule_on = false;     // "Anyone can be abducted" off: a plain death
        assert(PlanHost(o) == HostPlan::RuleOff);
        o = v;
        o.woke = true;         // the first kidnap is done: a later grab is a plain death (9421 ends on 9423)
        assert(PlanHost(o) == HostPlan::Woke);
        o = v;
        o.host_map = 0x1c000000;  // the host is in Yahar'gul (no Snatchers there)
        assert(PlanHost(o) == HostPlan::NotHere);
        o = v;
        o.guest_map = 0x18000000;  // the guest in the Cathedral Ward, the host in the Woods
        assert(PlanHost(o) == HostPlan::NotHere);
        o = v;
        o.members = 0;
        assert(PlanHost(o) == HostPlan::NotHere);
        o = v;
        o.in_world = false;    // loading
        assert(PlanHost(o) == HostPlan::NotHere);
        o = v;
        o.busy = true;         // the host is down / in limbo / in a boss fight
        assert(PlanHost(o) == HostPlan::Busy);
        o = v;
        o.age_s = kRequestMaxAge + 1;
        assert(PlanHost(o) == HostPlan::Stale);
        o.age_s = -1;
        assert(PlanHost(o) == HostPlan::Stale);
    }
    assert(SnatcherArea(0x18000000) && SnatcherArea(0x18010000) && SnatcherArea(0x1b000000));
    assert(!SnatcherArea(0x1c000000) && !SnatcherArea(0x15000000) && !SnatcherArea(0));

    // After 9420 on the host: travel once common 9421 moved the respawn point to the Gaol cell.
    assert(PlanHostStep(2412951, 0.0) == HostStep::Wait);
    assert(PlanHostStep(kGaolReturnPoint, 0.05) == HostStep::Travel);
    assert(PlanHostStep(2412951, kRespawnWait + 0.1) == HostStep::GiveUp);
    assert(PlanHostStep(kGaolReturnPoint, kRespawnWait + 5) == HostStep::Travel);

    // The grabbed guest's follow waits for its death only for the Gaol travel it asked for.
    assert(FollowWaitsForDeath(1.0, kGaolReturnPoint));
    assert(!FollowWaitsForDeath(-1.0, kGaolReturnPoint));          // never asked (another guest)
    assert(!FollowWaitsForDeath(kFollowWindow + 1, kGaolReturnPoint));
    assert(!FollowWaitsForDeath(1.0, 2402950));                    // a lamp travel of the host's own
    assert(kGaolReturnPoint == 2802959 && kTriggerFlag == 9420 && kWokeFlag == 9423 && kWakingRemo == 28000010);

    std::printf("party kidnap rules: ok\n");
    return 0;
}
