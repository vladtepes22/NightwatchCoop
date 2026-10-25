// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance rules (ERSC parity rows C4 death penalty, D7 enemy scaling, E2 overhead display):
// the pure part, no game code (tests/test_party_balance.cpp). The game side is party_balance.h.
//
// D7, enemy scaling. Bloodborne's own co-op scaling (measured in the 1.09 params and code,
// docs/party/four_players.md section 4 and docs/party/balance.md):
//   - bosses: the boss events (EMEVD 1003[109] -> 2004[8]) and Lua_MultiDoping put SpEffect 7500
//     (1 cooperator) / 7501 (2) on the boss (party_fourp H4: 7502 for 3): per cooperator
//     maxHpRate +0.5, status resist rates +0.5, changeSuperArmorPoint (poise) +30, damage +0;
//   - normal enemies: nothing (0x17e46e0 only multiplies SpEffect maxHpRate, the NG+ table and
//     the 9200 level sync).
// Our settings are per extra hunter (cooperator), as a percentage of that vanilla step
// (Nightreign semantics: 100 = vanilla): HP and poise "% of the vanilla boss step"; damage, for
// which vanilla has no step, "+% per extra hunter". Normal enemies use the same boss step as the
// reference (vanilla for them is 0 %). The values go into SpEffect rows 7500-7502 (bosses, by
// cooperator count) and 7503 (normal enemies; unused in 1.09) on every peer, the host's values.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::balance {

constexpr int kRotMaxStacks = 5;
constexpr int kRotHpPercentPerStack = 10;  ///< each stack: max HP -10 % (5 stacks: -50 %)
constexpr float kVanillaHpStep = 0.5f;     ///< SpEffect 7500 - 1.0
constexpr int kVanillaPoiseStep = 30;      ///< SpEffect 7500 changeSuperArmorPoint

/// What is shown above party members (ERSC overhead_player_display).
enum class Overhead : int { Off = 0, Name = 1, Level = 2, Health = 3, Deaths = 4, Ping = 5, Count = 6 };

