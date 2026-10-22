// SPDX-License-Identifier: GPL-3.0-or-later
// I5 "Gather the party" (docs/party/ERSC_PARITY.md I5), the game side. The rules, the wire and the
// texts are party_gather_rules.h (unit test tests/test_party_gather.cpp).
//
//   host   System > Party > Gather the party (party_status RequestGather -> the status bridge ->
//          RequestFromMenu), or the hotkey: the touchpad held + L3 (Back + L3 on pads without a
//          touch surface; src/runtime_pad.c ping_combo), the keyboard's H (bbport_overlay.cpp). The director's tick checks the host (HostRefusal: a boss fight, a
//          cutscene, dead, loading, a travel, nobody connected, the 30 s cooldown; the notice says
//          why), then sends EVENT gather {"op":"ask"} to every guest and sums the answers up in a
//          notice ("Gather: Ann coming; Bob said no; not now: Cid (in a menu)").
//   guest  checks itself (GuestRefusal: dead / spectating, loading, a cutscene, a boss fight, a
//          travel, a menu or a dialog: the notice says why, the host is told), else the game's own
//          YES / NO dialog (split::AskYesNo, the text BbGameMenu::Pages::GatherQuestionText). YES:
//          in the host's map an in-world warp beside the host (b2::GatherBesideHost, no load); in
//          another map the party's one-load travel into the host's map (b2::GatherTravelToHost:
//          the lamp nearest the host, a re-join with the host's snapshot), then beside the host;
//          in its own world the Rejoin (the bell; the summon brings it to the host).
//
// The host's rule (0.7.1): System > Party rules: World "Host can gather", bbport.ini party_gather,
// the launcher's Party Rules tab; BB_PARTY_GATHER=0/1 wins at start. Off: the host's press is
// refused ("Gather the party is off"); a guest follows the host's rule (party_balance Rules).
// Test aid: BB_PARTY_GATHER_TEST=yes,no,menu,... answers this guest's questions in order (yes / no:
// the dialog with the in-process pad, YES = Cross, NO = Right then Cross; menu: the in-game menu is
// opened (Options) before the next question, which is then refused, and closed again (Circle)).
#pragma once

#include "party_gather_rules.h"

#include <string>

namespace party {
class PartyLink;
}
namespace coop {
struct GameSnapshot;
}

namespace coop::gather {

/// The host's own setting (BbSettings party_gather; live).
bool Enabled();
/// Any thread (the status bridge, the menu's Gather the party row): the host gathers on the next tick.
void RequestFromMenu();
/// The hotkey (any thread): true while this game hosts a running party and gathering is on.
bool HotkeyAvailable();
/// The hotkey pressed (`source` for the log): RequestGather as the menu row does; false when the
/// hotkey is not this game's (a guest, no party, BB_PARTY_GATHER=0).
bool RequestFromHotkey(const char* source);
/// The link's callback thread: EVENT gather from `slot` (host: a guest's answer; guest: the host's ask).
void OnEvent(int slot, const std::string& body);
/// Main thread, once a frame (the director). `host`: this game hosts the party.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);

} // namespace coop::gather
