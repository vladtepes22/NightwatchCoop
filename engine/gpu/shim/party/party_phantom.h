// SPDX-License-Identifier: GPL-3.0-or-later
// Party phantoms as full players (docs/party/phantom_limits.md): what a guest gets besides
// following the host (party_travel.h).
//
//   1. Death / Hunter's Mark: a guest that dies (PartyGhostDeath_2) or uses a Hunter's Mark goes
//      to the host's last lamp in its own world instead of its pre-summon spot (the redirect is in
//      party_travel's funnel hook; this file keeps the host's lamp id current there), then the
//      director's Small Resonant Bell brings it back (auto-rejoin).
//      host -> guests: EVENT "phantom" {kind "lamp"} whenever the host's last lamp
//      (WorldTransitionState +0x1528) changes or the party's membership changes.
//   2. Vials / bullets: the guest runs the game's own auto-replenish (0x14DBB70 Blood Bullets
//      reset, 0x14DACE0 vials and bullets from storage, the pair MoveMapStep 0x19398D9 runs) on
//      the main thread, world up and no load pending, after the host rested (EVENT "phantom"
//      {kind "rested"}: the host warped into the Hunter's Dream from a lamp, died, or used a
//      Hunter's Mark: the loads that refill the host) and after its own redirected respawn.
//   3. Confinement walls: the EMEVD skip rules "7600:2005:3@6" and "7600:2006:2@7" (common event
//      7600 マルチ閉じ込め壁: skip only "wall on" and "wall SFX on"), active only while B1 travel is
//      on. The rule syntax "event:bank:id[@index]" is parsed here (seamless_rules uses it).
//   4. Insight: a white phantom already gets +1 Insight natively on a boss kill (BlockClear2
//      0x13849A0, chr type 1 branch, SpEffect 4680 at 0x13852B1); the host's own boss Insight
//      (common event 9350 "SAN値獲得", N x SpEffect 4680, `End if Client` at instruction 0) never
//      runs on a guest. The host's EMEVD filter sees a boss-defeat instruction (2003[12]/[15]/[53])
//      and then `2000[0] InitializeEvent(slot, 9350, N)` in the same event, and sends EVENT
//      "phantom" {kind "insight", n}; the guest applies SpEffect 4680 (PlayerIns vfunc +0x3F0,
//      BlockClear2's own call) one at a time, 15 frames apart. A 9350 start without a defeat
//      (first encounter, story) is sent with boss = false (all N). Insight the host's events give
//      with `2004[8] SetSpEffect(10000, 4680)` outside 9350 (the first kidnap's waking scene, the
//      Chalice Dungeon boss encounter / kill events) is counted per event (DirectInsight) and sent
//      the same way; a chalice layer boss's +2 counts as a boss kill (the native +1 is part of it).
//   5. Hunter's Mark for guests: the EquipParamGoods enable_white / enable_multi bits of goods 100
//      and 1400 (seamless_rules param rules; off by default).
//
// Env (party mode): BB_PARTY_GUEST_RESPAWN=0 (guests go home on death, vanilla),
//   BB_PARTY_GUEST_REFILL=0 (no vial refill), BB_PARTY_OPEN_WORLD=0 (confinement walls stay),
//   BB_PARTY_GUEST_INSIGHT=parity|full|0 (default parity: the guest ends with the host's N for a
//   boss kill: N - 1 on top of the native +1; full: N on top), BB_PARTY_GUEST_MARK=1 (Hunter's
//   Mark usable by guests), BB_PARTY_PHANTOM_PROBE=insight,refill (offline probe: once, 5 s after
//   the world is up: one SpEffect 4680 / one refill, logging Insight / vial counts around it).
//
// The pure part has no game dependency and is unit-tested (tests/test_party_phantom.cpp, built
// with BB_PARTY_PHANTOM_NO_GAME and BB_PARTY_TRAVEL_NO_GAME).
#pragma once

#include "party_travel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace party {
class PartyLink;
}