/// The party's rules: the host's settings, sent to every guest (event "balance").
struct Rules {
    /// C4: the death debuff. Off by default (user decision 2026-10-22; on by default 0.6.0 - 0.7.1):
    /// an unset bbport.ini party_rot is off, a saved value stays.
    bool rot = false;
    /// 0.7.2 (party_rot_rules.h): what cleanses part of the rot besides a rest - a boss's death (all
    /// of it), your killer's death (that death's stack), your bloodstain picked up (the stack of the
    /// death that dropped it). Only while rot is on. On by default; an older host sends none: off.
    bool rot_cleanse_boss = true;
    bool rot_cleanse_killer = true;
    bool rot_cleanse_echoes = true;
    int boss_hp = 150;     ///< % of the vanilla boss HP step (+50 % per cooperator) per extra hunter
    int boss_damage = 10;  ///< + % damage per extra hunter (vanilla: 0)
    int boss_poise = 100;  ///< % of the vanilla poise step (+30 per cooperator)
    int enemy_hp = 50;     ///< normal enemies: % of the boss HP step (vanilla: 0)
    int enemy_damage = 0;  ///< + % damage per extra hunter
    int enemy_poise = 0;   ///< % of the poise step (0..300: below 0 staggered less)
    /// I1: friendly fire / PvP (ERSC Judicator's Rulebook): party members can hurt each other.
    /// Off by default; only the host's value counts (party_pvp.h).
    bool friendly_fire = false;
    /// Statsaudit finding 5 (user decision 2026-10-15): lamps are shared by the whole party - a lamp
    /// any member lit is lit for everyone (each member's own save too). On by default; the host's.
    bool share_lamps = true;
    /// 0.7.2 (user decision 2026-10-21: "host state saves lamps, anybody can light them"): a guest's
    /// lamps from its own save also light up in the host's world (with share_lamps; the 2026-10-15
    /// union). Off by default: the host's world keeps its own lamps, anyone may light more there
    /// (party_guestvals_rules.h lampshare::Decide). A host from before it sends none: off.
    bool guests_bring_lamps = false;
    /// The host's deaths (party_b2; user decision 2026-10-21: in the game's menus): the host gets
    /// up at its lamp without a load, and in the arena after the guests kill the boss. The host's
    /// own settings; sent so a guest's menu shows them. On by default.
    bool host_revive = true;
    bool host_limbo_revive = true;
    /// 0.7.1 (user rule: every party gameplay switch in the menus): the kill record (enemies stay
    /// dead for the party, party_kill_record.h), the NPC presence mirror (party_npc_mirror.h) and
    /// I5 Gather the party (party_gather.h). The host's; a guest follows them. On by default (a
    /// host from before them sends none: on).
    bool kill_record = true;
    bool npc_mirror = true;
    bool gather = true;
    /// 0.7.3 (user decision 2026-10-22): covenant outcomes per player (party_progress
    /// covenant_outcome). The host's; on by default (a host from before it sends none: on).
    bool covenant_per_player = true;
    /// 0.7.3 lamp travel voting (party_vote.h): every member votes on a party travel. The host's;
    /// off by default (a host from before it sends none: off).
    bool travel_vote = false;
    /// D11 (0.7.3, party_npcfight_rules.h): NPC hunters fight the whole party (a guest's copy of the
    /// host-only NPC transitions runs). The host's; on by default (a host from before it sends none: on).
    bool npc_fights = true;
    /// 0.7.5 Party starts "skip the prologue" (party_start.h): a new character starts as after the
    /// prologue. The host's; off by default (a host from before sends none: off).
    bool skip_prologue = false;
    /// 0.7.3 (party_kidnap_rules.h): a Snatcher grabbing a guest takes the whole party to the Hypogean
    /// Gaol, as the host's own grab. The host's; on by default (a host from before it sends none: on).
    bool any_kidnap = true;
    /// 0.7.5 (levelsync, party_levelsync_rules.h): Bloodborne's level sync (a hunter above the host's level is
    /// scaled down: max HP, stamina, defence, resistances) and the phantom health cut (30 % less max HP
    /// for a joined hunter). The host's; on by default = vanilla (a host from before sends none: on).
    bool level_sync = true;
    bool phantom_hp = true;
    /// 0.7.4 (partyrules, user spec 2026-10-23): the session rules that were each player's own until
    /// 0.7.3 and now come from the host, so the party plays one way ("HOST DECIDES"; what reaches a
    /// guest's own save stays the guest's). Read live on every peer (party_status HostRuleOn):
    /// open_world (co-op area walls), guest_revive_inplace (a guest gets up without a load),
    /// guest_respawn (a dead guest comes back at the host's lamp), guest_refill (vials and bullets
    /// with the host's), chalice_rejoin (a guest stays in the host's Chalice Dungeon), story_mirror
    /// (cutscenes live with the host). Read once at start, so also in the join check (party_link
    /// identity_mismatch: "set X like the host"; sent for the guests' read-only rows): guest_lamps,
    /// guest_mark (Hunter's Mark for guests), full_rewards (full echoes and drops for guests). On by
    /// default (a host from before them sends none: on, their default).
    bool open_world = true;
    bool guest_revive_inplace = true;
    bool guest_respawn = true;
    bool guest_refill = true;
    bool chalice_rejoin = true;
    bool story_mirror = true;
    bool guest_lamps = true;
    bool guest_mark = true;
    bool full_rewards = true;
    bool operator==(const Rules& o) const;
    bool operator!=(const Rules& o) const { return !(*this == o); }
};
/// Bloodborne's own behaviour (all scaling as in 1.09, no rot).
Rules Vanilla();
/// 0.7.4: the session rules' JSON keys (= their bbport.ini keys without "party_") and members.
std::vector<std::pair<const char*, const bool*>> SessionRuleFields(const Rules& r);
std::vector<std::pair<const char*, bool*>> SessionRuleFields(Rules& r);

/// 0.7.4: the host's session rules its modules read only once, at start (a guest cannot take the
/// host's value later): every player must start with the host's, or the host refuses the join with
/// "set X like the host" (party_link identity_mismatch). guest_insight: bbport.ini
/// party_guest_insight's index (0 parity "Same as the host", 1 full, 2 the game's own +1);
/// start_immediate: BB_PARTY_START=immediate (the launcher's "Party starts: Right away").
struct StartupRules {
    bool guest_lamps = true;
    bool guest_mark = true;
    bool full_rewards = true;
    int guest_insight = 0;
    bool start_immediate = false;
};
/// The join check's part of the party link's rules text: "join: Guests use lamps=on; ...", each
/// entry "<the setting's menu name>=<its value as the menu shows it>".
constexpr const char* kJoinRulesPrefix = "join: ";
std::string JoinRulesTag(const StartupRules& r);

