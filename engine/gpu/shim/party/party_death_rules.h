// SPDX-License-Identifier: GPL-3.0-or-later
// Deaths, rests and same-map lamps in a party: the decisions (playtest 1 batch 1,
// docs/party/playtest1_triage.md bugs 3, 6, 7, 10, 12, 13, 18). Pure logic, no game access
// (unit test: tests/test_party_death_rules.cpp); the game side is party_b2.cpp (HostDeathIntercept,
// HostDownTick, HostRestLoad), party_split.cpp (the question) and party_guestsvc.cpp (requests).
//
// Rule of thumb (user, after playtest 1): where a vanilla loading screen fixes a glitch, the
// loading screen is the default; the seamless (no-load) variant is an opt-in switch.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace coop::deathrules {

/// An env switch that is ON only when set to a value starting with '1' (the opt-in switches:
/// BB_PARTY_B2_HOST_REVIVE, BB_PARTY_INWORLD, BB_PARTY_GUEST_LAMP_INWORLD).
inline bool OptIn(const char* v) {
    return v && v[0] == '1';
}

/// Members counted down: the most of what they reported ("member_down") and what is seen (their
/// chr at HP 0 / an early down notice). Never more than `members`.
inline int PartyDown(int members, int reported, int seen) {
    if (members <= 0) {
        return 0;
    }
    return std::min(members, std::max(0, std::max(reported, seen)));
}

/// Every member is down too: the party wipes (one respawn load for everyone).
inline bool Wiped(int members, int reported, int seen) {
    return members > 0 && PartyDown(members, reported, seen) >= members;
}

// ---- The host dies (SoloPlayDeath_2's funnel call) ----
enum class HostDeathPlan {
    Stock,          ///< no party / not the host: the game's own death (not taken, nothing broadcast)
    PartyLoad,      ///< the stock funnel: one respawn load, the party follows (default)
    Wipe,           ///< every member is down too: the stock funnel, everyone loads
    BossLimbo,      ///< in a boss fight: the host waits (spectates) until the boss is gone
    ReviveInPlace,  ///< opt-in BB_PARTY_B2_HOST_REVIVE=1: revived at its lamp in the loaded map, no load
    AloneLoad,      ///< opt-in revive, but the lamp is in another map: the host loads it alone
};
inline const char* HostDeathPlanName(HostDeathPlan p);
struct HostDeathView {
    bool party_on = true;      ///< B2 on, this player hosts (or the solo revive fixture)
    int members = 0;           ///< member tasks in the world
    int down_reported = 0;     ///< members that said they are down
    int down_seen = 0;         ///< members seen at HP 0 (early notices)
    bool boss_fight = false;
    /// The boss was killed between the host's HP 0 and this taken death (KilledAfterHp0): the fight
    /// is over, but the death still counts as one in it (hostcheck run C).
    bool boss_killed_late = false;
    bool arena_revive_on = true;  ///< BB_PARTY_HOST_LIMBO_REVIVE (the arena revive's switch)
    bool revive_opt_in = false;
    bool lamp_here = false;    ///< the host's last lamp has a spawn spot in the loaded map
    bool alone_opt = true;     ///< BB_PARTY_HOST_DEATH_ALONE (revive mode only)
    bool solo_fixture = false; ///< BB_PARTY_B2_HOST_REVIVE_SOLO=1 (test, no members needed)
    /// The death's own script moved the respawn point to another map (ScriptedDeathWarp: the
    /// Snatcher's kidnap to the Hypogean Gaol): always the party's respawn load, never the revive
    /// or a lone load.
    bool scripted_warp = false;
};
inline HostDeathPlan PlanHostDeath(const HostDeathView& v) {
    if (!v.party_on || (v.members <= 0 && !v.solo_fixture)) {
        return HostDeathPlan::Stock;
    }
    if (Wiped(v.members, v.down_reported, v.down_seen)) {
        return HostDeathPlan::Wipe;
    }
    // A boss killed while the host's death animation ran: the limbo too, so PlanDownTick waits for
    // the defeat to be saved and gets the host up in the arena (not at the lamp: run C). With the
    // arena revive off the death is planned as before (outside a fight).
    if (v.boss_fight || (v.boss_killed_late && v.arena_revive_on)) {
        return HostDeathPlan::BossLimbo;
    }
    if (!v.revive_opt_in || v.scripted_warp) {
        return HostDeathPlan::PartyLoad;
    }
    if (!v.lamp_here) {
        return v.alone_opt ? HostDeathPlan::AloneLoad : HostDeathPlan::PartyLoad;
    }
    return HostDeathPlan::ReviveInPlace;
}
/// The plan takes the game's funnel call (true) or lets it run (false: a load now).
inline bool PlanTakesFunnel(HostDeathPlan p) {
    return p == HostDeathPlan::BossLimbo || p == HostDeathPlan::ReviveInPlace;
}

// ---- A guest gets the host's death event (K12, the HostDead Lua state 0x1381870) ----
/// The guest's HostDead state is skipped: a B2 party guest (session role 6) whose chr is not an
/// invader (type 2: the "PK success" path) or type 8 (the game returns early for it anyway).
/// `skip_from` is BB_PARTY_HOSTDEAD_SKIP (0: never; N: from the Nth host death on), `nth` this
/// host death's number in the run (1 = the first).
inline bool SkipGuestHostDead(bool party_on, int role, int chr_type, int skip_from, int nth) {
    return party_on && role == 6 && chr_type != 2 && chr_type != 8 && skip_from > 0 && nth >= skip_from;
}

// ---- Each frame while the host is down (a taken death) ----
enum class DownTick {
    Wait,           ///< not yet (the revive delay, or the boss still fights)
    Wipe,           ///< everyone is down now: the stock funnel (a wipe load)
    Revive,         ///< revive in place (opt-in) at the lamp here
    PartyLoad,      ///< the stock funnel: the respawn load for everyone
    AloneLoad,      ///< the lamp is in another map: the host loads it alone (revive mode)
    ArenaRevive,    ///< the boss died while the host lay in limbo: it gets up in the arena, no load
};
inline const char* DownTickName(DownTick d);
struct DownView {
    int members = 0;
    int down_reported = 0;
    int down_seen = 0;
    bool limbo = false;        ///< the death came in a boss fight
    bool boss_active = false;  ///< the boss fight is still on
    /// The boss's HP reached 0 but the game's defeat event has not cleared its bar yet (it saves the
    /// defeat flag): a respawn load now would bring the boss back (Father Gascoigne, host in limbo).
    bool boss_dying = false;
    /// The limbo's boss was seen killed (its bar up over a chr at HP 0), not a bar that just went.
    bool boss_killed = false;
    /// BB_PARTY_HOST_LIMBO_REVIVE (default on; "0": the respawn load as before).
    bool arena_revive_on = true;
    /// A living member is in the world to get up beside (the one the host spectated).
    bool member_here = false;
    bool revive_opt_in = false;
    bool lamp_here = false;
    bool scripted_warp = false;  ///< as HostDeathView::scripted_warp (a death in a fight: after the limbo)
    double down_for = 0;       ///< s since the death was taken
    double delay = 1.0;        ///< BB_PARTY_B2_HOST_REVIVE_DELAY
    /// The limbo's fight (LimboFightView): members in the fight when the host died, and how many of
    /// them are out of it now (down, or gone: Hunter's Mark, lamp, Leave, disconnect).
    int fighters = -1;         ///< -1: not tracked (no limbo)
    int fighters_out = 0;
    /// A member that left by a load into this world (a Mark to the host's lamp) is still loading:
    /// the wipe waits for its arrival (one load after the other, not two at once).
    bool fighter_settling = false;
};
/// The host's boss-fight limbo has nobody left fighting (player report 2026-10-22, host_limbo_mark:
/// the last living guest used the Hunter's Mark, the host spectated its own corpse until the guest
/// lamped to the Dream): every member that was in the fight is down or gone from it.
inline bool LimboFightEmpty(int fighters, int out) {
    return fighters >= 0 && out >= fighters;
}
inline DownTick PlanDownTick(const DownView& v) {
    // Recounted every frame: a member whose death arrives a moment after the host's turns the
    // host's lone death into a wipe (playtest 1 bug 18: "0 down" counted 0.3 s too early).
    if (Wiped(v.members, v.down_reported, v.down_seen)) {
        return DownTick::Wipe;
    }
    // 0.7.4 (user decision): the last living member leaving the fight by any way (Hunter's Mark,
    // Bold Hunter's Mark, a lamp, its death, Leave, a disconnect) counts as a wipe: everyone
    // respawns as in one. Only while the boss still fights (its kill: the arena revive / load below).
    if (v.limbo && v.boss_active && !v.boss_killed && LimboFightEmpty(v.fighters, v.fighters_out)) {
        return v.fighter_settling ? DownTick::Wait : DownTick::Wipe;
    }
    if (v.down_for < v.delay) {
        return DownTick::Wait;
    }
    if (v.limbo && (v.boss_active || v.boss_dying)) {
        return DownTick::Wait;
    }
    // User request (2026-10-21): the guests killed the boss while the host lay dead in the fight:
    // the host gets up in the arena beside the member it followed and nobody loads (the defeat is
    // saved by now: the wait above). A wipe (above), a bar that went without a kill, or the switch
    // off: the respawn load as before.
    if (v.limbo && v.boss_killed && v.arena_revive_on && v.member_here) {
        return DownTick::ArenaRevive;
    }
    if (!v.revive_opt_in || v.scripted_warp) {
        return DownTick::PartyLoad;
    }
    if (!v.lamp_here) {
        return DownTick::AloneLoad;
    }
    return DownTick::Revive;
}

