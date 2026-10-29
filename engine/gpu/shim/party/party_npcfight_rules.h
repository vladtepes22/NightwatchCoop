// SPDX-License-Identifier: GPL-3.0-or-later
// ERSC parity D11 (0.7.3): scripted NPC hunter fights with the whole party. User decision
// 2026-10-22: the NPC hunter is there in sync for everyone, anyone can hurt it, and its death and
// the quest step count for all, like a boss. Covenant outcomes stay per player (user decision; the
// covenants work owns those flags, see CovenantOutcomeFlag).
//
// What the game data says (docs/party/npc_fights.md, EMEVD scan of every map):
//  - The story's NPC hunter fights are NOT AI-player summons. Henryk and Eileen at the Tomb of Oedon,
//    the blood-drunk Eileen at the Grand Cathedral, Alfred, Valtr, Djura, Brador and the DLC hunters
//    are c0000 map chrs that the map's events enable, warp, re-team (2004[2]: 26 friendly, 27 hostile,
//    28 the player's ally) and dress (SpEffect 7608 the ally's phantom look, 5560 / 8060 an
//    assassin hidden / shown). They never touch the summon slots, so a full party cannot block them.
//    Only the Beckoning-Bell NPC signs (template events xx04410, 2003[51]) and the Chalice Dungeons'
//    NPC invader (12904634, 2003[51] type 1) go through the 5-entry slot table (AiSummonFits).
//  - In the host's world every member's game holds the same chr (same handle): the host's game owns
//    it, its HP and death reach every game (live nf run 3: the guest's R1 hits took Henryk 1716 ->
//    1606 on both games; a HP 0 on the guest alone came back to 1606 within a second).
//  - Where it breaks: the transitions the host's event runs mid-session but a guest's copy of the
//    event skips. Some events end on a client (1003[6] "end if client") or wait for "chr type of the
//    local player is 0" (4[3], the own world). Their chr actions (team 27, backread, animation) then
//    happen on the host's game only: the guest keeps a friendly chr it cannot lock on to or hurt,
//    or does not see it at all (the blood-drunk Eileen stays out of the guest's backread).
//
// The rule (the host's, System > Party rules: World "NPC hunters fight the whole party", bbport.ini
// party_npc_fights, BB_PARTY_NPC_FIGHTS; default on): a guest's copy of each GuestRun event runs as
// the host's does. Its gate instruction is passed (4[3]: the chr type it waits for becomes the
// guest's own; 1003[6]: the client state it ends on becomes one that never matches) and its flag
// writes and save requests are skipped (the host's copy writes the flags; they reach the guest's
// save through C2, so the guest's copy only re-dresses the chrs, never writes progress twice).
// Assassins (Brador and the Fishing Hamlet hunters): their appearance waits for the host's own
// position and goods, so a guest's copy can never fire; a guest shows / hides the chr by the
// host-world flag the host's event sets (PlanAssassin).
//
// Pure logic (unit test tests/test_party_npcfight.cpp); the game side is party_npcfight.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::npcfight {

// ---- the slot table (AI-player summons only) ----

constexpr int kSlotEntries = 5;        ///< NetworkFlow cooperator slots (npc_peer.md 1.5): host + 4
constexpr int kVanillaMaxPlayers = 4;  ///< the SOS status clears the AI summon bit at 4 (npc_peer.md 5.2)

/// An AI-player summon (2003[51] NPC sign or the Chalice NPC invader) has room: the party's humans
/// plus the NPCs already summoned stay under the game's four-player limit and the slot table. The
/// party's default of 3 players always leaves one; a 4-player party has none (vanilla: 4 players).
constexpr bool AiSummonFits(int humans, int npcs) {
    return humans >= 1 && npcs >= 0 && humans + npcs < kVanillaMaxPlayers && humans + npcs < kSlotEntries;
}

// ---- guest copies of the host-only transitions ----

enum class Gate : std::uint8_t {
    ChrTypeHost,  ///< 4[3](group, 10000, 0): "the local player's chr type is 0" (the own world)
    EndIfClient,  ///< 1003[6](end type, 1): "end the event on a client"
};

