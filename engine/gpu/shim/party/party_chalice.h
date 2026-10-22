// SPDX-License-Identifier: GPL-3.0-or-later
// Chalice Dungeons in party play (docs/party/chalice.md): which dungeon the local game has.
//
// A player's Chalice Dungeons live in the event flag store, not in a separate save structure:
// slot k (0..6) is selected by the one-hot flags 9020+k (GetEventFlagValue(9020, 7) -> slot
// = 7 - bit length), and the ritual helper CSDungeonRitualHelper writes the slot's dungeon
// record into the type-9 flags of area 40+k (block 0, zone 0: ids 94000000 + k * 100000):
//   +32 / +64   ChannelId, low / high dword (0x8000000000000000: never uploaded)  0x1ab2970
//   +96..+127   4 bytes (helper +0xe0)                                          0x1ab2b40
//   +128        32 bits (helper +0xe8)
//   +160..+735  9 x {u32, u32} (helper +0xf8, the layout parameters)            0x1ab2f70
//   +736        16 bytes (helper +0x140)
//   +992..      0x30 bytes (helper +0x158, the creator's online id serialization)
// (writers 0x1aa7c00 Join, 0x1aa7f80 random join, 0x1aa81d0 quick join, 0x1aa8410 cancel,
// 0x1aaa3d0 state switch). The ChannelId is the board's id (from_api_channel.h), so two games
// with the same ChannelId in their current slot generate the same dungeon.
//
// The party layer only READS these flags: 9020-9026 and the chalice groups are never written
// from outside (party_progress NeverWritable). The one exception is a summoned guest's
// load-mode-1 storage (the host's world, transient): the quick-joined record is carried there,
// also under the guest's current slot (party_chalice.cpp SetLoadModeHook).
//
// Main thread only (game calls).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::chalice {

constexpr std::uint32_t kSlotFlag = 9020;         ///< 9020..9026 one-hot
constexpr int kSlots = 7;
constexpr std::uint32_t kRecordBase = 94000000;   ///< T9, area 40 + slot, block 0, zone 0
constexpr std::uint32_t kRecordStride = 100000;
constexpr std::uint32_t kChannelLoBit = 32, kChannelHiBit = 64;

/// The slot from GetEventFlagValue(9020, 7) (9026 is bit 0); -1 when no slot flag is set.
constexpr int SlotFromBits(std::uint32_t bits7) {
    int len = 0;
    for (std::uint32_t v = bits7 & 0x7fu; v; v >>= 1) ++len;
    const int slot = kSlots - len;
    return slot >= 0 && slot < kSlots ? slot : -1;
}
/// The GetEventFlagValue(9020, 7) bits that select `slot` (0 for no slot).
constexpr std::uint32_t BitsForSlot(int slot) {
    return slot >= 0 && slot < kSlots ? 1u << (kSlots - 1 - slot) : 0u;
}
/// A current-slot read while a summoned member stands in the host's dungeon: the quick-joined
/// slot's bits (`joined` >= 0) instead of the own current slot's, so the game's "dungeon I am in"
/// (FrpgNetMan +0xa88, the ChannelId signs and messages carry) is the host's. `own` otherwise.
constexpr std::uint32_t InDungeonSlotBits(std::uint32_t own, int joined) {
    return joined >= 0 && joined < kSlots && SlotFromBits(own) != joined ? BitsForSlot(joined) : own;
}
/// The ChannelId of two dwords; 0 when it is the game's "none" (top bit set) or 0.
constexpr std::uint64_t ChannelFromDwords(std::uint32_t lo, std::uint32_t hi) {
    return (hi & 0x80000000u) ? 0 : (static_cast<std::uint64_t>(hi) << 32 | lo);
}