// ---- Scripted death warps (quest_audit.md "Scripted death warps") ----
// A host death whose own EMEVD moves the respawn point (2003[23]) to another map. The party's
// respawn load takes everyone there with one loading screen (the guests replay it as a lamp warp
// to the host's last lamp record, which the script has just set); the in-place revive or a lone
// load would send the host away alone and leave the guests where they are (a player report: the
// host kidnapped alone, the guests shown its waking scene in the Cathedral Ward). The deaths in the
// game data (every 2003[23] outside the lamp events): common 9421 (the Snatcher's kidnap: 9420 from
// 12400780 / 12700500 -> the Gaol 2802959, until the waking scene sets 9423), 9404 (the first death
// with Insight after the first Dream -> the Dream 2102961), 9400 (the prologue's first death -> the
// Dream 2102962; the clinic has no multiplayer). All three end on a client: never a guest's.
constexpr std::uint32_t kGaolReturnPoint = 2802959;  ///< ReturnPointParam row of the Gaol cell (m28)
/// The kidnap is pending or done by this death: 9420 (the trigger) on, or the respawn record on the
/// Gaol, while the waking scene has not played (9423 off; 9421 ends once it is on: a later grab is a
/// plain death in the game data too).
inline bool KidnapDeath(std::uint32_t lamp, bool trigger_9420, bool woke_9423) {
    return !woke_9423 && (trigger_9420 || lamp == kGaolReturnPoint);
}
/// The respawn record changed after the player was last seen alive and points into another map than
/// the one it died in (area << 8 | block of each, 0 = unknown: no).
inline bool RespawnMovedAtDeath(std::uint32_t alive_lamp, std::uint32_t lamp, std::uint32_t lamp_map,
                                std::uint32_t death_map) {
    const bool known = alive_lamp != 0 && alive_lamp != 0xffffffffu && lamp != 0 && lamp != 0xffffffffu;
    return known && lamp != alive_lamp && lamp_map != 0 && death_map != 0 && lamp_map != death_map;
}
inline bool ScriptedDeathWarp(bool kidnap, bool respawn_moved) {
    return kidnap || respawn_moved;
}

// ---- Where the host gets up in the arena (DownTick::ArenaRevive) ----
// Its corpse spot (where it stood when it died, by its bloodstain) when that is in the arena with
// the member it followed and the death was no fall; else the followed member's own spot (a spot a
// living chr stands on: never inside a wall or over the void). Both are a short warp from where
// the spectating chr was carried (a 250 m in-map warp broke occlusion, playtest 1 bug 7).
constexpr double kArenaCorpseNear = 40.0;  ///< m (horizontal) from the followed member
constexpr double kArenaCorpseRise = 4.0;   ///< m (height) from the followed member
inline bool ArenaReviveAtCorpse(bool corpse_known, bool fall_death, double horiz_m, double dy_m) {
    return corpse_known && !fall_death && horiz_m <= kArenaCorpseNear && std::fabs(dy_m) <= kArenaCorpseRise;
}

// ---- Where a player gets up after a FALL death (fallsafe, user rule 2026-10-21) ----
// A fall death never gets up at its corpse spot (the bottom of the fall, often under the level: a
// report showed a revived guest under Cathedral Ward looking up at it). It gets up where the game
// itself puts the bloodstain: the chr's last safe ground spot, PlayerIns +0x400 -> +0x30 (position)
// / +0x3C (rotation), read by 0x1904660 / 0x1904690 for the stain record 0x13D2910 (its +0x00) and
// by the game's own safe-spot warp 0x13CBAD0. The game's value as it was at HP 0, never a computed
// height. Usable: read, finite, not the origin (no tracker: zeros) and within kSafeSpotNear m
// (horizontal) of where the chr stood at HP 0 (hostcheck: the stain up to ~11 m from the death spot).
constexpr double kSafeSpotNear = 60.0;
inline bool SafeSpotUsable(bool read_ok, const float safe[3], const float died[3]) {
    if (!read_ok || !std::isfinite(safe[0]) || !std::isfinite(safe[1]) || !std::isfinite(safe[2])) {
        return false;
    }
    if (std::fabs(safe[0]) < 0.01f && std::fabs(safe[1]) < 0.01f && std::fabs(safe[2]) < 0.01f) {
        return false;
    }
    if (!std::isfinite(died[0]) || !std::isfinite(died[2])) {
        return true;  // nothing to compare with (the death spot unknown): the game's value alone
    }
    const double dx = double(safe[0]) - double(died[0]), dz = double(safe[2]) - double(died[2]);
    return std::sqrt(dx * dx + dz * dz) <= kSafeSpotNear;
}
/// The arena revive's spot (host DownTick::ArenaRevive, guest GuestLimboEnd::ArenaRevive). A fall:
/// the game's safe spot, else on the followed / nearest living member's spot (never the corpse).
/// Any other death: its corpse spot when that is within 40 m / 4 m of the member, else the member's.
enum class ArenaSpot { Corpse, SafeFloor, Member };
inline const char* ArenaSpotName(ArenaSpot a) {
    return a == ArenaSpot::Corpse ? "where it died" : a == ArenaSpot::SafeFloor ? "on its last safe floor (a fall death)"
                                                                               : "on the living member's spot";
}
inline ArenaSpot PlanArenaReviveSpot(bool corpse_known, bool fall_death, double horiz_m, double dy_m, bool safe_ok) {
    if (fall_death) {
        return safe_ok ? ArenaSpot::SafeFloor : ArenaSpot::Member;
    }
    return ArenaReviveAtCorpse(corpse_known, false, horiz_m, dy_m) ? ArenaSpot::Corpse : ArenaSpot::Member;
}
/// A revive "where it lies" (GuestDeathAloneToOwnLamp: up again, then the alone travel to its own
/// lamp in another map). A fall: warped onto the game's safe spot first; with none, no revive here
/// (the caller's respawn load). Any other death: where it lies, as before.
/// The order of a fall revive (fallsafe, user report: the host was put on its safe spot and then
/// still got "YOU DIED"): nothing before the game's death is taken (the host's death funnel / a
/// guest's held death); then a body far below its spot (out of the world) is moved there while
/// still dead; the revive itself runs once the body stands on the spot (or kFallMoveWait s after
/// the move, whichever first) and has stood there kFallSettle s: the game's own warp updates the
/// position at once but its physics a few frames later, and a revive in the same frame met the kill
/// plane again (live r11: the guest's HP 0 again 36 ms after the revive, "YOU DIED", then a load).
constexpr double kFallMoveWait = 3.0;
constexpr double kFallSettle = 1.0;
enum class FallReviveStep { Wait, MoveBody, Revive };
inline FallReviveStep PlanFallReviveStep(bool death_taken, bool body_far_below, bool moved, bool at_spot,
                                         double since_move) {
    if (!death_taken) {
        return FallReviveStep::Wait;
    }
    if (body_far_below && !moved) {
        return FallReviveStep::MoveBody;
    }
    if (moved && since_move < (at_spot ? kFallSettle : kFallMoveWait)) {
        return FallReviveStep::Wait;
    }
    return FallReviveStep::Revive;
}
/// Where a fall death (off a ledge or out of the world) gets up outside a boss arena's revive (user
/// decision 2026-10-21: at a lamp, so the walk back stays the penalty; its bloodstain stays on its
/// last safe floor). In place with no load: its own last lamp when that is in the loaded map; a
/// guest otherwise the host's last lamp in the party's map (beside it). Never a load for a fall (a
/// respawn load after a death out of the world lands in the Hunter's Dream, as single player does):
/// with neither lamp in the loaded map, its last safe floor. `inplace_on` off (a guest's System >
/// Deaths & revives) or no fall: the respawn as before.
/// chalicelamp (a guest's report, "I died on the elevator, got teleported above the dungeon"): in a
/// Chalice Dungeon the lamps' spots are not in the party's table (LampTableSpotTrusted: the layout is
/// generated; the template's spot for lamp 2902954 was 180 m off, in the air) and the last safe floor
/// may be a lift car that has since moved on (the report's first death: the car's top stop). There a
/// guest's fall gets up beside the nearest living member (`dungeon_member`: in a dungeon outside a
/// boss fight, party_chalice_rejoin on, a living member here), a spot a living chr stands on.
enum class FallRespawn { OwnLamp, HostLamp, SafeFloor, Respawn, BesideMember };
inline const char* FallRespawnName(FallRespawn f) {
    return f == FallRespawn::OwnLamp        ? "its own last lamp"
           : f == FallRespawn::HostLamp     ? "the host's last lamp"
           : f == FallRespawn::SafeFloor    ? "its last safe floor (no lamp in the loaded map)"
           : f == FallRespawn::BesideMember ? "beside the nearest living member (Chalice Dungeon)"
                                            : "the respawn as before";
}
inline FallRespawn PlanFallRespawn(bool fall_death, bool inplace_on, bool own_lamp_here, bool host_lamp_here, bool safe_ok,
                                   bool host_role, bool dungeon_member = false) {
    if (!fall_death || !inplace_on) {
        return FallRespawn::Respawn;
    }
    if (dungeon_member && !host_role) {
        return FallRespawn::BesideMember;
    }
    if (own_lamp_here) {
        return FallRespawn::OwnLamp;
    }
    if (!host_role && host_lamp_here) {
        return FallRespawn::HostLamp;
    }
    return safe_ok ? FallRespawn::SafeFloor : FallRespawn::Respawn;
}
enum class LiesRevive { WhereItLies, SafeFloor, Load };
inline LiesRevive PlanLiesRevive(bool fall_death, bool safe_ok) {
    if (!fall_death) {
        return LiesRevive::WhereItLies;
    }
    return safe_ok ? LiesRevive::SafeFloor : LiesRevive::Load;
}
/// PR #6's wait after a limbo's boss died: its bar still up over the dead chr (the game's defeat
/// event, which saves the defeat flag, has not run), or less than 3 s since the bar went; at most
/// 20 s from the moment the boss was gone.
inline bool DefeatPending(double since_gone_s, bool bar_dying) {
    return since_gone_s < 20.0 && (bar_dying || since_gone_s < 3.0);
}

