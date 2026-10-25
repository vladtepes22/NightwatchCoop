// SPDX-License-Identifier: GPL-3.0-or-later
// D3 / D8: a guest's actions change the host's world (docs/party/world_actions.md).
//
// A summoned guest plays in the host's world: its game reads and writes the host-world flag
// overlay (load mode 1, event_flags.md 3), which the host-world mirror keeps equal to the host's
// world (party_progress "world mirror"). What the guest's own game does there - a lever, a door
// (also a one-way door opened from the other side), a shortcut, an elevator, a ladder kicked
// down, a bridge, an illusory wall, an NPC talk - sets flags in that overlay only. Those flags are
// the guest's "local changes": overlay bits that differ from what the host's image last put there
// (GuestWorldMirror::LocalChanges).
//
// Guest (main thread, load mode 1, steady world): every 0.25 s the local changes of the allowed
// categories that are NEW since the guest's world became steady (a baseline taken 3 s after each
// load / full mirror copy holds back what the guest's own game set while loading the map - client
// branches of events end early and write their completion flags) go to the host as
// EVENT "world_action" {v, map, set:[ids], clr:[ids]}; the same set bits go into the guest's own
// save (the pool), so its own world keeps the progress.
// Host (main thread, own world): validates every id again (WorldActionAllowed: mirror group and
// mask, category whitelist, the host's area, rate limit) and sets / clears it with SetEventFlag;
// the mirror carries the result back to every guest, C2 into every guest's save.
//
// Categories: shortcut_door, world_state, key_event (set; shortcut_door / world_state also clear:
// elevators toggle), npc_quest (set and clear: quest steps move state flags; the user's decision:
// NPC quest choices are shared). Never:
// boss_defeated, boss_area, lamp_unlocked (EVENT lamp_lit), cutscene_seen, item_lot_picked
// (instanced per player, D4), session_runtime, time_of_day, never_sync.
//
// The pure part is unit-tested (tests/test_party_worldact.cpp) and fuzzed (tests/party_fuzz.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "party/party_lift_rules.h"

namespace coop {
struct GameSnapshot;
}
namespace party {
class PartyLink;
}

