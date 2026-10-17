// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/hle/np.h @8f2746c
// Shared between np_matching2.cpp and np_signaling.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace bbnet::np {

// The SceNpId's online id text (its first 16 bytes).
std::string npid_text(const void* npid);
bool matching2_context_started();
std::string matching2_status();
// Guest: the party host is gone (its game crashed or quit without a word: the party link is
// down, or a restarted host does not know our room). A room we are in as a guest ends for the
// game the way a host leaving ends it: DEAD per member, then ROOM_DESTROYED (0x1104, cause
// LEAVE) - the game returns to its own world. False when there was no such room.
bool party_host_lost(const std::string& why);
// Guest: our game holds a room as a guest (summoned into, or joining, a host's world).
bool party_guest_in_room();
// Guest: called (without the np lock) when the host's game closes the room we are in as a guest
// because it left it (room_closed "host_left": the host pressed Leave / ended the party). The host
// leaves its game's room first and stops the party link ~0.4 s later, so this event comes first;
// the hook sends the guest home at once (party host_lost) instead of the game's own "Session
// lost" dialog and its send-home ~15-19 s later (parx2 2026-10-15). Without a hook the room just
// ends for the game. The hook ends the room itself (party_host_lost).
using HostClosedRoomHook = void (*)(const std::string& why);
void set_host_closed_room_hook(HostClosedRoomHook hook);
// ms since a member left (or was dropped from) the room we are in; a large value when none did.
std::int64_t ms_since_member_left();

// NpSignaling, told by NpMatching2 about room members:
// a member whose address is now in the peer table: connections to it can announce;
void signaling_peer_known(std::uint16_t member_id);
// every connection to this online id is dead (DEAD at +100 ms);
void signaling_peer_dead(const std::string& online_id);
// our room is over: every connection is dead without an event.
void signaling_reset();
// Party host: a player registered its context (joined or rejoined the party): connections to it
// still resolving are resolved again now (a guest that left and pressed Join party).
void signaling_peer_registered(const std::string& online_id);

}  // namespace bbnet::np
