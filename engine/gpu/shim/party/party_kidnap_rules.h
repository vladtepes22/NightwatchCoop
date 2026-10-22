// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.3 "Anyone can be abducted" (party_any_kidnap, the host's rule, on by default): a Snatcher's
// grab of a GUEST in the host's world takes the whole party to the Hypogean Gaol, as the host's own
// grab does. Pure rules (tests/test_party_kidnap.cpp); the game side is in party_b2.cpp.
//
// How the grab works (game data, docs/party/quest_audit.md item 1 / "Scripted warps"):
//   m24_00 12400780 (4 slots, one per Snatcher 2400872..875) / m27 12700500 (Snatcher 2700680):
//     4[3]  chr 10000 type == 0, else end          <- a summoned guest is type 1: its copy ends here
//     4[0]  the Snatcher alive; 4[1] chr 10000 damaged by it; 4[2] chr 10000 HP ratio == 0
//     3[0]  9401 and 9404 on                        (story state; a guest reads the host world's)
//     2003[2] 9420 on                               <- the kidnap trigger
//   common 9421 (host only, until 9423): 9420 on -> 2003[23] respawn point 2802959 (the Gaol cell),
//     9420 off. The death's respawn load then wakes the player in the Gaol.
//   common 9422 (host only, m28, 9421 done, until 9423): remo 28000010 (the waking scene), 9423 on,
//     SpEffect 4680 (+1 Insight).
// A guest's grab: the guest's copy runs past the type gate (PassGate), its own 9420 write is never
// done (SkipTrigger: 9420 must never reach a guest's flags or save, quest_audit F1) and the host is
// told instead. The host sets its own 9420 (its 9421 moves its respawn point to the Gaol as for its
// own grab) and makes the party's travel to 2802959 (one loading screen for everyone). The grabbed
// guest follows once its own death is taken (its bloodstain where it fell, as the host's own grab
// leaves one): the wipe follow (party_death_rules.h PlanWipeFollow), dead, to the cell.
#pragma once

#include <cstdint>

namespace coop::kidnap {

constexpr std::uint32_t kGaolReturnPoint = 2802959;  ///< ReturnPointParam row of the Gaol cell (m28)
constexpr std::uint32_t kTriggerFlag = 9420;         ///< the kidnap trigger (never a guest's)
constexpr std::uint32_t kWokeFlag = 9423;            ///< the waking scene played: the kidnap is done
constexpr std::uint32_t kWakingRemo = 28000010;      ///< common 9422's scene (plays for every member)
constexpr std::int32_t kChrTypeArg = 8;              ///< 4[3]: s8 group @0, i32 entity @4, s8 chr type @8

/// The Snatcher's grab events (one per map with Snatchers).
constexpr bool KidnapEvent(std::int32_t event_id) {
    return event_id == 12400780 || event_id == 12700500;
}

/// What a game does with one instruction of a kidnap event.
enum class GuestAct : std::uint8_t {
    None,         ///< as the game data says
    PassGate,     ///< 4[3]: the type to test is this guest's own (its copy runs on)
    RestoreGate,  ///< 4[3]: the data's own type 0 again (own world, the rule off, the host)
    SkipTrigger,  ///< 2003[2] 9420 on a guest: never written; the host is told when the rule is on
};

/// `client`: a summoned guest in the host's world (session role 6). `rule_on`: the host's rule.
/// `flag_arg`: the 2003[2]'s flag (0 for any other instruction).
constexpr GuestAct PlanInstr(std::int32_t event_id, int bank, int id, bool client, bool rule_on,
                             std::uint32_t flag_arg = 0) {
    if (!KidnapEvent(event_id)) {
        return GuestAct::None;
    }
    if (bank == 4 && id == 3) {
        return client && rule_on ? GuestAct::PassGate : GuestAct::RestoreGate;
    }
    if (bank == 2003 && id == 2 && flag_arg == kTriggerFlag) {
        return client ? GuestAct::SkipTrigger : GuestAct::None;
    }
    return GuestAct::None;
}

/// The guest's copy restarts while it lies at HP 0: one request per grab.
constexpr double kRequestRepeat = 20.0;
constexpr bool SendRequest(double since_last_s) {
    return !(since_last_s >= 0.0 && since_last_s < kRequestRepeat);
}

/// The host's answer to a guest's grab.
enum class HostPlan : std::uint8_t {
    Start,     ///< 9420 on the host, then the party's travel to the Gaol
    RuleOff,   ///< "Anyone can be abducted" off: the guest's death is a plain death
    Woke,      ///< 9423 on (the kidnap was done): a later grab is a plain death, as in the game
    NotHere,   ///< not in the world / no member / not in the Snatchers' area / the guest elsewhere
    Busy,      ///< the host is down, travelling or already taking the party to the Gaol
    Stale,     ///< the request is too old (a load or a long hitch in between)
};
constexpr const char* HostPlanName(HostPlan p) {
    return p == HostPlan::Start     ? "start"
           : p == HostPlan::RuleOff ? "the rule is off"
           : p == HostPlan::Woke    ? "the kidnap was done already (9423)"
           : p == HostPlan::NotHere ? "not here"
           : p == HostPlan::Busy    ? "busy"
                                    : "stale";
}
constexpr double kRequestMaxAge = 10.0;
struct HostView {
    bool rule_on = true;
    bool in_world = false;      ///< world up, no load
    int members = 0;            ///< member tasks
    bool busy = false;          ///< down / limbo / a kidnap or travel under way
    bool woke = false;          ///< 9423
    std::uint32_t host_map = 0; ///< 0xAABBCCDD
    std::uint32_t guest_map = 0;///< the grabbed guest's (0 unknown)
    double age_s = 0;           ///< since the request came
};
/// An area with Snatchers: Cathedral Ward (m24) or the Forbidden Woods (m27).
constexpr bool SnatcherArea(std::uint32_t map) {
    return (map >> 24) == 24u || (map >> 24) == 27u;
}
constexpr HostPlan PlanHost(const HostView& v) {
    if (!v.rule_on) return HostPlan::RuleOff;
    if (!(v.age_s >= 0.0 && v.age_s <= kRequestMaxAge)) return HostPlan::Stale;
    if (v.woke) return HostPlan::Woke;
    if (!v.in_world || v.members <= 0 || !SnatcherArea(v.host_map)) return HostPlan::NotHere;
    if (v.guest_map != 0 && v.guest_map != 0xffffffffu && (v.guest_map >> 24) != (v.host_map >> 24)) {
        return HostPlan::NotHere;
    }
    if (v.busy) return HostPlan::Busy;
    return HostPlan::Start;
}

/// After 9420 on the host: its common 9421 sets the respawn point within a few frames.
enum class HostStep : std::uint8_t { Wait, Travel, GiveUp };
constexpr double kRespawnWait = 3.0;
constexpr HostStep PlanHostStep(std::uint32_t respawn_record, double since_s) {
    if (respawn_record == kGaolReturnPoint) return HostStep::Travel;
    return since_s < kRespawnWait ? HostStep::Wait : HostStep::GiveUp;
}

/// Guest: the host's travel to the Gaol shortly after this guest asked for the kidnap is the
/// party's kidnap: the grabbed guest's follow waits for its own death (wipe follow), never a follow
/// of its dying body. `since_sent_s` < 0: never asked.
constexpr double kFollowWindow = 30.0;
constexpr bool FollowWaitsForDeath(double since_sent_s, std::uint32_t travel_lamp) {
    return since_sent_s >= 0.0 && since_sent_s < kFollowWindow && travel_lamp == kGaolReturnPoint;
}

}  // namespace coop::kidnap
