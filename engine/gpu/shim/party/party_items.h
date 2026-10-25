// SPDX-License-Identifier: GPL-3.0-or-later
// Party items, phase C3 "every member keeps every item the party collects" (docs/party/items.md
// section 6): the host's item-lot awards are replayed into each guest's own world and save.
//
//   host:  the award function 0x17DDC50(MapItemMan*, lot, hostOnly) is hooked at its entry. Every
//          host-only lot (EMEVD 2003[4], Lua GetRateItem: boss drops, keys, NPC gifts) the host
//          gets in its own world becomes an ItemGrant {seq, lot, flag, ledger}. C2 progress sync
//          reports the host's item-pickup flags (treasure 5AABZnnn, key items 5000-5299) through
//          OnHostFlagSet(flag), which maps the flag back to its lot (ITEM_LOT_FLAG in
//          party_items.inc). Each grant is sent once (de-duplicated by flag / ledger index); the
//          director polls PopHostItems() and sends EVENT "items". On a (re)join the director asks
//          for TakeHostFullItems() (main thread: every lot whose flag, or whose awarding event's
//          completion flag, is set in the host's save) and sends EVENT "items_full".
//   guest: RequestGuestItems() (any thread) queues grants; ItemsTick() (main thread) applies one
//          at a time, only in the guest's own world (flag store load mode 0, own-world byte, world
//          steady, no loading screen): skip when the lot's flag (or the ledger flag
//          60009000 + idx for flagless lots) is already set, else 0x17DDC50(mim, lot, 0), which
//          rolls with the guest's own flags, gives, shows the popup and sets the lot flag; then
//          the ledger flag. Lots a guest received itself while summoned (2003[36] / hostOnly 0
//          awards, whose flag only reached the discarded overlay) are remembered and their flag
//          is set in the own world first ("mark"), so the host's later replay does not duplicate.
//   both:  optional parity patches (docs/party/items.md 5): full kill echoes, full boss clear
//          bonus and full enemy drops for clients, byte-verified, with BB_PARTY set unless
//          BB_PARTY_FULL_REWARDS=0.
//
// Env: BB_PARTY_ITEMS=0 (no hook, no replay), BB_PARTY_FULL_REWARDS=0 (no parity patches),
//      BB_PARTY_ITEMS_TEST=<lot>[,<lot>...] (debug: apply these lots once in the own world, any role).
//
// The pure part (tables, classification, JSON, queues, the apply gate) has no game dependency and
// is unit-tested (tests/test_party_items.cpp, built with BB_PARTY_ITEMS_NO_GAME).
#pragma once

#include "net/json.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace coop {

constexpr std::int32_t kItemNone = -1;
constexpr std::uint32_t kItemLedgerBase = 60009000; ///< T6 global zone 9: saved pool, unused by the data
constexpr std::int32_t kItemLedgerSize = 1000;
/// Set once the ledger was seeded from a save's own history (SeedLedgerFromOwnSave); the last bit of
/// the ledger range, never a lot's index.
constexpr std::uint32_t kItemLedgerSeeded = kItemLedgerBase + kItemLedgerSize - 1;

enum class ItemSource : std::uint8_t {
    Unknown = 0,
    Award,  ///< host hook on 0x17DDC50 (hostOnly lot)
    Flag,   ///< host item flag reported by C2 (OnHostFlagSet)
    Full,   ///< host's full state for a (re)join
    Mark,   ///< guest-local: received while summoned, set the flag only
    Test,   ///< BB_PARTY_ITEMS_TEST
    Own,    ///< guest-local: a per-player Messenger gift its game refused in the host's world
    Repeat, ///< guest-local: a repeatable NPC gift (common 9100) its talk asked for in the host's world
    Chalice, ///< host: a Chalice Dungeon award (boss rewards, area 29): no flag, once per kill, for the
             ///< guests summoned in the dungeon at that moment (G1; docs/party/chalice.md 5)
};
const char* ItemSourceName(ItemSource s);
ItemSource ItemSourceFromName(const std::string& name);

/// A blood gem's instance data (SprjGaitem instance +0x18..+0x34, written by 0x1A87E20 when the
/// gem is generated): [0] flags (bit 0/1 from the GemGenParam row), [1] rank, [2..7] the six
/// GemeffectParam ids (-1 = empty slot).
constexpr std::size_t kGemFields = 8;
/// A generated gem's id is its lot slot's id plus the area's rank offset (0x1A88130: the sum of
/// the area's gem-level rows, then the GemGenParam row with that id); 1.09's rows step by 1..3.
constexpr std::int32_t kMaxGemIdOffset = 99;

