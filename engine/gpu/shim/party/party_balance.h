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
//       Kept in <user>/party_rot.txt across restarts, per save slot (character). Echoes are not touched (the bloodstain is
//       the hostdeath work).
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
/// H3: what the game's kill-echo payer (0x18C5070) checks on a dead chr, for the logs.
std::string DescribeKillEchoGates(std::uint64_t chr);

}  // namespace coop::balance
