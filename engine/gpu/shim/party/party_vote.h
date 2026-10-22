// SPDX-License-Identifier: GPL-3.0-or-later
// Lamp travel voting (0.7.3), the game side. The rules, the wire and the texts are
// party_vote_rules.h (unit test tests/test_party_vote.cpp).
//
//   host   party_split's YES to "Travel with the party?" (its own lamp, headstone or Hunter's
//          Mark) and party_guestsvc's run of a guest's lamp request call HostBegin instead of
//          the travel: with the rule on the host's tick sends EVENT travel_vote {"op":"ask"} to
//          every voter, sums the answers up and on all YES runs the travel it was given; one NO
//          or the timeout cancels it (EVENT {"op":"end"} with the notice to everyone). A guest's
//          vote asks the host too: the host's own YES / NO dialog.
//   guest  the ask opens the game's YES / NO dialog ("<who> wants the party to travel to
//          <lamp>."), when no menu or other dialog is up; its answer goes back to the host. The
//          member who started the travel only gets the waiting notice.
//
// The host's rule: System > Party rules: World "Party votes on travel", bbport.ini party_travel_vote
// (off by default), the launcher's Party Rules tab; BB_PARTY_TRAVEL_VOTE=0/1 wins at start.
// A guest follows the host's rule (party_balance Rules), shown read-only on its page.
// Test aids: BB_PARTY_VOTE_TEST=yes,no,none,... answers this game's vote questions in order
// (YES = Cross; NO = Right, then Cross; none = left open); BB_PARTY_VOTE_TIMEOUT=S (the host's
// timeout, default 20 s).
#pragma once

#include "party_vote_rules.h"

#include <cstdint>
#include <functional>
#include <string>

namespace party {
class PartyLink;
}
namespace coop {
struct GameSnapshot;
}

namespace coop::vote {

/// The host's own setting (BbSettings party_travel_vote; live).
bool Enabled();
/// Host, main thread: a party travel to lamp `lamp` that party slot `starter` (0: the host) chose.
/// False: no vote (the rule is off): the caller runs the travel now, as before. True: the vote
/// owns it - `go` runs on a later tick once every voter said YES (or at once when nobody else is
/// connected); nothing runs when it is cancelled. `what` for the log ("the host's lamp").
bool HostBegin(std::uint32_t lamp, int starter, std::function<void()> go, const char* what);
/// A vote is running on this game (host: its round; any role: this game's own question is up).
bool Busy();
/// The link's callback thread: EVENT travel_vote from `slot` (host: an answer; guest: ask / end).
void OnEvent(int slot, const std::string& body);
/// Main thread, once a frame (the director). `host`: this game hosts the party.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);

} // namespace coop::vote