/// One item of a lot as the host resolved it (EVENT "items" row element 6): lotItemCategory
/// (0 weapon, 1 armor, 2 rune / accessory, 4 goods, 8 blood gem), lotItemId (for a gem: the
/// GemGenParam id the host's game generated), the count given, and for a gem its instance data.
struct GrantItem {
    std::uint32_t category = 0;
    std::int32_t id = -1;
    std::int32_t count = 1;
    bool has_gem = false;
    std::int32_t gem[kGemFields] = {};
    bool operator==(const GrantItem& o) const {
        if (category != o.category || id != o.id || count != o.count || has_gem != o.has_gem) {
            return false;
        }
        for (std::size_t i = 0; has_gem && i < kGemFields; ++i) {
            if (gem[i] != o.gem[i]) {
                return false;
            }
        }
        return true;
    }
};
/// Gem data a peer may send: flags 0..3, rank 0..1000, effect ids -1 or 0..99,999,999.
bool GemDataPlausible(const std::int32_t (&gem)[kGemFields]);
/// May this item reach the direct give: a supported category, or a blood gem with plausible
/// instance data (count 1).
bool GrantItemSupported(const GrantItem& it);
/// Items per grant at most (a lot and its continuation rows; the longest chain in 1.09 is 3).
constexpr std::size_t kMaxGrantItems = 16;

struct ItemGrant {
    std::uint64_t seq = 0;
    std::int32_t lot = kItemNone;
    std::int32_t flag = kItemNone;   ///< the lot's pickup flag (getItemFlagId), -1 if none
    std::int32_t ledger = kItemNone; ///< ledger index for flagless lots, else -1
    ItemSource source = ItemSource::Unknown;
    /// What the host's award really gave (captured in its 0x17DDC50 call) or what it rolled once
    /// for a random lot; empty = resolve from ItemLotParam (deterministic lots).
    std::vector<GrantItem> items = {};

    /// ItemSource::Repeat: this grant's own key (no flag: every talk gives it again).
    std::int64_t repeat_key = -1;

    /// The "already given" flag a guest checks: the lot flag, or the ledger bit.
    std::int64_t DoneFlag() const;
    /// De-duplication key (one grant per flag / ledger bit; a repeatable gift: its own key).
    std::int64_t Key() const {
        return source == ItemSource::Repeat || source == ItemSource::Chalice ? repeat_key : DoneFlag();
    }
};
std::string DescribeItem(const ItemGrant& g);

// ---- Tables (party_items.inc, generated by tools/party/item_tool.py) ----
/// getItemFlagId of `lot` (rows with a flag only), else -1.
std::int32_t ItemLotFlag(std::int32_t lot);
/// The lot that owns `flag`: treasure (flag - 50,000,000 when that row has the flag), else the
/// lowest lot with that flag (alternatives share flags; the hook's capture is preferred by the
/// caller). -1 when no lot has it.
std::int32_t ItemLotForFlag(std::int32_t flag);
/// Ledger index of a flagless host-only lot, else -1.
std::int32_t ItemLedgerIndex(std::int32_t lot);
/// The completion flag of the event that awards ledger lot `idx` (host's "already awarded").
std::int32_t ItemLedgerDoneFlag(std::int32_t idx);
std::int32_t ItemLedgerLot(std::int32_t idx);
/// The item spot flags of a flagged host-only map award (ITEM_AWARD_SPOT): the awarding event's
/// own item_lot_picked completion flag (Old Yharnam's altar: lot 2300170 -> 12300130), which -
/// not the lot's flag - hides the spot. A guest given the lot gets them in its own save, so its
/// copy of the host's world (the overlay's item bits follow the own pool, D4) no longer offers the
/// spot, after loads too. Empty for every other lot.
std::vector<std::int32_t> ItemAwardSpotFlags(std::int32_t lot);
/// Ledger seeding (pure part of SeedLedgerFromOwnSave): with `is_set` reading the save, the ledger
/// bits to set - one per ledger lot whose done flag is set and whose bit is not - or nothing when
/// the save already carries the kItemLedgerSeeded marker.
std::vector<std::uint32_t> LedgerBitsToSeed(const std::function<bool(std::uint32_t)>& is_set);
/// Key-item catch-up (playtest 1 save audit, docs/party/playtest1_save_audit.md): the ledger lots
/// this save says were given (ledger bit set) for a boss this save has beaten (done flag set,
/// `boss_flag(done)` true), in table order without duplicates. The playtest guest's save had the
/// ledger bits of the Oedon Tomb Key (31000) and the Sword Hunter Badge (50000010) and both boss
/// flags, but neither item: 0.6.0 gave them while summoned, and a summoned guest's key items are
/// not what its save keeps. Then every SharedKeyTreasureLot whose treasure flag this save has
/// (the guest's save had 52411000, the Blood Gem Workshop Tool's flag, and no tool). The caller
/// gives each one whose slots are all missing unique goods with a key item (KeyCatchupNeeded) in
/// the own world, from the save alone (no host needed).
std::vector<std::int32_t> KeyCatchupLots(const std::function<bool(std::uint32_t)>& own_flag,
                                         const std::function<bool(std::uint32_t)>& boss_flag);
std::size_t ItemLotFlagCount();
std::size_t ItemLedgerCount();

