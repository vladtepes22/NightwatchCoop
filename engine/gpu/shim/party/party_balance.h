// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance, the game side (ERSC parity C4 death penalty, D7 enemy scaling, E2 overhead
// display; docs/party/balance.md). The rules themselves are pure: party_balance_rules.h.
//
// The party's rules are the host's settings (bbport.ini party_rot, party_boss_* /
// party_enemy_*), sent to every guest as EVENT "balance" (on change and every 15 s); a guest uses
// them as soon as they arrive (its own settings until then).
//
//   D7  SpEffect rows 7500 / 7501 / 7502 (the game's boss "doping" for 1 / 2 / 3 cooperators)
//       get the boss values, row 7503 (unused in 1.09) the normal enemy values for the current
//       cooperator count (param writes, re-checked every second; a row whose maxHpRate is
//       neither the 1.09 value nor ours is left alone). With cooperators present and enemy
//       scaling not zero, every normal enemy (NpcParam teamType 23, not a boss soul) in this
//       world gets SpEffect 7503 (chr vfunc +0x3f0), except the bosses (the entities the boss
//       events dope, resolved with 0x13c97a0). The
//       game's own per-frame HP update (0x1a0d7d0)
//       raises current HP with the max, so full-health enemies stay full.
//   C4  "Rot": each death of the local player (HP > 0 -> 0 in the world) adds a stack (max 5) of
//       a kind picked at random: max HP -10 % (the max-HP rate 0x17e46e0) or max stamina -10 %
//       (the stamina rate 0x17e4a20); both prologues replaced, the local player's list only. A lamp / headstone use started by the
//       player (party_travel SetLampUseObserver; not the respawn warp of a death) cleanses it.
//       Kept in <user>/party_rot.txt across restarts, per save slot (character). Off by default (0.7.2).
//       0.7.2 (party_rot_rules.h): a boss's death cleanses all of it, the death of the enemy that killed
//       this player (the kill log 0x1FEB2A0's attacker) that death's stack, the own bloodstain picked
//       up the stack of the death that dropped it (host rules party_rot_cleanse_*).
//   E2  This player's level, death count and rot stacks go out with the party HUD's VITALS.
//
// Env: BB_PARTY_BALANCE=0 turns all of it off (no hook, no param writes, no events);
//      BB_PARTY_BALANCE_LOG=1 logs each enemy pass.
#pragma once

#include "party_balance_rules.h"
#include "party_enemy_census.h"

#include <cstdint>
#include <vector>

#include <string>

namespace party {
class PartyLink;
}

namespace coop {
struct GameSnapshot;
}

namespace coop::balance {

constexpr const char* kEventName = "balance";

/// PartyInit (bbgpu_patch_image, after HooksInit): the max-HP rate hook, the lamp observer.
void Init();
/// The director's tick (main thread, party mode only). `host`: this game hosts (or plays without
/// a link): its own settings are the party's.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);
/// Any thread: a lamp's travel menu opened (direct lamp travel's warp menu, which is no generic
/// dialog): counts as the lamp's menu (#21, a rest for the rot) on the next tick.
void NoteLampMenuOpened();
/// 0.7.2, what else cleanses rot (party_rot_rules.h; any thread, applied by the next tick under
/// the rules in force). A boss died with this player in the host's world (party_phantom: the
/// game's boss clear here, or the host's boss-kill notice; `entity` 0 when unknown).
void NoteBossDefeated(std::uint32_t entity, const char* from);
/// An enemy of the party died (the kill record: seen here, or a member's kill from the host).
void NotePartyKill(std::uint32_t entity, const char* from);
/// The kill record's state (party_progress_bridge, every tick): `follows_host_world` a guest's
/// record follows the host's world (its own loads are then no new world); `new_world` the host's
/// world was reset now (a rest, a wipe: every killer respawned).
void NoteKillWorld(bool follows_host_world, bool new_world, const char* from);
/// This player's own bloodstain was picked up (party_b2: its Blood Echoes came back).
void NoteEchoesRetrieved(const char* from);
/// Guest: the host's EVENT "balance" (any thread). False (and *error) when malformed.
bool OnHostRules(const std::string& json, std::string* error = nullptr);
/// The rules in force now.
Rules InForce();
/// The local settings as rules.
Rules FromSettings();
/// Guest: the host's rules once received (EVENT "balance"); false before (a guest's own settings
/// never count for the party rules such as I1 friendly fire).
bool HostRules(Rules* out);
/// For the party HUD's VITALS.
int LocalDeaths();
int LocalRotStacks();
/// C4 "item discovery" rot: the discovery an enemy drop roll of the local player uses (`own`, the
/// request's value), with this player's discovery stacks taken off (RotItemDiscovery); unchanged
/// when the kind is not in force, rot is off or there are no such stacks. Any thread (the roll).
float RotDropDiscovery(float own);
/// The loaded map's enemies as the H4 census sees them (team 23, entity id chr +0x3B8, HP +0xF8,
/// position): the host_kill / guest_kill test steps pick from this list (main thread).
std::vector<EnemyKillCandidate> LoadedEnemies();
/// A chr's HP (+0xF8 of its data module; -1 unreadable) and physics position.
int EnemyHp(std::uint64_t chr);
bool ChrPosition(std::uint64_t chr, float out[3]);
/// Main thread: the entity ids of the chrs the loaded world has now, players left out, alive, not
/// disabled and not kept hidden (the map's NPC sites show only where their NPC is; map/map_page.cpp).
std::vector<int> PresentEntities();
/// Debug (BB_MAP_NPC_LOG=1): logs every loaded non-player chr with a map entity id and its state bytes.
void LogPresentChrs();
/// HP 0 on a living chr (the state a lethal hit leaves: the chr's own update runs its death, drop
/// roll and kill echoes as for a kill). False: unreadable or already dead. Main thread.
bool SetEnemyHpZero(std::uint64_t chr);
/// The same death for an enemy that is already dead in the party (the kill record: its map
/// streamed in again, or a joiner's game): marked echoes-paid first (+0x1E1 0x40000, which the
/// payer 0x18C5070 skips) and its drop roll skipped (party_droproll SkipDrop). Main thread.
bool SetEnemyDeadQuietly(std::uint64_t chr);
/// Guest: the host's EVENT "npc_off" (party_npc_mirror.h; any thread). False when it is not the
/// host's (slot 0) or malformed.
bool OnNpcOff(int slot, const std::string& json);
/// Test aid (host_spfx, D7 doping dedupe): SpEffect `id` on the chr of map entity `entity`
/// through the chr's own ApplySpEffect (vfunc +0x3f0, the path the boss events' 2004[8] take, H4
/// sees it); `*what` the chr's doping rows / HP rate after it. False: no such chr loaded.
bool ApplySpEffectToEntity(int entity, int id, std::string* what);
/// H3: what the game's kill-echo payer (0x18C5070) checks on a dead chr, for the logs.
std::string DescribeKillEchoGates(std::uint64_t chr);

}  // namespace coop::balance