namespace coop {

// ---- EMEVD skip rules: "event:bank:id[@index]" ("*" = any event), comma separated ----
struct EmevdSkipRule {
    std::int32_t event = -1; ///< -1: any event
    std::int32_t bank = 0, id = 0;
    std::int32_t index = -1; ///< instruction index in the event (+0xA0); -1: any
    bool needs_travel = false; ///< only while B1 travel is on (built-in open-world rules)
    bool client_only = false;  ///< only while the local player is a client (session role 6)
    /// 0.7.4: only while the party's "Open world" rule is on (the host's, read live: party_status
    /// HostRuleOn(OpenWorld)); the confinement walls.
    bool open_world = false;
};
/// Parses `text`; good items go to *out, each bad item adds a message to *errors (when given).
/// Returns the number of rules added.
std::size_t ParseEmevdSkipRules(const std::string& text, std::vector<EmevdSkipRule>* out,
                                std::vector<std::string>* errors = nullptr);
bool EmevdSkipMatches(const EmevdSkipRule& r, std::int32_t event, std::int32_t bank, std::int32_t id,
                      std::int32_t index);
std::string DescribeSkipRule(const EmevdSkipRule& r);
/// Every co-op wall event's wall-on (2005[3]) and SFX-on (2006[2]) instructions (bossfog::
/// ConfinementWalls: common 7600, its Chalice Dungeon copy 12907600, m34 13400010 behind Ludwig),
/// needs_travel (the open world).
std::vector<EmevdSkipRule> ConfinementWallRules();
/// Party mode, whatever the open world: a progress wall's "not in multiplayer" condition (13400010:
/// down at Ludwig's death while the party stays) and, on a client, the re-fight events' client
/// skip of the bosses whose first fight starts by the host's action (bossfog::FirstFights: the
/// room stays open until the fight starts).
std::vector<EmevdSkipRule> BossRoomRules();

// ---- EVENT "phantom" (host -> guests) ----
constexpr const char* kPhantomEventName = "phantom";
/// BossBonus (statsaudit finding 4): the host's boss clear bonus (BlockClear2's 0x1335400 result,
/// PlayerGameData +0xF8): a guest whose own defeat did not run is paid the same.
enum class PhantomEventKind : std::uint8_t { Unknown = 0, Lamp, Rested, Insight, BossBonus };
const char* PhantomEventKindName(PhantomEventKind k);
struct PhantomEvent {
    PhantomEventKind kind = PhantomEventKind::Unknown;
    std::uint64_t seq = 0;        ///< host counter (de-duplication of rested / insight)
    std::uint32_t lamp = kTravelNone; ///< Lamp: the host's last lamp id (+0x1528 low dword)
    std::int32_t insight = 0;     ///< Insight: N of the host's 9350 start
    std::uint32_t source_event = 0; ///< Insight: the host's EMEVD event (diagnostics)
    bool boss = true;             ///< Insight: a boss kill (the guest's game adds +1 natively); false =
                                  ///< a first encounter / story Insight (G2): the guest gets all N
    std::int32_t bonus = 0;       ///< BossBonus: the host's boss clear bonus in Blood Echoes
};
std::string PhantomEventToJsonText(const PhantomEvent& e);
bool PhantomEventFromJsonText(const std::string& text, PhantomEvent* out, std::string* error = nullptr);
/// A host's phantom event before it reaches the game (bbport security pass): false (with `why`)
/// for a Lamp whose id is no ReturnPointParam row (TravelLampKnown; the guest's death redirect
/// warps there through 0x13CDF30).
bool SanitizePeerPhantom(const PhantomEvent& e, std::string* why = nullptr);

/// A host travel that refills the host (and so the guests): lamp into the Hunter's Dream
/// (respawn mode 2), host death, Hunter's Mark.
bool RestedFromTravel(const TravelIntent& t);

// ---- Insight ----
enum class InsightMode : std::uint8_t { Off, Parity, Full };
InsightMode InsightModeFromString(const char* s); ///< nullptr / "" -> Parity
const char* InsightModeName(InsightMode m);
/// SpEffect 4680 applications for the guest when the host's boss kill granted `host_n`.
int GuestInsightGrant(int host_n, InsightMode mode);
/// pard4 (the guest's Insight was doubled, live B2 2026-10-15: Logarius killed with the guest in
/// the fight: host +3, each guest +6 - its own copy of the defeat event ran and paid the boss's
/// Insight natively, and the relay added its +2 on top; a guest-started first encounter +2): a
/// relayed grant is settled about 5 s after it arrives against what this guest's own game paid
/// meanwhile. `want` is the Insight this guest should end up with from the moment (the relay's
/// grant plus the native cooperator +1 it assumes for a boss kill), `native_gain` the Insight its
/// game gave on its own since just before the moment; the result (0..grant) is what the relay
/// still adds.
int InsightTopUp(int grant, int want, int native_gain);

/// Host: pairs a boss-defeat instruction with the 9350 start of the same event.
class BossInsightTracker {
public:
    static constexpr double kWindow = 120.0; ///< s between the defeat marker and the 9350 start
    static constexpr std::int32_t kInsightEvent = 9350;
    /// 2003[12] / [15] / [53] ran in `event`.
    void OnBossDefeat(std::int32_t event, double now);
    /// `2000[0]` started `target` with first argument `n` from `event`: N when this is a boss
    /// kill's Insight (once per defeat), else nothing.
    std::optional<int> OnInitializeEvent(std::int32_t event, std::int32_t target, std::int32_t n, double now);
    /// A defeat marker ran in `event` within the window (not consumed; DirectInsight's boss test).
    bool DefeatedRecently(std::int32_t event, double now) const;

private:
    std::unordered_map<std::int32_t, double> defeats_;
};

/// Host (G2): Insight the host's EMEVD grants with SpEffect 4680 on the player directly, outside
/// common event 9350 (whose starts BossInsightTracker reads): the first kidnap's waking scene
/// (common 9422) and the Chalice Dungeon boss events of m29 (12901588/589/591: +2 at a layer boss
/// kill; 12901708/712: the kill reward table; 12901718/722: the first-encounter table). Every one
/// of them ends on a client (`1003[6]` multiplayer state 1) or waits for chr type 0, so a guest
/// never gets it. Each `2004[8] (10000, 4680)` of an allowed event counts +1; an event's count is
/// handed out once it has been quiet for kQuiet seconds (its 4680s come 10 frames apart).
class DirectInsight {
public:
    static constexpr std::int32_t kPlayer = 10000;
    static constexpr std::int32_t kSpEffect = 4680;
    static constexpr double kQuiet = 1.0;
    static constexpr int kMaxPerEvent = 20;
    /// The events above (9350 itself and the summon refund 9183 are not: 9350 goes through
    /// BossInsightTracker, 9183 gives back Insight the player's own NPC summon took).
    static bool Allowed(std::int32_t event);
    /// `2004[8]` ran in `event` with these arguments; true when it was counted.
    bool OnSetSpEffect(std::int32_t event, std::int32_t entity, std::int32_t speffect, double now);
    struct Grant {
        std::int32_t event;
        int n;
    };
    /// The events that have been quiet for kQuiet seconds, with their count (then forgotten).
    std::vector<Grant> Due(double now);

private:
    struct Pending {
        int n = 0;
        double last = 0;
    };
    std::unordered_map<std::int32_t, Pending> pending_;
};

/// Guest: spaces SpEffect applications (the game's 9350 waits 10 frames between them).
class InsightDripper {
public:
    static constexpr int kInterval = 15; ///< ticks between applications
    static constexpr int kMaxPending = 99;
    void Add(int n);
    /// Once a frame; true = apply one now (then counted as done).
    bool Step(bool allowed);
    int Pending() const { return pending_; }

private:
    int pending_ = 0;
    int wait_ = 0;
};

/// Guest: caps the Insight a host's event stream can grant (bbport security pass) without dropping
/// legitimate grants that come close together (G2: a boss's first-encounter Insight and its kill
/// Insight a few seconds apart, two story moments in a row). Rolling window of kWindow seconds.
class InsightBudget {
public:
    static constexpr double kWindow = 60.0;
    static constexpr int kMaxPerEvent = 10;
    static constexpr int kMaxPerWindow = 20;
    /// Insight the guest may apply now for an event of `n` (0 = none), and books it.
    int Take(int n, double now);

private:
    struct Grant {
        double at;
        int n;
    };
    std::vector<Grant> grants_;
};

// ---- Vial refill ----
enum class RefillReason : std::uint8_t { HostRested, GuestRespawn, Probe };
const char* RefillReasonName(RefillReason r);
class RefillScheduler {
public:
    static constexpr int kStableTicks = 30;     ///< world steady this many frames first
    static constexpr double kLoadWait = 45.0;   ///< s to wait for the expected load to start
    static constexpr double kTimeout = 240.0;   ///< s: give up
    /// `expect_load`: a warp is under way; refill after it, not before.
    void Request(RefillReason why, bool expect_load, double now);
    /// Once a frame with the game state; true = refill now (the request is then done).
    bool Step(const ReplayState& s, double now);
    bool Pending() const { return pending_; }
    RefillReason Reason() const { return why_; }

private:
    bool pending_ = false;
    bool expect_load_ = false, load_seen_ = false;
    RefillReason why_ = RefillReason::HostRested;
    double since_ = 0;
    int stable_ = 0;
};

/// Host: when to (re)send the last lamp: it changed, or the party's members changed.
class LampAnnouncer {
public:
    bool ShouldSend(std::uint32_t lamp, std::uint64_t members_key);

private:
    bool sent_ = false;
    std::uint32_t lamp_ = kTravelNone;
    std::uint64_t members_ = 0;
};

#ifndef BB_PARTY_PHANTOM_NO_GAME
/// PartyInit, after InstallTravelPatches: reads the env, connects the travel redirect.
void PhantomInit();
/// The EMEVD filter wants the boss-Insight observer (insight mode on, party mode).
bool PhantomWantsEmevd();
/// Built-in EMEVD skip rules (the confinement walls unless BB_PARTY_OPEN_WORLD=0).
std::vector<EmevdSkipRule> PhantomSkipRules();
/// From the EMEVD filter (game thread), for 2003[12/15/53], 2000[0] and 2004[8] (DirectInsight) only.
void PhantomEmevdInstruction(std::int32_t event, std::int32_t bank, std::int32_t id, const std::uint8_t* args,
                             std::size_t size);
/// Director (host), for every travel it sends.
void PhantomNoteHostTravel(const TravelIntent& t);
/// Director tick (main thread): host announcements over `link`, guest refill / Insight.
void PhantomTick(party::PartyLink* link);
/// Guest: EVENT "phantom" (PartyLink callback thread).
void PhantomOnEvent(const std::string& body);
#endif

} // namespace coop