/// Per-player or repeatable lots that are never replayed (docs/party/items.md 6.1 item 3).
bool ItemLotDenied(std::int32_t lot, std::int32_t flag);
/// Party policy (0.7.5, generated table ITEM_UNSHARED): the lot gives no key item and no unique item
/// (goodsType 1, maxNum 1, weapons / armor / rings), so it is per player. ItemLotDenied includes it.
bool ItemLotUnshared(std::int32_t lot);
/// The Doll / Messenger gifts every player gets once in the own Hunter's Dream (Beckoning Bell
/// 10010, Small Resonant Bell 10000, Old Hunter Bell 10050). Their talk awards them hostOnly, which
/// a summoned guest's game refuses (items.md 3): the guest gives the lot to itself instead
/// (ItemSource::Own), with its own flag (playtest 1: the Old Hunter Bell was lost). The Old Hunter
/// Bell is also replayed from the host (0.7.3, not ItemLotDenied): its flag 6670 is each player's
/// own, so every member gets it once, whoever talked.
bool MessengerGiftLot(std::int32_t lot);
/// P2: the repeatable NPC gifts of common 9100 (Blood of Arianna 28000, Blood of Adella 32020,
/// Iosefka's Blood Vial 23000, Blue Elixir 24060, Queenly Flesh 17010).
bool RepeatGiftLotId(std::int32_t lot);
/// 0.7.3: the repeatable gifts that also go to the whole party: Iosefka's Blood Vial 23000 (her
/// talk sets 72410301 "given", shared with the host's world, so after one player's talk nobody
/// else got one). A guest's talk reports it up (gift_up with the lot's ledger bit); the host's
/// world awards it once (its own ledger bit) and C3 gives it to every member (ledger 5).
bool SharedRepeatGiftLot(std::int32_t lot);
/// 0.7.3: Valtr's gifts of m27 event 12700906 (43120 Impurity rune, flag 6676; 43130 League Staff,
/// flag 6678; party_worldact OwnTalkGiftLot): per player (personal flags), never replayed, given by
/// a guest to itself when its talk asked for them in the host's world.
bool OwnTalkGiftLotId(std::int32_t lot);
/// Keys of repeatable gifts: kRepeatKeyBase + n, far above every flag and ledger bit.
constexpr std::int64_t kRepeatKeyBase = std::int64_t(1) << 40;
/// The guest's own grant of repeatable gift `lot`, the `n`th this session (no flag, no ledger bit:
/// it is given every time). Nothing for a lot that is not one of them.
std::optional<ItemGrant> MakeRepeatGrant(std::int32_t lot, std::uint64_t n);
/// Keys of Chalice Dungeon awards (one per kill): kChaliceKeyBase + n, above the repeat keys.
constexpr std::int64_t kChaliceKeyBase = std::int64_t(1) << 41;
/// A Chalice Dungeon lot (1100xxxxx / 2001xxxxx and every lot >= 100,000,000, area 29 lots): C3
/// never replays it by flag (dungeon flags are the slot's own); the host's award goes to the guests
/// present instead (ItemSource::Chalice).
bool ChaliceAwardLot(std::int32_t lot);
/// The host's Chalice award `n` of this session: what its roll gave (`items`, required: a guest
/// never rolls a dungeon lot itself), no flag, no ledger bit. nullopt for anything else.
std::optional<ItemGrant> MakeChaliceGrant(std::int32_t lot, const std::vector<GrantItem>& items, std::uint64_t n);
/// Guest: take a Chalice award only while it is a summoned phantom (load mode 1) in a Chalice
/// Dungeon (map area 29): the guests at the kill. At home / elsewhere it is not this player's.
bool ChaliceGrantAccepted(bool summoned, std::uint32_t map_id);

/// Host hook: the grant for an award of `lot` (no seq yet), or nothing (client lots, denied,
/// flagless lots outside the ledger).
std::optional<ItemGrant> ClassifyAward(std::int32_t lot, bool host_only);
/// Host flag: the grant for item flag `flag`; `captured` maps flags to lots the hook saw (it
/// wins over the table for shared flags).
std::optional<ItemGrant> ClassifyFlag(std::int32_t flag, const std::map<std::int32_t, std::int32_t>& captured);
/// Per-player world treasure (D4, `instanced`: guests pick up their own copy): a map treasure's
/// pickup flag (5AABZnnn, area > 0) the host set by picking it up (no award hook `captured` it)
/// is not handed to the party - every player takes the treasure in the own world. Except the
/// key items of SharedKeyTreasureLot: those go to everyone.
bool InstancedTreasureFlag(std::int32_t flag, bool instanced, bool captured);
/// Map treasure lots that hold a key item (EquipParamGoods.goodsType 1: unique, maxNum 1) or are
/// on the questline list (Gascoigne's daughter: ribbons, brooch): generated ITEM_SHARED_TREASURE
/// rows (item_tool.py). When the host picks one up, every member gets it (C3), so a guest who did
/// not walk to the corpse is not locked out of a door, the Memory Altar or a questline. A guest
/// that holds the item or has the lot's flag already gets nothing twice (docs/party/items.md 6.2).
/// BB_PARTY_SHARED_TREASURE=0 (testing): nothing is shared, every treasure is per player.
bool SharedKeyTreasureLot(std::int32_t lot);

