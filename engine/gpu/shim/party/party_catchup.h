// SPDX-License-Identifier: GPL-3.0-or-later
// Late joiner catch-up (C1/C2/C3): a guest whose own save is behind the host's (a fresh
// character, one still in the prologue) is brought up to the host before any summon. The rules
// are coop::Catchup (party_start.h, unit-tested); this is the game glue: the opening cutscene
// skip (the game's own remo skip request 0x1D07400, game_state.h RequestRemoSkip), the campaign-start flags and the
// starting weapons into the own save (own world, load mode 0), a save request, then the gather
// warp to the host's last lamp (party_travel's host lamp record; the Hunter's Dream when unknown).
// The warp's load opens C2's post-load window, where the host's snapshot (lamps, shortcuts,
// bosses) lands; C3's full item list follows. Guests only, connected to a host whose snapshot
// shows it past the prologue; party processes only (the party save, never a solo save).
#pragma once

#include "game_state.h"

namespace coop {

/// Main thread, once a frame (the director's tick). `guest_connected`: this process is the
/// party's guest and its link to the host is up.
void CatchupTick(const GameSnapshot& s, bool guest_connected, double now, std::uint32_t host_map = 0);
/// Applying / warping: the director holds its bells and grants.
bool CatchupBusy();

} // namespace coop