struct GuestRun {
    std::int32_t event;
    std::int16_t index;  ///< the gate's instruction index (+0xA0)
    Gate gate;
    std::uint32_t area_block;  ///< map area * 100 + block (24_00 -> 2400)
    const char* who;
};

/// Every event whose NPC hunter transition runs only on the host's game (EMEVD scan, all maps; the
/// init events that set the same chr state at a load run on every game and are not here).
inline constexpr GuestRun kGuestRuns[] = {
    {12400593, 0, Gate::ChrTypeHost, 2400, "Alfred turns hostile (Cathedral Ward, 2400760 / 2400763)"},
    {12400656, 1, Gate::ChrTypeHost, 2400, "Eileen falls to the blood (Grand Cathedral, 2400902)"},
    {12500050, 30, Gate::EndIfClient, 2500, "Alfred hostile at a load (Cainhurst, 2500790)"},
    {12500053, 0, Gate::EndIfClient, 2500, "Alfred turns hostile (Cainhurst, 2500790)"},
    {12700905, 0, Gate::ChrTypeHost, 2700, "Valtr turns hostile (Forbidden Woods, 2700912)"},
    {13400920, 0, Gate::ChrTypeHost, 3400, "a Hunter's Nightmare hunter turns hostile (3400902 / 3400903)"},
    {13500920, 0, Gate::ChrTypeHost, 3500, "the Research Hall hunter turns hostile (3500900)"},
};

inline const GuestRun* GuestRunFor(std::int32_t event) {
    for (const GuestRun& g : kGuestRuns)
        if (g.event == event) return &g;
    return nullptr;
}

/// A flag write or save request: on a guest's copy of a GuestRun event these are skipped (the
/// host's copy writes them; C2 brings them into the guest's save).
constexpr bool ProgressWrite(std::int32_t bank, std::int32_t id) {
    return (bank == 2003 && (id == 2 || id == 9 || id == 17 || id == 22 || id == 31 || id == 32 || id == 41)) ||
           (bank == 2000 && id == 5);
}

constexpr std::int32_t kChrTypeArg = 8;   ///< 4[3]: s8 group @0, i32 entity @4, s8 chr type @8
constexpr std::int32_t kMpStateArg = 1;   ///< 1003[6]: u8 end type @0, u8 multiplayer state @1
constexpr std::uint8_t kClientState = 1;  ///< 1003[6]'s "client" (12500050 / 12500053 data)
constexpr std::uint8_t kNoState = 0x7f;   ///< a multiplayer state the game never reports

enum class Act : std::uint8_t {
    none,
    pass_gate,     ///< write the guest's value into the gate's argument (chr type / no state)
    restore_gate,  ///< write the data's own value back (the host, solo, the rule off)
    skip,          ///< skip the instruction (a guest copy's progress write)
    foe_team,      ///< 2004[2] team 27 -> kGuestFoeTeam (a guest's copy: a foe a phantom can hit)
    restore_team,  ///< 2004[2] kGuestFoeTeam -> 27 again (the host, solo, the rule off)
};

// ---- the hostile team on a guest's game ----
//
// An NPC hunter turned hostile gets team 27 (2004[2], every listed event's data). In the game's
// two team tables (pvp.md: the relation table 0x5127B90, lock-on / nearest enemy; the SpEffect
// target table 0x4731560, an attack's effects), row 19 (the host's body) has 1 at column 27 but
// row 20 (a cooperator, white phantom) has 0 in both: a guest cannot lock on to a team-27 chr nor
// hurt it (live nfa run 2: three R3 at 0.9 m, no lock-on; eight R1, Alfred's HP unchanged). Team 23
// (the map enemies, Henryk's MSB team) has 1 in rows 19 and 20 of both tables, so a guest's copy
// switches the chr to 23 instead: on its game only (the host's game keeps 27, its AI and its
// events); the HP, the death and the kill flags stay the host's (the Tomb fight's path).
constexpr std::uint8_t kHostileTeam = 27;
constexpr std::uint8_t kGuestFoeTeam = 23;
constexpr std::int32_t kTeamArg = 4;  ///< 2004[2]: i32 entity @0, u8 team @4