// ---- PartyLink EVENT "items" / "items_full" payload ----
constexpr const char* kItemsEventName = "items";
constexpr const char* kItemsFullEventName = "items_full";
/// {"items": [[seq, lot, flag, ledger, source], ...]} (compact: the full list has ~1000 rows).
json::Value ItemsToJson(const std::vector<ItemGrant>& items);
bool ItemsFromJson(const json::Value& v, std::vector<ItemGrant>* out, std::string* error = nullptr);
std::string ItemsToJsonText(const std::vector<ItemGrant>& items);
bool ItemsFromJsonText(const std::string& text, std::vector<ItemGrant>* out, std::string* error = nullptr);
/// Rows per event at most (the full list has ~1000; ItemsFromJson refuses more).
constexpr std::size_t kMaxItemsPerEvent = 4096;
/// May a grant the host sent reach the game (bbport security pass)? A host source (award, flag,
/// full), a lot that is not denied, and a lot / flag (or ledger row) pair the tables agree on.
/// A lot the tables do not know is refused whatever flag comes with it (an honest host only
/// sends table pairs; docs/party/items.md 6.5). `why` says what is wrong.
bool ItemGrantFromPeerAllowed(const ItemGrant& g, std::string* why = nullptr);
/// The allowed rows of `in`; `rejected` (optional) counts the rest.
std::vector<ItemGrant> FilterPeerItems(const std::vector<ItemGrant>& in, std::size_t* rejected = nullptr);

// ---- D5: an NPC's gift to a guest reaches the host (and through C3 every other member) ----
// NPC choices are shared (D8): a guest's talk moves the NPC's quest in the host's world, so the
// gift the talk handed the guest would be gone for the host and every other guest (the NPC is
// past that step). The guest reports the lot (EVENT "gift_up"); the host takes the same lot
// through the game's own award (skipped when its flag is already set) and C3 hands it on.
constexpr const char* kGiftUpEventName = "gift_up";
/// May a guest's own award of `lot` (flag `flag`) in the host's world go up: a flagged lot the
/// tables know with exactly that flag, not denied (per-player / repeatable), not a map treasure
/// (5AABZnnn, area > 0: instanced per player, D4); or a SharedRepeatGiftLot with its ledger bit
/// (60009000 + its ledger index) as the flag.
bool GuestGiftShareable(std::int32_t lot, std::int32_t flag);
std::string GiftUpToJsonText(std::int32_t lot, std::int32_t flag);
/// Parses and validates (GuestGiftShareable) one report.
bool GiftUpFromJsonText(const std::string& text, std::int32_t* lot, std::int32_t* flag, std::string* error = nullptr);
/// Host: gifts one guest slot may report per 60 s.
class GiftRateLimit {
public:
    static constexpr int kPerWindow = 4;
    static constexpr double kWindow = 60.0;
    bool Take(int slot, double now);

private:
    double start_[8] = {-1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18};
    int used_[8] = {};
};

// ---- Host ledger: what was sent ----
class HostItems {
public:
    static constexpr std::size_t kMaxQueue = 4096;
    /// Queues `g` with the next seq unless its key was offered before. True when queued.
    bool Offer(ItemGrant g);
    bool Pop(ItemGrant* out);
    std::size_t Size() const { return q_.size(); }
    /// Remembers `lot` for `flag` (hook capture), consulted by ClassifyFlag.
    void Capture(std::int32_t flag, std::int32_t lot) { captured_[flag] = lot; }
    const std::map<std::int32_t, std::int32_t>& Captured() const { return captured_; }
    std::uint64_t NextSeq() { return ++seq_; }
    void SeedSeq(std::uint64_t s) { if (s > seq_) seq_ = s; }

private:
    std::deque<ItemGrant> q_;
    std::set<std::int64_t> sent_;
    std::map<std::int32_t, std::int32_t> captured_;
    std::uint64_t seq_ = 0;
};

// ---- Guest: queue and apply gate ----
struct ItemApplyState {
    bool world_up = false;
    bool loading = false;
    int load_mode = -1;       ///< SprjEventFlagMan +0x80: 0 own world
    int own_world = -1;       ///< WorldTransitionState +0x1592: 1 own world
    int session_role = -1;    ///< SprjSessionManager +0x124
    bool game_data = false;   ///< GameDataMan +8 and the MapItemMan exist
    bool calm = true;         ///< the player lost no HP lately (not in a fight): a popup may open
    bool alive = true;        ///< the local player's HP > 0
    bool cutscene = false;    ///< a remo (cutscene) plays
    bool boss_quiet = true;   ///< no boss health bar and the boss-defeat banner had time to end
    bool game_dialog = false; ///< a game message / dialog is open (menu +0x78; parity67 D4: a box under it)
};
/// Why the item-get dialog may not open now (playtest 1 #15 / #12: a box pushed while the player
/// was dead, in a cutscene or under the boss-defeat banner stayed on screen and could not be
/// dismissed), or nullptr when it may: "in a fight", "dead", "cutscene", "boss defeat banner".
const char* ItemPopupHold(const ItemApplyState& s);
/// Seconds after a boss health bar went away before an item popup may open (the "Prey
/// Slaughtered" banner and its sound run ~6 s).
constexpr double kBossBannerQuiet = 10.0;