/// The menu sliders' 11 positions (0..10) for each kind of value.
extern const int kHpTable[11];      ///< % of the HP step: 0 .. 400
extern const int kDamageTable[11];  ///< + % per extra hunter: 0 .. 100
extern const int kPoiseTable[11];   ///< % of the poise step: 0 .. 300
int Position(const int* table, int value);  ///< the nearest position
int AtPosition(const int* table, int position);

/// Values clamped to the tables' ranges.
Rules Clamped(const Rules& r);
std::string ToJson(const Rules& r);
/// False (and *error) on malformed JSON; missing members keep the defaults; values are clamped.
bool FromJson(const std::string& text, Rules* out, std::string* error = nullptr);

/// One SpEffect row's values for `cooperators` extra hunters (0: identity).
struct Doping {
    float hp_rate = 1.0f;      ///< maxHpRate (+0x10)
    float resist_rate = 1.0f;  ///< regist{Poizon,Illness,Blood,Curse}ChangeRate (+0x108..+0x114)
    float attack_rate = 1.0f;  ///< {physics,magic,fire,thunder}AttackPowerRate (+0x48..+0x54)
    int super_armor = 0;       ///< changeSuperArmorPoint (+0x13a, s16)
    bool operator==(const Doping& o) const;
};
Doping Boss(const Rules& r, int cooperators);
Doping Enemy(const Rules& r, int cooperators);
/// The normal enemy row only matters when it changes something.
bool EnemyScalingActive(const Rules& r);

/// C4: the max HP factor for `stacks` rot stacks (clamped 0..kRotMaxStacks).
float RotHpFactor(int stacks);

/// C4 "stamina cost" rot (0.6.2): the game never asks the SpEffect consumeStaminaRate for a roll or
/// a swing (live: 0 calls), so the rot charges the extra itself. One tick's stamina drop (prev ->
/// now, the max unchanged) times (factor - 1), with the fraction carried to the next drop: the
/// whole points to take now (0 when nothing was spent, the max changed or the factor is 1).
int RotExtraStaminaDrain(int prev, int now, int prev_max, int now_max, float factor, float* carry);

/// C4 "rally" rot (0.6.2): the SpEffect regainRate getter is never asked either (live: 0 calls while
/// a guest fought), so the rot takes back part of each HP gain itself: one tick's gain (prev ->
/// now, the max unchanged, alive before) that is not a Blood Vial's (`vial` false) times
/// (1 - factor), the fraction carried. The whole points to take back now (0: nothing to take).
int RotRallyTakeBack(int prev, int now, int prev_max, int now_max, float factor, bool vial, float* carry);
/// The "rally" rot counts an HP gain only this long after the player last took damage (the game's
/// rally window: a hit back regains part of what was just lost); later gains (a regeneration
/// effect, a healing item other than a Blood Vial) are not a rally and are kept whole.
constexpr double kRotRallyWindowS = 6.0;
/// True when an HP gain `since_damage_s` seconds after the last damage (negative: no damage
/// seen since the rot came into force) falls inside the rally window.
bool RotRallyGainInWindow(double since_damage_s);

/// C4 "item discovery" rot (0.6.2): the itemDropRate sum is never asked (live: 0 calls), but every
/// enemy drop roll carries the player's discovery in its request (party_droproll, 1.0 = the stat
/// screen's 100). Each stack takes 10 points (0.1) off it, never below kRotDiscoveryFloor; a
/// discovery the roll does not carry (<= 0) is left alone.
constexpr float kRotDiscoveryPerStack = 0.1f;
constexpr float kRotDiscoveryFloor = 0.1f;
float RotItemDiscovery(float own, int stacks);