// ---- The party's chalices (progress for everyone) ----
// The chalices (goods; HolygrailExParam rows that are EquipParamGoods, isOnlyOne, never consumed
// by a ritual) any member owns go to every member that lacks them (a guest's to the host, the
// host's to every guest), in the receiver's own world: whichever dungeon the party clears, every
// member can perform the next ritual itself, and the host can enter a dungeon a guest made.
// Only the fixed chalices (ids ending in 9, HolygrailExParam isGenericHolygrail 0): a Root Chalice
// (isGenericHolygrail 1: 6100, 6200, 6210, 6300, 6302, 6310, 6312, 6401, 6420, 6500, 6502, 6520,
// 6522, 6530, 6532) is used up by its ritual, so sharing it would hand a used one back.
constexpr std::uint32_t kChaliceGoods[] = {6109, 6209, 6219, 6309, 6319, 6409, 6429, 6509, 6529, 6539};
bool IsChaliceGoods(std::uint32_t goods_id);
constexpr const char* kGoodsEventName = "chalice_goods";
/// {"goods":[id, ...]}
std::string GoodsToJsonText(const std::vector<std::uint32_t>& ids);
/// The chalice goods of a peer's event (anything else is dropped); false on a malformed body.
bool GoodsFromJsonText(const std::string& text, std::vector<std::uint32_t>* out, std::string* error = nullptr);

struct State {
    bool valid = false;         ///< the flags could be read
    int slot = -1;              ///< -1: no chalice slot selected
    std::uint64_t channel = 0;  ///< the slot's ChannelId, 0: none / never uploaded
    int load_mode = -1;         ///< SprjEventFlagMan +0x80: 0 own world, 1 another world's flags
};

#ifndef BB_PARTY_CHALICE_NO_GAME
/// The summon message probe (logs each summon message's map / ChannelId, 0x14ba340). Once, at init.
void InstallProbes();
/// The FormData (version 4, 0x7C bytes; reader 0x1a9c490) of the dungeon in `slot`, rebuilt from
/// the slot record: {4, 0x7C, +96 u32, +128 u32, +160 18 u32 (layout), +736 4 u32, 0...}. Empty
/// when unreadable. What a summoned member's quick join needs (docs/party/chalice.md 4).
std::vector<std::uint8_t> SlotFormData(int slot);
/// Reads the local game's chalice slot and its ChannelId.
State Read();
/// Once a frame (coop tick): re-reads every ~2 s, logs changes, and keeps LocalChannel(); the
/// every member notes the chalices it owns (PopHostGoods) and gives itself the peers' chalices it
/// lacks (in its own world; a host always): any member can perform the next ritual.
void Tick(bool host);
/// The local chalices to send (when they changed, and every 60 s for members that rejoined):
/// a host broadcasts them, a guest sends them to the host (which then has them and re-broadcasts).
bool PopHostGoods(std::vector<std::uint32_t>* out);
/// A peer's chalices (any thread; applied by Tick in the own world).
void RequestGuestGoods(const std::vector<std::uint32_t>& ids);
constexpr const char* kFormEventName = "chalice_form";
/// Guest: its own ritual dungeon's FormData for the host's board ({"id":"N","form":"b64"}).
bool PopGuestForm(std::string* body);
/// Host: a guest's FormData event (`member`: its roster name): the dungeon goes on the board (added,
/// or its missing FormData) and counts as that member's latest ritual.
void OnGuestForm(int slot, const std::string& member, const std::string& body);
/// Host: true once after the altar's glyph box took the party dungeon's glyph: the director then
/// answers the remaining prompts (OK, the search result, YES, the altar, the awaken point).
bool TakeAltarAutopilot();
/// Host: its own online id (the roster's host entry), so the party's dungeon at its altar is
/// another member's ritual (docs/party/chalice.md 7).
void SetHostName(const std::string& name);
/// The dungeon the local game has in its current slot while it is in its own world (load mode
/// 0); 0 when none or unknown. Any thread (the last Tick's value).
std::uint64_t LocalChannel();
#endif

} // namespace coop::chalice