/// The native item-get dialog's watchdog (#15 / #12): the box is modal and only the player's
/// confirm clears it; a box left over a load or a death, or one whose confirm check stays held,
/// can stay on screen for good. Clear it (the items are in the inventory already: the dialog is
/// only the notice) when entries stay up
///  - after a load that started while they were up (the world they belonged to is gone),
///  - while the player is dead for kDeadFor s,
///  - or with the confirm check held (player busy / no action-button manager) for kHeldFor s.
class ItemGetWatch {
public:
    static constexpr double kDeadFor = 3.0;
    static constexpr double kHeldFor = 20.0;
    /// Once a frame. `entries` in the dialog list, `armed` = the confirm check can fire. True =
    /// clear the list now; `why` names the reason.
    bool Step(double now, int entries, bool armed, bool alive, bool loading, const char** why);
    double UpSince() const { return up_since_; }

private:
    bool up_ = false, crossed_load_ = false;
    double up_since_ = 0, dead_since_ = -1, held_since_ = -1;
};


/// In the own world with the flag store in mode 0, no loading screen, not joining/leaving/client.
bool ItemApplyAllowedNow(const ItemApplyState& s);
/// Summoned into the host's world (session role 6, flag store mode 1 = the host's overlay, own
/// world byte 0), world steady: grants go straight into the own inventory and the own pool.
bool ItemSummonedApplyAllowedNow(const ItemApplyState& s);
enum class ItemApplyMode : std::uint8_t { None, Home, Summoned };
ItemApplyMode ItemApplyModeNow(const ItemApplyState& s);

// ---- Does a lot still fit the guest's inventory? ----
// ---- Awarding while summoned (no own-world phase with seamless play) ----
/// One ItemLotParam row (ITEMLOT_PARAM_ST) as far as the summoned give needs it.
struct LotRowData {
    std::int32_t lot = -1;
    std::int32_t flag = -1;               ///< getItemFlagId (<= 0: none)
    std::int32_t id[8] = {};              ///< lotItemId01..08
    std::uint32_t category[8] = {};       ///< lotItemCategory01..08
    std::uint16_t base[8] = {};           ///< lotItemBasePoint01..08
    std::uint16_t cumulate[8] = {};       ///< cumulateLotPoint01..08
    std::uint8_t num[8] = {};             ///< lotItemNum01..08
};
/// The roll (0x17CEAF0) walks lot, lot + 1, ... while rows exist; this many at most here.
constexpr std::size_t kMaxLotChain = 16;
/// Categories the direct give (GiveItemDirect 0x131CB70, type bits category << 28) handles:
/// weapon, armor, rune, goods. Blood gems (8) are generated per drop (0x1A88130, the area's gem
/// level); a summoned guest recreates the gem the host's game generated (GrantItem::gem) and
/// waits for the own world only when the host sent no gem data.
bool GrantCategorySupported(std::uint32_t category);
/// Does `chain` have a weighted blood-gem slot (the host's roll cannot stand in for it)?
bool LotChainHasGem(const std::vector<LotRowData>& chain);
enum class LotPlanResult : std::uint8_t {
    Give,        ///< `items` (maybe empty: nothing left) - give them, then set the flags
    Random,      ///< a row picks between several slots and the host sent no item list
    Unsupported, ///< a blood gem without the host's gem data (or an unknown category): own world only
    Missing,     ///< no ItemLotParam row
};
const char* LotPlanResultName(LotPlanResult r);
struct LotPlan {
    LotPlanResult result = LotPlanResult::Missing;
    std::vector<GrantItem> items;
    std::vector<std::int32_t> row_flags; ///< flags of the rows that give (set with the done flag)
    std::string why;
};
/// What a summoned guest gives for a lot: `chain` is its ItemLotParam row and continuation rows;
/// `host_items` (the host's capture / roll, may be empty) is used when every item matches a
/// different row's slot (category, id, 1..lotItemNum; a blood gem: a gem slot whose id is at most
/// kMaxGemIdOffset below the gem's, count 1, with gem data), else the rows decide - one weighted slot
/// per row (base or cumulate point > 0) is deterministic. Rows whose own flag `row_done` reports
/// set give nothing (the roll skips them the same way).
LotPlan PlanSummonedLot(const std::vector<LotRowData>& chain, const std::vector<GrantItem>& host_items,
                        const std::function<bool(std::int32_t)>& row_done);
/// Host: one roll of `chain` (each row: one slot by lotItemBasePoint weight; luck and cumulate
/// counters ignored) for a random lot, so every guest gets the same items. Item discovery (per
/// player on the shared enemy drop roll, party_droproll.h) cannot change these: of the 1.09 C3
/// lots only six rows are random (3401731/41/51, 9905300, 22502610, 99052010), and the only ones
/// with a luck slot (3401731/41/51: a gem) are never rolled here (LotChainHasGem: the guest's own
/// game rolls them in its own world, with its own discovery). The others have no or only luck
/// slots, where the game's discovery factor cancels out (dropdisc 2026-10-15).
std::vector<GrantItem> RollLotChain(const std::vector<LotRowData>& chain, std::uint64_t seed);
/// Host: does `chain` need a roll (a row with more than one weighted slot)?
bool LotChainRandom(const std::vector<LotRowData>& chain);

