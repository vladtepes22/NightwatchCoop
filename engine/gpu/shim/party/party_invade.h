// SPDX-License-Identifier: GPL-3.0-or-later
// Invasions into open public games (0.7.8, CL-4cc63c), the game side. The rules, the wire and the limits are
// party_invade_rules.h (unit test party-invade-test); the link's invader admission path is party_link.cpp; the team
// tables are party_pvp.cpp. Design and decisions: docs/party/invasions.md.
#pragma once

#include "party_invade_rules.h"

#include <cstdint>
#include <string>

namespace party {
class PartyLink;
}

namespace coop {
struct GameSnapshot;
}

namespace coop::invade {

/// This process joins as an invader (BB_PARTY_INVADE=1, set by the launcher for that one start). Never saved.
bool JoinAsInvader();
/// The host's setting "Allow invaders" (BbSettings party_invaders; live).
bool HostAllows();

/// Any thread, host: the link's invader gate (party_link.h LinkConfig::invader_gate). "" admits, else the named reason.
std::string Gate(const std::string& name, std::uint32_t ip_network_order, bool final_check, int invader_level = 0);
/// Invader side: this game's Blood Echoes level for the HELLO (0.7.9; 0 unknown; BB_PARTY_INVADE_LEVEL overrides it for tests).
int OwnLevel();
/// Any thread, host: an open-party join (no password) of a new member that said `level`: "" admits, else the refusal text
/// (the vanilla co-op range, party_invade_rules.h OpenLevelOk). The host's level is the one its tick last read.
std::string OpenGate(int level);

/// The link callback thread: EVENT "invade" (a guest or the invader, from the host) / "invade_fell" (the host, from the
/// invader).
void OnEvent(int from_slot, const std::string& name, const std::string& body);

/// Main thread, once a frame (the director). The host tracks invaders, ends invasions, tells everybody; the invader goes
/// home when it falls; everybody else only shows the notices.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);

/// Any thread: what a host announces about its place and the invasions (the lobby and LAN providers). `invadable` is false
/// whenever the switch is off, a boss fight or a loading screen is on, an invader is in or the 4 slots are taken.
struct Where {
    bool allow = false, invadable = false, boss = false;
    std::uint32_t area = 0;
    std::uint8_t phase = 0, ng = 255;  // party_invade_rules / scripts/party_where.py codes
};
Where Announced();

/// Test aid (smoke): the host's view of its own moment forced for the gate and the tick: "boss", "cutscene", "loading",
/// "full" (4 players in the world) or "none". Reached through the director's own_rule=invade_view:<word> step.
void TestView(const std::string& word);

/// Any thread: the host's "Block the invader" (System > Party): the current invader is kicked and blocked for the session,
/// by name and address (the gate refuses it with a named reason; the party link refuses the name until the game restarts).
void RequestBlock();

/// Any thread: an invasion is on in this world (pending or in) and the invader's real member name ("" none). The PvP module
/// puts that member's chr on its own team.
std::string InvaderName();
/// This game is the invader and stands in the host's world (its own chr is the invader's).
bool LocalIsInvader();
/// Any thread, the invader's game: the host's phantom health for this invasion (1 full health, 0 the game's cut), -1
/// before the host said or with no invasion on. The host's rules decide it (an invasion is PvP); the invader gets no
/// host rules, so seamless_rules ApplyPhantomHp follows this.
int HostFullHp();

}  // namespace coop::invade