// ---- The boss is killed between a player's HP 0 and its taken death ----
// The game takes a death 8-13 s after HP 0 (the death animation; live arenaguest A1, hostcheck run
// C: the host's HP 0 at +169.5 s, the boss killed at +181.4 s, the death taken at +182.2 s with no
// living bar left, so the host got the lamp revive). Such a death still counts as one in the fight:
// a living boss bar was up at HP 0, that boss was killed after it (its bar's last living frame no
// earlier than HP 0) and the death is taken within kLateKillWindow s of HP 0. Both roles: the
// guest's held death (PlanGuestLimboEnd) and the host's (HostDeathView::boss_killed_late).
constexpr double kLateKillWindow = 60.0;
inline bool KilledAfterHp0(bool hp0_in_fight, bool boss_killed, double hp0_at, double gone_at, double taken_at) {
    return hp0_in_fight && boss_killed && hp0_at > 0 && gone_at >= hp0_at && taken_at - hp0_at < kLateKillWindow;
}

// ---- A guest's boss-fight limbo ends (its held death; user request 2026-10-21) ----
// The boss is gone while a guest lay dead in the fight. Killed (not a bar that just went), the
// switch on (BB_PARTY_HOST_LIMBO_REVIVE, the host's arena revive's) and a living member here:
// once the defeat is saved (DefeatPending) the guest gets up in the arena, no load, its stain stays
// where it fell. Otherwise its respawn at the host's lamp as before. A wipe never gets here (the
// host's death load takes the guest along first).
enum class GuestLimboEnd { Lamp, Wait, ArenaRevive };
inline const char* GuestLimboEndName(GuestLimboEnd g) {
    return g == GuestLimboEnd::Lamp ? "the host's lamp" : g == GuestLimboEnd::Wait ? "wait for the defeat" : "arena revive";
}
inline GuestLimboEnd PlanGuestLimboEnd(bool boss_killed, bool switch_on, bool defeat_pending, bool member_here) {
    if (!boss_killed || !switch_on) {
        return GuestLimboEnd::Lamp;
    }
    if (defeat_pending) {
        return GuestLimboEnd::Wait;
    }
    return member_here ? GuestLimboEnd::ArenaRevive : GuestLimboEnd::Lamp;
}

// ---- The host's record of members that are down (issue #4 item 6) ----
// A member's "member_down" notice is sent once per HP-0 period, so the record must last until the
// member is back: a confirmed revival ("member_revived"), a new chr ("arrived": a rejoin or a
// respawn load) or the member gone from the roster (left / replaced). It used to be dropped after
// 90 s, so a guest that spectated longer than that no longer counted when the host died: no wipe,
// both players stuck in a boss fight's limbo. Reconcile() also clears a record whose member's own
// vitals say alive for kDownAliveConfirm s (a revive notice that never came), but only once the
// notice is kDownNoticeGrace s old (vitals can lag the notice).
constexpr double kDownNoticeGrace = 3.0;
constexpr double kDownAliveConfirm = 3.0;
struct MemberNow {
    int slot = -1;
    bool in_roster = true;    ///< the slot is in the party's roster (connected or kept while lost)
    bool vitals_fresh = false;///< its vitals are recent and read from a live chr
    bool alive = false;       ///< those vitals say hp > 0
};
class DownLedger {
public:
    void Down(int slot, double now) {
        if (slot > 0 && slot < 16) {
            e_[slot] = Entry{now, -1.0};
        }
    }
    /// A confirmed revival, a new chr (rejoin / respawn load) or a leave.
    void Clear(int slot) {
        e_.erase(slot);
    }
    void ClearAll() {
        e_.clear();
    }
    bool Has(int slot) const {
        return e_.count(slot) != 0;
    }
    int Count() const {
        return int(e_.size());
    }
    /// Against the members' current state; returns how many records it cleared (the last slot in
    /// *cleared_slot, for the log).
    template <class Container>
    int Reconcile(const Container& members, double now, int* cleared_slot = nullptr) {
        int cleared = 0;
        for (auto it = e_.begin(); it != e_.end();) {
            const MemberNow* m = nullptr;
            for (const MemberNow& x : members) {
                if (x.slot == it->first) {
                    m = &x;
                    break;
                }
            }
            bool drop = false;
            if (!m || !m->in_roster) {
                drop = true;  // left the party (or replaced: a new member has another record)
            } else if (m->vitals_fresh && m->alive && now - it->second.since >= kDownNoticeGrace) {
                if (it->second.alive_since < 0) {
                    it->second.alive_since = now;
                }
                drop = now - it->second.alive_since >= kDownAliveConfirm;
            } else {
                it->second.alive_since = -1.0;  // dead (or unknown) again: the confirm starts over
            }
            if (drop) {
                if (cleared_slot) {
                    *cleared_slot = it->first;
                }
                ++cleared;
                it = e_.erase(it);
            } else {
                ++it;
            }
        }
        return cleared;
    }

private:
    struct Entry {
        double since = 0;
        double alive_since = -1.0;
    };
    std::map<int, Entry> e_;
};
/// The host's boss-fight limbo: who was fighting when the host died and who has left the fight
/// since (DownView fighters / fighters_out / fighter_settling). Host, under g_dead_mu.
class LimboFight {
public:
    /// The limbo starts: the members in the roster now (MembersNow, the host excluded).
    template <class Container>
    void Begin(const Container& members) {
        on_ = true;
        in_.clear();
        left_.clear();
        for (const MemberNow& m : members) {
            if (m.in_roster && m.slot > 0) in_.push_back(m.slot);
        }
    }
    void End() {
        on_ = false;
        in_.clear();
        left_.clear();
    }
    bool On() const { return on_; }
    /// A fighter left the fight (Hunter's Mark, a lamp, Leave). `settle_until`: a load into this world
    /// still running until then (its "arrived" ends it sooner); 0 when it went elsewhere. True: newly.
    bool Left(int slot, double settle_until) {
        if (!on_ || !Has(slot)) return false;
        const bool fresh = left_.count(slot) == 0;
        left_[slot] = settle_until;
        return fresh;
    }
    /// Its load into this world is over ("arrived" at `now`): settled kLimboArriveGrace s later (still
    /// out of the fight). The guest sends "arrived" a moment before its re-join ends, and a host
    /// travel that reaches it before that end is dropped (live bl1: the wipe's load 13 ms after it).
    static constexpr double kLimboArriveGrace = 2.5;
    void Arrived(int slot, double now) {
        auto it = left_.find(slot);
        if (it != left_.end()) it->second = now + kLimboArriveGrace;
    }
    int Fighters() const { return on_ ? int(in_.size()) : -1; }
    /// Fighters out: left, gone from the roster, or `down(slot)` (reported down).
    template <class Container, class DownFn>
    int Out(const Container& members, DownFn down) const {
        int n = 0;
        for (int slot : in_) {
            bool here = false;
            for (const MemberNow& m : members) {
                if (m.slot == slot && m.in_roster) here = true;
            }
            n += (!here || left_.count(slot) || down(slot)) ? 1 : 0;
        }
        return n;
    }
    /// A fighter that left by a load into this world has not arrived yet (and `now` is before its cap).
    bool Settling(double now) const {
        for (const auto& kv : left_) {
            if (kv.second > now) return true;
        }
        return false;
    }

private:
    bool Has(int slot) const {
        for (int s : in_) {
            if (s == slot) return true;
        }
        return false;
    }
    bool on_ = false;
    std::vector<int> in_;
    std::map<int, double> left_;
};
/// Members seen at HP 0 right now (their own fresh vitals): HostDeathView / DownView down_seen.
template <class Container>
inline int SeenDown(const Container& members) {
    int n = 0;
    for (const MemberNow& m : members) {
        n += (m.in_roster && m.vitals_fresh && !m.alive) ? 1 : 0;
    }
    return n;
}

// ---- A guest's respawn at a lamp of the loaded map (issue #4 item 7) ----
// The in-place revive (no load) puts the chr on the lamp's spawn spot. After a FALL death that
// broke the guest's game (issue #4: a 27 m fall, revived 157 m from its lamp: camera underground,
// only forward movement, broken world); correct over seamless: then the vanilla respawn loading
// screen (a re-join load for this guest only). The distance alone does not matter: the issue's
// bundles had seven in-place revives 60-191 m from the lamp after normal deaths that all worked,
// so the 60 m rule of the first fix is gone (user decision: "fall state only").
// kGuestReviveFallDrop: the chr dropped at least this far in the 3 s before its death or after it
// (lethal falls in Bloodborne are well over 10 m; a void fall keeps dropping): a fall death.
constexpr double kGuestReviveFallDrop = 6.0;
// The game's own fall-death state, left on the chr after the death (issue #4 bundles: only the fall
// death had them; nine normal deaths had neither): PlayerIns +0x1E1 (5 bytes read as one value)
// bit 0x40 of its first byte and 0x04 of its fifth (0x04'0000'0040), and the HP module's +0x200 word
// bits 0x7800 (the fall read 0xF800; normal deaths 0x0000 or 0x8000 in the bundles). The +0x200
// word is no fall sign (revivecheck 2026-10-21 live, normal deaths with no drop: an enemy's kill
// 0x7000, ForceDeads 0xD000 and 0xF800 - the fall's own value - each sent to the loading screen
// as a fall), so only the +0x1E1 flags count (with the 6 m drop test above). kDeathBitsHp200:
// what the revive's "flags" step still clears (the revives after 0xD000 -> 0x8000 worked).
constexpr std::uint64_t kFallFlags1e1 = 0x0400000040ull;
constexpr std::uint16_t kDeathBitsHp200 = 0x7800;
inline bool FallDeathState(std::uint64_t f1e1, std::uint16_t /*hp200*/) {
    return (f1e1 & kFallFlags1e1) != 0;
}
/// [ChrCtrl +0x38] +0x48 on a revive: the game's own revive (0x13CBAD0) does `& 0xFC`. Bit 0 is the death
/// bit; bit 1 is set by a fall death and, left set, killed the revived chr again a frame later (player
/// report cainhurst_death_loop). `both` false = the old bit-0-only clear (BB_PARTY_REVIVE_BIT1=0).
inline std::uint8_t ReviveCtrl38Clear(std::uint8_t v, bool both = true) {
    return std::uint8_t(v & ~(both ? 3u : 1u));
}
enum class GuestRevive { InPlace, LoadFall, LoadFallState };
inline const char* GuestReviveName(GuestRevive g) {
    return g == GuestRevive::InPlace    ? "in place"
           : g == GuestRevive::LoadFall ? "a fall death: loading screen"
                                        : "the game's fall-death state: loading screen";
}
/// `fell_m`: how far it dropped around its death (negative / 0: no fall); `fall_known` false: no
/// height history (a death right after a load); `fall_state`: FallDeathState of the dead chr.
inline GuestRevive PlanGuestRevive(double fell_m, bool fall_known, bool fall_state) {
    if (fall_known && fell_m >= kGuestReviveFallDrop) {
        return GuestRevive::LoadFall;
    }
    if (fall_state) {
        return GuestRevive::LoadFallState;
    }
    return GuestRevive::InPlace;
}
/// The drop around a death from the height history: the highest point in the 3 s before the death
/// against the height at the death and now (a body falling into the void keeps dropping).
inline double FallDrop(double peak_before, double y_at_death, double y_now) {
    return peak_before - std::min(y_at_death, y_now);
}