/// One ItemLotParam slot as the guest's game sees it (only slots that can drop: id > 0, a
/// category other than -1, a base point > 0).
struct LotSlotState {
    std::uint32_t category = 0xffffffffu; ///< lotItemCategory: 0 weapon, 1 armor, 2 rune, 4 goods
    std::int32_t id = -1;                 ///< lotItemId
    int num = 1;                          ///< lotItemNum (0 counts as 1)
    int owned = -1;                       ///< goods: how many the player holds; -1 unknown
    int max_num = -1;                     ///< goods: EquipParamGoods maxNum; -1 unknown
    int max_storage = -1;                 ///< goods: EquipParamGoods maxRepositoryNum; -1 unknown
    int goods_type = -1;                  ///< goods: EquipParamGoods goodsType (1 key item); -1 unknown
    int usable = -1;                      ///< goods: 1 the player can use it from the menu (refId >= 0 or
                                          ///< goodsUseAnim != 0: Gold Pendant, Tear Stone, vials), 0 not
                                          ///< (keys, badges, workshop tools); -1 unknown
};
enum class LotFit : std::uint8_t {
    Give,  ///< nothing in the way: award it
    Owned, ///< a unique goods item (maxNum 1, no storage) of the lot is held already (badge, key)
    Full,  ///< a goods item would go over maxNum with no storage room: the game would refuse it
           ///< with a blocking "inventory or storage max" dialog
};
const char* LotFitName(LotFit f);
/// EquipParamGoods (EQUIP_PARAM_GOODS_ST, 1.09) limits of a goods row (at least kGoodsRowBytes
/// bytes) into `s`: maxNum s16 +0x36, goodsType u8 +0x3A, maxRepositoryNum s16 +0x48 - the offset
/// the game's own storage cap reads (0x15813A0: `*(short*)(row + 0x48)`; Pebble 20 / 99), and
/// `usable` from refId s32 +0x00 / goodsUseAnim u8 +0x3E (KeyRegiveAllowed).
constexpr std::size_t kGoodsRowBytes = 0x4A;
void GoodsLimitsFromRow(const std::uint8_t* row, LotSlotState* s);
/// Owned / Full when any goods slot would be refused (the whole lot is then skipped: one refused
/// item opens the blocking dialog); `why` names the slot. Unknown counts (-1) never refuse.
LotFit ClassifyLotFit(const std::vector<LotSlotState>& slots, std::string* why = nullptr);
/// A ledger skip that looks wrong (#16): the ledger says the lot was given, yet the player holds
/// none of its items and they are unique goods (maxNum 1, no storage: keys, badges). The guest gets
/// it after all only when the items are never used up (KeyCatchupNeeded: a missing Oedon Tomb Key
/// blocks the game; a used Gold Pendant must not come back).
bool LedgerSkipSuspicious(const std::vector<LotSlotState>& slots);
/// verify_c6 (live): a summoned guest's inventory never shows its key items (EquipParamGoods
/// goodsType 1: Oedon Tomb Key 4000, 4114, Old Hunter Bell 4312, ...): the game leaves them out of
/// the summoned view at every host-world load (held 1 -> 0 across a party travel, while the
/// Sword Hunter Badge, goodsType 0, stayed), and the own save keeps them (held 1 again at home,
/// on disk). While summoned, a key item reading 0 is no sign of a lost item: such a lot is checked
/// in the own world instead of re-given at every join (an item-get box each time).
bool LotHasKeyItem(const std::vector<LotSlotState>& slots);
/// H3 (par_c6): goods given while summoned (GiveItemDirect, load mode 1) looked lost after the next
/// summon load or a restart. verify_c6 (live) found the cause: key items (goodsType 1) never show
/// in a summoned guest's inventory (LotHasKeyItem); the save written while summoned does keep the
/// gives (Sword Hunter Badge on disk with no own-world visit; Oedon Tomb Key held at home after
/// three crashes). Playtest 1 save audit: that evidence came from a base save that already held
/// both keys; the playtest guest's real save (0.6.0) did NOT keep a key item given while summoned
/// (4114 "held 0 -> 1" at G5:40207, ledger bit 60009036 in the save, no 4114 in its key-item list
/// an hour of play later). So this is the fix, not just a safety net: each give is remembered (on disk) with
/// the held count right after it (`expect`); in the next own world the missing part is given
/// again, min(count, expect - now) (0 when nothing is missing or a count is unknown), and saved.
int SummonedRegiveCount(int expect, int now, int count);
/// 0.6.2: goods whose held count a summoned guest cannot trust: a key item (goodsType 1) or any
/// unique goods (maxNum 1, no storage). Regression run 0.6.1: Amelia's reward 4001 (unique,
/// goodsType 0) read 0 summoned and 1 at home; the summoned give then opened the game's blocking
/// "Unable to acquire ... due to inventory or storage max" box. The game refuses a second copy.
bool KeyGoods(const LotSlotState& s);
/// Any KeyGoods slot in `slots`.
bool LotHasKeyGoods(const std::vector<LotSlotState>& slots);
/// May a summoned guest give these items without more? Not when one is KeyGoods: such a grant is
/// given and also owed to the own world (the key-item catch-up reads the real held count there and
/// gives it again only if missing; the game puts the saved key section back at home), or with
/// BB_PARTY_KEY_ITEMS_SUMMONED=0 only waits for the own world.
bool SummonedGiveAllowed(const std::vector<LotSlotState>& slots);
/// P2: a repeatable NPC gift (ItemSource::Repeat) may also be given while summoned when its unique
/// goods are consumables (goodsType 0: Blood of Arianna / Adella, Iosefka's Blood Vial, maxNum 1):
/// a summoned guest's consumables read their real count (it uses its vials there), unlike key items
/// (goodsType 1, e.g. Queenly Flesh), which wait for the own world. One held already: Owned (skip).
bool RepeatSummonedGiveAllowed(const std::vector<LotSlotState>& slots);
/// May the own world give one more of this goods slot now? Not a KeyGoods item already held
/// (`held` > 0), and never with an unknown count (-1) for KeyGoods.
bool KeyGiveAllowedHeld(const LotSlotState& s, int held);
/// May a held count of 0 be read as "lost" for this slot, so that it is given again? Only for
/// KeyGoods the player cannot use up: not usable from the menu (keys, badges, workshop tools,
/// chalices; `usable` 0). A usable unique item at 0 was most likely used: the Gold Pendant and the
/// Tear Stone turn into a blood gem when used (0.6.9 playtest 2: a host's Gold Pendant came back
/// after every load, one more gem each time). Unknown (-1): no.
bool KeyRegiveAllowed(const LotSlotState& s);
/// A lot to catch up in the own world: every slot unique goods not held (LedgerSkipSuspicious)
/// and every slot one that is never used up (KeyRegiveAllowed). 0.6.2 - 0.6.9 caught up any
/// unique goods, so a used Gold Pendant (Amelia's ledger lot 50000001) was given back at the next
/// own-world load. Own-world reads only: summoned, keys read 0.
bool KeyCatchupNeeded(const std::vector<LotSlotState>& slots);
/// May a summoned guest give these key goods now (and owe them to the own world, where a copy the
/// game dropped is given again)? Only when every KeyGoods slot is KeyRegiveAllowed; a usable unique
/// item (Gold Pendant) waits for the own world as before 0.7.0 (a copy given now and used while
/// summoned would come back at home).
bool SummonedKeyGiveNow(const std::vector<LotSlotState>& slots);
/// Own world, a lot owed to this save slot (party_owed_lots.txt): give this goods slot? `given`:
/// the lot was given while summoned (its done flag is in the save; a copy may have been dropped):
/// only a KeyRegiveAllowed item reading 0. Not given (deferred): never when the lot's done flag is
/// in the save now (the award or another path gave it), else one only when none is held.
bool OwedGiveAllowed(bool given, bool done_flag_set, const LotSlotState& s, int held);
/// H4 (0.7.1, items.md 10.7): the inventory keeps two key sections, the character's own (the save's)
/// and a summoned one the game empties at every host-world load and shows instead. BB_PARTY_KEY_VIEW
/// unset or `1`: a party guest is shown its own section while summoned too; `0`: the game's view.
bool KeyViewOn(const char* env);
/// Give a key item / unique goods to a summoned guest at once (`give_env` = BB_PARTY_KEY_ITEMS_SUMMONED)?
/// With the own key section shown (`key_view`) the held count is the real one, so yes unless `0`.
/// Without it only `1` gives at once, and never a usable unique item (`usable`, the Gold Pendant).
bool SummonedKeyGiveNowRule(bool key_view, const char* give_env, bool usable);
/// party_summoned_gives.txt is one file per user folder, shared by every character in the save
/// slots: an entry given to the character in save slot `entry_slot` is re-given only to that one
/// (-1 on either side = unknown: an older file or an unreadable slot; given as before).
bool SummonedGiveForSlot(int entry_slot, int current_slot);

