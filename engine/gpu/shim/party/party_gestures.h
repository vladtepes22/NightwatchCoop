// SPDX-License-Identifier: GPL-3.0-or-later
// Party gestures: a gesture any party member learns reaches every member's own save (shared
// progress, both ways), with an owed list like the owed key items of party_items.h.
//
// How the game keeps gestures [static, checked on the test save]: in GestureGameData
// (PlayerGameData +0x5B8; 0x14F1510 builds it, 0x14EDD10 / 0x14EDD40 read / write its 0x78 bytes
// with the character): 30 u32 entries at +0x10, entry i = display index << 16 | param id << 1 |
// learned (bit 0); param id p = gesture index + 1, the gesture table 0x55526D0 (0x14EC980) row p
// holds goods 800 + index (the name and icon of EquipParamGoods 800..822), message 301000 + index,
// the index and the motion. Gestures are no inventory items: the save holds only these bits
// (userdata0000 +0x19120: bits of 800..803, 808, 809, 812, 813, 821). A talk script teaches one with
// ESD command 0x41 (the item-get notice of goods 800 + i, a null handle), command 0x42 (index):
// sets bit 0 of that entry (0x203FFD0 case 0x42; 0x14EDF10 is the same, uncalled) and its "taught"
// flag 6740 + i (League Oath 822: 6770; personal block 6000-8999: the own pool even in load mode 1).
// So a summoned guest that lost the bit keeps the flag, and the NPC never teaches it again.
//
//   every member: its learned gestures (bit 0) are sent when they change and every 60 s: a guest to
//          the host, the host to everyone (it learns a guest's new gesture at once, so its list is
//          the party's union).
//   receiver: a peer's gesture this save lacks is "owed" (on disk, per save slot: a quit from the
//          host's world keeps it) and learned at once, summoned or at home: bit 0, its taught flag,
//          the item-get notice, a save request. One learned while summoned (given, or taught by an
//          NPC in the host's world) stays owed until the own world reads the bit, and is learned
//          there again if it is missing. At home a set taught flag without the bit (a gesture lost
//          by an older build) is learned again.
//
// Env: BB_PARTY_GESTURES=0 (off), BB_PARTY_GESTURE_TEST=<goods>[:summoned][,...] (test aid: the
// local player learns these gestures once, at the next steady own world or, with :summoned,
// while summoned; logs the gesture state and the inventory sections).
//
// The pure part (ids, JSON, the owed store, the decision) is unit-tested
// (tests/test_party_gestures.cpp, built with BB_PARTY_GESTURES_NO_GAME).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace coop::gestures {

constexpr std::uint32_t kFirstGoods = 800;
constexpr int kCount = 23;
bool IsGestureGoods(std::uint32_t goods_id);
/// The talk scripts' "taught" flag of a gesture's goods (6740 + i; 822 -> 6770), -1 for the
/// gestures no talk teaches (800..803, 808, 812, 821: a new character's).
std::int32_t TeachFlag(std::uint32_t goods_id);
/// "Point Forward" .. "League Oath" (the gesture menu's names), "?" for other ids.
const char* GestureName(std::uint32_t goods_id);
/// The goods of a GestureGameData entry (0 for an empty / foreign one); `learned`: its bit 0.
std::uint32_t EntryGoods(std::uint32_t entry, bool* learned = nullptr);

constexpr const char* kEventName = "gestures";
/// {"goods":[id, ...]}
std::string ToJsonText(const std::vector<std::uint32_t>& goods);
/// A peer's gestures (anything but gesture goods is dropped, duplicates too); false on a malformed body.
bool FromJsonText(const std::string& text, std::vector<std::uint32_t>* out, std::string* error = nullptr);

/// The owed gestures on disk (user\party_gestures.txt, "owed SLOT GOODS SUMMONED"): per save slot;
/// SUMMONED 1 = learned while summoned, still to be read back in the own world.
struct Store {
    struct Owed {
        std::uint32_t goods = 0;
        int slot = -1;
        bool summoned = false;
    };
    std::vector<Owed> owed;

    /// Adds an entry unless one exists for (slot, goods); true when added.
    bool Add(int slot, std::uint32_t goods, bool summoned = false);
    Owed* Find(int slot, std::uint32_t goods);
    void Drop(int slot, std::uint32_t goods);
    std::string Dump() const;
    static Store Parse(const std::string& text);
};

/// The peer's gestures this save lacks: not learned and not owed to `slot` yet.
std::vector<std::uint32_t> NewlyOwed(const std::vector<std::uint32_t>& peer, const std::set<std::uint32_t>& learned,
                                     const Store& store, int slot);

enum class Step : std::uint8_t {
    Have,  ///< this save holds it: the owed entry goes
    Learn, ///< learn it now
    Wait,  ///< keep it owed (learned while summoned: only the own world can say the save kept it)
           ///< or the state is unreadable
};
const char* StepName(Step s);
/// What to do with an owed gesture. `home`: own world, load mode 0; else summoned. `learned`:
/// its bit 0 (-1 unreadable); `summoned_before`: it was learned while summoned. A missing bit is
/// learned in any world (a bit cannot be doubled); a set one learned while summoned stays owed
/// until the own world reads it.
Step Decide(bool home, int learned, bool summoned_before);

#ifndef BB_PARTY_GESTURES_NO_GAME
/// Main thread, once a frame (CoopTick): reads the own gestures (every ~1 s), notes new ones to
/// send, learns owed ones, runs BB_PARTY_GESTURE_TEST.
void Tick(bool host);
/// The local gestures to send (when they changed, and every 60 s): a host broadcasts them, a
/// guest sends them to the host.
bool PopOwn(std::vector<std::uint32_t>* out);
/// A peer's gestures (any thread; applied by Tick).
void RequestPeer(const std::vector<std::uint32_t>& goods);
#endif

} // namespace coop::gestures