// ---- The camera after an in-world revive (fallcam) ----
// A guest's fall death beside Djura's tower (an 18 m drop, the game's fall-death state on the chr),
// revived at the host's lamp 158 m away with no load: up with full HP, but the camera stayed on the
// sky and the own chr was never seen again ("perpetual spectate"; the host's death did not end it).
// WorldChrMan +0x68 is the game's fall-death camera latch: the ChrCam update 0x14368B0 goes to its
// mode 4 (the fall-die camera: it stops where the fall was and only turns after the body) while that
// byte is set, and leaves mode 4 (back to the follow camera, reset behind the chr) only once it is 0.
// Vanilla never revives after a fall death: the death load builds a new world with it 0, and the
// game's own safe-spot warp-and-revive 0x13CBAD0 writes 0 for the local player. Our revive cleared
// it only for a body moved up from far below; the lamp revive of an ordinary fall left it at 1
// (the report: "WorldChrMan +0x68 1"). A fall out of the world (the kill plane, smoke fl) never sets it.
// Every in-world revive or warp of the local player releases the latch and resets the camera behind
// the chr (the Lua CamReset 0x132F5A0: chr transform re-applied, [[+0x3B0] +0x58] +0x28 +0xC = 1).
struct ReviveCamera {
    bool release_latch = false;  // WorldChrMan +0x68 -> 0
    bool cam_reset = false;      // the camera behind the chr at its new spot
};
/// `latch_set`: WorldChrMan +0x68 is set; `warped`: the chr was put on a new spot (a lamp, a safe
/// floor, the arena revive) rather than revived where it lies.
inline ReviveCamera PlanReviveCamera(bool latch_set, bool warped) {
    ReviveCamera c;
    c.release_latch = latch_set;
    c.cam_reset = latch_set || warped;
    return c;
}
// The pitch (fallcam 2: after the release the camera kept the fall camera's steep downward look, an
// almost top-down view): leaving its mode 4 the ChrCam update hands the follow camera the current
// direction as its new angles (+0x13C reset request with +0x13D set: pitch / yaw from +0x148 /
// +0x14C). A reset request with +0x13D clear is the follow camera's own level reset (pitch +0x140 =
// 0, yaw behind the chr), what a new chr after a load starts with. LevelCameraDue: ask for that one
// (ChrCam put straight from mode 4 to 0, as its own 4 -> 0 switch writes) after a release, a warp
// or a ChrCam still in mode 4.
constexpr int kChrCamFallDieMode = 4;
inline bool LevelCameraDue(bool released, bool warped, int chrcam_mode) {
    return released || warped || chrcam_mode == kChrCamFallDieMode;
}
// The follow camera looks down at the chr from a little above (fl run 1: view y -0.00); the fall
// camera's leftover pitch read -0.78 .. -0.98 (fdj run r3). Steeper than this is not the level view.
constexpr double kReviveLookDownMax = -0.4;
// The ladder (fallcam 3, a player's report: "dying on a ladder makes it so you can only walk
// forward"). The behavior script asks the chr's ladder action state (HKS env GetLadderActState
// 0x1259850 -> 0x1290E50: the int8 at [ChrCtrl +0x38] +0x60; IsLadderAct 0x1259860: it is not -1)
// and drives the chr as on a ladder while it is set. A death on a ladder leaves it; a vanilla death
// load builds a new chr (its action ctrl's constructor 0x150A480 writes +0x60 = 0xFF). An in-world
// revive keeps the chr: every revive / in-world warp of the local player puts it back to -1.
constexpr int kNoLadder = -1;
constexpr int kLadderUnread = -1000;  // no ChrCtrl / action ctrl to read
inline bool LadderLeftover(int ladder_act) {
    return ladder_act >= 0 && ladder_act <= 127;  // a ladder action's state index
}
// The walk check after it (smoke ld): a stick walk toward each of four points 4 m away must move the
// chr at least kWalkMin m toward it (a chr stuck in the ladder's climbing axis moves one way only).
constexpr double kWalkMin = 1.5;
inline bool WalkedToward(double dx, double dz, double tx, double tz) {
    const double len = std::sqrt(tx * tx + tz * tz);
    return len > 0.0 && (dx * tx + dz * tz) / len >= kWalkMin;
}
// The check after a revive (logged; smoke fl / fd): the render camera follows the chr (within
// kReviveCamNear m of its physics position: the follow camera sits 3-6 m behind, a wall pushes it
// nearer), the latch is off, and the chr is drawn where it stands (ChrCtrl's world matrix within
// kReviveDrawnOff m of the physics position; the fall-die camera looked from 150 m away).
constexpr double kReviveCamNear = 12.0;
constexpr double kReviveDrawnOff = 1.0;
enum class ReviveView { Ok, FallCamera, CameraAway, DrawnAway, NoCamera, Steep };
/// `cam_dist`: render camera to the chr's physics position (NaN: no camera frame); `drawn_off`:
/// drawn position to physics position (NaN: unknown, not judged); `latch`: WorldChrMan +0x68;
/// `look_y`: the camera's view direction, up component (NaN: not judged).
inline ReviveView JudgeReviveView(double cam_dist, double drawn_off, bool latch,
                                  double look_y = std::numeric_limits<double>::quiet_NaN()) {
    if (latch) {
        return ReviveView::FallCamera;
    }
    if (!std::isfinite(cam_dist)) {
        return ReviveView::NoCamera;
    }
    if (cam_dist > kReviveCamNear) {
        return ReviveView::CameraAway;
    }
    if (std::isfinite(drawn_off) && drawn_off > kReviveDrawnOff) {
        return ReviveView::DrawnAway;
    }
    if (std::isfinite(look_y) && look_y < kReviveLookDownMax) {
        return ReviveView::Steep;
    }
    return ReviveView::Ok;
}
inline const char* ReviveViewName(ReviveView v) {
    switch (v) {
    case ReviveView::Ok: return "ok";
    case ReviveView::FallCamera: return "BAD: the fall-death camera is still on";
    case ReviveView::CameraAway: return "BAD: the camera is away from the chr";
    case ReviveView::DrawnAway: return "BAD: the chr is drawn away from where it stands";
    case ReviveView::NoCamera: return "no camera frame (not judged)";
    case ReviveView::Steep: return "BAD: the camera looks steeply down (the fall camera's pitch)";
    }
    return "?";
}

// ---- Lifts and moving platforms (liftfall) ----
// A player's report (Nightmare of Mensis): the long lift 2601240 drops 60 m at ~4 m/s, and the fall
// recovery nets (6 s of dropping more than 1.5 m/s) warped the host back to its lamp mid-ride and
// put the summoned guest back on the top floor while the car went on down (then a fall down the
// empty shaft). PlayerIns +0x1E5 bit 0x02 (the fifth byte of the +0x1E1 flags) is set on a REMOTE
// chr standing in a lift car (the report: all 62 samples on lift rides, none elsewhere; off within a
// second of leaving the car, on for a body lying in it); the local chr carried it on the library lift
// (liftfix: 0200085098 / 0200005098 the whole ride, cleared the moment the chr fell off), not in liftfall's
// el1 on lift 2601240 (0000005098), so the game side also asks whether a lift car plays a ride animation
// where the chr stands (worldact::OnMovingLiftCar), and the rate tests below hold without either.
constexpr std::uint64_t kRideFlag1e1 = 0x0200000000ull;
inline bool RidingObject(std::uint64_t f1e1) {
    return (f1e1 & kRideFlag1e1) != 0;
}
// A lift's car is steady and slow (most Bloodborne lifts ~5 m/s; Cainhurst Castle's, the fastest seen,
// 12.7 m/s at its peak: liftfix run 5); a fall out of the world is not (the kill plane of m24_01, ~1470 m
// below the Great Bridge, in ~30 s: ~49 m/s on average). A drop faster than this per sample is a fall
// even with the ride flag on (a stale flag never hides one).
constexpr double kLiftMaxRate = 16.0;
/// One recovery sample: the chr dropped `dy` m in `dt` s. A fall sample: at least `min_rate` m/s
/// down, and not a ride on a moving object at a lift's rate.
inline bool FallSample(double dy, double dt, double min_rate, bool riding) {
    if (!(dt > 0.0) || !std::isfinite(dy)) {
        return false;
    }
    const double rate = dy / dt;
    return rate >= min_rate && !(riding && rate < kLiftMaxRate);
}
// The nets fire after kFallRecoverySeconds of fall samples and only for a real fall's drop over that
// time: a free fall covers well over 100 m in 6 s; a lift that never set the ride flag 25-30 m.
// liftfix: and only once a sample of that fall went faster than any lift (`peak_rate`, m/s, the
// fastest sample since the fall began): a free fall passes kLiftMaxRate within ~1.6 s, while a lift
// never does, listed or not (the Chalice Dungeons' generated lifts, a 60 m ride whose car the ride
// test missed: 0.7.1 still took 40 m of a 5 m/s ride in 8 s for a fall).
constexpr double kFallRecoverySeconds = 6.0;
constexpr double kFallRecoveryMinDrop = 40.0;
inline bool FallRecoveryDue(double falling_for, double dropped_m, double peak_rate) {
    return falling_for >= kFallRecoverySeconds && dropped_m >= kFallRecoveryMinDrop && peak_rate >= kLiftMaxRate;
}
/// Where the host-world fall recovery puts a guest back (lamprule run 7: a title-joined guest placed
/// beside the host before the floor's collision had loaded fell from y -38 to -1500 and died; it had
/// never stood still, so there was no spot to go back to). The game's last safe ground spot near
/// where it last stood, else where it last stood, else beside the host (its known position, same
/// map), else nothing (the game's own fall death).
enum class FallBack { GameSafe, Stable, Host, None };
inline FallBack FallBackTarget(bool game_safe_usable, bool have_stable, bool have_host) {
    if (have_stable) return game_safe_usable ? FallBack::GameSafe : FallBack::Stable;
    if (game_safe_usable) return FallBack::GameSafe;
    return have_host ? FallBack::Host : FallBack::None;
}