namespace coop::worldact {

constexpr const char* kEventName = "world_action";
constexpr std::size_t kMaxSet = 32, kMaxClear = 16;
/// Host: flags one guest may change per 10 s window.
constexpr int kRateFlags = 96;
constexpr double kRateWindow = 10.0;

/// May a guest set (clear = false) or clear (clear = true) flag `id` in the host's world, the host
/// standing in map `host_map` (0xAABBCCDD; areas must match for map flags)?
bool WorldActionAllowed(std::uint32_t id, bool clear, std::uint32_t host_map);
/// Rule "party quest progress is shared by everyone" (pard3): a boss a guest killed while it stood
/// alone in a map the host is not in (split travel) is the host's progress too: its defeat flag
/// (boss_defeated, set only) of the area the guest stands in goes to the host's save (no rewards
/// for the host; the guest's game gave its own). Only while apart: in the host's world the
/// host's own game has the kill.
bool BossKillActionAllowed(std::uint32_t id, bool clear, std::uint32_t member_map, bool apart);

/// An NPC gift trigger: the flag a talk script sets to hand over its gift, which common event 9040
/// (slots 2-37) waits for to award the gift's item lot (9042, 9045-9052, 9054-9064, 9066, 9067, 9069,
/// 9071-9077; quest_audit.md P1). 9040 ends at once on a client, so a guest's talk awards nothing
/// in its own game: the guest sends the trigger to the host (set only), whose 9040 awards the lot
/// and C3 hands it to every member. 9043 is a repeatable 9100 slot, not one of these.
bool NpcGiftTrigger(std::uint32_t id);

/// P2 (quest_audit.md): the repeatable NPC gifts of common event 9100 (`NPCアイテム入手_何度でも`):
/// the trigger flag a talk script sets -> the item lot 9100 awards (then it clears the trigger):
/// 72400489 -> 28000 Blood of Arianna, 72400398 -> 32020 Blood of Adella (Oedon Chapel), 72410310
/// -> 23000 Iosefka's Blood Vial, 72410359 -> 24060 Blue Elixir x3 (Iosefka's clinic), 9043 ->
/// 17010 Queenly Flesh (Annalise). 9100 ends at once on a client, so a guest's talk awarded
/// nothing, and as a world action the trigger made the HOST's 9100 award the guest's gift to the
/// host. Now the trigger stays local, the guest gives the lot to itself (items OfferRepeatGift) and
/// clears the trigger as 9100 would. -1: not such a trigger.
std::int32_t RepeatGiftLot(std::uint32_t trigger);

/// 0.7.3: NPC gifts a talk hands over through a map EMEVD award, not common 9040 (m27 event 12700906:
/// ends when the lot's personal flag is set, tests the player's character type 4[3](10000, 0), waits
/// for the trigger, then the host-only award 2003[4]): Valtr's 72700321 -> 43120 Impurity rune (flag 6676), 72700322 ->
/// 43130 League Staff (flag 6678). A guest who joins the League in the host's world sets the trigger
/// only in the host-world overlay (discarded) and its own copy of the event never awards, so the
/// guest gives the lot to itself (party_items OfferOwnGift: own inventory, own personal flag). The
/// trigger is never a world action (as one, the HOST's 12700906 gave the host the guest's rune).
/// -1: not such a trigger.
std::int32_t OwnTalkGiftLot(std::uint32_t trigger);

constexpr std::size_t kMaxKicks = 4, kMaxFog = 2, kMaxStory = 2, kMaxLift = 4;

/// D3 (lifts071): lifts (elevators), their table and the party's decisions about them: the rules
/// header (party_lift_rules.h, unit party-lift-rules-test).
using liftrules::Lift;
using liftrules::LiftByCar;
using liftrules::LiftByPosFlag;
using liftrules::Lifts;
using liftrules::LiftSync;
using liftrules::HostLiftSync;
using liftrules::LiftNeedsReplay;
using liftrules::GuestLiftRideDone;
/// Any role, main thread (liftfall): replays lifts' ends (Lift::Replays) moved by another game.
void LiftReplayTick(const GameSnapshot& s);
/// liftfall: the local chr at `p` stands within kLiftCarRadius m (horizontally) of a lift car that
/// plays an animation (a ride): the fall recovery's "on a moving object" (the chr's own ride flag
/// is not set on the local chr: live, Mensis lift 2601240).
constexpr float kLiftCarRadius = 4.0f;
/// The local player stands in a car (the library lift's two cages are 1.7 m apart).
constexpr float kLiftInCarRadius = 1.2f;
bool OnMovingLiftCar(const float p[3]);
/// liftfall: a ride of lift `l` runs on this game (its car animates) or the local player stands
/// within `radius` m (horizontally) of its car. False when the car is not loaded.
bool LiftBusyHere(const Lift& l, float radius);

struct Request {
    std::uint32_t map = 0;  ///< the guest's map when it sent this (logged; not trusted)
    std::vector<std::uint32_t> set, clr;
    /// ObjAct kick event ids the guest's player kicked (ObjAct entry +0xC): the host replays the
    /// kick on its own object so it animates there too (when its game did not get it itself).
    std::vector<std::int32_t> kick;
    /// B2: guest-entered flags of boss fogs (BossGate::guest_flag) the guest just went through:
    /// the host opens that fight (BossGateHostOpens). Never written into anyone's save.
    std::vector<std::uint32_t> fog;
    /// D10: story events (StoryTrigger::event) whose trigger spot the guest reached: the host's chr
    /// runs the event (its scene plays for the whole party). Never written into anyone's save.
    std::vector<std::uint32_t> story;
    /// D3 (lifts071): lift position flags (Lift::pos) the guest's own copy of a lift's events moved
    /// to 1 / 0 (a finished ride); the host applies them when its own copy did not run (HostLiftSync).
    std::vector<std::uint32_t> lift_set, lift_clr;
    bool operator==(const Request& o) const {
        return map == o.map && set == o.set && clr == o.clr && kick == o.kick && fog == o.fog && story == o.story &&
               lift_set == o.lift_set && lift_clr == o.lift_clr;
    }
};
std::string RequestToJson(const Request& r);
/// Parses and validates the shape (sizes, duplicates, u32 ids, each id allowed in some map:
/// WorldActionAllowed with the id's own area). The host re-checks against its own map.
bool RequestFromJson(const std::string& text, Request* out, std::string* error);

/// Host: a sliding count of flags per guest slot.
class RateLimit {
public:
    /// True (and counted) when `n` more flags from `slot` at time `now` stay within the limit.
    bool Take(int slot, int n, double now);

private:
    struct Slot {
        double start = -1e18;
        int used = 0;
    };
    Slot slots_[8];
};

/// Talk scripts a guest may run in the host's world (D8): every world NPC's script - not the
/// Hunter's Dream's own (guest services decide those), not lamps / headstones (lamp requests),
/// not Gehrman's ending choice.
bool WorldTalkAllowed(std::uint32_t talk);

/// Catch-up (user decision: a guest behind the host gets every boss into its own save): the area
/// gate flags 2AAx / 3AAx a boss's defeat event writes next to its boss flag (EMEVD data,
/// docs/party/world_actions.md 7). C2 never syncs them (they are never_sync: 2410-style flags
/// block summons in the host's world); the guest derives them in its own pool from its own boss
/// flags. `pairs` = {boss flag, gate flag}; 2410 needs both 12411800 and 12411700.
struct GateRule {
    std::uint32_t boss, gate, also = 0;  ///< also: a second boss flag the gate needs (0: none)
};
const std::vector<GateRule>& GateRules();

/// B2 (ERSC parity): any party member may walk through a boss fog first. Vanilla lets a guest
/// through only after the host ("host entered" flag ON, condition 3[0] of each guest-entry event
/// `ゲストがボス部屋入場_*`). From every map EMEVD: the guest-entry event, the host-entered flag the
/// boss's start event waits on, the guest-entered flag the event sets (network-synced: the host's
/// game receives it), the boss-defeated flag. Amygdala (13204831) is left out: its guest flag is
/// an event argument.
struct BossGate {
    std::uint32_t guest_event, host_flag, guest_flag, boss_flag;
    /// The guest-entry event's other ON condition: the host's first-fight entry (usually an intro
    /// cutscene) has run. It stays a condition, so a FIRST fight still starts with the host; 0 none.
    std::uint32_t intro_flag = 0;
    std::int32_t fog_entity = 0;  ///< the fog's map object (the event's action-button target)
    /// A separate event that sets guest_flag (Rom: the guest-entry event only walks the player in;
    /// 13204821 `ゲストが着地` sets 13204801 once the guest stands in the arena, behind a 3[0] on
    /// host_flag too, and gives it 5 s of invincibility): its host-entered condition is skipped
    /// like the entry event's. 0: the entry event sets guest_flag itself.
    std::uint32_t landing_event = 0;
};
const std::vector<BossGate>& BossGates();
const BossGate* BossGateByEvent(std::int32_t guest_event);
/// The gate whose landing_event is `event_id` (Rom), else null.
const BossGate* BossGateByLandingEvent(std::int32_t event_id);
const BossGate* BossGateByGuestFlag(std::uint32_t guest_flag);
/// Guest: skip condition instruction bank:id (3[0] IfEventFlag on `flag`) of event `event_id`?
/// Only the host-entered condition of a guest-entry event (or of a gate's landing event); the
/// boss-alive and intro-seen checks stay.
bool BossGateSkipCondition(std::int32_t event_id, int bank, int id, std::uint32_t flag);
/// Host: open the boss fight for a guest who went through the fog first (set host_flag)? The boss
/// lives, a guest is in, the host's own flag is not yet set and the host stands in the boss's area
/// block (map 0xAABBCCDD).
bool BossGateHostOpens(const BossGate& g, bool guest_in, bool host_in, bool boss_dead, std::uint32_t host_map);
/// Guest at a boss fog (playtest 1 bug 8): the fog has no prompt because the host has not had its
/// first fight here yet (intro_flag off). The guest is told to wait for the host.
bool BossGateWaitsForHost(const BossGate& g, bool intro_done, bool host_in, bool boss_dead);
/// The map 0xAABBCCDD is the gate's area block.
bool BossGateInBlock(const BossGate& g, std::uint32_t map);
/// P5 / D10 (a guest starts a first fight): the gates whose host first-fight event (intro_flag,
/// which is that event's own id) waits only on the host's position - a 3[2] region or a 3[3]
/// distance condition next to its flag and chr-type checks (EMEVD data, quest_audit.md P5). A
/// guest may go through such a fog before the host's first fight; the host's game then runs the
/// first-fight event as if the host had walked in, so its intro scene plays for the whole party
/// (live mirror) and the boss starts. DLC (pard2): Ludwig and the Orphan of Kos (a two-part first
/// fight: a first event waits for the host in the region, the scene event for that event AND the
/// host still in the region) and the Living Failures (one region) too. Host-first (an action in
/// the room starts them, bossfog::FirstFights): Laurence (the host holding Laurence's Skull at the
/// altar, 3[4] goods 4014 + 3[2] 3402855; player report 2026-10-22: a guest's walk through his fog
/// was taken for the fight's start and shut it in),
/// Ebrietas (her first-fight event waits for the HOST's own hit on her, 3[23] attacker 10000),
/// Lady Maria (her first fight follows the host's examining her in her chair, 13500948), the
/// Celestial Emissary (no first-fight event); Rom has no fog gate (anyone's hit starts her fight,
/// 3[23] attacker -1, so a guest may start it already).
bool BossGateGuestStarts(const BossGate& g);
/// Guest: the intro (first-fight-done) condition of a BossGateGuestStarts gate's guest-entry
/// event (3[0] on intro_flag): skipped, so the fog has its prompt before the host's first fight.
bool BossGateSkipFirstFight(std::int32_t event_id, int bank, int id, std::uint32_t flag);
/// The gate whose host first-fight event is `event_id`, else null.
const BossGate* BossGateByIntroEvent(std::int32_t event_id);
/// Host: where the host's chr must stand for one moment so its first-fight event's position
/// condition holds (MSB data): the 3[2] region's origin raised 0.5 m (inside the box), or 2 m from
/// the 3[3] distance target. EMEVD conditions are registered once when the event reaches them (the
/// dispatcher counts ~12k instructions a minute), so a later skip cannot pass a condition that is
/// already waiting: the host's chr visits the spot instead and is put back as soon as the event
/// went on (pard live S1b, 2026-10-15).
struct FirstFightSpot {
    std::uint32_t intro_event;
    float pos[3];
    /// Two-part first fight (the DLC's 前半 / 後半 events): the first part sets 9180 before the
    /// scene event has seen the host in the region, so 9180 does not end the visit.
    bool chained = false;
    /// The first-fight event plays an intro scene (2002): the host stays at the spot until the
    /// scene starts and through it (the scene ends with the player at the arena's entrance; a
    /// host elsewhere was killed there, live pard2), then goes back.
    bool scene = false;
};
const FirstFightSpot* FirstFightSpotFor(std::uint32_t intro_event);

/// D10 (pard3): a story scene whose event only the host's chr can run (4[3] chr type 0 next to a
/// distance condition), so a guest standing at the spot started nothing. The Blood Moon (m32_00
/// 13201803, `ボス撃破後花嫁出現`: Rom dead (13201800) -> the player within 12 m of Yharnam's
/// phantom 3200801 -> scene 32000000 with the time of day 3, 70002802, story warp 2802958). The
/// guest reports the spot (world_action "story"); the host's chr visits it like a guest-started
/// first fight (FirstFightSpot), stays through the scene (live mirror for every guest) and is
/// put back; the story warp then asks the host "Travel with the party?" (P8).
struct StoryTrigger {
    std::uint32_t event;       ///< the host's event; its own flag is on once it ran
    std::uint32_t needs_flag;  ///< must be on (the event's 1003[101] / 3[0] gate)
    std::uint32_t done_flag;   ///< on once the scene ran
    float centre[3];           ///< the distance condition's target (MSB position)
    float radius;              ///< the condition's distance
    float spot[3];             ///< where the host's chr stands for it (inside the radius, on ground)
};
const std::vector<StoryTrigger>& StoryTriggers();
const StoryTrigger* StoryTriggerByEvent(std::uint32_t event);
/// The trigger's map block (0xAABB0000 of the event id) is `map`'s.
bool StoryTriggerInBlock(const StoryTrigger& t, std::uint32_t map);
/// Guest: report it now? In its block, the gate flag on, not run yet, within the radius (`me`
/// null: position unknown, no).
bool StoryTriggerDue(const StoryTrigger& t, std::uint32_t map, bool needs_on, bool done_on, bool event_done,
                     const float* me);

/// D6 (pard3): a gate that asks for a key item with EMEVD 3[4] (IfItemHeld) and an action button
/// in the same event. Cathedral Ward's plaza gate (m24_00 event 12400760, `円形広場の門の開閉`):
/// group 1 = Hunter Chief Emblem (goods 4011) NOT held + the button at object 2401220 -> the
/// "need the emblem" dialog; group 2 = held + the button -> the gate opens (12400160). A summoned
/// guest's key items read 0 in the host's world (verify_c6), so its copy always took group 1 even
/// with the emblem in its own save: in vanilla only the host could open the gate. With the emblem
/// in the guest's progress (its own pickup flag, or the host world's: the emblem is a shared key
/// treasure, F2), the guest's copy has both checks inverted (the 0 it reads opens the gate). The
/// host's world then gets 12400160 as a world action and the gate's far-side lever ObjAct
/// (far_kick) is kicked there, so the host's copy of the event opens the gate with its animation.
struct KeyItemGate {
    std::int32_t event;      ///< the gate's event
    std::int32_t goods;      ///< the 3[4] item (equip type 3, goods)
    std::uint32_t own_flag;  ///< the item's pickup flag (ItemLotParam getItemFlagId)
    std::int32_t held_group; ///< the condition group whose 3[4] checks "held" in the data
    std::uint32_t open_flag; ///< the flag the event sets once the gate is open
    std::int32_t far_kick;   ///< the ObjAct event id that opens it in the same event (no player anim)
};
const KeyItemGate* KeyItemGateFor(std::int32_t event_id);
const KeyItemGate* KeyItemGateByOpenFlag(std::uint32_t open_flag);
/// The 3[4] "held state" (its last argument) this game's copy of the gate should check for
/// condition `group`: the data's own (held_group: 1, the other: 0), both flipped when `invert`.
std::uint8_t KeyItemGateHeldState(const KeyItemGate& g, int group, bool invert);
/// Guest: invert the gate's checks? Only a summoned guest (role 6, the host-world overlay) whose
/// own save or the host's world has the item's pickup flag, and whose inventory reads the item as
/// not held (`held` <= 0). In the game's summoned key view every key reads 0; with the own key
/// section shown (items.md 10.7, BB_PARTY_KEY_VIEW) a held emblem reads 1 and the data's own check
/// opens the gate - inverted, it would take the "need the emblem" branch.
bool KeyItemGateInvert(bool summoned_guest, bool own_flag, bool host_world_flag, int held);
/// The visit is over (put the host back): the event went on (the boss-started flag, the intro's
/// boss-area flag 9180 - not for a chained spot - or a cutscene) or it timed out.
bool FirstFightVisitDone(bool host_flag_on, bool flag9180_on, bool cutscene, double visit_s, bool chained = false,
                         bool scene = false);
/// The host may make the visit now (besides a steady world in the boss's block): alive, not in
/// another boss fight (its bar up) and no cutscene. A dead host's visit waits for its respawn.
bool FirstFightHostReady(int host_hp, bool boss_bar_up, bool cutscene);
/// After the first-fight event (its scene) the host stands by the arena (`here` within
/// kFirstFightSceneRadius of the spot) and away from where it was put back: the scene placed it
/// there (Amelia's intro leaves the player at her arena's entrance), so it goes back again.
bool FirstFightPutBackAfterScene(const float here[3], const float back[3], const float spot[3]);
constexpr float kFirstFightBackSlack = 3.0f;
constexpr float kFirstFightSceneRadius = 40.0f;
constexpr double kFirstFightVisitSeconds = 2.0;
constexpr int kFirstFightVisitTries = 3;
/// The host's screen holds its last picture this long after the put-back (the camera settles).
constexpr double kFirstFightHoldAfter = 0.2;
/// After the intro scene the host stays at the spot this long before it is put back.
constexpr double kFirstFightAfterScene = 1.0;
/// What the host does with a guest's fog report.
enum class FogAction {
    none,        ///< boss dead, the host already in, or nothing to do
    open,        ///< the host's first fight was had: set host_flag (the boss starts)
    start_first, ///< first fight, guest-startable: run the host's first-fight event (intro for all)
    wait_host,   ///< first fight, but the host stands in another block: start it once it is there
};
FogAction HostFogAction(const BossGate& g, bool guest_starts_on, bool intro_done, bool host_in, bool boss_dead,
                        std::uint32_t host_map);
/// Seconds a guest's first-fight start waits for the host to reach the boss's block.
constexpr double kFirstFightWaitSeconds = 600.0;
/// The guest's notice at such a fog.
constexpr const char* kBossFogWaitNotice = "Boss fog: the host goes in first (first fight here)";
/// Within this many metres of the fog object the notice shows.
constexpr float kBossFogNoticeRange = 5.0f;
/// D3 (pard4): a guest alone in another area than the host (split travel) is still a client of
/// the host's world there, and the host's game does not have that area loaded: a boss fight's
/// start and defeat events are host-only copies, so they ran nowhere (live LB2: the guest went
/// through the Blood-starved Beast's fog, the boss stayed idle, its HP 0 ran no defeat). Correct
/// over seamless: while the host's area is another one the fog keeps the vanilla phantom rule (no
/// prompt until the host has gone in), a notice says why, and a guest that got through anyway (its
/// fog had opened while the host was still there) is put back outside. host_map 0 / 0xffffffff:
/// the host's place is unknown (no decision: open as before).
bool GuestFogOpenWhileHostAt(const BossGate& g, std::uint32_t host_map);
/// The guest's notice at a fog while the host is in another area.
constexpr const char* kBossFogApartNotice =
    "Boss fight: the host is in another area. Travel together to fight this boss";
/// Seconds after a guest went through a fog while the host is elsewhere before it is put back
/// outside (the walk through the fog ends first).
constexpr double kFogApartPutBackAfter = 2.5;
/// Game side, any thread (the EMEVD dispatch): may this guest's guest-entry event `guest_event`
/// skip its host-entered / first-fight condition now? False while the host is in another area.
bool GuestFogSkipAllowed(std::int32_t guest_event);
/// The notice is due: the gate's flags were read, the gate waits for the host and the guest
/// stands within kBossFogNoticeRange of the fog (dist_m < 0: unknown, no notice).
bool BossFogNoticeDue(const BossGate& g, bool read, bool intro_done, bool host_in, bool boss_dead, float dist_m);
/// The local player's position for distance checks (playtest 1 bug 8, live 2026-10-14): the
/// physics position (ChrIns +0x58 -> +8 -> +0x3B0 -> +0x68: +0x1E0) when readable, else the chr's
/// +0xC0. On a guest in the host's world +0xC0 stays at its arrival spot (it read 19 m from the
/// fog while the guest stood 3 m from it), so the fog notice never fired. Null: neither readable.
const float* PickPlayerPos(bool physics_ok, const float* physics, bool drawn_ok, const float* drawn);

// ---- game side ----
void Init();
bool Enabled();
/// Host: EVENT world_action from `slot` (link thread): parsed, queued for the tick.
bool OnHostEvent(int slot, const std::string& body, std::string* why);
/// Main thread, once a frame (director tick).
void Tick(const GameSnapshot& s, party::PartyLink* link);
/// BB_PARTY_GUEST_FIRST_FIGHT (default on; 0 = the host always has the first fight).
bool GuestFirstFightEnabled();

/// Test fixture: stand the local chr `dist` m from map object `entity`, on the side `angle_deg`
/// from its facing (0 = in front), facing it, `dy` m above the object's origin (a raised ladder's
/// top landing). False when the object is not loaded.
bool StandAtObject(std::int32_t entity, float dist, float angle_deg, std::string* what, float dy = 0.0f);
/// The point StandAtObject would use (DIST m from map object ENTITY at ANGLE degrees from its
/// front), without moving anyone; false when the object is not loaded.
bool ObjectPoint(std::int32_t entity, float dist, float angle_deg, float out[3]);
/// Test aid (lifts071): a map object's animation state, as the EMEVD 2003[18] "wait for the end"
/// condition reads it (0x13CC180): the per-block object entry (0x2E0 bytes) of `entity`'s handle,
/// +0x208 the playing animation id, +0x1FE bit 3 "playing", +0x1FC flags, +0x230 the anim state.
/// False when the object is not loaded.
struct ObjAnimState {
    std::uint64_t entry = 0;
    std::uint32_t handle = 0, flags = 0;
    std::int32_t anim = -1, state = -1;
    bool playing = false;
};
bool ReadObjAnim(std::int32_t entity, ObjAnimState* out);
/// Test fixture: the same for a character (NPC) entity.
bool StandAtChr(std::int32_t entity, float dist, float angle_deg, std::string* what);
/// BB_PARTY_WORLDACT_TEST: the fixture's objects and pickups once more, its "after=" s from now.
void FixtureRearm();
/// Counters for logs / tests.
struct Stats {
    std::uint64_t sent = 0, sent_flags = 0, received = 0, applied = 0, refused = 0, own_save = 0;
};
Stats GetStats();

} // namespace coop::worldact