/// Events that switch an NPC hunter to team 27 on every game (no host-only gate; a guest's copy
/// runs them as data). Together with kGuestRuns these are every 2004[2] team 27 in the 1.09 EMEVDs
/// (all maps scanned); their 27 becomes 23 on a guest:
///   12400590 Alfred hostile at a load (Cathedral Ward, 2400760 / 2400763)
///   12400650 the blood-drunk Eileen at a load (Grand Cathedral)
///   12400658 Eileen turns hostile when the host attacks her (talk flag 72400526, Cathedral Ward)
///   12400900 a hostile NPC (Cathedral Ward, 2400775)
///   12410800 / 12410806 the Tomb of Oedon init / Eileen turns hostile there (2410900)
///   12700901 Valtr's init (Forbidden Woods)
///   12906742 a hostile NPC in the Chalice Dungeons (entity from the event's parameters)
///   13200109 a hostile NPC in Yahar'gul (entity from the event's parameters)
///   13400950 the Hunter's Nightmare hunters' init
///   13500950 a hostile NPC's init in the Research Hall (entity from the event's parameters)
inline constexpr std::int32_t kTeamEvents[] = {12400590, 12400650, 12400658, 12400900, 12410800, 12410806,
                                               12700901, 12906742, 13200109, 13400950, 13500950};

inline bool TeamEvent(std::int32_t event) {
    if (GuestRunFor(event)) return true;
    for (std::int32_t e : kTeamEvents)
        if (e == event) return true;
    return false;
}

/// The team byte a 2004[2] of a TeamEvent gets for `act` when it holds `have`; `have` itself when
/// nothing changes (another team in the data, e.g. Valtr's friendly 8, is never touched).
constexpr std::uint8_t TeamFor(Act act, std::uint8_t have) {
    return act == Act::foe_team && have == kHostileTeam        ? kGuestFoeTeam
           : act == Act::restore_team && have == kGuestFoeTeam ? kHostileTeam
                                                                : have;
}

/// What the dispatch filter does with instruction (event, index, bank, id). `client`: this game is
/// a guest in the host's world (session role 6); `rule_on`: the host's rule in force.
inline Act Decide(std::int32_t event, std::int32_t index, std::int32_t bank, std::int32_t id, bool client,
                  bool rule_on) {
    if (bank == 2004 && id == 2 && TeamEvent(event)) return client && rule_on ? Act::foe_team : Act::restore_team;
    const GuestRun* g = GuestRunFor(event);
    if (!g) return Act::none;
    const bool gate = index == g->index && ((g->gate == Gate::ChrTypeHost && bank == 4 && id == 3) ||
                                            (g->gate == Gate::EndIfClient && bank == 1003 && id == 6));
    const bool on = client && rule_on;
    if (gate) return on ? Act::pass_gate : Act::restore_gate;
    return on && ProgressWrite(bank, id) ? Act::skip : Act::none;
}

// ---- assassins (the hunter that hunts the hunters) ----

constexpr std::int32_t kHiddenEffect = 5560;  ///< SpEffect of a waiting assassin (2004[8] / [21])
constexpr std::int32_t kShownEffect = 8060;   ///< SpEffect of an assassin in the fight

struct Assassin {
    std::uint32_t entity;
    std::uint32_t appeared;  ///< X4: on while it fights (its appear event sets it, the return clears it)
    std::uint32_t done;      ///< X8: repelled / dead (it never comes again)
    const char* who;
};
inline constexpr Assassin kAssassins[] = {
    {3500936, 13505911, 13505912, "Brador (Research Hall)"},
    {3600900, 13605960, 13605970, "a Fishing Hamlet assassin (1)"},
    {3600901, 13605961, 13605971, "a Fishing Hamlet assassin (2)"},
    {3600902, 13605962, 13605972, "a Fishing Hamlet assassin (3)"},
};