/// A title-joined guest's move beside the host after its load (the forced placement already put it
/// at the host's spot): none when it already stands within kBesideSkipM of the target; otherwise
/// only once the map is known and it stands on loaded ground (the game's safe spot set near it),
/// at most kBesideWaitS after the load (then none: the game's own placement stands).
constexpr float kBesideSkipM = 3.0f;
constexpr double kBesideWaitS = 10.0;
enum class BesideMove { Skip, Wait, Warp };
inline BesideMove PlanBesideMove(float dist_m, bool map_known, bool grounded, double since_load_s) {
    if (dist_m >= 0.0f && dist_m <= kBesideSkipM) return BesideMove::Skip;
    if (map_known && grounded) return BesideMove::Warp;
    return since_load_s >= kBesideWaitS ? BesideMove::Skip : BesideMove::Wait;
}
/// The drop test's top (PlanGuestRevive): the highest point since the chr last stood on a moving
/// object. A ride down (12 m in the 3 s window at a lift's rate) is no fall; a fall off the car or
/// onto it counts from where it left the car. `hist`: {t, y, riding} samples in time order.
struct HeightSample {
    double t = 0, y = 0;
    bool riding = false;
};
template <class Container>
inline double PeakSinceRide(const Container& hist, double y_at_death) {
    double peak = y_at_death;
    for (const HeightSample& h : hist) {
        peak = h.riding ? h.y : std::max(peak, h.y);
    }
    return std::max(peak, y_at_death);
}
/// FallDrop for a chr that may lie in a lift's car: a body carried on down by the car (riding at
/// HP 0) did not fall, so only the drop up to HP 0 counts then.
inline double FallDropRide(double peak_before, double y_at_death, double y_now, bool riding_at_death) {
    return riding_at_death ? peak_before - y_at_death : FallDrop(peak_before, y_at_death, y_now);
}

// ---- Split travel's question ("Travel with the party?") ----
constexpr double kAskTimeout = 30.0;  ///< s an open question waits (was 120 s)
enum class AskClose { Keep, Load, Dead, Timeout };
inline const char* AskCloseName(AskClose c);
/// An asked or queued question is closed by a load (a host travel, a death load), by the local
/// player's death, or after kAskTimeout s open (playtest 1: three questions stayed open 120 s
/// across the host's load and death).
inline AskClose PlanAskClose(bool open, bool loading, bool local_dead, double open_for) {
    if (loading) {
        return AskClose::Load;
    }
    if (local_dead) {
        return AskClose::Dead;
    }
    if (open && open_for > kAskTimeout) {
        return AskClose::Timeout;
    }
    return AskClose::Keep;
}

// ---- Whose lamp / headstone use started a lamp warp (playtest 1 bug 20) ----
/// The "used" flags the lamp warp events wait for: a world lamp -> the Dream (common event 7200,
/// one instance per lamp, flag 7AAB01xx: 72410101 Central Yharnam ...), a Dream headstone -> its
/// lamp (m21 event 12107000 / common 7300, flags 72102xxx / 72103xxx). The lamp's or the
/// headstone's talk script sets it on the user's own game (SetEventFlag). The same warp event
/// also runs on every other game in that map (live splitq: the guest's 12107000 reached its warp
/// 2003[49] 0.05 s after the host's, with no flag set there), so both players were asked.
/// The 7AAB01xx shape alone also matched the talk state flags of m21_00 (72100100-72100102 the
/// Doll's level-up handshake, 72100120-72100136 Gehrman's lines) and 700001xx (live 2026-10-20:
/// a guest's Doll / Gehrman talk and the workbench logged as "this player's lamp use", which then
/// counted as its own use for a lamp warp of another member's within 10 s). Lamps are 7AAB01xx of
/// areas 22-36 and m21_01 (72110100); see TalkStateFlag for the rest.
inline bool LampTriggerFlag(std::uint32_t id) {
    if (id / 1000 == 72102 || id / 1000 == 72103) {
        return true;
    }
    if (!(id >= 70000000u && id < 80000000u && id / 1000 % 10 == 0 && id % 1000 / 100 == 1)) {
        return false;
    }
    const std::uint32_t area = id / 100000 % 100, block = id / 10000 % 10;
    return area >= 21 && !(area == 21 && block == 0);
}
/// The lamp warp events: world lamp 7200, Dream headstone 12107000, common 7300 (the Dream side of
/// a headstone warp).
inline bool LampWarpEvent(std::uint32_t id) {
    return id == 7200 || id == 7300 || id == 12107000;
}
/// A peer's packet that would run this game's copy of a lamp warp event: packet 0x0f (a peer's
/// CompleteEvent flag, 4 bytes) with a lamp / headstone flag, or 0x0e (a peer's replicated EMEVD
/// event state, 0x14 bytes, `words` its 5 words) naming a lamp warp event. Its warp here would
/// only be dropped as "another member's lamp", and running it uses the event up: a lamp's event
/// (7200, rest 0) waits for its flag once per area load, so this player's own Dream choice at that
/// lamp later set the flag and nothing warped (live, 0.2.2 b1: a guest's lone Dream trip, then
/// the host at the same lamp). Such packets are dropped before the game reads them.
inline bool PeerLampPacket(std::uint32_t type, std::uint32_t size, const std::uint32_t* words) {
    if (!words) {
        return false;
    }
    if (type == 0xf && size == 4) {
        return LampTriggerFlag(words[0]);
    }
    if (type == 0xe && size == 0x14) {
        for (int i = 0; i < 5; ++i) {
            if (LampWarpEvent(words[i])) {
                return true;
            }
        }
    }
    return false;
}
/// The other 7AAB01xx flags: per-player talk state (the Dream's 721001xx, 700001xx), set and
/// cleared by each game's own talk scripts and events; kept out of the host-world mirror like the
/// lamp flags (a mirrored host 0 would undo a guest's Doll handshake mid level-up).
inline bool TalkStateFlag(std::uint32_t id) {
    return id >= 70000000u && id < 80000000u && id / 1000 % 10 == 0 && id % 1000 / 100 == 1 && !LampTriggerFlag(id);
}
/// The party's lamp spawn table (party_ids ReturnSpot: the MSB revive points) holds this lamp's
/// real spot. Not for a Chalice Dungeon's lamps (area 29, 2902950..): a dungeon is assembled from
/// pieces at load time, so its lamps stand elsewhere (a tester's report: the host's revive for lamp
/// 2902952 put it 80 m from that lamp, at the template's spot); their spot is the one seen there.
inline bool LampTableSpotTrusted(std::uint32_t lamp) {
    return lamp / 100000 != 29;
}
/// A guest still in the host's session but apart from it, in a map outside the Chalice Dungeon the host
/// is in (user decision 2026-10-21, option b). Only the summon's quick join enters a dungeon, so:
/// after a death or a forced trip (a death respawn, a scripted warp or kidnap, a host travel it could
/// not follow) it goes home after `kChaliceApartWait` s and the summon brings it back (SendHome); a
/// guest that CHOSE to leave (alone at a lamp or headstone, its own Hunter's Mark) stays out (Stay:
/// a one-time notice, Party > Rejoin brings it back). `rule_on`: party_chalice_rejoin.
enum class ChaliceApart { Wait, SendHome, Stay };
constexpr double kChaliceApartWait = 6.0;
inline ChaliceApart PlanChaliceApart(bool rule_on, bool chosen, double apart_s) {
    if (!rule_on || chosen) return ChaliceApart::Stay;
    return apart_s >= kChaliceApartWait ? ChaliceApart::SendHome : ChaliceApart::Wait;
}
/// Party > Rejoin pressed by a guest: true when it must first leave the session to its own world (it is
/// still a session client, apart, while the host is in a Chalice Dungeon): the bell's summon then
/// quick-joins it in. Outside the session the Rejoin rings as before.
inline bool ChaliceRejoinLeavesFirst(bool in_session, bool apart, bool host_in_dungeon) {
    return in_session && apart && host_in_dungeon;
}
/// A summoned guest's boss clear (Lua BlockClear2 0x13849A0: PREY SLAUGHTERED, the clear bonus
/// echoes, Insight 4680) that runs within `kStaleBossClearS` of its arrival in the host's world is a
/// boss that was already dead there (chalamp cl6 / cl8: re-summoned into a cleared Chalice Dungeon,
/// the boss came up alive for a moment and its defeat paid 1995 echoes and +1 Insight each time).
/// A guest never arrives into a running boss fight (the host's summon waits for the fight's end,
/// A11), so a real kill is never that early: the first real kill pays everyone as before.
constexpr double kStaleBossClearS = 15.0;
inline bool StaleBossClear(bool summoned_guest, double since_arrival_s) {
    return summoned_guest && since_arrival_s >= 0 && since_arrival_s < kStaleBossClearS;
}
inline const char* ChaliceApartNotice() {
    return "The host is in a Chalice Dungeon: Party > Rejoin to go back";
}
constexpr double kLampOriginWindow = 10.0;  ///< s from a lamp's flag to its warp (the event waits ~3-4 s)
enum class LampOrigin { Own, Other };
/// A lamp warp on this game: this player's own use, or another member's (`local_age` / `remote_age`:
/// s since this game set a lamp flag itself / got one from a peer (packet 0x0e / 0x0f), < 0
/// never). This player's only when its own game set the flag (or a test step used the lamp) and
/// no peer's came after it; `flag_hooks` false (the SetEventFlag hook is missing): always this
/// player's, the old behaviour.
inline LampOrigin PlanLampOrigin(double local_age, double remote_age, bool flag_hooks = true) {
    const bool remote = remote_age >= 0 && remote_age <= kLampOriginWindow;
    const bool local = local_age >= 0 && local_age <= kLampOriginWindow;
    if (!flag_hooks) {
        return LampOrigin::Own;
    }
    return local && !(remote && remote_age < local_age) ? LampOrigin::Own : LampOrigin::Other;
}