/// C4 rot, what cleanses it (ERSC: resting at a grace). A lamp warp is the death's respawn (no
/// rest) only while the player is still down from that death: dead at the warp, or the warp came
/// within kRespawnWarpS of the death before the player was up again. Playtest 1 (H5:6072/10228):
/// a host revived in place used a lamp 19 s after its death and the old 20 s rule called it the
/// respawn.
constexpr double kRespawnWarpS = 20.0;
bool LampWarpIsRespawn(bool alive_at_warp, double since_death_s, bool up_since_death);
/// Playtest 1 #21 ("rot wasn't removed on lamp rest, just triggering the lamp"): a dialog that
/// opens with the player alive beside a lamp (its revive spot within kLampMenuRadiusM, height
/// within kLampMenuHeightM) is the lamp's own menu ("Return to the Hunter's Dream?") or the
/// party's travel question: a rest, whatever is answered.
constexpr float kLampMenuRadiusM = 4.0f;
constexpr float kLampMenuHeightM = 3.0f;
bool LampMenuRest(bool dialog_opened, bool alive, float lamp_dx, float lamp_dy, float lamp_dz);

/// The rot belongs to the character: party_rot.txt keeps it across restarts, one entry per save
/// slot (RotFile), so a second character in the same user folder never gets the first one's. A
/// save in its own prologue cannot have earned any (prologue deaths give none, #19), so stacks seen
/// while this player stands kStaleRotPrologueS in its own prologue still go: a new game made in a
/// slot whose earlier character was deleted. A guest in the host's world is never in its prologue.
constexpr double kStaleRotPrologueS = 3.0;
bool RotFromAnotherCharacter(int stacks, bool own_prologue, double prologue_s);

/// Playtest 1 save audit S4: party_rot.txt, the rot of each character by save slot, one line per
/// slot with rot: "slot S kinds H ST SC R D [stain K]" (stacks per kind: health, stamina, stamina cost,
/// rally, discovery). A file from before the slot key ("stacks N" + "kindK N" lines) cannot say
/// whose rot it was: it is dropped (legacy_stacks says how much, for the log). Dropping a penalty
/// costs nobody anything (a lamp would cleanse it anyway); giving it to the wrong character was
/// the bug.
constexpr int kRotFileKinds = 5;
struct SlotRot {
    int slot = -1;  ///< save slot 0-9, -1 unknown
    int kind[kRotFileKinds] = {};
    /// 0.7.2: the kind of the stack the bloodstain's death added, while that stain lies there
    /// (picking it up removes it: party_rot_rules.h); -1 none. " stain K" after the kinds (an
    /// older build reads the five kinds and ignores it).
    int stain = -1;
    int Stacks() const;
};
struct RotFile {
    std::vector<SlotRot> slots;  ///< only slots with rot
    int legacy_stacks = 0;       ///< an old unkeyed file's stacks (dropped)
    /// The rot of `slot` (none: all zero, slot set).
    SlotRot Get(int slot) const;
    /// Replaces (or removes, when it has no stacks) the entry of r.slot.
    void Set(const SlotRot& r);
};
RotFile ParseRotFile(const std::string& text);
std::string FormatRotFile(const RotFile& f);
/// The loaded character's rot is another one's: a known save slot that is not the active one.
bool RotSlotChanged(int active_slot, int current_slot);

/// C4 rot as the player sees it (System > Party, the party HUD): "Rot 3/5: max HP -20 %, stamina
/// costs +10 %" from the stacks per kind (party_rot.txt's order: health, stamina, stamina cost,
/// rally, discovery); empty without stacks.
std::string DescribeRot(const int kind[kRotFileKinds], bool russian);
/// The parts of DescribeRot, for a HUD that colours each effect: "Rot 3/5: " (empty without rot)
/// and one effect ("max HP -20 %"; empty when that kind has no stacks).
std::string DescribeRotHead(const int kind[kRotFileKinds], bool russian);
std::string DescribeRotEffect(int k, int stacks, bool russian);
/// The one-line short form for the party HUD while a boss bar is up ("Rot 2/5: HP, rally"; empty
/// without rot): the full line can wrap to three lines and cover the boss's name.
std::string DescribeRotShort(const int kind[kRotFileKinds], bool russian);

/// Short value texts for the menu (help line): "150 % (+75 % HP per extra hunter)".
std::string DescribeHp(int percent, bool russian);
std::string DescribeDamage(int percent, bool russian);
std::string DescribePoise(int percent, bool russian);

}  // namespace coop::balance