class GuestItems {
public:
    static constexpr int kStableTicks = 90;      ///< frames the gate must hold (world settled)
    static constexpr double kSpacing = 8.0;      ///< s between two awards (each opens a popup)
    static constexpr std::size_t kMaxQueue = 4096;

    /// Queues grants whose key is not queued or done yet; returns how many were taken. Marks
    /// go to the front (they must land before an item replay of the same flag).
    std::size_t Offer(const std::vector<ItemGrant>& items);
    /// Once a frame: the grant to apply now, else nothing. The caller reports Done(key) after,
    /// or Defer(g) for a grant that cannot be given while summoned (random lot without the host's
    /// list, blood gems): it keeps its place and waits for the own world.
    std::optional<ItemGrant> Step(const ItemApplyState& s, double now);
    void Done(std::int64_t key) { done_.insert(key); home_only_.erase(key); }
    void Defer(const ItemGrant& g);
    /// The mode of the grant Step returned last.
    ItemApplyMode LastMode() const { return last_mode_; }
    /// Why the last Step held a grant that was due (ItemPopupHold), else nullptr.
    const char* LastHold() const { return last_hold_; }
    /// The grant Step returned last is a quiet one (a host catch-up grant while summoned: no spacing,
    /// no popup; the caller shows one summary notice for the batch).
    bool LastQuiet() const { return last_quiet_; }
    std::size_t HomeOnly() const { return home_only_.size(); }
    /// The grant Step returned was really awarded (a popup opened): the next waits kSpacing (not
    /// after a quiet one, LastQuiet).
    /// Skips (flag already set, item held) take no time, so a long catch-up list goes fast.
    void Awarded(double now) { last_apply_ = now; }
    std::size_t Pending() const { return q_.size(); }
    bool IsDone(std::int64_t key) const { return done_.count(key) != 0; }

private:
    std::deque<ItemGrant> q_;
    std::set<std::int64_t> queued_, done_;
    std::set<std::int32_t> adhoc_lots_;  // lots outside the tables, taken once each
    std::set<std::int64_t> home_only_;   // deferred while summoned
    ItemApplyMode stable_mode_ = ItemApplyMode::None, last_mode_ = ItemApplyMode::None;
    const char* last_hold_ = nullptr;
    bool last_quiet_ = false;
    int stable_ = 0;
    double last_apply_ = -1e9;
};