/// Story warps (EMEVD `2003[49]` outside the lamp / warp-object events). Each is that player's own
/// use when its game runs it, so the warp gets the travel question instead of being dropped as
/// "another member's lamp" (no lamp flag is set). Live (parg, 2026-10-14): the Blood Moon warp
/// 13201803 -> 2802958 was dropped on the host, leaving it at Moonside Lake.
/// Host-only ones (chr type 0 / `End if Client`): 3503 DLC cleanup -> Dream, 12405263 Amygdala
/// grab -> DLC, 12800430 Yahar'gul -> Lecture Building after the One Reborn, 13201803 Rom -> Blood
/// Moon -> Yahar'gul. A guest's copy of these runs only by accident (its chr type reads 0 for a
/// moment after a load, live parg re-run: the guest got its own Blood Moon question) and stays
/// dropped there; the host's travel brings the guest. Any player's: 12200131 Hemwick carriage
/// (action button), 12800431 Amygdala grab (own Tonsil Stone), 13200310 / 13200311 Lecture
/// Building doors (ObjAct).
inline bool StoryWarpEvent(std::int32_t event, bool host) {
    switch (event) {
    case 3503:
    case 12405263:
    case 12800430:
    case 13201803:
        return host;
    case 12200131:
    case 12800431:
    case 13200310:
    case 13200311:
        return true;
    default:
        return false;
    }
}
/// frontierfall (0.7.6 report): the Lecture Building door's story warp is held for the "Travel with the
/// party?" question (and the host's travel after it); the players free-fell from the door through the
/// ground until the load. While a story warp's question is pending, and for kStoryHoldGrace s after it
/// (a dropped duplicate question waits for the host's travel), the chr is held at its spot every frame.
/// Released by any load, and never longer than kStoryHoldMax s (a question closes after 30 s).
constexpr double kStoryHoldGrace = 10.0;
constexpr double kStoryHoldMax = 40.0;
inline bool StoryWarpHold(double since_story_s, bool question_pending, bool loading, bool world_up) {
    if (since_story_s < 0 || loading || !world_up || since_story_s > kStoryHoldMax) {
        return false;
    }
    return question_pending || since_story_s <= kStoryHoldGrace;
}
/// After a story warp's cutscene the travel question waits this long once the scene is over (live
/// parg re-run: opened in the frame the Blood Moon scene ended, the box never showed and six Cross
/// presses did nothing).
constexpr double kAskAfterSceneDelay = 2.0;

// ---- P4 (quest_audit.md): one question for a scripted warp that runs on two games ----
/// This game's question for warp `mine` (pending: queued or open) closes when a member with a
/// lower party slot asks for the same warp (the host first). Slots < 0 / id 0: unknown, keep it.
inline bool DropSameWarpQuestion(bool pending, std::uint32_t mine, int my_slot, std::uint32_t theirs,
                                 int their_slot) {
    return pending && mine != 0 && mine == theirs && my_slot >= 0 && their_slot >= 0 && their_slot < my_slot;
}

// ---- B5: the host dies while a guest is apart ----
/// A living, idle guest in another area than the host (split up) does not follow the host's
/// death load: it plays on where it is (ERSC: each respawns at their own grace). Areas 0 =
/// unknown: follow as before. A dead guest (a wipe) follows.
inline bool StayApartAtHostDeath(bool guest_dead, bool guest_idle, std::uint32_t host_area, std::uint32_t own_area) {
    return !guest_dead && guest_idle && host_area != 0 && own_area != 0 && host_area != own_area;
}

// ---- C3: the last guest down starts the wipe (player report 2026-10-21, Logarius) ----
// The guest's HP 0 tells the host at once (member_down), so the wipe's respawn load comes while
// this guest's death animation still runs; the game takes the death (PartyGhostDeath, ~8 s after
// HP 0) only later. Followed at once, the load moved the dying body to the host's lamp, the death
// was then taken there (its bloodstain at the lamp) and the held death revived it alone at its own
// last lamp beside the arena. The follow waits for the death instead: the bloodstain where it
// died, then one dead follow to the host's lamp with everyone.
constexpr double kWipeDeathWait = 15.0;  ///< s; the dead watchdog takes the death at 11 s
enum class WipeFollow { Now, WaitDeath, DeadFollow, TakeDeath };
inline const char* WipeFollowName(WipeFollow w) {
    return w == WipeFollow::Now ? "now" : w == WipeFollow::WaitDeath ? "wait for the death" : w == WipeFollow::DeadFollow ? "dead follow" : "take the death";
}
/// A guest at the host's death travel (`host_death`), its own chr's HP (`hp` -1 unknown), its
/// death taken (held), `waited_s` since the travel came.
inline WipeFollow PlanWipeFollow(bool host_death, int hp, bool death_taken, double waited_s) {
    if (!host_death || hp != 0) {
        return WipeFollow::Now;  // alive (or unknown): the follow as before
    }
    if (death_taken) {
        return WipeFollow::DeadFollow;
    }
    return waited_s < kWipeDeathWait ? WipeFollow::WaitDeath : WipeFollow::TakeDeath;
}
/// A death taken (held) within kWipeArrivalWindow s after the guest arrived with the host's death
/// load: part of that wipe, never a second death of its own. Up already: dropped (no revive at its
/// own lamp); still down: revived at the host's lamp, where the party is.
constexpr double kWipeArrivalWindow = 30.0;
enum class AfterWipe { None, Drop, HostLamp };
inline AfterWipe PlanHeldAfterWipe(double since_arrival_s, bool alive) {
    if (!(since_arrival_s >= 0.0 && since_arrival_s < kWipeArrivalWindow)) {
        return AfterWipe::None;
    }
    return alive ? AfterWipe::Drop : AfterWipe::HostLamp;
}
/// A dead guest waits for the host's wipe decision (no revive of its own) while the host's chr is
/// down, at most kGuestHostDownHold s (a stale host chr must not hold the respawn for good; was 30 s,
/// shorter than a long boss-fight spectate).
constexpr double kGuestHostDownHold = 120.0;
inline bool GuestWaitsForHostDown(bool host_down, double held_s) {
    return host_down && held_s < kGuestHostDownHold;
}
/// The guest's bloodstain goes where it died (its HP 0 spot) when a load moved the body between
/// its HP 0 and the death's record, never to the load's destination.
inline bool StainAtDeathSpot(bool spot_known, bool loaded_since_hp0) {
    return spot_known && loaded_since_hp0;
}

// ---- A11 (live a11c): the host's chr is not placed yet ----
/// A joiner is put beside the host's chr only when that chr stands where the host is: a host
/// whose first sync waits (a cutscene started as the guest loaded in) leaves its chr on the
/// guest's game at the origin (0, 0, 0); a warp beside it dropped the guest into the void.
/// `vitals` (the host's own reported position) when known: within 10 m; else not at the origin.
inline bool HostChrPlaced(const float chr[3], bool have_vitals, const float vitals[3]) {
    if (have_vitals) {
        const float dx = chr[0] - vitals[0], dy = chr[1] - vitals[1], dz = chr[2] - vitals[2];
        return dx * dx + dy * dy + dz * dz <= 100.0f;
    }
    return !(chr[0] > -1.0f && chr[0] < 1.0f && chr[1] > -1.0f && chr[1] < 1.0f && chr[2] > -1.0f && chr[2] < 1.0f);
}

// ---- Playtest 1 bug 22: nobody to travel with = single player ----
/// The party's own lamp / headstone / Hunter's Mark handling (the in-world lamp move without a
/// load, "already standing at it", the guest's Mark to the host's lamp) applies only when there
/// is a party to travel with: a guest standing in the host's world (session client, or the flag
/// store still on the host-world overlay, load mode 1) or a host with members. A Join-a-party
/// guest whose host never answered (or between sessions, in its own world) and a host alone use
/// lamps, headstones and the Mark exactly as in single player (the vanilla loading screen).
inline bool PartyTravelActive(bool guest, bool host, bool guest_in_host_world, bool host_has_members) {
    return (guest && guest_in_host_world) || (host && host_has_members);
}
/// A guest is in the host's world: its session role is client (6), or its flag store still holds
/// the host-world overlay (load mode 1; a phantom's Hunter's Mark leaves the session before the
/// funnel runs). Its own world: load mode 0 and no client role.
inline bool GuestInHostWorld(int session_role, int flag_load_mode) {
    return session_role == 6 || flag_load_mode == 1;
}

// ---- C5: the spectator's on-screen notice ----
/// "Spectating Hunter0" (+ "  -  Cross: next hunter" when another living member can be chosen);
/// empty when there is nobody to follow. `name` empty: the member is not matched yet.
inline std::string SpectateNotice(const std::string& name, int living) {
    if (living <= 0) {
        return {};
    }
    std::string t = "Spectating " + (name.empty() ? std::string("a party member") : name);
    if (living > 1) {
        t += "  -  Cross: next hunter";
    }
    return t;
}

