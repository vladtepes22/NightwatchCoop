// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.8 (giveitem078, CL-63e235): give items to a party member, the game side. The rules, the wire and the two state
// machines are party_give_rules.h (unit test tests/test_party_give.cpp); this file reads the inventory, moves the
// items and talks over the party link.
//
//   player   the game's own inventory: an item's command menu (Use / Discard / Give), then a member list and an
//            amount list, both native command menus (bbport_game_menu.cpp). Needs the own switch (bbport.ini party_give_items, default OFF; System > My party:
//            Your items; the launcher's My Party tab) - the receiver's switch is its consent.
//   link     EVENT give {op: offer / ready / refuse / commit / abort / done}. A guest sends to the host, who relays to
//            the addressed member and sets "from" to the sender's slot (so host -> guest, guest -> host and
//            guest -> guest all work). Nothing is stored: the transfer lives in memory, a restarted game knows no id.
//   giver    OFFER, then on READY removes the items (the count must drop by exactly n, else they are put back), then
//            COMMIT, then DONE {got}; a missing DONE is asked for (ABORT); only a receiver that cannot be asked for
//            kAbortWaitS / kLinkDownS makes the giver put the whole amount back (the "rolled back" notice).
//   receiver checks the moment (loading / cutscene / boss fight / dead / travelling), the goods rules on its own param
//            data and its room (inventory up to maxNum, then the storage box), answers READY, and gives on COMMIT with
//            the game's own item-get dialog and the notice "<item> xN from <name>".
//   nothing  during a loading screen, a cutscene, a boss fight, a party travel, while dead; no key / unique / story
//            items (party_give_rules.h GoodsAllowed), a quick-slot item always keeps one.
//
// Env: BB_PARTY_GIVE_ITEMS=1 switches the feature on whatever the ini says (tests). Test aids: BB_PARTY_GIVE_TEST=
// "<goods>:<n>:<slot>@<s>[,...]" (this player gives n of goods to the member in `slot` s seconds after its world is
// steady; every request is logged with the reason when refused) and BB_PARTY_GIVE_FAULT=mute (a receiver ignores COMMIT
// and ABORT: the giver's timeout roll-back) | linkdrop (the giver drops its link right after COMMIT).
#pragma once

#include "party_give_rules.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace party {
class PartyLink;
}
namespace coop {
struct GameSnapshot;
}

namespace coop::give {

/// The own switch (BbSettings party_give_items, or BB_PARTY_GIVE_ITEMS=1). Any thread.
bool Enabled();

/// A give of `n` of goods `goods` to the member in `to_slot` (any thread): handled on the next tick; the result goes to the
/// party notice corner. (The item menu's last step; also the hidden test aid BB_PARTY_GIVE_TEST.)
void RequestGive(std::uint32_t goods, int n, int to_slot);

/// The inventory's item command menu (bbport_game_menu.cpp, built on the game thread): offer "Give" for this item
/// (type bits | goods id)? Only with the switch on, a party with another member, and an item the rules allow.
bool MenuOffers(std::uint32_t item);
/// The members the Give picker lists (connected, not this player): slot and shown (filtered) name.
std::vector<std::pair<int, std::string>> MenuMembers();
/// The amounts the picker offers for `item`: 1, 2, 5, 10, 20, 50 below the most that can be given, then "all" last
/// (the most one give can carry: held, at most 99, a quick-slot item keeps one). Empty: nothing can be given now.
std::vector<int> MenuAmounts(std::uint32_t item);
/// The picker's last step: give `n` of the item (type bits | id) to the member in `slot`.
void MenuGive(std::uint32_t item, int n, int slot);
/// The player chose Give in the item menu (item = type bits | id, inventory index).
void MenuChosen(std::uint32_t item, std::uint32_t index);

/// The link's callback thread: EVENT give/// The link's callback thread: EVENT give from `slot`. Only queues.
void OnEvent(int slot, const std::string& body);
/// Main thread, once a frame (the director). `host`: this game hosts the party.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);

}  // namespace coop::give