struct AssassinPlan {
    bool show = false;  ///< drop 5560, add 8060, hittable (as the host's appear event)
    bool hide = false;  ///< add 5560, drop 8060, untouchable (as the host's return event; only one we showed)
};
/// A guest's copy of an assassin: `host_appeared` / `host_done` the host-world flags, `hidden` the
/// guest's chr carries 5560, `ours` this game showed it.
constexpr AssassinPlan PlanAssassin(bool rule_on, bool host_appeared, bool host_done, bool hidden, bool ours) {
    AssassinPlan p;
    if (!rule_on) {
        p.hide = ours && !hidden;  // the rule went off: back as the guest's own event left it
        return p;
    }
    p.show = host_appeared && !host_done && hidden;
    p.hide = !host_appeared && !host_done && ours && !hidden;
    return p;
}

// ---- covenant outcomes (per player) ----

/// The quest states whose end defines a covenant (Annalise 1040-1059, Alfred 1340-1359, Valtr
/// 1790-1809): per player by the user's decision (the covenants work, party_flags.inc
/// covenant_outcome). Nothing here writes them: a guest's copy of a GuestRun event skips every flag
/// write (Decide), so the host's Alfred executing Annalise reaches no guest's save from here.
constexpr bool CovenantOutcomeFlag(std::uint32_t id) {
    return (id >= 1040 && id <= 1059) || (id >= 1340 && id <= 1359) || (id >= 1790 && id <= 1809);
}

// ---- bell helpers and the boss fog (npcfog2077, NW-dd78dd9 part 2) ----
//
// A Beckoning-Bell NPC helper (Alfred before the Blood-starved Beast, the Old Yharnam / Cathedral Ward /
// Forbidden Woods / Nightmare helpers) joins the fight by two map events (docs/party/npc_fights.md): xx04450
// waits for the host-entered flag of the boss (xx04800) and sends the NPC to the fog front (AI command 990
// to the fog's region), xx04460 waits for the NPC inside that region and walks it through the fog into the
// arena. Both run on the host's game only (1003[6] end on a client) and both assume the HOST walks in: the
// host's chr is at the fog, the NPC beside it. When a guest goes through the fog first (the guest's fog
// report sets the host-entered flag, the host stays outside) the NPC followed the party's other chr away
// from the fog instead (live npf run: it walked 50 m back east and stood there for the whole fight), so
// xx04460 never saw it at the fog front and it never entered. The host's game puts a helper that is
// still outside the arena some seconds after the host-entered flag at the arena's entry spot (the spot
// the player lands on, xx04460's first arena region) - the same place the event's own walk ends.
// Data: party_npcfog.inc (tools/party/npcfog_tool.py gen).

struct FogHelper {
    std::uint32_t npc;        ///< the helper's chr entity
    std::uint32_t join;       ///< xx04420: it is in the party (its sign was answered)
    std::uint32_t leave;      ///< xx04430: it left
    std::uint32_t host_flag;  ///< xx04800: the boss's host-entered flag (the fight is on)
    std::uint32_t region;     ///< the arena's entry region (xx04460's first arena region)
    float x, y, z, yaw;       ///< that region's spot
};

inline constexpr FogHelper kFogHelpers[] = {
#include "party_npcfog.inc"
};

constexpr double kFogFollowGrace = 10.0;  ///< s after the host-entered flag: the events' own walk gets this long
constexpr float kFogFollowNear = 15.0f;   ///< m: a helper this close to the entry spot is in the arena

enum class FogFollow : std::uint8_t {
    none,  ///< nothing to do (no fight, no helper, or already handled)
    wait,  ///< the fight is on: the events' own walk has the grace period
    warp,  ///< outside the arena after the grace: put it at the entry spot
    done,  ///< inside the arena: nothing to do for this fight
};

/// One helper's tick on the host's game. `fight`: its host-entered flag is on; `joined` / `left`: its
/// xx04420 / xx04430 flags; `alive`: chr present, enabled, HP above 0; `since`: seconds since `fight`
/// became true; `dist`: metres from the helper to the entry spot.
constexpr FogFollow PlanFogFollow(bool rule_on, bool fight, bool joined, bool left, bool alive, double since, float dist,
                                  bool handled) {
    if (!rule_on || !fight || !joined || left || !alive || handled) return FogFollow::none;
    if (dist <= kFogFollowNear) return FogFollow::done;
    return since < kFogFollowGrace ? FogFollow::wait : FogFollow::warp;
}


}  // namespace coop::npcfight