#ifndef BB_PARTY_ITEMS_NO_GAME
/// bbgpu_patch_image, once, after coop::HooksInit (party mode only): the award hook and the
/// parity patches (byte-verified; a mismatch is logged and that site skipped). Registers
/// OnHostFlagSet as C2's item-flag observer (progress::SetItemFlagObserver).
void InstallItemsPatches();
/// The own key section is shown to this summoned guest (KeyViewOn and its load hook is in).
bool SummonedKeyViewActive();
/// Host: C2 reports a host item-pickup flag 0 -> 1 (any thread).
void OnHostFlagSet(std::uint32_t flag);
/// Main thread, own world (load mode 0), before C2 applies any host flag (C2's before-apply hook):
/// once per save (marker kItemLedgerSeeded), the ledger bit of every flagless lot whose awarding
/// event's completion flag the save already holds - the player earned it solo, so a host's replay
/// does not give it twice. The awarding event's flag is not checked at apply time: C2 writes the
/// host's boss flags into the guest's save first. Saves that took C2 flags before seeding existed
/// may hold host boss flags and so skip a lot they never got (dev saves only).
void SeedLedgerFromOwnSave();
/// Host: the next grant to broadcast (any thread).
bool PopHostItems(ItemGrant* out);
/// Host: a member (re)joined and needs the full list (any thread; the PartyLink callback).
void RequestHostFullItems(int slot);
/// Host, main thread (the director tick): once the host's own world is up, takes one waiting
/// slot and builds its full list from the host's flags (every lot whose pickup flag, or whose
/// awarding event's completion flag, is set). False while nothing waits or the world is not up.
bool TakeHostFullItems(int* slot, std::vector<ItemGrant>* out);
/// Guest: grants from the host (any thread; the PartyLink callback).
void RequestGuestItems(const std::vector<ItemGrant>& items);
/// Main thread, once a frame (the coop tick): the guest replay.
void ItemsTick();
/// Guest (main thread): a repeatable NPC gift (RepeatGiftLotId) its talk asked for in the host's
/// world: queued as its own grant (given while summoned like any grant). False: not such a lot or
/// items off.
bool OfferRepeatGift(std::int32_t lot);
/// Guest (main thread): an own talk gift (OwnTalkGiftLotId) its talk asked for in the host's world:
/// queued as its own grant (ItemSource::Own, the lot's personal flag: once per player). False: not
/// such a lot, items off, or already queued.
bool OfferOwnGift(std::int32_t lot);
/// Test aid (main thread): the game's own item-lot award (AwardItemLot) for this player only: a
/// guest's award in the host's world is not reported up as an NPC gift (D5), so the host does not
/// receive it too (BB_PARTY_GUESTSVC_TEST `lot=N`).
bool AwardLotGuestOnly(std::uint32_t lot);
/// D4 (pard4), guest: it picked up map treasure flag `flag` in the host's world. A shared
/// progression key / workshop tool (SharedKeyTreasureLot) is reported up like an NPC gift
/// (gift_up): the host's world awards it and C3 gives it to every member (party progress is shared
/// both ways). False: not such a treasure (instanced: own save only) or items off.
bool ReportGuestKeyTreasure(std::int32_t flag);
/// Guest: the next NPC gift to report to the host (any thread; the director posts it).
bool PopGuestGift(std::string* body);
/// Host: EVENT gift_up from `slot` (link thread): validated, rate limited, queued for the tick.
bool OnHostGiftUp(int slot, const std::string& body, std::string* why);
#endif

} // namespace coop
