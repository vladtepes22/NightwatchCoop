// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op C2: host-authoritative progress sync of event flags (design:
// docs/party/event_flags.md). The host's flag changes in the syncable categories
// (party_flags.inc) are captured and sent to the guests, who write them into their OWN save pool,
// so a guest's save keeps up with the host's world even though the game itself never progresses
// a summoned phantom's save (load mode 1, see the design doc section 3).
//
//   host:  hooks SetEventFlag 0x13cfcc0, SetEventFlagValue 0x13d0060, Lua CompleteEvent
//          (BroadcastSetFlag) 0x132aad0 and the EMEVD completion writer
//          SprjEmkEventIns::EndOrRestart 0x12ed100 note candidate ids; on the main-thread tick,
//          at safe points, the noted ids and (every BB_PARTY_PROGRESS_SCAN_MS, default 1000) every
//          syncable block are compared with a shadow copy, so writers that bypass the hooks are
//          caught too. Changes -> PopHostFlagChanges(); joins -> FullSnapshot().
//   guest: ApplyHostFlags / ApplyHostSnapshot queue (any thread); the main-thread tick applies
//          them at safe points (world up, no loading screen, store stable): load mode 1 ->
//          the guest's own pool directly (pool + key*125, MSB-first); load mode 0 -> SetEventFlag
//          (only in the window right after a map load, where event triggers are least harmful).
//
// Policy (guest side, per category; host sends every candidate category):
//   set-only:  boss_defeated, lamp_unlocked, shortcut_door, key_event (not the endings),
//              cutscene_seen, world_state
//   state:     elevators / toggling gates (both 0 and 1 follow the host)
//   follow:    npc_quest (shared quest choices; BB_PARTY_PROGRESS_NPC=0 turns it off)
//   per player: covenant_outcome (0.7.3: Annalise, Alfred, Valtr) while the host's rule
//              "Covenant outcomes per player" is on (Policy::covenant_per_player); off: its base
//              category's mode
//   off:       item_lot_picked (always: C3, party_items, owns item pickups - the host side hands
//              every item-pickup flag it sees set to the SetItemFlagObserver), boss_area,
//              session_runtime, time_of_day (C4 cutscene replay only), never_sync (incl. the area
//              gate flags of 0x47304B0/0x47301B0 - ON blocks the bells - and the ending flags),
//              ids the game randomises / counts / rebuilds each load.
//   never written from outside: 9020-9026 (chalice slot), chalice / area 99 groups.
//   C3's ledger flags 60009000+ (type-6 global group) are never_sync here and never touched.
//
// Env: BB_PARTY_PROGRESS=0 disables everything (no hooks, nothing sent or applied).
//      BB_PARTY_FLAG_DUMP=1 writes BBPF v1 snapshots (tools/party/flag_tool.py) to
//        <user dir>/party/flags_<label>_<frame>.bbpf at session start / end and on each sync
//        (at most one per 2 s), plus the raw own pool ("<label>-pool").
//      BB_PARTY_PROGRESS_NPC=0 stops npc_quest (default on, SyncMode::Follow) on guests. BB_PARTY_PROGRESS_MIRROR=1 also writes set-only flags into the
//        host-world overlay while in load mode 1 (visible at once; default off: the overlay
//        already follows the host through the vanilla snapshot / CompleteEvent channels).
//      BB_PARTY_PROGRESS_SCAN_MS (host diff period, default 1000, min 100).
//
// Addresses are our offsets (raw ELF VA of eboot.elf). Everything touching the game is
// main-thread only unless noted. The pure parts (categories, keys, bits, the store walker over
// injectable memory ops, policy, diff, JSON, BBPF) are testable without the game
// (tests/test_party_progress.cpp, target party-progress-test).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace json {
struct Value;
}

namespace coop {
struct GameSnapshot;
}
namespace party {
class PartyLink;
}