// ---- A guest uses a lamp / headstone / Hunter's Mark in the host's world ----
enum class GuestLamp {
    Plain,     ///< nothing to decide here (not a guest in the host's world, or standing at it)
    SoloLoad,  ///< the lamp is in the loaded map: this guest's own loading screen, no party move
    Ask,       ///< another map: the question (YES party travel, NO alone + a rest load for the host)
};
inline const char* GuestLampName(GuestLamp g);
inline GuestLamp PlanGuestLamp(bool guest_in_host_world, bool at_lamp_spot, bool lamp_in_loaded_map, bool mark) {
    if (!guest_in_host_world || (at_lamp_spot && !mark)) {
        return GuestLamp::Plain;
    }
    // Playtest 1 bug 10: a guest's warp to a lamp of the map the party stands in moved the host
    // too (an in-world party move). Now only the guest moves, with a vanilla loading screen.
    if (lamp_in_loaded_map) {
        return GuestLamp::SoloLoad;
    }
    return GuestLamp::Ask;
}
/// A guest that travels alone from a lamp (NO) has rested: the host's world (where the guest
/// was) resets with a rest load for everyone there. Not for a guest's death (no rest). D9 (pard4,
/// live E2): a guest's lamp of the map the party stands in (its own loading screen, `same_map_solo`)
/// is a rest too - before, its reload brought the host's kills back to life on the guest's screen
/// only (guest census 327 alive, host 325) while the host's world kept them dead.
/// 0.7.3 (user decision 2026-10-22: "only the host's own rest resets the world"): a guest that
/// leaves the Hunter's Dream alone (`from_dream`: a headstone, NO) never rests the host's world;
/// its trip is a plain travel for the guest (the dreamstone da run gave the host a rest load).
/// Nightwatch (playtest 23 Oct, the same decision): a guest that travels away alone (to the Dream or
/// another lamp) never rests the host's world either: the host no longer reloads where it stands.
/// Only a lamp of the map the party stands in stays a rest (`same_map_solo`): the guest reloads that
/// map itself, and without the host's rest load the host's kills would live again on its screen only.
inline bool GuestAloneRestsHostWorld(bool alone, bool same_map_solo, bool from_dream) {
    return alone && same_map_solo && !from_dream;
}

// ---- The host runs a rest load for a guest's rest ----
/// BossFight (C7, ERSC: a rest removes the others from the boss room): a run, but to the host's
/// last lamp instead of where it stands - the fight resets and everyone here is taken out of it.
enum class RestLoad { Run, NotReady, Dead, BossFight, InDream, Busy, TooSoon };
inline const char* RestLoadName(RestLoad r);
constexpr double kRestLoadMinInterval = 20.0;
struct RestView {
    bool world_up = false;
    bool loading = false;
    bool alive = false;
    bool boss_fight = false;
    bool in_dream = false;          ///< the Hunter's Dream has nothing to reset
    bool travel_pending = false;    ///< a stage request is armed / the host is down
    double since_last = 1e9;        ///< s since the last rest load
};
inline RestLoad PlanRestLoad(const RestView& v) {
    if (!v.world_up || v.loading) {
        return RestLoad::NotReady;
    }
    if (!v.alive) {
        return RestLoad::Dead;  // its own death load resets the world anyway
    }
    if (v.travel_pending) {
        return RestLoad::Busy;
    }
    if (v.since_last < kRestLoadMinInterval) {
        return RestLoad::TooSoon;
    }
    if (v.boss_fight) {
        return RestLoad::BossFight;  // out of the boss room: a load at the host's last lamp
    }
    if (v.in_dream) {
        return RestLoad::InDream;
    }
    return RestLoad::Run;
}

/// How many members a guest's rest load waits for (playtest 1 batch 6): the guest that rested
/// travels alone - to another area it never arrives here; to a lamp of the host's area
/// (`rest_dest_area` == `host_area`) it is in the host's world again and follows the load (its
/// world resets too). Members known in another area stay there; a member in the host's area or
/// still loading (area 0, unknown) follows the load. 0 = nobody follows: the host's load is an
/// alone load (no re-sync, no 60 s wait for an arrival that never comes).
struct RestMember {
    int slot = -1;
    bool connected = false;
    std::uint32_t area = 0;  ///< 0: unknown (loading)
};
/// `rest_rides_rejoin`: the resting guest comes back by its own re-join, served after this load
/// (RestRidesRejoin): it does not follow the load, it arrives into the reset world.
inline int RestLoadExpected(const RestMember* m, int n, int rest_slot, std::uint32_t host_area,
                            std::uint32_t rest_dest_area, bool rest_rides_rejoin = false) {
    int expected = 0;
    for (int i = 0; i < n; ++i) {
        if (!m[i].connected) {
            continue;
        }
        if (m[i].slot == rest_slot) {
            expected += !rest_rides_rejoin && rest_dest_area != 0 && rest_dest_area == host_area ? 1 : 0;
            continue;
        }
        if (m[i].area == 0 || m[i].area == host_area) {
            ++expected;
        }
    }
    return expected;
}

// ---- A guest rests and travels alone back into the host's area (one load, pt1polish) ----
/// The playtest replay (S2): a guest at the Dream lamp chose NO to a lamp of the host's area. It
/// travelled alone (its re-join load), then the host ran the queued rest load and the guest
/// followed it: two loading screens (~15 s). Now the rest rides on the re-join request: the host
/// runs its rest load first and serves the re-join after it, so the guest sees one loading screen
/// (raised while it waits for the snapshot) and arrives into the reset world. Only for a rest
/// (not a Hunter's Mark) into the area the host stands in.
inline bool RestRidesRejoin(bool rest, std::uint32_t dest_area, std::uint32_t host_area) {
    return rest && dest_area != 0 && dest_area == host_area;
}
/// Host: how long a re-join that carries a rest waits for the rest load to start. Past it the
/// re-join is served (the rest load, if it still runs, is a second load as before).
constexpr double kRestCarryHoldS = 8.0;
inline bool RestCarryHeld(double now, double carry_at, bool rest_load_started) {
    return carry_at > 0 && !rest_load_started && now - carry_at < kRestCarryHoldS;
}
/// Host: a pending re-join request (a guest's respawn / alone travel into this world) is served
/// now: in the world, not inside an own load's settle (its re-sync covers the request), and not
/// held for a rest load that is about to start.
inline bool HostServesRejoin(bool loading, bool own_load_settling, bool rest_carry_held) {
    return !loading && !own_load_settling && !rest_carry_held;
}

// ---- Game banners in a party (the SummonMessage dialog, EventText ids; pt1polish) ----
/// "The bell's resonance has beckoned <name>." (102001) and the arrival / "Entered the world"
/// banners (102002..102700) belong to the first summon. After a B2 re-join (a wipe or death
/// respawn load, a rest follow, an alone travel back) the member is the same one: no banner.
inline bool IsJoinBanner(int id) {
    return id >= 102001 && id <= 102799;
}
/// "You died. Returning to your world." (103100, 103200..103500) and "Your beckoner has died.
/// Returning to own world." (103101): with B2 a guest's death is held and it stays with the party.
inline bool IsReturnHomeBanner(int id) {
    return id == 103100 || id == 103101 || id == 103200 || id == 103300 || id == 103400 || id == 103500;
}
constexpr double kRejoinBannerWindowS = 15.0;
/// `since_rejoin`: s since a B2 re-join was last running on this game (negative: never).
inline bool HidePartyBanner(int id, bool b2_on, bool guest, double since_rejoin) {
    if (!b2_on) {
        return false;
    }
    if (IsReturnHomeBanner(id)) {
        return guest;
    }
    return IsJoinBanner(id) && since_rejoin >= 0 && since_rejoin < kRejoinBannerWindowS;
}

// ---- An empty bloodstain (pt1polish) ----
/// A death with 0 Blood Echoes still records a stain (flag +0x28 set, record echoes 0): the game
/// then shows "Acquire Blood Echoes" for nothing. In a party it is not placed (the flag cleared
/// before the respawn load builds it). A stain with echoes, or none, is left alone.
inline bool ClearEmptyStain(bool party_on, int flag, int echoes) {
    return party_on && flag != 0 && echoes == 0;
}
/// After any call of the stain record 0x13D2910 (`flag_after` GameDataMan +0x28, `record_echoes`
/// the record's +0x38 after the call): a stain with 0 echoes is removed at once (the function
/// builds the live stain itself). Not only the death's record call (third argument 1): with 0 it
/// skips the record but still sets the flag and builds the stain from the old record - the
/// respawn load's call put the 0-echo stain back (pt1last live: "Acquire Blood Echoes" at the
/// lamp after a wipe although the death's call had removed it). Unknown echoes (-1): left as is.
inline bool SkipEmptyStainRecord(bool party_on, int flag_after, int record_echoes) {
    return party_on && flag_after != 0 && record_echoes == 0;
}

// ---- Test steps (BB_PARTY_DIRECTOR_TEST) ----
/// A step waits (pt1polish, replay S1/S4): one that moves / acts with the local chr while that
/// chr is dead, and any step while a B2 re-join or load is in flight (a die step landed inside the
/// previous wipe's load and made no second wipe).
/// The guest's dead-without-a-death-state watchdog (party_b2 DeadAfterClearTick) redirects a guest
/// that stayed at HP 0 for `limit_s` with no death hold. Never while it follows the host's travel:
/// that load revives it (regression run 0.6.1: a wipe whose host travel came 10 s after the guest's
/// death; the redirect 4.5 s into the follow ended the session, ~50 s re-join).
inline bool DeadWatchdogFires(double dead_s, double limit_s, bool death_held, bool following_host_travel) {
    return !following_host_travel && !death_held && dead_s >= limit_s;
}

inline bool TestStepWaits(bool acts_with_own_chr, bool own_alive, bool world_up, bool rejoin_running) {
    if (!world_up || rejoin_running) {
        return true;
    }
    return acts_with_own_chr && !own_alive;
}

/// A die step (host_die / guest_die) whose own chr is at HP 0 already, from something else (an
/// enemy, the boss) after the previous step: the death the step asks for happened - the step is
/// taken as done, so the chain goes on after the respawn (it waited for life forever and stalled
/// every later step: wipe_late when Gascoigne killed guest 1 first). Not after a die step (a second
/// die step must not be taken by the first one's death) and not during a load / re-join.
inline bool TestDieStepLatches(bool die_step, bool own_alive, bool world_up, bool rejoin_running, bool prev_was_die) {
    return die_step && !own_alive && world_up && !rejoin_running && !prev_was_die;
}

