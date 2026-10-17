// SPDX-License-Identifier: GPL-3.0-or-later
// I1, friendly fire / PvP the host switches on and off (ERSC Judicator's Rulebook), the game side.
// The rule is a party rule (coop::balance::Rules::friendly_fire, EVENT "balance": the host's
// value goes to every guest; a guest's own setting never counts). In a party session with the
// rule on, the team-relation table cells between the party players' teams are opened on this
// game (party_pvp_rules.h), so attacks, bullets and their effects hit party members both ways,
// with the game's own hit reactions and the normal party death flow. Off (default), at the end of
// the session and when the rule goes off, the original bytes are put back.
// Env: BB_PARTY_FRIENDLY_FIRE=0/1 (the host's setting at start), BB_PARTY_PVP=0 (never patch),
// BB_PARTY_PVP_TEST=steps (live verification, party_pvp_rules.h TestStep).
#pragma once

#include "game_state.h"

namespace coop::pvp {

/// The director's tick (main thread, party mode only). `host`: this game hosts the party.
void Tick(const GameSnapshot& s, bool host);
/// Any thread: the rule in force on this player (host: its setting; guest: the host's rule).
bool RuleOn();
/// Any thread: the table cells are open now (a party session with the rule on).
bool Active();

}  // namespace coop::pvp