namespace coop::progress {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- categories (party_flags.inc: first match wins; no row = never_sync) ----

enum class Category : u8 {
    never_sync = 0,
    boss_defeated,
    boss_area,
    lamp_unlocked,
    shortcut_door,
    npc_quest,
    item_lot_picked,
    key_event,
    cutscene_seen,
    world_state,
    session_runtime,
    time_of_day,  ///< 9800-9802: only through the cutscene replay path (C4), never silently
    /// 0.7.3 (user decision 2026-10-22): the quest state of the covenant leaders and of the NPC whose
    /// quest ends a covenant (Annalise, Alfred, Valtr; flag_tool.py COVENANT_ROWS). Per player while
    /// the host's rule "Covenant outcomes per player" is on (CovenantPerPlayer): never copied into
    /// another player's save, never a guest's world action; the world mirror still shows the host's
    /// state in the host's world. Off: the base category (BaseCategoryOf) as before.
    covenant_outcome,
    kCount
};
constexpr int kCategoryCount = static_cast<int>(Category::kCount);
const char* CategoryName(Category c);
bool CategoryFromName(const std::string& name, Category* out);

/// Disjoint sorted ranges equivalent to the first-match table.
struct CategoryRange {
    u32 first, last;
    Category cat;
};
const std::vector<CategoryRange>& CategoryRanges();
/// Binary search over CategoryRanges(); never_sync when no row covers the id.
Category CategoryOf(u32 id);
/// The first-match linear scan of the raw table (reference for tests).
Category CategoryOfLinear(u32 id);
/// The category of `id` without the covenant_outcome rows (what such an id was before 0.7.3 and is
/// again while the rule is off); CategoryOf for every other id.
Category BaseCategoryOf(u32 id);
/// The rule "Covenant outcomes per player" in force (the host's setting; a guest follows the host's
/// party rules). Default on. Any thread (atomic); the party glue sets it every tick.
void SetCovenantPerPlayer(bool on);
bool CovenantPerPlayer();
/// The category that decides what happens to `id` now: covenant_outcome while the rule is on, the
/// base category of a covenant id while it is off.
inline Category EffectiveCategory(u32 id, bool covenant_per_player) {
    const Category c = CategoryOf(id);
    return c == Category::covenant_outcome && !covenant_per_player ? BaseCategoryOf(id) : c;
}

// ---- flag ids, group keys, bits ----

constexpr u32 kBitsPerBlock = 1000;
constexpr u32 kBlockBytes = 125;
constexpr u32 kPoolBlocks = 1200;

/// resolve_group_key 0x13bc710, mirrored exactly (type digit, area, map block, zone).
int GroupKey(int type, int area, int block, int zone);
/// The pool index of a flag block (id / 1000), or -1 when the block is not in the saved pool
/// (area 99, unknown map, invalid type).
int PoolKeyOfBlock(u32 block);
inline int PoolKeyOfId(u32 id) { return PoolKeyOfBlock(id / kBitsPerBlock); }
/// The canonical block of a pool key (inverse of PoolKeyOfBlock; chalice instance keys use map
/// block 0), or 0xffffffff.
u32 BlockOfPoolKey(int key);
/// MSB-first: flag f of a block is bit (7 - f % 8) of byte f / 8.
inline bool GetBit(const u8* data, u32 bit) { return (data[bit >> 3] & (0x80u >> (bit & 7))) != 0; }
inline void PutBit(u8* data, u32 bit, bool on) {
    const u8 m = static_cast<u8>(0x80u >> (bit & 7));
    data[bit >> 3] = on ? static_cast<u8>(data[bit >> 3] | m) : static_cast<u8>(data[bit >> 3] & ~m);
}
/// Ids nothing outside the game may write (9020-9026 chalice slot, chalice areas 40-47 / 29,
/// area 99, block-less ids).
bool NeverWritable(u32 id);

// ---- policy ----

// Follow (npc_quest, D8 shared quest choices): the host's live changes are applied both ways (a
// quest step clears the old state flag and sets the new one: Iosefka 1141 off, 1143 on); a
// snapshot (catch-up) sets only, except the 1000-1999 NPC state machines (20 flags per NPC): a
// group the host has any bit of is copied exactly, so the guest's save never keeps a stale
// state next to the host's (two states of one NPC at once).
enum class SyncMode : u8 { Off = 0, SetOnly = 1, State = 2, Follow = 3 };
struct Policy {
    std::array<SyncMode, kCategoryCount> mode{};
    bool mirror_overlay = false;
    /// The host's rule "Covenant outcomes per player": covenant_outcome ids are Off; false: they
    /// follow their base category's mode.
    bool covenant_per_player = true;
    static Policy Defaults();
    /// Defaults plus BB_PARTY_PROGRESS_NPC / _MIRROR. item_lot_picked stays Off (C3 owns items).
    static Policy FromEnv();
    bool operator==(const Policy& o) const {
        return mode == o.mode && mirror_overlay == o.mirror_overlay && covenant_per_player == o.covenant_per_player;
    }
    bool operator!=(const Policy& o) const { return !(*this == o); }
};
/// What the guest does with `id` (category, the state list, the exclusions, NeverWritable).
SyncMode ModeFor(const Policy& p, u32 id, Category cat);
inline SyncMode ModeFor(const Policy& p, u32 id) { return ModeFor(p, id, CategoryOf(id)); }
/// The 1000-1999 NPC quest state machine of `id` (0-49), or -1.
int NpcStateGroup(u32 id);
/// The host captures and sends this id (any category a guest may enable, minus the exclusions).
bool HostCandidate(u32 id, Category cat);
inline bool HostCandidate(u32 id) { return HostCandidate(id, CategoryOf(id)); }
/// Per-block mask of the host-candidate bits (nullptr when the block has none).
const std::array<u8, kBlockBytes>* CandidateMask(u32 block);
/// Every block that has candidate bits, ascending.
const std::vector<u32>& CandidateBlocks();

// ---- changes and snapshots ----

struct FlagChange {
    u32 id = 0;
    bool value = false;
    Category cat = Category::never_sync;
    u64 seq = 0;
    bool operator==(const FlagChange& o) const {
        return id == o.id && value == o.value && cat == o.cat && seq == o.seq;
    }
};

/// Host's candidate flags: per block the set bit offsets; a listed block's other candidate bits
/// are 0 (state-synced ids follow that too).
struct FlagSnapshot {
    u64 epoch = 0;    ///< host session id (a guest drops its seq cursor when it changes)
    u64 seq = 0;      ///< the last change seq this snapshot includes
    u32 map_id = 0xffffffffu;
    std::map<u32, std::vector<u32>> blocks;  ///< block -> set bits (ascending)
    bool Get(u32 id, bool* known) const;
    bool operator==(const FlagSnapshot& o) const {
        return epoch == o.epoch && seq == o.seq && map_id == o.map_id && blocks == o.blocks;
    }
};

/// Shadow of the host's candidate bits, the diff and the seq counter (pure; the game side feeds
/// it blocks). Not thread-safe by itself.
class HostTracker {
public:
    explicit HostTracker(u64 epoch = 0) : epoch_(epoch) {}
    u64 epoch() const { return epoch_; }
    u64 seq() const { return seq_; }
    bool has_baseline() const { return baseline_; }
    /// The block as the game holds it now. The first time a block is seen, nothing is emitted
    /// unless the baseline is already complete (EndScan ran once) - a block that appears later
    /// (a new map's groups) emits its set candidate bits.
    void ObserveBlock(u32 block, const u8* data, std::vector<FlagChange>* out);
    /// One id read from the game (the hooks' confirmation path).
    void ObserveId(u32 id, bool value, std::vector<FlagChange>* out);
    /// A full scan finished: the baseline is complete.
    void EndScan() { baseline_ = true; }
    FlagSnapshot Snapshot(u32 map_id) const;

private:
    u64 epoch_ = 0, seq_ = 0;
    bool baseline_ = false;
    std::map<u32, std::array<u8, kBlockBytes>> shadow_;  // candidate-masked
};

/// Guest-side cursor: drops changes it already has (same epoch, seq <= last).
class GuestCursor {
public:
    /// Keeps the changes not seen yet (in order), advances the cursor.
    std::vector<FlagChange> Filter(u64 epoch, const std::vector<FlagChange>& in);
    /// A snapshot resets the cursor to its seq (and epoch).
    void Reset(u64 epoch, u64 seq);
    u64 epoch() const { return epoch_; }
    u64 last_seq() const { return last_; }

private:
    u64 epoch_ = 0, last_ = 0;
    bool any_ = false;
};

class FlagStore;

/// Guest-side queue: changes / snapshots in arrival order, de-duplicated by the cursor, applied to
/// a store by policy. Not thread-safe by itself.
class GuestApplier {
public:
    struct Counters {
        u64 received = 0, applied = 0, already = 0, skipped = 0, dropped_seq = 0, failed = 0, npc_cleared = 0;
    };
    void QueueChanges(u64 epoch, const std::vector<FlagChange>& changes);
    void QueueSnapshot(const FlagSnapshot& snap);
    /// What Apply would write for `snap` under `policy`: the (id, value) pairs it hands to the
    /// store, in Apply's order. Pure (no store): the game glue computes it on the link's thread,
    /// so the main thread's Apply only touches those flags instead of every candidate bit.
    struct SnapshotPlan {
        bool valid = false;
        Policy policy;
        std::vector<std::pair<u32, bool>> flags;
    };
    static SnapshotPlan PlanSnapshot(const FlagSnapshot& snap, const Policy& policy);
    /// QueueSnapshot with its plan (used by Apply while the policy is still plan.policy).
    void QueueSnapshot(const FlagSnapshot& snap, SnapshotPlan plan);
    std::size_t Pending() const { return items_.size() + (held_.empty() ? 0 : 1); }
    std::size_t Held() const { return held_.size(); }
    /// Applies everything queued. In load mode 0 nothing is applied (the queue is kept) unless
    /// `allow_mode0`. Returns the number of flags written. `map_id` (load mode 0, when not
    /// kNoMap): boss flags of that map are held back (HoldBossFlagInMode0) and written later - in
    /// load mode 1 (the own pool, invisible to the running events) or after a load into another
    /// map - so a catch-up never fires the map's boss-kill event (banner, music, rewards).
    static constexpr u32 kNoMap = 0;
    std::size_t Apply(const FlagStore& store, const Policy& policy, bool allow_mode0, u32 map_id = kNoMap);
    const Counters& counters() const { return n_; }
    const GuestCursor& cursor() const { return cursor_; }

private:
    bool ApplyOne(const FlagStore& store, const Policy& policy, u32 id, bool value);
    void Trim();
    struct Item {
        bool snapshot = false;
        std::vector<FlagChange> changes;
        FlagSnapshot snap;
        SnapshotPlan plan;
    };
    std::vector<Item> items_;
    std::vector<std::pair<u32, bool>> held_; // boss flags of the map the guest stood in (mode 0)
    GuestCursor cursor_;
    Counters n_;
};

/// A boss flag (boss_defeated) of the map `map_id` (area << 24 | block << 16) the guest stands in:
/// written in its own world (load mode 0) the running boss event would see the kill and play it
/// (PREY SLAUGHTERED, music, the reward branch). Flag ids TAABZnnn name their map (AA, B).
bool HoldBossFlagInMode0(u32 id, u32 map_id);

// Peer input caps (bbport security pass): a "flags" event's changes, a snapshot's blocks, the
// guest's queue of events not applied yet.
constexpr std::size_t kMaxChangesPerEvent = 65536;
constexpr std::size_t kMaxSnapshotBlocks = 65536;
constexpr std::size_t kMaxPendingItems = 1024;

// JSON bodies of the PartyLink EVENTs "flags" and "flag_snapshot".
constexpr const char* kEventFlags = "flags";
constexpr const char* kEventFlagSnapshot = "flag_snapshot";
std::string ChangesToJson(u64 epoch, const std::vector<FlagChange>& changes);
bool ChangesFromJson(const std::string& text, u64* epoch, std::vector<FlagChange>* out, std::string* error);
std::string SnapshotToJson(const FlagSnapshot& s);
bool SnapshotFromJson(const std::string& text, FlagSnapshot* out, std::string* error);

// ---- host-world mirror: the guest's flag overlay (load mode 1) as a copy of the host's world ----
//
// The vanilla join snapshot (packet 0x10) fills only 12 of the ~900 overlay groups (globals 0-1999,
// 5000-5999, 9000-9999 and the host's current map type-1 zones 0,1,4-7 / type-6 zones 0,1); every
// other group of the guest's host-world overlay is zero: the host's lit lamps (7AAB02xx, 1AAB78x0),
// item pickups (5AABZnnn), boss / area gates (2000-4999), other maps. The host sends the raw bytes
// of every mirror group of its own pool ("world_groups": a full image to each new member, then the
// groups that changed); the guest writes them into its overlay buffers (SprjEventFlagMan +0x88,
// key = GroupKey) right after each vanilla snapshot apply (a full copy) and, while it stays, only
// the bits the host changed since (its own runtime bits in the overlay stay). Never the own pool:
// the overlay is discarded on return home.
//
// Mirror groups: global types 0,1,5,6,7 minus blocks 6-8 (6000-8999 stay in the own pool in mode 1)
// and 60009000+ (C3's ledger); map types 1,5,6,7 of the 17 table maps minus type-1 zones 4-6
// (boss-fight state and enemy AI: the vanilla snapshot carries the current map's, a live copy breaks
// the fog / boss scripts) and the chalice maps (29, 40-47). Bits never written: 2000-2099 (session
// flags the guest's own session code keeps), 9020-9026 (chalice slot, set by SetLoadMode(1)) and
// the shop stock flags (ShopStockFlag: the guest's own, from its pool).

struct MemOps;
using GroupBytes = std::array<u8, kBlockBytes>;
bool WorldMirrorKey(int key);
/// Every mirror key, ascending.
const std::vector<int>& WorldMirrorKeys();
/// The bits of a mirror key the guest writes (1 = written; all zero for a key outside the mirror).
const GroupBytes& WorldMirrorMask(int key);
/// The item_lot_picked bits of a mirror key (D4). With instanced items on, the mirror leaves them
/// alone: the guest's overlay shows the guest's OWN pickups (its pool), not the host's.
const GroupBytes& WorldItemMask(int key);
/// Shop stock is per player: the one-time ShopLineupParam rows' stock flags (party_flags.inc rows
/// "shop stock row ...": the Bath Messengers' armor 70009000-70009490, the Dream's starting weapons
/// 72101000-72101011, the bells / chalice 6610-6660). The global T7 zone 9 and the m21_00 T7 zone 1
/// groups are host-world overlay groups in load mode 1, so without this a guest's shop read the
/// host's purchases (sold out, never bought) and its own purchase went into the discarded overlay.
bool ShopStockFlag(u32 id);
/// The shop stock bits of a mirror key (never mirrored: the overlay keeps the guest's own stock).
const GroupBytes& WorldShopMask(int key);
/// Guest, load mode 1: one overlay group against the same group of the own pool, shop stock bits
/// only, set-only both ways (a stock flag only goes 0 -> 1): a purchase the guest made in the
/// host's world goes to its pool (`saved`), its own earlier purchases show in the overlay
/// (`shown`). Bytes outside `mask` are never touched. Pure.
struct ShopMerge {
    std::vector<u32> saved, shown;  ///< flag ids written to the pool / to the overlay
};
ShopMerge MergeShopStock(u32 block, const GroupBytes& mask, GroupBytes* overlay, GroupBytes* pool);
/// D4 (user decision: world items are instanced per player). Any thread; set once at init.
void SetInstancedItems(bool on);
bool InstancedItems();

/// Groups of the host's world: key -> 125 bytes. `full`: an image of every mirror group, absent
/// keys are all zero; otherwise only the groups that changed.
struct WorldImage {
    u64 epoch = 0;
    bool full = false;
    std::map<u32, GroupBytes> groups;
    bool operator==(const WorldImage& o) const { return epoch == o.epoch && full == o.full && groups == o.groups; }
};

/// Host: shadow of the mirror groups of its own pool (1200 x 125 bytes, pool order).
class HostWorldTracker {
public:
    /// The pool as the game holds it now; the changed mirror groups are added to `out` (latest
    /// bytes win). The first call only takes the baseline.
    void Observe(const u8* pool, WorldImage* out);
    bool has_baseline() const { return baseline_; }
    /// Every non-zero mirror group (full = true).
    WorldImage Full(u64 epoch) const;

private:
    bool baseline_ = false;
    std::vector<GroupBytes> shadow_;
};

/// Guest: the host's world image and what was written to the overlay. Not thread-safe by itself.
class GuestWorldMirror {
public:
    /// An image from the host (a full one replaces the whole image; a partial one its groups).
    void Receive(const WorldImage& img);
    bool has_image() const { return have_; }
    std::size_t Dirty() const { return dirty_.size(); }
    struct Result {
        std::size_t groups = 0, bytes = 0, bits = 0;  ///< groups written, bytes and bits that changed
        std::size_t missing = 0;                      ///< mirror groups with data the overlay lacks
        std::vector<std::pair<int, int>> changed;     ///< (key, bits changed) per group written
    };
    /// `overlay`: key -> buffer address (the overlay's current groups). Full: every mirror group
    /// becomes the host's (masked bits). Otherwise only the bits the host changed since the last
    /// apply. False (nothing written) without an image.
    bool Apply(const std::map<u32, u64>& overlay, const MemOps& ops, bool full, Result* r);
    /// The host's bytes of a group (zero when unknown).
    const GroupBytes& Group(int key) const;
    /// D3 (guest actions): the mirror bits of `overlay` (key -> buffer) that differ from what the
    /// host's image last put there - the guest's own changes in the host's world. Keys with a host
    /// change not yet applied are skipped (their difference may be the host's). Each entry is
    /// (flag id, the overlay's value). At most `max` entries.
    std::vector<std::pair<u32, bool>> LocalChanges(const std::map<u32, u64>& overlay, const MemOps& ops,
                                                   std::size_t max) const;
    /// F5 test aid (BB_PARTY_DESYNC_TEST, party_desync.h): flips one byte of the image (not the
    /// overlay) so the next digest differs. Returns the key, -1 without an image.
    int TestCorrupt();
    /// liftfix: while flag `id` is held, Apply leaves its overlay bit as it is (a full copy too); at
    /// the release the overlay's value stands and the image's value counts as applied (no write), so
    /// only a later host change of it goes in. A lift's position flag while this game's own copy of
    /// a ride has not written its end yet (liftrules::MirrorHold).
    void Hold(u32 id, bool on);
    bool Held(u32 id) const { return held_.count(id) != 0; }

private:
    bool have_ = false;
    std::vector<GroupBytes> image_, applied_;
    std::set<int> dirty_;
    std::set<u32> held_;
};

constexpr const char* kEventWorldGroups = "world_groups";
std::string WorldImageToJson(const WorldImage& img);
bool WorldImageFromJson(const std::string& text, WorldImage* out, std::string* error);

// ---- the store (SprjEventFlagMan 0x553b100) over injectable memory access ----

struct MemOps {
    bool (*read)(u64 address, void* out, std::size_t n) = nullptr;
    bool (*write)(u64 address, const void* in, std::size_t n) = nullptr;
    /// The game's SetEventFlag(man, id, on) (main thread); nullptr = write through the tree.
    void (*set_event_flag)(u64 manager, u32 id, int on) = nullptr;
};

/// SprjEventFlagMan field offsets (docs/party/event_flags.md section 2; checked in 0x13cfcc0).
namespace man {
constexpr u64 kBits = 0x1c, kStride = 0x20, kCount = 0x24, kPool = 0x28, kTreeHead = 0x38, kLoadMode = 0x80,
              kOverlayHead = 0x90;
// MSVC std::map node: left +0, parent +8, right +0x10, is_nil +0x19, key +0x20, kind +0x28,
// storage +0x30 (kind 1: u32 pool index; kind 2: buffer pointer). The head's parent is the root.
constexpr u64 kNodeLeft = 0, kNodeParent = 8, kNodeRight = 0x10, kNodeIsNil = 0x19, kNodeKey = 0x20,
              kNodeKind = 0x28, kNodeStorage = 0x30;
} // namespace man

struct BlockRef {
    u32 block = 0;
    int kind = 0;    ///< 1 pooled, 2 pointer (overlay / dynamic)
    u64 data = 0;    ///< address of the block's 125 bytes (0 = absent)
};

class FlagStore {
public:
    FlagStore(u64 manager, const MemOps& ops);
    /// Header plausible: 1000 bits, stride 125, 1200 pool blocks, pool and tree present, mode 0/1.
    bool Valid() const { return valid_; }
    u64 manager() const { return man_; }
    int LoadMode() const { return mode_; }
    u64 Pool() const { return pool_; }
    /// The visible tree (what Get/SetEventFlag see).
    bool FindBlock(u32 block, BlockRef* out) const;
    /// In-order walk of the visible tree (ascending blocks); false when it looked corrupt.
    bool ForEachBlock(const std::function<void(const BlockRef&)>& fn, std::size_t max_nodes = 8192) const;
    bool ReadVisible(u32 id, bool* value) const;
    bool ReadBlockBytes(const BlockRef& r, u8* out) const;
    /// The saved pool (pool + key * 125).
    u64 PoolBlockAddress(int key) const;
    bool ReadOwnSave(u32 id, bool* value) const;
    /// Writes the own save: mode 1 -> the pool directly; mode 0 -> SetEventFlag (or the tree).
    bool WriteOwnSave(u32 id, bool value) const;
    /// Writes through the visible tree (mode 1: the overlay). SetEventFlag when provided.
    bool WriteVisible(u32 id, bool value) const;
    bool WritePoolBit(u32 id, bool value) const;
    /// The host-world overlay (+0x88/+0x90: std::map<u32 group key, u8* buffer>, node key +0x20,
    /// buffer +0x28): key -> buffer address. False when it looked corrupt.
    bool OverlayGroups(std::map<u32, u64>* out, std::size_t max_nodes = 4096) const;
    /// The overlay's identity (map head, root node, size): changes when 0x13BECA0 rebuilds it.
    u64 OverlayIdentity() const;
    const MemOps& ops() const { return ops_; }

private:
    u64 man_ = 0;
    MemOps ops_;
    bool valid_ = false;
    int mode_ = -1;
    u64 pool_ = 0, head_ = 0;
};

// ---- BBPF v1 (tools/party/flag_tool.py) ----

struct BbpfBlock {
    u32 block = 0;
    u8 kind = 1;
    std::array<u8, kBlockBytes> data{};
};
struct BbpfHeader {
    u32 map_id = 0xffffffffu;
    int load_mode = 0;
    int role = -1;
    u64 unix_ms = 0;
    u64 frame = 0;
    std::string label;
};
std::vector<u8> BbpfEncode(const BbpfHeader& h, const std::vector<BbpfBlock>& blocks);
bool BbpfDecode(const std::vector<u8>& bytes, BbpfHeader* h, std::vector<BbpfBlock>* blocks);
bool BbpfWriteFile(const std::string& path, const BbpfHeader& h, const std::vector<BbpfBlock>& blocks);
/// The visible tree / the raw own pool of a store as BBPF blocks.
std::vector<BbpfBlock> CollectVisible(const FlagStore& s);
std::vector<BbpfBlock> CollectPool(const FlagStore& s);

// ---- the game side (gpu/shim/party only; main thread unless noted) ----

enum class Role { None, Host, Guest };

/// Reads the env (BB_PARTY_PROGRESS, BB_PARTY_FLAG_DUMP, ...) and builds the tables. Idempotent.
void Init();
bool Enabled();
/// After coop::HooksInit (bbgpu_patch_image, before the game runs): installs the four
/// byte-verified entry hooks. Returns how many were installed.
int InstallHooks();
/// Host or Guest of the party session (any thread). Host starts capturing.
void SetRole(Role role);
Role CurrentRole();
/// The directory for BB_PARTY_FLAG_DUMP files ("<user dir>/party"); default from the env.
void SetDumpDir(const std::string& dir);
/// Once a frame on the main thread (CoopTick); `s` = ReadGameState(...) of this frame.
void Tick(const GameSnapshot& s);
/// Session boundaries (dumps; guest: the cursor is kept across rejoins of the same host epoch).
void SessionStarted(const char* label);
void SessionEnded();
/// The director glue (party_progress_bridge.cpp), PartyDirector::Tick right after ReadGameState,
/// main thread, `link` may be null: Tick(s); host: the captured changes to every member (EVENT
/// "flags"), the full snapshot to each newly connected / resumed member (EVENT "flag_snapshot",
/// retried until the baseline exists), session boundaries (hosting / first member connected ->
/// "host", last one gone -> ended); guest: entering / leaving the host's world -> "guest" / ended.
void DirectorTick(const GameSnapshot& s, party::PartyLink* link);
/// Guest, any thread: the next enemy census check is logged whatever its result (a host revive in
/// place: the census after it is the desync check, hostcheck; otherwise only the 1st / 20th match).
void CensusLogNext(const char* why);

// Host.
/// C3 hook point (party_items): called on the main thread, outside every progress lock, once for
/// each item_lot_picked flag the host's hooks / diff see go 0 -> 1 (incl. treasure flags
/// 5AABZnnn and the global 5000-5299). nullptr unregisters. The director wires it to
/// coop::OnHostFlagSet once party_items lands; either side can be merged first.
using ItemFlagObserver = void (*)(u32 id);
void SetItemFlagObserver(ItemFlagObserver fn);
/// Called on the game thread for every flag THIS game sets to 1 itself (SetEventFlag 0x13CFCC0,
/// Lua CompleteEvent 0x132AAD0; not our own writes, not a peer's packet 0x0f, not the mirror):
/// split travel tells its own lamp use from another member's. Keep it cheap. nullptr unregisters.
using LocalFlagSetObserver = void (*)(u32 id);
void SetLocalFlagSetObserver(LocalFlagSetObserver fn);
/// One more observer of the same calls (several modules); false when the slots are full.
bool AddLocalFlagSetObserver(LocalFlagSetObserver fn);
/// Whether the SetEventFlag hook that feeds it is installed.
bool LocalFlagSetHooked();
/// Called on the main thread at each steady load (load mode 0 or 1) right before any host flag
/// is applied there (C3's ledger seeding). nullptr unregisters.
using BeforeApplyHook = void (*)();
void SetBeforeApplyHook(BeforeApplyHook fn);
/// Changes captured since the last call (in seq order; item_lot_picked changes are included for
/// logs/diffs but guests never apply them). Any thread.
std::vector<FlagChange> PopHostFlagChanges();
/// The host's candidate flags as of the last scan; false before the first full scan. Any thread.
bool FullSnapshot(FlagSnapshot* out);
u64 HostEpoch();
/// Host-world mirror: the mirror groups that changed since the last call (partial image). Any thread.
WorldImage PopHostWorldChanges();
/// The full image of the host's mirror groups; false before the first scan. Any thread.
bool HostWorldImage(WorldImage* out);

// Guest. Any thread (the link callback): queued, applied by Tick at a safe point.
void ApplyHostFlags(u64 epoch, const std::vector<FlagChange>& changes);
void ApplyHostSnapshot(const FlagSnapshot& snap);
/// Parses the EVENT body and queues it; false (and *error) on a bad body or another name.
/// Guest, the thread of the vanilla packet-0x10 apply (party_b2 K11, right after 0x13BECA0): the
/// host's world image into the freshly built overlay, before the host-world load reads it.
void OnVanillaSnapshotApplied();
bool OnLinkEvent(const std::string& name, const std::string& body, std::string* error = nullptr);
void SetPolicy(const Policy& p);
Policy CurrentPolicy();

// Flag access (main thread; the live game store).
bool ReadFlag(u32 id, bool* value);
bool WriteFlagOwnSave(u32 id, bool value);
/// The own save's value (the pool; in load mode 1 not what the host-world overlay shows).
bool ReadFlagOwnSave(u32 id, bool* value);
/// Guest: the host's value of `id` in its last full snapshot (EVENT flag_snapshot); false when
/// no snapshot arrived or the id's block is not in it (not a candidate category). Any thread.
bool HostSnapshotFlag(u32 id, bool* value);
/// Guest: the host's value of `id` in the host-world mirror image; false when unknown (no image, not
/// a mirror group). Any thread.
bool HostWorldFlag(u32 id, bool* value);
/// Guest, liftfix: the world mirror holds flag `id` back (GuestWorldMirror::Hold): a host change
/// of it waits until the release. A lift's position flag while this game's own copy rides.
void SetMirrorHold(u32 id, bool hold);
/// The flag store's load mode: 0 own world, 1 the host-world overlay, -1 no store.
int FlagLoadMode();
/// Guest, main thread, load mode 1: the flags the guest's own game changed in the host-world
/// overlay (GuestWorldMirror::LocalChanges); false when there is no image / overlay.
bool GuestLocalWorldChanges(std::vector<std::pair<u32, bool>>* out, std::size_t max);

struct Stats {
    u64 hook_hits = 0, hook_candidates = 0, scans = 0, host_changes = 0;
    u64 guest_received = 0, guest_applied = 0, guest_skipped = 0, guest_dropped_seq = 0;
    std::size_t guest_pending = 0;
};
Stats GetStats();

} // namespace coop::progress