/// Test aid BB_PARTY_TEST_GUARD=1 (smoke rt, testinfra 0.7.3): after each death of this player, once
/// it is up again, its chr is kept invincible until the chain's next step that asks for a death (a
/// die step, or a chr step: stand at an enemy to be killed by it). A frail guest revived next to the
/// enemy that killed it (the stain test warps it onto its stain) was killed again by the same enemy
/// before the host's kill step, which shifted every later death (rel2 0.7.2: death 2 by 2410202).
struct TestGuard {
    bool died = false;  // seen at HP 0 since the last step that asks for a death
    bool on = false;    // up again after that death: invincible
};
/// One tick: hp = the own chr's HP (-1: no chr). True while the chr is to be invincible.
inline bool TestGuardTick(TestGuard& g, int hp, bool world_up) {
    if (!world_up || hp < 0) return false;
    if (hp == 0) {
        g.died = true;
        g.on = false;
        return false;
    }
    if (g.died) {
        g.died = false;
        g.on = true;
    }
    return g.on;
}
/// A step of the chain runs: a die / chr step ends the guard. True when it was on (clear the bit).
inline bool TestGuardStepRan(TestGuard& g, bool death_step) {
    if (!death_step) return false;
    const bool was = g.on;
    g.on = false;
    g.died = false;
    return was;
}

// ---- Own bloodstain record (GameDataMan +0x28 flag, +0x30 record +0x38 echoes) ----
enum class StainChange { None, Recorded, Taken, Replaced };
inline const char* StainChangeName(StainChange c);
/// What changed between two reads of the own bloodstain record (echoes -1 / flag 0 = none).
inline StainChange PlanStainChange(int flag_was, int echoes_was, int flag_now, int echoes_now) {
    const bool had = flag_was != 0 && echoes_was > 0, has = flag_now != 0 && echoes_now > 0;
    if (had && !has) {
        return StainChange::Taken;
    }
    if (!had && has) {
        return StainChange::Recorded;
    }
    if (had && has && echoes_was != echoes_now) {
        return StainChange::Replaced;
    }
    return StainChange::None;
}

// ---- Blood Echoes an enemy took (echodup, player report for 0.7.3) ----
// Vanilla: the stain object (SprjEventMan +0x80 [+8]) is given to a living enemy near it while the
// player is away (0x18686D0, every few s and at each load: enemy vfunc 0x18E75A0 sets the stain
// object's +0xC0 "held", the chr's +0x1E5 bit 0x20 and SpEffect 5100, the glowing eyes; the live
// stain goes). That enemy's death (0x18C59D0 from the death path 0x18BD9D0) clears the hold and the
// record's flag (GameDataMan +0x28) and pays the record (0x130AB80: +0x38 into the echoes, then
// +0x38 = -1). The party stain (a guest's stain in the host's world, the marker) copies the record,
// and the game builds that stain on the guest's game too, so the enemy took it there: the kill paid
// the echoes and the party stain paid them again. One recovery only:
enum class StainHold { Free, Held, Void };
/// `record_is_stain`: the game's record holds this party stain's echoes; `hold_flag`: the
/// stain object's +0xC0; `holder_alive`: a living loaded enemy with the holder bit;
/// `holder_gone_for`: seconds since one was last seen (the hold may outlive it for a moment while
/// its death runs). Held: the enemy has the echoes (no marker on the ground, no pickup there: kill
/// it). Void: the hold outlived its enemy (unloaded, or a death the game did not pay): the party
/// stain is recoverable again, once.
inline StainHold PlanStainHold(bool record_is_stain, int hold_flag, bool holder_alive, double holder_gone_for,
                               double grace = 10.0) {
    if (!record_is_stain || hold_flag == 0) {
        return StainHold::Free;
    }
    if (holder_alive || holder_gone_for < grace) {
        return StainHold::Held;
    }
    return StainHold::Void;
}
/// The game paid the record (an enemy's kill, or its own stain picked up; both 0x130AB80, which
/// leaves the record's echoes -1): the record held this stain's echoes the last frame
/// (`was_stain`), now other echoes, and the echoes came back (`gain` within a few s, at least half).
/// The record's flag is not used: a summoned load drops it (pard2 c10) while the hold and its kill
/// payment go on. The party stain then goes: no second payment.
inline bool StainTakenByGame(bool was_stain, int record_echoes_now, int stain_echoes, int gain) {
    if (!was_stain || stain_echoes <= 0) {
        return false;
    }
    return record_echoes_now != stain_echoes && gain >= std::max(1, stain_echoes / 2);
}
/// The stain object's hold reads 0 (live eh2: 2 s after the take; the stain stood on the ground
/// again) while a living enemy still has the holder bit and the glowing eyes (its kill pays the
/// record): the hold is put back, so the echoes have one place again (that enemy), as in vanilla.
inline bool RestoreHold(bool record_is_stain, int hold_flag, bool holder_alive) {
    return record_is_stain && hold_flag == 0 && holder_alive;
}
/// echohold (smoke eh, live 2026-10-24): 1.6-4.7 s after the take the game itself lets the holder go
/// (0x18E76C0: the stain object's +0xC0, the chr's +0x1E5 bit 0x20 and the glowing eyes all clear in one
/// frame; the holder alive, unmoved, record intact), so the stain stood on the ground again and the
/// enemy's kill paid nothing. The holder seen `seen_ago` s ago with the hold (<= kRetakeWindow), alive
/// now, the hold gone while the party stain is the record, and not let go by us (`released_by_us`: a
/// new death, the pickup): it takes the echoes again (0x18E75A0), as in vanilla where it keeps them.
constexpr double kRetakeWindow = 3.0;
inline bool RetakeHold(bool record_is_stain, int hold_flag, bool holder_alive_now, double seen_ago,
                       bool released_by_us) {
    return record_is_stain && hold_flag == 0 && holder_alive_now && !released_by_us && seen_ago >= 0 &&
           seen_ago <= kRetakeWindow;
}
/// A new death's record while an enemy still holds the old echoes (a no-load revive keeps that
/// enemy alive): the old hold is released first (vanilla: the old echoes are lost, and the death's
/// load brings that enemy back without them); else the new stain is never built (0x1868A50 builds
/// none while held) and the old holder's kill would pay the new echoes.
inline bool ReleaseHoldAtDeath(bool death_record, int hold_flag) {
    return death_record && hold_flag != 0;
}

/// Verification replay (playtest 1, 2026-10-14): a guest's own body comes out of a B2 host-world
/// load (host death / wipe respawn, rest follow) and of a title join with PlayerIns +0x78 (chr type)
/// 0, an own-world body, instead of 1 (white phantom, what a direct summon leaves). The death
/// dispatch OnEvent_4000 0x13813D0 then takes SoloPlayDeath, which leaves the session at 0x13CDEF6
/// instead of PartyGhostDeath_2 (the B2 funnel at 0x1382663): in a wipe the guest left the party and
/// rejoined 50 s later instead of following the one load. Due: a client in the host's world, alive,
/// flow 4, type 0 for `steady_s` >= kGuestChrTypeSettle, or at once when the chr is down (HP 0):
/// playtest 1, a title-joined guest died ~1 s after its arrival, inside the old 2 s settle, so its
/// death left the session (to the title screen, a re-join 50 s later). Writing 1 where the summon
/// load would set 1 itself changes nothing, so the settle is only a few frames.
constexpr double kGuestChrTypeSettle = 0.25;
inline bool GuestChrTypeFixDue(bool client_in_world, int chr_type, int hp, int flow, double steady_s) {
    return client_in_world && chr_type == 0 && hp >= 0 && flow == 4 && (hp == 0 || steady_s >= kGuestChrTypeSettle);
}

inline const char* HostDeathPlanName(HostDeathPlan p) {
    switch (p) {
    case HostDeathPlan::Stock: return "stock";
    case HostDeathPlan::PartyLoad: return "party respawn load";
    case HostDeathPlan::Wipe: return "wipe";
    case HostDeathPlan::BossLimbo: return "boss limbo";
    case HostDeathPlan::ReviveInPlace: return "revive in place";
    case HostDeathPlan::AloneLoad: return "alone load";
    }
    return "?";
}
inline const char* DownTickName(DownTick d) {
    switch (d) {
    case DownTick::Wait: return "wait";
    case DownTick::Wipe: return "wipe";
    case DownTick::Revive: return "revive";
    case DownTick::PartyLoad: return "party load";
    case DownTick::AloneLoad: return "alone load";
    case DownTick::ArenaRevive: return "arena revive";
    }
    return "?";
}
inline const char* AskCloseName(AskClose c) {
    switch (c) {
    case AskClose::Keep: return "keep";
    case AskClose::Load: return "a load started";
    case AskClose::Dead: return "the player died";
    case AskClose::Timeout: return "no answer in 30 s";
    }
    return "?";
}
inline const char* GuestLampName(GuestLamp g) {
    switch (g) {
    case GuestLamp::Plain: return "plain";
    case GuestLamp::SoloLoad: return "solo load";
    case GuestLamp::Ask: return "ask";
    }
    return "?";
}
inline const char* RestLoadName(RestLoad r) {
    switch (r) {
    case RestLoad::Run: return "run";
    case RestLoad::NotReady: return "the host is not in the world";
    case RestLoad::Dead: return "the host is dead (its death load resets the world)";
    case RestLoad::BossFight: return "a boss fight is on: everyone here is taken out of the boss room";
    case RestLoad::InDream: return "the host is in the Hunter's Dream";
    case RestLoad::Busy: return "a travel or death is under way";
    case RestLoad::TooSoon: return "a rest load ran less than 20 s ago";
    }
    return "?";
}
inline const char* StainChangeName(StainChange c) {
    switch (c) {
    case StainChange::None: return "none";
    case StainChange::Recorded: return "recorded";
    case StainChange::Taken: return "taken";
    case StainChange::Replaced: return "replaced";
    }
    return "?";
}

} // namespace coop::deathrules
