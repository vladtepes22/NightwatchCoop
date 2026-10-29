// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op C2: host-authoritative progress sync of event flags (see party_progress.h and
// docs/party/event_flags.md). The first half is pure (tables, keys, store walker over injectable
// memory ops, policy, diff, JSON, BBPF) and builds without the game (BB_PARTY_PROGRESS_NO_GAME,
// tests/test_party_progress.cpp); the second half is the game glue (hooks, tick, globals).
#include "party_progress.h"
#include "party_death_rules.h"
#include "party_async_io.h"
#include "party_perf.h"
#ifndef BB_PARTY_PROGRESS_NO_GAME
#include "party_director.h"  // PartyPrologueHold (#19)
#endif

#include "../net/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>

namespace coop::progress {

// =============================================================================================
// categories

namespace {

struct Row {
    u32 first, last;
    Category cat;
    const char* note;
};

const Row kRows[] = {
#include "party_flags.inc"
};
constexpr std::size_t kRowCount = sizeof kRows / sizeof kRows[0];

const char* const kCategoryNames[kCategoryCount] = {
    "never_sync", "boss_defeated", "boss_area",  "lamp_unlocked", "shortcut_door",  "npc_quest",
    "item_lot_picked", "key_event", "cutscene_seen", "world_state", "session_runtime", "time_of_day",
    "covenant_outcome",
};

// First-match table -> disjoint sorted ranges: sweep the row boundaries; on each elementary
// interval the lowest row index among the covering rows wins. `base`: without the covenant_outcome
// rows (BaseCategoryOf).
std::vector<CategoryRange> BuildRanges(bool base = false) {
    std::vector<std::pair<u64, int>> events;  // (position, +row+1 open / -(row+1) close)
    events.reserve(kRowCount * 2);
    for (std::size_t i = 0; i < kRowCount; ++i) {
        if (base && kRows[i].cat == Category::covenant_outcome) {
            continue;
        }
        events.emplace_back(kRows[i].first, int(i) + 1);
        events.emplace_back(u64(kRows[i].last) + 1, -(int(i) + 1));
    }
    std::sort(events.begin(), events.end());
    std::set<int> active;
    std::vector<CategoryRange> out;
    std::size_t k = 0;
    while (k < events.size()) {
        const u64 pos = events[k].first;
        for (; k < events.size() && events[k].first == pos; ++k) {
            if (events[k].second > 0) {
                active.insert(events[k].second - 1);
            } else {
                active.erase(-events[k].second - 1);
            }
        }
        if (active.empty() || k >= events.size()) {
            continue;
        }
        const u64 end = events[k].first;  // exclusive
        const Category c = kRows[*active.begin()].cat;
        if (!out.empty() && out.back().cat == c && u64(out.back().last) + 1 == pos) {
            out.back().last = u32(end - 1);
        } else {
            out.push_back({u32(pos), u32(end - 1), c});
        }
    }
    return out;
}

} // namespace

const char* CategoryName(Category c) {
    const int i = static_cast<int>(c);
    return i >= 0 && i < kCategoryCount ? kCategoryNames[i] : "?";
}

bool CategoryFromName(const std::string& name, Category* out) {
    for (int i = 0; i < kCategoryCount; ++i) {
        if (name == kCategoryNames[i]) {
            *out = static_cast<Category>(i);
            return true;
        }
    }
    return false;
}

const std::vector<CategoryRange>& CategoryRanges() {
    static const std::vector<CategoryRange> ranges = BuildRanges();
    return ranges;
}

namespace {
Category Lookup(const std::vector<CategoryRange>& r, u32 id) {
    auto it = std::upper_bound(r.begin(), r.end(), id, [](u32 v, const CategoryRange& x) { return v < x.first; });
    if (it == r.begin()) {
        return Category::never_sync;
    }
    --it;
    return id <= it->last ? it->cat : Category::never_sync;
}
std::atomic<bool> g_covenant_per_player{true};
} // namespace

Category CategoryOf(u32 id) {
    return Lookup(CategoryRanges(), id);
}

Category BaseCategoryOf(u32 id) {
    static const std::vector<CategoryRange> base = BuildRanges(true);
    return Lookup(base, id);
}

void SetCovenantPerPlayer(bool on) {
    g_covenant_per_player.store(on, std::memory_order_relaxed);
}

bool CovenantPerPlayer() {
    return g_covenant_per_player.load(std::memory_order_relaxed);
}

Category CategoryOfLinear(u32 id) {
    for (const Row& r : kRows) {
        if (r.first <= id && id <= r.last) {
            return r.cat;
        }
    }
    return Category::never_sync;
}

// =============================================================================================
// group keys (resolve_group_key 0x13bc710; tables at 0x4730bf0, 0x4730c10, 0x47314d0, read from
// the 1.09 eboot)

namespace {
constexpr int kGlobalTypes[5] = {0, 1, 5, 6, 7};
constexpr int kMapTypes[5] = {1, 5, 6, 7, 9};
constexpr int kMaps[17][2] = {{21, 0}, {21, 1}, {22, 0}, {23, 0}, {24, 0}, {24, 1}, {24, 2}, {25, 0}, {26, 0},
                              {27, 0}, {28, 0}, {29, 0}, {32, 0}, {33, 0}, {34, 0}, {35, 0}, {36, 0}};
} // namespace

int GroupKey(int type, int area, int block, int zone) {
    const u32 z = static_cast<u32>(zone);
    if (block == 0 && area == 0) {
        for (int i = 0; i < 5; ++i) {
            if (kGlobalTypes[i] == type) {
                return z < 10 ? i + int(z) * 5 : -1;
            }
        }
        return -1;
    }
    if (area == 99 && block == 0) {
        int idx = -1;  // the game's quirk: an unknown type yields -1 + 0x4b0 + zone * 5
        for (int i = 0; i < 5; ++i) {
            if (kMapTypes[i] == type) {
                idx = i;
                break;
            }
        }
        return idx + 0x4b0 + int(z) * 5;
    }
    int map = -1;
    for (int i = 0; i < 17; ++i) {
        if (kMaps[i][0] == area && kMaps[i][1] == block) {
            map = i;
            break;
        }
    }
    if (area == 40) {
        map = 11;
    } else if (u32(area - 41) < 7) {
        map = area - 24;
    }
    int t = -1;
    for (int i = 0; i < 5; ++i) {
        if (kMapTypes[i] == type) {
            t = i;
            break;
        }
    }
    const int tk = (t >= 0 && z < 10) ? int(z) * 115 + 50 + 23 * t : -1;
    if (map < 23 && map >= 0 && tk >= 0) {
        return tk + map;
    }
    return -1;
}

int PoolKeyOfBlock(u32 block) {
    if (block >= 100000) {
        return -1;
    }
    const int type = int(block / 10000), area = int(block / 100 % 100), mblock = int(block / 10 % 10),
              zone = int(block % 10);
    if (area == 99) {
        return -1;
    }
    const int key = GroupKey(type, area, mblock, zone);
    return key >= 0 && key < int(kPoolBlocks) ? key : -1;
}

namespace {
const std::array<u32, kPoolBlocks>& InverseKeys() {
    static const std::array<u32, kPoolBlocks> inv = [] {
        std::array<u32, kPoolBlocks> a;
        a.fill(0xffffffffu);
        auto put = [&](u32 block) {
            const int k = PoolKeyOfBlock(block);
            if (k >= 0 && a[k] == 0xffffffffu) {
                a[k] = block;
            }
        };
        for (int t : kGlobalTypes) {
            for (u32 z = 0; z < 10; ++z) {
                put(u32(t) * 10000 + z);
            }
        }
        for (int t : kMapTypes) {
            for (const auto& m : kMaps) {
                for (u32 z = 0; z < 10; ++z) {
                    put(u32(t) * 10000 + u32(m[0]) * 100 + u32(m[1]) * 10 + z);
                }
            }
            for (u32 area = 40; area <= 46; ++area) {
                for (u32 z = 0; z < 10; ++z) {
                    put(u32(t) * 10000 + area * 100 + z);
                }
            }
        }
        return a;
    }();
    return inv;
}
} // namespace

u32 BlockOfPoolKey(int key) {
    return key >= 0 && key < int(kPoolBlocks) ? InverseKeys()[key] : 0xffffffffu;
}

bool NeverWritable(u32 id) {
    if (id >= 9020 && id <= 9026) {
        return true;
    }
    const u32 area = id / 100000 % 100;
    if (id >= 10000 && (area == 29 || (area >= 40 && area <= 47) || area == 99)) {
        return true;
    }
    return PoolKeyOfId(id) < 0;
}

// =============================================================================================
// policy

namespace {

struct IdRange {
    u32 first, last;
};

// Ids whose current state follows the host (both 0 and 1): elevators and gates the game itself
// moves back and forth. From the EMEVD scan (flag_tool.py emevd: SetEventFlag state 0 / toggle on
// shortcut_door / world_state ids outside constructors), plus the elevator events 12400147/148
// named in the design doc.
constexpr IdRange kStateIds[] = {
    {12200125, 12200126}, {12400147, 12400148}, {12400150, 12400151}, {12400157, 12400157},
    {12400167, 12400169}, {12400177, 12400178}, {12400827, 12400828}, {12410350, 12410351},
    {12420154, 12420155}, {12500074, 12500074}, {12601250, 12601256}, {12700134, 12700135},
    {12700144, 12700145}, {12700173, 12700174}, {12800610, 12800611}, {13300115, 13300116},
    {13501106, 13501107}, {13501116, 13501117}, {13501126, 13501127}, {13601106, 13601107},
    {62411300, 62411300},
};

// Candidate-category ids that are never synced: the endings (player-owned), flags the game
// clears every load / randomises / counts (value ops: partial sync corrupts them), and boss-fight
// state that sits in a boss_defeated row.
constexpr IdRange kExcluded[] = {
    {21, 23},             // endings A/B/C reached (never_sync rows too; kept as a second guard)
    {2100, 2100},         // area gate flags (0x47304B0 / 0x47301B0): never_sync rows; second guard
    {2410, 2410},
    {9800, 9802},         // time of day: C4 cutscene replay only
    {72100130, 72100131}, // Gehrman's offer -> A ending
    {6000, 8999},         // personal blocks 6-8 (own pool even in load mode 1; item-lot rows there too)
    {9180, 9180},         // first-death cutscene flag, cleared by the preconstructor (common 50)
    {12100000, 12100000}, // ending events (m21)
    {12100002, 12100002},
    {12100122, 12100122}, // Doll key-item check (runtime)
    {12100180, 12100180}, // ending event (m21)
    {12100500, 12100510}, // Doll position (random)
    {12100520, 12100524}, // Doll random talk
    {12104809, 12104809}, // Gehrman fight state (cleared on defeat)
    {12300751, 12300751}, // talk suppression
    {12400733, 12400734}, // value counter (shelter residents)
    {12401000, 12401000}, // warp marker
    {12410490, 12410491}, // hanging corpse runtime
    {12410646, 12410649}, // value counter (fake doctor residents)
    {12500250, 12500279}, // Cainhurst church marks / effect timers
    {12600080, 12600081}, // giant idle (random)
    {13200101, 13200101}, // Patches "went behind" check
    {13500947, 13500947}, // Lady Maria fight state
};

template <std::size_t N>
bool InRanges(const IdRange (&r)[N], u32 id) {
    for (const IdRange& x : r) {
        if (x.first <= id && id <= x.last) {
            return true;
        }
    }
    return false;
}

bool CandidateCategory(Category c) {
    switch (c) {
    case Category::boss_defeated:
    case Category::lamp_unlocked:
    case Category::shortcut_door:
    case Category::key_event:
    case Category::cutscene_seen:
    case Category::world_state:
    case Category::item_lot_picked:
    case Category::npc_quest:
    case Category::covenant_outcome:  // tracked as its base category (HostCandidate); the guest decides
        return true;
    default:
        return false;
    }
}

bool EnvOn(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
        return fallback;
    }
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' ||
             ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F')));
}

} // namespace

Policy Policy::Defaults() {
    Policy p;
    p.mode.fill(SyncMode::Off);
    for (Category c : {Category::boss_defeated, Category::lamp_unlocked, Category::shortcut_door,
                       Category::key_event, Category::cutscene_seen, Category::world_state}) {
        p.mode[static_cast<int>(c)] = SyncMode::SetOnly;
    }
    return p;
}

Policy Policy::FromEnv() {
    Policy p = Defaults();
    // NPC quest choices are shared (user decision 2026-10-11): on unless BB_PARTY_PROGRESS_NPC=0.
    if (EnvOn("BB_PARTY_PROGRESS_NPC", true)) {
        p.mode[static_cast<int>(Category::npc_quest)] = SyncMode::Follow;
    }
    p.mirror_overlay = EnvOn("BB_PARTY_PROGRESS_MIRROR", false);
    return p;
}

int NpcStateGroup(u32 id) {
    return id >= 1000 && id <= 1999 ? int((id - 1000) / 20) : -1;
}

bool HostCandidate(u32 id, Category cat) {
    if (cat == Category::covenant_outcome) {
        // The host keeps tracking these (its own snapshot / changes carry them, so a guest of a host
        // with the rule off still gets them); the guest's policy drops them while the rule is on.
        cat = BaseCategoryOf(id);
        if (cat == Category::covenant_outcome) {
            return false;
        }
    }
    return CandidateCategory(cat) && !InRanges(kExcluded, id) && !NeverWritable(id);
}

SyncMode ModeFor(const Policy& p, u32 id, Category cat) {
    if (cat == Category::covenant_outcome) {
        // 0.7.3: covenant outcomes stay in each player's own save while the host's rule is on.
        if (p.covenant_per_player) {
            return SyncMode::Off;
        }
        cat = BaseCategoryOf(id);
    }
    if (!HostCandidate(id, cat)) {
        return SyncMode::Off;
    }
    if (cat == Category::item_lot_picked) {
        return SyncMode::Off;  // C3 (party_items) replays item pickups; C2 never writes them
    }
    const SyncMode m = p.mode[static_cast<int>(cat)];
    if (m == SyncMode::Off) {
        return m;
    }
    return InRanges(kStateIds, id) ? SyncMode::State : m;
}

namespace {
struct MaskTable {
    std::map<u32, std::array<u8, kBlockBytes>> masks;
    std::vector<u32> blocks;
};
const MaskTable& Masks() {
    static const MaskTable t = [] {
        MaskTable m;
        for (int key = 0; key < int(kPoolBlocks); ++key) {
            const u32 block = BlockOfPoolKey(key);
            if (block == 0xffffffffu) {
                continue;
            }
            std::array<u8, kBlockBytes> mask{};
            bool any = false;
            const u32 base = block * kBitsPerBlock;
            for (const CategoryRange& r : CategoryRanges()) {
                if (r.last < base || r.first >= base + kBitsPerBlock || !CandidateCategory(r.cat)) {
                    continue;
                }
                const u32 lo = std::max(r.first, base), hi = std::min(r.last, base + kBitsPerBlock - 1);
                for (u32 id = lo; id <= hi; ++id) {
                    if (HostCandidate(id, r.cat)) {
                        PutBit(mask.data(), id - base, true);
                        any = true;
                    }
                }
            }
            if (any) {
                m.masks[block] = mask;
                m.blocks.push_back(block);
            }
        }
        std::sort(m.blocks.begin(), m.blocks.end());
        return m;
    }();
    return t;
}
} // namespace

const std::array<u8, kBlockBytes>* CandidateMask(u32 block) {
    const auto& m = Masks().masks;
    const auto it = m.find(block);
    return it == m.end() ? nullptr : &it->second;
}

const std::vector<u32>& CandidateBlocks() {
    return Masks().blocks;
}

// =============================================================================================
// host tracker, guest cursor, snapshot

bool FlagSnapshot::Get(u32 id, bool* known) const {
    const auto it = blocks.find(id / kBitsPerBlock);
    if (it == blocks.end()) {
        if (known) {
            *known = false;
        }
        return false;
    }
    if (known) {
        *known = true;
    }
    return std::binary_search(it->second.begin(), it->second.end(), id % kBitsPerBlock);
}

void HostTracker::ObserveBlock(u32 block, const u8* data, std::vector<FlagChange>* out) {
    const auto* mask = CandidateMask(block);
    if (!mask) {
        return;
    }
    std::array<u8, kBlockBytes> now;
    for (u32 i = 0; i < kBlockBytes; ++i) {
        now[i] = static_cast<u8>(data[i] & (*mask)[i]);
    }
    auto it = shadow_.find(block);
    if (it == shadow_.end()) {
        if (baseline_ && out) {
            for (u32 b = 0; b < kBitsPerBlock; ++b) {
                if (GetBit(now.data(), b)) {
                    const u32 id = block * kBitsPerBlock + b;
                    out->push_back({id, true, CategoryOf(id), ++seq_});
                }
            }
        }
        shadow_.emplace(block, now);
        return;
    }
    std::array<u8, kBlockBytes>& old = it->second;
    std::vector<FlagChange> found;  // ascending ids (bytes ascending, bits MSB first)
    for (u32 i = 0; i < kBlockBytes; ++i) {
        const u8 diff = static_cast<u8>(old[i] ^ now[i]);
        for (u32 k = 0; diff && k < 8; ++k) {
            const u32 b = i * 8 + k;
            if ((diff & (0x80u >> k)) && b < kBitsPerBlock) {
                const u32 id = block * kBitsPerBlock + b;
                found.push_back({id, GetBit(now.data(), b), CategoryOf(id), 0});
            }
        }
    }
    old = now;
    if (out) {
        for (FlagChange& c : found) {
            c.seq = ++seq_;
            out->push_back(c);
        }
    }
}

void HostTracker::ObserveId(u32 id, bool value, std::vector<FlagChange>* out) {
    const u32 block = id / kBitsPerBlock, bit = id % kBitsPerBlock;
    const auto* mask = CandidateMask(block);
    if (!mask || !GetBit(mask->data(), bit)) {
        return;
    }
    auto it = shadow_.find(block);
    if (it == shadow_.end()) {
        return;  // the next scan takes the block
    }
    if (GetBit(it->second.data(), bit) == value) {
        return;
    }
    PutBit(it->second.data(), bit, value);
    if (out) {
        out->push_back({id, value, CategoryOf(id), ++seq_});
    }
}

FlagSnapshot HostTracker::Snapshot(u32 map_id) const {
    FlagSnapshot s;
    s.epoch = epoch_;
    s.seq = seq_;
    s.map_id = map_id;
    for (const auto& [block, data] : shadow_) {
        std::vector<u32>& bits = s.blocks[block];
        for (u32 b = 0; b < kBitsPerBlock; ++b) {
            if (GetBit(data.data(), b)) {
                bits.push_back(b);
            }
        }
    }
    return s;
}

std::vector<FlagChange> GuestCursor::Filter(u64 epoch, const std::vector<FlagChange>& in) {
    if (!any_ || epoch != epoch_) {
        epoch_ = epoch;
        last_ = 0;
        any_ = true;
    }
    std::vector<FlagChange> out;
    for (const FlagChange& c : in) {
        if (c.seq > last_) {
            out.push_back(c);
            last_ = c.seq;
        }
    }
    return out;
}

void GuestCursor::Reset(u64 epoch, u64 seq) {
    epoch_ = epoch;
    last_ = seq;
    any_ = true;
}

// =============================================================================================
// JSON

std::string ChangesToJson(u64 epoch, const std::vector<FlagChange>& changes) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(epoch));
    json::Value arr = json::Value::make_array();
    for (const FlagChange& c : changes) {
        json::Value e = json::Value::make_array();
        e.push(static_cast<unsigned>(c.id));
        e.push(c.value ? 1 : 0);
        e.push(CategoryName(c.cat));
        e.push(static_cast<unsigned long long>(c.seq));
        arr.push(std::move(e));
    }
    v.set("c", std::move(arr));
    return json::dump(v, 0);
}

bool ChangesFromJson(const std::string& text, u64* epoch, std::vector<FlagChange>* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (json::u32(v, "v") != 1) {
            throw std::runtime_error("unsupported version");
        }
        *epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        out->clear();
        if (json::arr(v, "c").size() > kMaxChangesPerEvent) {
            throw std::runtime_error("too many changes");
        }
        for (const json::Value& e : json::arr(v, "c")) {
            if (e.type != json::Value::Type::Array || e.array.size() != 4) {
                throw std::runtime_error("bad change entry");
            }
            FlagChange c;
            const u64 id = json::as_u64(e.array[0], "id");
            if (id > 0xffffffffull) {
                throw std::runtime_error("id out of range");
            }
            c.id = static_cast<u32>(id);
            c.value = json::as_u64(e.array[1], "value") != 0;
            if (e.array[2].type != json::Value::Type::String || !CategoryFromName(e.array[2].string, &c.cat)) {
                throw std::runtime_error("bad category");
            }
            c.seq = json::as_u64(e.array[3], "seq");
            out->push_back(c);
        }
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
    return true;
}

std::string SnapshotToJson(const FlagSnapshot& s) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(s.epoch));
    v.set("seq", static_cast<unsigned long long>(s.seq));
    v.set("map", static_cast<unsigned>(s.map_id));
    json::Value blocks = json::Value::make_array();
    for (const auto& [block, bits] : s.blocks) {
        json::Value b = json::Value::make_array();  // [block, [set bits...]]
        b.push(static_cast<unsigned>(block));
        json::Value set = json::Value::make_array();
        for (u32 x : bits) {
            set.push(static_cast<unsigned>(x));
        }
        b.push(std::move(set));
        blocks.push(std::move(b));
    }
    v.set("b", std::move(blocks));
    return json::dump(v, 0);
}

bool SnapshotFromJson(const std::string& text, FlagSnapshot* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (json::u32(v, "v") != 1) {
            throw std::runtime_error("unsupported version");
        }
        FlagSnapshot s;
        s.epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        s.seq = json::u64(v, "seq");
        s.map_id = json::u32(v, "map");
        if (json::arr(v, "b").size() > kMaxSnapshotBlocks) {
            throw std::runtime_error("too many blocks");
        }
        for (const json::Value& b : json::arr(v, "b")) {
            if (b.type != json::Value::Type::Array || b.array.size() != 2 ||
                b.array[1].type != json::Value::Type::Array) {
                throw std::runtime_error("bad block entry");
            }
            const u64 block64 = json::as_u64(b.array[0], "block");
            if (block64 > 0xffffffffull || b.array[1].array.size() > kBitsPerBlock) {
                throw std::runtime_error("bad block");
            }
            const u32 block = static_cast<u32>(block64);
            std::vector<u32>& bits = s.blocks[block];
            for (const json::Value& x : b.array[1].array) {
                const u64 bit = json::as_u64(x, "bit");
                if (bit >= kBitsPerBlock) {
                    throw std::runtime_error("bit out of range");
                }
                bits.push_back(static_cast<u32>(bit));
            }
            std::sort(bits.begin(), bits.end());
            bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
        }
        *out = std::move(s);
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
    return true;
}

// =============================================================================================
// the store

namespace {
template <class T>
bool Rd(const MemOps& ops, u64 at, T* v) {
    return at && ops.read && ops.read(at, v, sizeof(T));
}
} // namespace

FlagStore::FlagStore(u64 manager, const MemOps& ops) : man_(manager), ops_(ops) {
    if (!man_ || !ops_.read) {
        return;
    }
    u32 bits = 0, stride = 0, count = 0;
    std::int32_t mode = -1;
    if (!Rd(ops_, man_ + man::kBits, &bits) || !Rd(ops_, man_ + man::kStride, &stride) ||
        !Rd(ops_, man_ + man::kCount, &count) || !Rd(ops_, man_ + man::kPool, &pool_) ||
        !Rd(ops_, man_ + man::kTreeHead, &head_) || !Rd(ops_, man_ + man::kLoadMode, &mode)) {
        return;
    }
    if (bits != kBitsPerBlock || stride != kBlockBytes || count != kPoolBlocks || !pool_ || !head_ ||
        (mode != 0 && mode != 1)) {
        return;
    }
    mode_ = mode;
    valid_ = true;
}

namespace {
struct NodeView {
    u64 left = 0, right = 0;
    u8 is_nil = 1;
    u32 key = 0;
    std::int32_t kind = 0;
    u64 storage = 0;
};
// Two guarded reads per node (links + is_nil, then key / kind / storage of a real node) instead
// of one per field: the host's periodic diff walks every node of the tree on the main thread.
bool ReadNode(const MemOps& ops, u64 n, NodeView* v) {
    static_assert(man::kNodeLeft == 0 && man::kNodeRight == 0x10 && man::kNodeIsNil == 0x19, "node links");
    static_assert(man::kNodeKey == 0x20 && man::kNodeKind == 0x28 && man::kNodeStorage == 0x30, "node payload");
    if (!n || !ops.read) {
        return false;
    }
    u8 links[0x1a];
    if (!ops.read(n, links, sizeof links)) {
        return false;
    }
    std::memcpy(&v->left, links + man::kNodeLeft, 8);
    std::memcpy(&v->right, links + man::kNodeRight, 8);
    v->is_nil = links[man::kNodeIsNil];
    if (v->is_nil) {
        return true;
    }
    u8 payload[0x18];
    if (!ops.read(n + man::kNodeKey, payload, sizeof payload)) {
        return false;
    }
    std::memcpy(&v->key, payload, 4);
    std::memcpy(&v->kind, payload + (man::kNodeKind - man::kNodeKey), 4);
    std::memcpy(&v->storage, payload + (man::kNodeStorage - man::kNodeKey), 8);
    return true;
}
} // namespace

namespace {
BlockRef MakeRef(u64 pool, u32 block, const NodeView& v) {
    BlockRef r;
    r.block = block;
    r.kind = v.kind;
    if (v.kind == 2) {
        r.data = v.storage;
    } else if (v.kind == 1) {
        // as the game: (u32)(stride * index) + pool
        r.data = pool + u64(u32(kBlockBytes * u32(v.storage)));
    }
    return r;
}
} // namespace

bool FlagStore::FindBlock(u32 block, BlockRef* out) const {
    if (!valid_) {
        return false;
    }
    u64 root = 0;
    if (!Rd(ops_, head_ + man::kNodeParent, &root)) {
        return false;
    }
    u64 cand = head_, n = root;
    NodeView cv;
    for (int depth = 0; depth < 96; ++depth) {
        NodeView v;
        if (!ReadNode(ops_, n, &v)) {
            return false;
        }
        if (v.is_nil) {
            break;
        }
        if (block <= v.key) {
            cand = n;
            cv = v;
            n = v.left;
        } else {
            n = v.right;
        }
    }
    if (cand == head_ || cv.key != block) {
        return false;
    }
    *out = MakeRef(pool_, block, cv);
    return out->data != 0;
}

bool FlagStore::ForEachBlock(const std::function<void(const BlockRef&)>& fn, std::size_t max_nodes) const {
    if (!valid_) {
        return false;
    }
    u64 n = 0;
    if (!Rd(ops_, head_ + man::kNodeParent, &n)) {
        return false;
    }
    std::vector<NodeView> stack; // the nodes as read on the way down (each node is read once)
    std::size_t visited = 0;
    bool have_prev = false;
    u32 prev = 0;
    for (;;) {
        for (;;) {
            NodeView v;
            if (!ReadNode(ops_, n, &v)) {
                return false;
            }
            if (v.is_nil) {
                break;
            }
            if (stack.size() > 96) {
                return false;
            }
            stack.push_back(v);
            n = v.left;
        }
        if (stack.empty()) {
            return true;
        }
        const NodeView v = stack.back();
        stack.pop_back();
        if (++visited > max_nodes || (have_prev && v.key <= prev)) {
            return false;
        }
        have_prev = true;
        prev = v.key;
        const BlockRef r = MakeRef(pool_, v.key, v);
        if (r.data) {
            fn(r);
        }
        n = v.right;
    }
}

bool FlagStore::OverlayGroups(std::map<u32, u64>* out, std::size_t max_nodes) const {
    out->clear();
    if (!valid_) {
        return false;
    }
    u64 head = 0, n = 0;
    if (!Rd(ops_, man_ + man::kOverlayHead, &head) || !head) {
        return true;  // no overlay
    }
    if (!Rd(ops_, head + man::kNodeParent, &n)) {
        return false;
    }
    // In-order walk (left +0, right +0x10, is_nil +0x19, key +0x20, buffer +0x28).
    std::vector<std::pair<u32, std::pair<u64, u64>>> stack;  // key, (buffer, right)
    std::size_t visited = 0;
    bool have_prev = false;
    u32 prev = 0;
    for (;;) {
        for (;;) {
            u8 links[0x1a];
            if (!n || !ops_.read(n, links, sizeof links)) {
                return false;
            }
            if (links[man::kNodeIsNil]) {
                break;
            }
            u8 payload[0x10];
            if (stack.size() > 96 || !ops_.read(n + man::kNodeKey, payload, sizeof payload)) {
                return false;
            }
            u32 key = 0;
            u64 buf = 0, left = 0, right = 0;
            std::memcpy(&key, payload, 4);
            std::memcpy(&buf, payload + 8, 8);
            std::memcpy(&left, links + man::kNodeLeft, 8);
            std::memcpy(&right, links + man::kNodeRight, 8);
            stack.push_back({key, {buf, right}});
            n = left;
        }
        if (stack.empty()) {
            return true;
        }
        const auto e = stack.back();
        stack.pop_back();
        if (++visited > max_nodes || (have_prev && e.first <= prev)) {
            out->clear();
            return false;
        }
        have_prev = true;
        prev = e.first;
        if (e.second.first) {
            (*out)[e.first] = e.second.first;
        }
        n = e.second.second;
    }
}

u64 FlagStore::OverlayIdentity() const {
    u64 head = 0, root = 0, leftmost = 0;
    if (!valid_ || !Rd(ops_, man_ + man::kOverlayHead, &head) || !head) {
        return 0;
    }
    if (!Rd(ops_, head + man::kNodeParent, &root) || !root || root == head) {
        return 0;  // empty (before the first snapshot apply)
    }
    Rd(ops_, head + man::kNodeLeft, &leftmost);  // the head's left = the smallest node
    u64 buf = 0;
    if (leftmost && leftmost != head) {
        Rd(ops_, leftmost + man::kNodeKind, &buf);
    }
    return head * 0x9e3779b97f4a7c15ull ^ root * 0xc2b2ae3d27d4eb4full ^ buf;
}

bool FlagStore::ReadBlockBytes(const BlockRef& r, u8* out) const {
    return r.data && ops_.read && ops_.read(r.data, out, kBlockBytes);
}

bool FlagStore::ReadVisible(u32 id, bool* value) const {
    BlockRef r;
    if (!FindBlock(id / kBitsPerBlock, &r)) {
        return false;
    }
    const u32 bit = id % kBitsPerBlock;
    u8 byte = 0;
    if (!Rd(ops_, r.data + (bit >> 3), &byte)) {
        return false;
    }
    *value = (byte & (0x80u >> (bit & 7))) != 0;
    return true;
}

u64 FlagStore::PoolBlockAddress(int key) const {
    if (!valid_ || key < 0 || key >= int(kPoolBlocks)) {
        return 0;
    }
    return pool_ + u64(key) * kBlockBytes;
}

bool FlagStore::ReadOwnSave(u32 id, bool* value) const {
    const u64 at = PoolBlockAddress(PoolKeyOfId(id));
    if (!at) {
        return false;
    }
    const u32 bit = id % kBitsPerBlock;
    u8 byte = 0;
    if (!Rd(ops_, at + (bit >> 3), &byte)) {
        return false;
    }
    *value = (byte & (0x80u >> (bit & 7))) != 0;
    return true;
}

bool FlagStore::WritePoolBit(u32 id, bool value) const {
    if (NeverWritable(id) || !ops_.write) {
        return false;
    }
    const u64 at = PoolBlockAddress(PoolKeyOfId(id));
    if (!at) {
        return false;
    }
    const u32 bit = id % kBitsPerBlock;
    u8 byte = 0;
    if (!Rd(ops_, at + (bit >> 3), &byte)) {
        return false;
    }
    PutBit(&byte, bit & 7, value);
    return ops_.write(at + (bit >> 3), &byte, 1);
}

bool FlagStore::WriteVisible(u32 id, bool value) const {
    if (NeverWritable(id)) {
        return false;
    }
    BlockRef r;
    if (!FindBlock(id / kBitsPerBlock, &r)) {
        return false;  // the game's own setter is a no-op for an absent block too
    }
    if (ops_.set_event_flag) {
        ops_.set_event_flag(man_, id, value ? 1 : 0);
    } else {
        const u32 bit = id % kBitsPerBlock;
        u8 byte = 0;
        if (!ops_.write || !Rd(ops_, r.data + (bit >> 3), &byte)) {
            return false;
        }
        PutBit(&byte, bit & 7, value);
        if (!ops_.write(r.data + (bit >> 3), &byte, 1)) {
            return false;
        }
    }
    bool now = !value;
    return ReadVisible(id, &now) && now == value;
}

bool FlagStore::WriteOwnSave(u32 id, bool value) const {
    if (!valid_ || NeverWritable(id)) {
        return false;
    }
    if (mode_ == 1) {
        return WritePoolBit(id, value);
    }
    BlockRef r;
    if (FindBlock(id / kBitsPerBlock, &r) && r.kind == 1) {
        return WriteVisible(id, value);
    }
    return WritePoolBit(id, value);
}

// =============================================================================================
// guest applier

void GuestApplier::QueueChanges(u64 epoch, const std::vector<FlagChange>& changes) {
    n_.received += changes.size();
    std::vector<FlagChange> fresh = cursor_.Filter(epoch, changes);
    n_.dropped_seq += changes.size() - fresh.size();
    if (!fresh.empty()) {
        Item it;
        it.changes = std::move(fresh);
        items_.push_back(std::move(it));
    }
    Trim();
}

void GuestApplier::QueueSnapshot(const FlagSnapshot& snap) {
    QueueSnapshot(snap, SnapshotPlan{});
}

namespace {
// The (id, value) pairs a snapshot writes under `policy`, in block / bit order.
std::vector<std::pair<u32, bool>> SnapshotWrites(const FlagSnapshot& snap, const Policy& policy) {
    std::vector<std::pair<u32, bool>> out;
    std::array<bool, 50> npc_group{};  // NPC state machines the host has any bit of
    for (const auto& [block, bits] : snap.blocks) {
        for (u32 b : bits) {
            const int g = b < kBitsPerBlock ? NpcStateGroup(block * kBitsPerBlock + b) : -1;
            if (g >= 0) {
                npc_group[std::size_t(g)] = true;
            }
        }
    }
    for (const auto& [block, bits] : snap.blocks) {
        const auto* mask = CandidateMask(block);
        if (!mask) {
            continue;
        }
        std::array<u8, kBlockBytes> set{};
        for (u32 b : bits) {
            if (b < kBitsPerBlock) {
                PutBit(set.data(), b, true);
            }
        }
        for (u32 b = 0; b < kBitsPerBlock; ++b) {
            if (!GetBit(mask->data(), b)) {
                continue;
            }
            const u32 id = block * kBitsPerBlock + b;
            const bool v = GetBit(set.data(), b);
            const SyncMode m = ModeFor(policy, id);
            const int g = NpcStateGroup(id);
            if (m == SyncMode::State || (m != SyncMode::Off && v) ||
                (m == SyncMode::Follow && g >= 0 && npc_group[std::size_t(g)])) {
                out.emplace_back(id, v);
            }
        }
    }
    return out;
}
} // namespace

GuestApplier::SnapshotPlan GuestApplier::PlanSnapshot(const FlagSnapshot& snap, const Policy& policy) {
    SnapshotPlan plan;
    plan.valid = true;
    plan.policy = policy;
    plan.flags = SnapshotWrites(snap, policy);
    return plan;
}

void GuestApplier::QueueSnapshot(const FlagSnapshot& snap, SnapshotPlan plan) {
    cursor_.Reset(snap.epoch, snap.seq);
    // A newer snapshot supersedes a queued one (a host's stream of them must not pile up).
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Item& i) { return i.snapshot; }), items_.end());
    Item it;
    it.snapshot = true;
    it.snap = snap;
    it.plan = std::move(plan);
    items_.push_back(std::move(it));
    Trim();
}

void GuestApplier::Trim() {
    // Bounded while the store cannot take them (mode 0 outside the post-load window).
    if (items_.size() > kMaxPendingItems) {
        n_.dropped_seq += items_.size() - kMaxPendingItems;
        items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(items_.size() - kMaxPendingItems));
    }
}

bool GuestApplier::ApplyOne(const FlagStore& store, const Policy& policy, u32 id, bool value) {
    const SyncMode m = ModeFor(policy, id);
    if (m == SyncMode::Off || (m == SyncMode::SetOnly && !value)) {
        ++n_.skipped;
        return false;
    }
    bool cur = false;
    if (store.ReadOwnSave(id, &cur) && cur == value) {
        ++n_.already;
    } else if (store.WriteOwnSave(id, value)) {
        ++n_.applied;
        if (m == SyncMode::Follow && !value) {
            ++n_.npc_cleared;  // D8: the host's quest step took this state away
        }
    } else {
        ++n_.failed;
        return false;
    }
    if (store.LoadMode() == 1 && policy.mirror_overlay && m == SyncMode::SetOnly) {
        BlockRef r;
        bool vis = false;
        if (store.FindBlock(id / kBitsPerBlock, &r) && r.kind == 2 && store.ReadVisible(id, &vis) && !vis) {
            store.WriteVisible(id, true);
        }
    }
    return true;
}

bool HoldBossFlagInMode0(u32 id, u32 map_id) {
    if (id < 10000000u || CategoryOf(id) != Category::boss_defeated) {
        return false;
    }
    const u32 area = id / 100000 % 100, block = id / 10000 % 10;
    return area == ((map_id >> 24) & 0xff) && block == ((map_id >> 16) & 0xff);
}

std::size_t GuestApplier::Apply(const FlagStore& store, const Policy& policy, bool allow_mode0, u32 map_id) {
    if (!store.Valid() || (items_.empty() && held_.empty()) || (store.LoadMode() == 0 && !allow_mode0)) {
        return 0;
    }
    const u64 before = n_.applied;
    const bool hold = store.LoadMode() == 0 && map_id != kNoMap;
    std::vector<std::pair<u32, bool>> held;
    auto apply = [&](u32 id, bool v) {
        if (hold && HoldBossFlagInMode0(id, map_id)) {
            held.emplace_back(id, v); // silent later (mode 1 pool / another map)
            return;
        }
        ApplyOne(store, policy, id, v);
    };
    const std::vector<std::pair<u32, bool>> again = std::move(held_);
    held_.clear();
    for (const auto& [id, v] : again) {
        apply(id, v);
    }
    for (const Item& it : items_) {
        if (!it.snapshot) {
            for (const FlagChange& c : it.changes) {
                apply(c.id, c.value);
            }
            continue;
        }
        const std::vector<std::pair<u32, bool>> writes =
            it.plan.valid && it.plan.policy == policy ? it.plan.flags : SnapshotWrites(it.snap, policy);
        for (const auto& [id, v] : writes) {
            apply(id, v);
        }
    }
    items_.clear();
    held_ = std::move(held);
    return std::size_t(n_.applied - before);
}

// =============================================================================================
// BBPF v1

namespace {
void PutLe(std::vector<u8>& out, u64 v, int n) {
    for (int i = 0; i < n; ++i) {
        out.push_back(static_cast<u8>(v >> (8 * i)));
    }
}
u64 GetLe(const u8* p, int n) {
    u64 v = 0;
    for (int i = 0; i < n; ++i) {
        v |= u64(p[i]) << (8 * i);
    }
    return v;
}
} // namespace

std::vector<u8> BbpfEncode(const BbpfHeader& h, const std::vector<BbpfBlock>& blocks_in) {
    std::vector<BbpfBlock> blocks = blocks_in;
    std::sort(blocks.begin(), blocks.end(), [](const BbpfBlock& a, const BbpfBlock& b) { return a.block < b.block; });
    std::vector<u8> out;
    out.reserve(64 + blocks.size() * (8 + kBlockBytes));
    out.insert(out.end(), {'B', 'B', 'P', 'F'});
    PutLe(out, 1, 2);
    PutLe(out, 64, 2);
    PutLe(out, kBitsPerBlock, 4);
    PutLe(out, blocks.size(), 4);
    PutLe(out, h.map_id, 4);
    PutLe(out, u32(h.load_mode), 4);
    PutLe(out, u32(h.role), 4);
    PutLe(out, 0, 4);
    PutLe(out, h.unix_ms, 8);
    PutLe(out, h.frame, 8);
    for (std::size_t i = 0; i < 16; ++i) {
        out.push_back(i < h.label.size() ? static_cast<u8>(h.label[i]) : 0);
    }
    for (const BbpfBlock& b : blocks) {
        PutLe(out, b.block, 4);
        PutLe(out, kBlockBytes, 2);
        out.push_back(b.kind);
        out.push_back(0);
        out.insert(out.end(), b.data.begin(), b.data.end());
    }
    return out;
}

bool BbpfDecode(const std::vector<u8>& b, BbpfHeader* h, std::vector<BbpfBlock>* blocks) {
    if (b.size() < 64 || std::memcmp(b.data(), "BBPF", 4) != 0 || GetLe(&b[4], 2) != 1) {
        return false;
    }
    const std::size_t hsz = std::size_t(GetLe(&b[6], 2));
    if (GetLe(&b[8], 4) != kBitsPerBlock || hsz < 64) {
        return false;
    }
    const u32 n = u32(GetLe(&b[0x0c], 4));
    h->map_id = u32(GetLe(&b[0x10], 4));
    h->load_mode = std::int32_t(u32(GetLe(&b[0x14], 4)));
    h->role = std::int32_t(u32(GetLe(&b[0x18], 4)));
    h->unix_ms = GetLe(&b[0x20], 8);
    h->frame = GetLe(&b[0x28], 8);
    h->label.assign(reinterpret_cast<const char*>(&b[0x30]), strnlen(reinterpret_cast<const char*>(&b[0x30]), 16));
    blocks->clear();
    std::size_t pos = hsz;
    for (u32 i = 0; i < n; ++i) {
        if (pos + 8 > b.size()) {
            return false;
        }
        BbpfBlock blk;
        blk.block = u32(GetLe(&b[pos], 4));
        const std::size_t len = std::size_t(GetLe(&b[pos + 4], 2));
        blk.kind = b[pos + 6];
        pos += 8;
        if (len != kBlockBytes || pos + len > b.size()) {
            return false;
        }
        std::memcpy(blk.data.data(), &b[pos], len);
        pos += len;
        blocks->push_back(blk);
    }
    return true;
}

bool BbpfWriteFile(const std::string& path, const BbpfHeader& h, const std::vector<BbpfBlock>& blocks) {
    const std::vector<u8> bytes = BbpfEncode(h, blocks);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

std::vector<BbpfBlock> CollectVisible(const FlagStore& s) {
    std::vector<BbpfBlock> out;
    s.ForEachBlock([&](const BlockRef& r) {
        BbpfBlock b;
        b.block = r.block;
        b.kind = static_cast<u8>(r.kind);
        if (s.ReadBlockBytes(r, b.data.data())) {
            out.push_back(b);
        }
    });
    return out;
}

std::vector<BbpfBlock> CollectPool(const FlagStore& s) {
    std::vector<BbpfBlock> out;
    BlockRef r;
    for (int key = 0; key < int(kPoolBlocks); ++key) {
        const u32 block = BlockOfPoolKey(key);
        if (block == 0xffffffffu) {
            continue;
        }
        r.block = block;
        r.kind = 1;
        r.data = s.PoolBlockAddress(key);
        BbpfBlock b;
        b.block = block;
        b.kind = 1;
        if (s.ReadBlockBytes(r, b.data.data())) {
            out.push_back(b);
        }
    }
    std::sort(out.begin(), out.end(), [](const BbpfBlock& a, const BbpfBlock& b) { return a.block < b.block; });
    return out;
}

// =============================================================================================
// host-world mirror (guest overlay)

namespace {
struct MirrorTables {
    std::vector<int> keys;
    std::vector<GroupBytes> masks;  // per pool key; all zero outside the mirror
    std::vector<GroupBytes> items;  // per pool key: the masked item_lot_picked bits
    std::vector<GroupBytes> cov;    // per pool key: the masked covenant_outcome bits (0.7.7: per player in the overlay)
    std::vector<GroupBytes> shop;   // per pool key: the shop stock bits (never in `masks`)
};
std::atomic<bool> g_instanced_items{false};
const MirrorTables& Mirror() {
    static const MirrorTables t = [] {
        MirrorTables m;
        m.masks.assign(kPoolBlocks, GroupBytes{});
        m.items.assign(kPoolBlocks, GroupBytes{});
        m.cov.assign(kPoolBlocks, GroupBytes{});
        m.shop.assign(kPoolBlocks, GroupBytes{});
        // BB_PARTY_SHOP_STOCK=0: the 0.6.9 behaviour (the host's stock mirrored; a live repro aid).
        const char* ss = std::getenv("BB_PARTY_SHOP_STOCK");
        const bool shop_per_player = !(ss && ss[0] == '0');
        for (int key = 0; key < int(kPoolBlocks); ++key) {
            const u32 block = BlockOfPoolKey(key);
            if (block == 0xffffffffu) {
                continue;
            }
            const u32 type = block / 10000, area = block / 100 % 100, mblock = block / 10 % 10, zone = block % 10;
            const bool global = area == 0 && mblock == 0;
            bool in = false;
            if (global) {
                // 6000-8999 stay in the own pool in mode 1; 60009000+ is C3's ledger.
                in = (type == 0 && (zone < 6 || zone == 9)) || type == 1 || type == 5 || type == 7 ||
                     (type == 6 && zone != 9);
            } else if (area != 29 && !(area >= 40 && area <= 47)) {
                in = (type == 1 && !(zone >= 4 && zone <= 6)) || type == 5 || type == 6 || type == 7;
            }
            if (!in) {
                continue;
            }
            m.keys.push_back(key);
            GroupBytes& mask = m.masks[key];
            mask.fill(0xff);
            const u32 first = block * kBitsPerBlock;
            for (u32 bit = 0; bit < kBitsPerBlock; ++bit) {
                const u32 id = first + bit;
                // Lamp / headstone "used" triggers are per player: mirrored, the host's headstone ran the
                // guest's warp event too (playtest 1 bug 20); so are the talk state flags of that shape.
                if ((id >= 2000 && id <= 2099) || (id >= 9020 && id <= 9026) || deathrules::LampTriggerFlag(id) ||
                    deathrules::TalkStateFlag(id)) {
                    PutBit(mask.data(), bit, false);
                }
                // Shop stock is per player: mirrored, the host's purchase sold the item out on the guest.
                if (shop_per_player && ShopStockFlag(id)) {
                    PutBit(mask.data(), bit, false);
                    PutBit(m.shop[key].data(), bit, true);
                }
            }
            for (u32 bit = kBitsPerBlock; bit < kBlockBytes * 8; ++bit) {
                PutBit(mask.data(), bit, false);  // padding bits of the last byte
            }
            GroupBytes& cov = m.cov[key];
            for (u32 bit = 0; bit < kBitsPerBlock; ++bit) {
                if (GetBit(mask.data(), bit) && CategoryOf(first + bit) == Category::covenant_outcome) {
                    PutBit(cov.data(), bit, true);
                }
            }
            GroupBytes& items = m.items[key];
            for (u32 bit = 0; bit < kBitsPerBlock; ++bit) {
                if (GetBit(mask.data(), bit) && CategoryOf(first + bit) == Category::item_lot_picked) {
                    PutBit(items.data(), bit, true);
                }
            }
        }
        return m;
    }();
    return t;
}
const GroupBytes kZeroGroup{};
bool NonZero(const GroupBytes& g) {
    return std::any_of(g.begin(), g.end(), [](u8 b) { return b != 0; });
}
} // namespace

bool ShopStockFlag(u32 id) {
    // The "shop stock row" rows of party_flags.inc (flag_tool.py gen-inc: every ShopLineupParam
    // eventFlag), as sorted ranges.
    static const std::vector<std::pair<u32, u32>> ranges = [] {
        std::vector<std::pair<u32, u32>> r;
        for (const Row& row : kRows) {
            if (row.note && std::strncmp(row.note, "shop stock row", 14) == 0) {
                r.emplace_back(row.first, row.last);
            }
        }
        std::sort(r.begin(), r.end());
        return r;
    }();
    auto it = std::upper_bound(ranges.begin(), ranges.end(), std::make_pair(id, 0xffffffffu));
    return it != ranges.begin() && id <= std::prev(it)->second;
}

const GroupBytes& WorldShopMask(int key) {
    return key >= 0 && key < int(kPoolBlocks) ? Mirror().shop[key] : kZeroGroup;
}

ShopMerge MergeShopStock(u32 block, const GroupBytes& mask, GroupBytes* overlay, GroupBytes* pool) {
    ShopMerge r;
    for (u32 i = 0; i < kBlockBytes; ++i) {
        const u8 to_pool = u8((*overlay)[i] & ~(*pool)[i] & mask[i]);
        const u8 to_overlay = u8((*pool)[i] & ~(*overlay)[i] & mask[i]);
        for (u32 b = 0; (to_pool | to_overlay) && b < 8; ++b) {
            const u8 m = u8(0x80u >> b);
            if (to_pool & m) {
                r.saved.push_back(block * kBitsPerBlock + i * 8 + b);
            }
            if (to_overlay & m) {
                r.shown.push_back(block * kBitsPerBlock + i * 8 + b);
            }
        }
        (*pool)[i] = u8((*pool)[i] | to_pool);
        (*overlay)[i] = u8((*overlay)[i] | to_overlay);
    }
    return r;
}

bool WorldMirrorKey(int key) {
    const auto& k = Mirror().keys;
    return std::binary_search(k.begin(), k.end(), key);
}

const std::vector<int>& WorldMirrorKeys() {
    return Mirror().keys;
}

const GroupBytes& WorldItemMask(int key) {
    return key >= 0 && key < int(kPoolBlocks) ? Mirror().items[key] : kZeroGroup;
}

void SetInstancedItems(bool on) {
    g_instanced_items.store(on);
}

bool InstancedItems() {
    return g_instanced_items.load();
}

const GroupBytes& WorldCovenantMask(int key) {
    return key >= 0 && key < int(kPoolBlocks) ? Mirror().cov[key] : kZeroGroup;
}

const GroupBytes& WorldMirrorMask(int key) {
    return key >= 0 && key < int(kPoolBlocks) ? Mirror().masks[key] : kZeroGroup;
}

void HostWorldTracker::Observe(const u8* pool, WorldImage* out) {
    if (shadow_.empty()) {
        shadow_.assign(kPoolBlocks, GroupBytes{});
    }
    for (int key : WorldMirrorKeys()) {
        const u8* now = pool + std::size_t(key) * kBlockBytes;
        GroupBytes& sh = shadow_[key];
        if (std::memcmp(sh.data(), now, kBlockBytes) == 0) {
            continue;
        }
        std::memcpy(sh.data(), now, kBlockBytes);
        if (baseline_ && out) {
            out->groups[u32(key)] = sh;
        }
    }
    baseline_ = true;
}

WorldImage HostWorldTracker::Full(u64 epoch) const {
    WorldImage img;
    img.epoch = epoch;
    img.full = true;
    if (shadow_.empty()) {
        return img;
    }
    for (int key : WorldMirrorKeys()) {
        if (NonZero(shadow_[key])) {
            img.groups[u32(key)] = shadow_[key];
        }
    }
    return img;
}

void GuestWorldMirror::Receive(const WorldImage& img) {
    if (image_.empty()) {
        image_.assign(kPoolBlocks, GroupBytes{});
        applied_.assign(kPoolBlocks, GroupBytes{});
    }
    if (img.full) {
        for (int key : WorldMirrorKeys()) {
            const auto it = img.groups.find(u32(key));
            const GroupBytes& g = it == img.groups.end() ? kZeroGroup : it->second;
            if (image_[key] != g) {
                image_[key] = g;
                dirty_.insert(key);
            }
        }
        have_ = true;
        return;
    }
    for (const auto& [key, g] : img.groups) {
        if (WorldMirrorKey(int(key)) && image_[key] != g) {
            image_[key] = g;
            dirty_.insert(int(key));
        }
    }
}

void GuestWorldMirror::Hold(u32 id, bool on) {
    if (on) {
        held_.insert(id);
        return;
    }
    if (held_.erase(id) && have_) {
        // The overlay's value stands (the guest's own copy wrote it); the host's value of the image is
        // taken as applied without a write, so only a later host change goes in. (Applying it here put
        // a stale image's old end over the copy's own: live run 2.)
        const int key = PoolKeyOfId(id);
        if (key >= 0 && WorldMirrorKey(key)) {
            const u32 bit = id % kBitsPerBlock;
            PutBit(applied_[key].data(), bit, GetBit(image_[key].data(), bit));
        }
    }
}

int GuestWorldMirror::TestCorrupt() {
    if (!have_ || image_.empty()) {
        return -1;
    }
    int pick = -1;
    for (int key : WorldMirrorKeys()) {  // a group the host has bits in, else the first mirror key
        if (pick < 0 || NonZero(image_[key])) {
            pick = key;
            if (NonZero(image_[key])) {
                break;
            }
        }
    }
    if (pick >= 0) {
        image_[pick][0] ^= 0x01;
    }
    return pick;
}

const GroupBytes& GuestWorldMirror::Group(int key) const {
    return key >= 0 && key < int(image_.size()) ? image_[key] : kZeroGroup;
}

std::vector<std::pair<u32, bool>> GuestWorldMirror::LocalChanges(const std::map<u32, u64>& overlay,
                                                                const MemOps& ops, std::size_t max) const {
    std::vector<std::pair<u32, bool>> out;
    if (!have_ || !ops.read) {
        return out;
    }
    for (const auto& [key, buf] : overlay) {
        if (!buf || !WorldMirrorKey(int(key)) || dirty_.count(int(key))) {
            continue;
        }
        GroupBytes cur;
        if (!ops.read(buf, cur.data(), kBlockBytes)) {
            continue;
        }
        const GroupBytes& mask = Mirror().masks[key];
        const GroupBytes& host = applied_[key];
        const bool inst = g_instanced_items.load(std::memory_order_relaxed);
        const bool cov_own = CovenantPerPlayer();
        const u32 base = BlockOfPoolKey(int(key)) * kBitsPerBlock;
        for (u32 i = 0; i < kBlockBytes; ++i) {
            const u8 d = u8((cur[i] ^ host[i]) & mask[i] & (inst ? u8(~Mirror().items[key][i]) : u8(0xff)) &
                            (cov_own ? u8(~Mirror().cov[key][i]) : u8(0xff)));
            if (!d) {
                continue;
            }
            for (u32 b = 0; b < 8; ++b) {
                if (d & (0x80u >> b)) {
                    if (out.size() >= max) {
                        return out;
                    }
                    out.emplace_back(base + i * 8 + b, (cur[i] & (0x80u >> b)) != 0);
                }
            }
        }
    }
    return out;
}

bool GuestWorldMirror::Apply(const std::map<u32, u64>& overlay, const MemOps& ops, bool full, Result* r) {
    Result res;
    if (!have_ || !ops.read || !ops.write) {
        if (r) {
            *r = res;
        }
        return false;
    }
    auto one = [&](int key) {
        const auto it = overlay.find(u32(key));
        const GroupBytes& want = image_[key];
        if (it == overlay.end() || !it->second) {
            res.missing += NonZero(want) ? 1 : 0;
            return;
        }
        GroupBytes mask = Mirror().masks[key];
        if (g_instanced_items.load(std::memory_order_relaxed)) {
            for (u32 i = 0; i < kBlockBytes; ++i) {
                mask[i] = u8(mask[i] & ~Mirror().items[key][i]);  // the guest's own pickups (D4)
            }
        }
        if (CovenantPerPlayer()) {
            for (u32 i = 0; i < kBlockBytes; ++i) {
                mask[i] = u8(mask[i] & ~Mirror().cov[key][i]);  // 0.7.7: the guest's own covenant state
            }
        }
        GroupBytes cur;
        if (!ops.read(it->second, cur.data(), kBlockBytes)) {
            return;
        }
        // liftfix: held flags (Hold) keep the overlay's bit and their host change stays pending.
        GroupBytes keep{};
        const u32 base = BlockOfPoolKey(key) * kBitsPerBlock;
        for (const u32 id : held_) {
            if (id >= base && id - base < kBitsPerBlock) {
                PutBit(keep.data(), id - base, true);
            }
        }
        GroupBytes next = cur, applied = want;
        for (u32 i = 0; i < kBlockBytes; ++i) {
            // full: every masked bit becomes the host's; else only the bits the host changed since
            // the last apply (the guest's own runtime bits in the overlay stay).
            const u8 m = u8((full ? mask[i] : u8(mask[i] & (applied_[key][i] ^ want[i]))) & ~keep[i]);
            next[i] = u8((cur[i] & ~m) | (want[i] & m));
            applied[i] = u8((want[i] & ~keep[i]) | (applied_[key][i] & keep[i]));
        }
        applied_[key] = applied;
        if (next == cur) {
            return;
        }
        std::size_t bytes = 0, bits = 0;
        for (u32 i = 0; i < kBlockBytes; ++i) {
            if (const u8 d = u8(next[i] ^ cur[i])) {
                ++bytes;
                bits += std::size_t(__builtin_popcount(d));
            }
        }
        if (ops.write(it->second, next.data(), kBlockBytes)) {
            ++res.groups;
            res.changed.emplace_back(key, int(bits));
            res.bytes += bytes;
            res.bits += bits;
        }
    };
    if (full) {
        for (int key : WorldMirrorKeys()) {
            one(key);
        }
    } else {
        for (int key : dirty_) {
            one(key);
        }
    }
    dirty_.clear();
    if (r) {
        *r = res;
    }
    return true;
}

std::string WorldImageToJson(const WorldImage& img) {
    static const char* hexd = "0123456789abcdef";
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(img.epoch));
    v.set("full", img.full ? 1 : 0);
    json::Value arr = json::Value::make_array();
    for (const auto& [key, g] : img.groups) {
        json::Value e = json::Value::make_array();
        e.push(static_cast<unsigned>(key));
        std::string h(kBlockBytes * 2, '0');
        for (u32 i = 0; i < kBlockBytes; ++i) {
            h[2 * i] = hexd[g[i] >> 4];
            h[2 * i + 1] = hexd[g[i] & 15];
        }
        e.push(h);
        arr.push(std::move(e));
    }
    v.set("g", std::move(arr));
    return json::dump(v, 0);
}

bool WorldImageFromJson(const std::string& text, WorldImage* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    try {
        if (json::u32(v, "v") != 1) {
            throw std::runtime_error("unsupported version");
        }
        WorldImage img;
        img.epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        img.full = json::u32(v, "full") != 0;
        if (json::arr(v, "g").size() > kPoolBlocks) {
            throw std::runtime_error("too many groups");
        }
        for (const json::Value& e : json::arr(v, "g")) {
            if (e.type != json::Value::Type::Array || e.array.size() != 2 ||
                e.array[1].type != json::Value::Type::String || e.array[1].string.size() != kBlockBytes * 2) {
                throw std::runtime_error("bad group entry");
            }
            const u64 key = json::as_u64(e.array[0], "key");
            if (key >= kPoolBlocks || !WorldMirrorKey(int(key))) {
                throw std::runtime_error("not a mirror group");
            }
            GroupBytes g;
            const std::string& h = e.array[1].string;
            for (u32 i = 0; i < kBlockBytes; ++i) {
                const int hi = nib(h[2 * i]), lo = nib(h[2 * i + 1]);
                if (hi < 0 || lo < 0) {
                    throw std::runtime_error("bad hex");
                }
                g[i] = u8(hi << 4 | lo);
            }
            img.groups[u32(key)] = g;
        }
        *out = std::move(img);
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
    return true;
}

} // namespace coop::progress

#ifndef BB_PARTY_PROGRESS_NO_GAME
// =============================================================================================
// the game side

#include "coop_hooks.h"
#include "game_state.h"
#include "party_desync.h"
#include "../net/bbnet_internal.h"

#include <filesystem>
#include <random>

namespace coop::progress {
namespace {

// SprjEventFlagMan singleton slot; hooked functions (our offsets, prologues checked in the 1.09
// eboot with capstone):
constexpr u64 kEventFlagManSlot = 0x553b100;
// SetEventFlag(man, id, on): mov r8d, edx; mov ecx, [rdi+0x1c]  (41 89 d0 / 8b 4f 1c)
constexpr u64 kSetEventFlag = 0x13cfcc0;
// SetEventFlagValue(man, first, bits, v): push rbp; push r15; push r14  (55 / 41 57 / 41 56)
constexpr u64 kSetEventFlagValue = 0x13d0060;
// Lua CompleteEvent = BroadcastSetFlag(L?, id): push rbp; mov rbp, rsp; push r15
constexpr u64 kBroadcastSetFlag = 0x132aad0;
// SprjEmkEventIns::EndOrRestart(this, restart): push rbp; mov rbp, rsp; push r15. Sets flag
// (event id u32 +0x28) + (slot s16 +0x2c) when the sum > 98, set-only, both paths. The design
// doc's 0x16ed100 is not an instruction boundary; this is the function its decompile describes.
constexpr u64 kEmkEndOrRestart = 0x12ed100;
constexpr u64 kEmkEventId = 0x28, kEmkSlot = 0x2c;

constexpr std::size_t kMaxHookIds = 1 << 16;
constexpr u64 kStableFrames = 30;         // world up, no loading, same store for this long
constexpr u64 kMode0Window = 600;         // frames after that in which mode 0 applies
constexpr double kDumpMinGap = 2.0;       // seconds between sync dumps
constexpr std::size_t kScanSlices = 4;  // frames a scan round's block reads are spread over
constexpr u64 kRefWalkEvery = 300;        // host scans between two safety walks of the tree (scan_refs)

using Clock = std::chrono::steady_clock;

void Log(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party progress: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

struct Global {
    std::once_flag init_once;
    std::atomic<bool> enabled{false};
    std::atomic<bool> dump{false};
    std::atomic<int> role{0};  // Role
    std::atomic<bool> capture{false};
    std::atomic<ItemFlagObserver> item_observer{nullptr};
    std::atomic<BeforeApplyHook> before_apply{nullptr};
    thread_local static bool self_write;

    // hooks -> tick (any thread)
    std::mutex hook_mu;
    std::vector<u32> hook_ids;
    std::atomic<bool> hook_overflow{false};
    std::atomic<u64> hook_hits{0}, hook_candidates{0};

    // host state (tracker under mu; the tick and FullSnapshot)
    std::mutex mu;
    HostTracker tracker;
    std::vector<FlagChange> outbox;
    u32 map_id = 0xffffffffu;
    u64 scans = 0, host_changes = 0;

    // guest state (under mu)
    GuestApplier applier;
    Policy policy;

    // host-world mirror (under mu): host shadow + outbox, guest image + the overlay it last filled
    HostWorldTracker world;
    WorldImage world_out;
    GuestWorldMirror mirror;
    u64 overlay_id = 0;          // OverlayIdentity() of the overlay the last full apply wrote
    bool mirror_full_due = false; // a vanilla snapshot apply came before the host's image
    u64 mirror_full_applies = 0, mirror_live_bits = 0;
    std::vector<u8> pool_copy;   // host, main thread: the pool read for the world diff

    // main thread only
    // The candidate blocks' addresses, from the last full walk of the flag tree (ScanRefs): the
    // periodic diff reads just these 267 blocks; the ~1200-node walk runs after every load (the
    // tree is rebuilt there), every kRefWalkEvery scans and whenever a cached read fails. A walk
    // takes its own frame; the blocks are then read and compared in kScanSlices frames.
    std::vector<BlockRef> scan_refs;
    u64 scan_refs_man = 0, scan_refs_pool = 0;
    u64 scans_since_walk = 0;
    bool scan_refs_ok = false;
    bool round_active = false;   // a scan round runs (its slices on consecutive frames)
    std::size_t round_pos = 0;   // next scan_refs index
    std::size_t round_blocks = 0;
    u64 round_at_stable = 0;     // g.stable of the round's last frame (a load in between ends it)
    bool scan_overflow = false;  // the hooks overflowed: a round is due
    u64 frame = 0, stable = 0;
    u64 last_man = 0;
    int last_mode = -2;
    Clock::time_point last_scan{}, last_dump{};
    bool dump_session = false;   // a session boundary dump is due
    bool dump_home = false;      // guest: a session ended; dump once back in the own world (mode 0)
    bool dump_sync = false;      // a sync dump is due
    std::string label = "party";
    std::string dump_dir;
    int scan_ms = 1000;
    std::vector<u32> test_clear;  // BB_PARTY_PROGRESS_TEST_CLEAR: cleared once, first world (own or summoned)
    std::vector<u32> test_set;    // BB_PARTY_PROGRESS_TEST_SET: set once, same place (after the clears)
    std::vector<u32> test_read;
    FlagSnapshot host_snap;       // guest: the host's last full snapshot (late-joiner catch-up)
    bool have_host_snap = false;   // BB_PARTY_PROGRESS_TEST_READ: own-save values logged at each steady load
    bool start_dumped = false;    // BB_PARTY_FLAG_DUMP: one "start" dump at the first steady world
};
thread_local bool Global::self_write = false;

Global& G() {
    static Global g;
    return g;
}

u64 NewEpoch() {
    std::random_device rd;
    const u64 t = u64(std::chrono::system_clock::now().time_since_epoch().count());
    return (u64(rd()) << 32 ^ u64(rd())) ^ t;
}

void CallSetEventFlag(u64 man, u32 id, int on) {
    using Fn = void(BB_COOP_SYSV*)(u64, u32, int);
    const u64 fn = Guest(kSetEventFlag);
    if (!fn) {
        return;
    }
    Global::self_write = true;
    reinterpret_cast<Fn>(fn)(man, id, on);
    Global::self_write = false;
}

void SetFlagThunk(u64 man, u32 id, int on) {
    CallSetEventFlag(man, id, on);
}

MemOps GameOps() {
    MemOps ops;
    ops.read = [](u64 a, void* o, std::size_t n) { return SafeRead(a, o, n); };
    ops.write = [](u64 a, const void* i, std::size_t n) { return SafeWrite(a, i, n); };
    ops.set_event_flag = &SetFlagThunk;
    return ops;
}

u64 Manager() {
    u64 v = 0;
    const u64 at = Guest(kEventFlagManSlot);
    return at && SafeGet(at, &v) ? v : 0;
}

void NoteId(u32 id) {
    Global& g = G();
    g.hook_hits.fetch_add(1, std::memory_order_relaxed);
    if (!g.capture.load(std::memory_order_relaxed) || Global::self_write || !HostCandidate(id)) {
        return;
    }
    g.hook_candidates.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g.hook_mu);
    if (g.hook_ids.size() >= kMaxHookIds) {
        g.hook_overflow = true;  // the next scan catches them
        return;
    }
    g.hook_ids.push_back(id);
}

bool Capturing() {
    return G().capture.load(std::memory_order_relaxed) && !Global::self_write;
}

std::atomic<LocalFlagSetObserver> g_local_set_observer{nullptr};
// Further observers (AddLocalFlagSetObserver): the guest's NPC gift triggers (party_worldact).
constexpr int kExtraLocalSetObservers = 3;
std::atomic<LocalFlagSetObserver> g_local_set_extra[kExtraLocalSetObservers]{};
std::atomic<bool> g_set_flag_hooked{false};

void NoteLocalSet(u32 id) {
    if (Global::self_write) {
        return;
    }
    if (const LocalFlagSetObserver obs = g_local_set_observer.load(std::memory_order_relaxed)) {
        obs(id);
    }
    for (auto& slot : g_local_set_extra) {
        if (const LocalFlagSetObserver obs = slot.load(std::memory_order_relaxed)) {
            obs(id);
        }
    }
}

// Test aid (BB_PARTY_WORLD_WATCH=id,...): every write of a watched flag through SetEventFlag /
// CompleteEvent, with the game return addresses found on the stack (who wrote it); 600 lines at most.
const std::vector<u32>& WatchedWrites() {
    static const std::vector<u32> ids = [] {
        std::vector<u32> v;
        const char* e = std::getenv("BB_PARTY_WORLD_WATCH");
        while (e && *e) {
            char* end = nullptr;
            const unsigned long x = std::strtoul(e, &end, 10);
            if (end == e) {
                break;
            }
            v.push_back(u32(x));
            e = *end ? end + 1 : end;
        }
        return v;
    }();
    return ids;
}
__attribute__((noinline)) void TraceWatchedWrite(const char* via, u64 man, u32 id, u32 on) {
    static std::atomic<int> lines{0};
    if (lines.load(std::memory_order_relaxed) >= 600) {
        return;
    }
    const std::vector<u32>& w = WatchedWrites();
    if (std::find(w.begin(), w.end(), id) == w.end()) {
        return;
    }
    lines.fetch_add(1, std::memory_order_relaxed);
    const u64 base = Guest(0), size = ImageSize();
    char callers[160] = {};
    int n = 0, len = 0;
    const u64* sp = static_cast<const u64*>(__builtin_frame_address(0));
    for (int i = 0; i < 160 && n < 5; ++i) {
        u64 v = 0;
        if (!SafeGet(reinterpret_cast<u64>(sp + i), &v) || v <= base || v >= base + size) {
            continue;
        }
        len += std::snprintf(callers + len, sizeof callers - std::size_t(len), " 0x%llx", static_cast<unsigned long long>(v - base));
        ++n;
    }
    std::printf("[coop] Party progress: watched flag write %u = %u via %s (%s manager); stack:%s\n", id, on, via,
                man == 0 ? "no" : "a", callers);
    std::fflush(stdout);
}

BB_COOP_SYSV void OnSetEventFlag(u64 man, u64 id, u64 on, u64, u64, u64) {
    perf::Scope z(perf::Zone::HookSetFlag);
    if (!WatchedWrites().empty()) {
        TraceWatchedWrite(man == Manager() ? "SetEventFlag" : "SetEventFlag (other manager)", man, u32(id), u32(on));
    }
    if (Capturing() && man == Manager()) {
        NoteId(u32(id));
    }
    if (u32(on) && man == Manager()) {
        NoteLocalSet(u32(id));
    }
}

BB_COOP_SYSV void OnSetEventFlagValue(u64 man, u64 first, u64 bits, u64, u64, u64) {
    perf::Scope z(perf::Zone::HookSetFlagValue);
    if (!Capturing() || man != Manager() || u32(bits) == 0 || u32(bits) > 32) {
        return;
    }
    for (u32 k = 0; k < u32(bits); ++k) {
        NoteId(u32(first) + k);
    }
}

BB_COOP_SYSV void OnBroadcastSetFlag(u64, u64 id, u64, u64, u64, u64) {
    perf::Scope z(perf::Zone::HookBroadcastFlag);
    if (!WatchedWrites().empty()) {
        TraceWatchedWrite("CompleteEvent", 1, u32(id), 1);
    }
    if (std::int32_t(u32(id)) >= 0) {
        NoteLocalSet(u32(id));
    }
    if (Capturing() && std::int32_t(u32(id)) >= 0) {
        NoteId(u32(id));
    }
}

BB_COOP_SYSV void OnEmkEndOrRestart(u64 self, u64, u64, u64, u64, u64) {
    perf::Scope z(perf::Zone::HookEmkEnd);
    if (!Capturing()) {
        return;
    }
    u32 event = 0;
    std::int16_t slot = 0;
    if (!SafeGet(self + kEmkEventId, &event) || !SafeGet(self + kEmkSlot, &slot)) {
        return;
    }
    const std::int32_t id = std::int32_t(event) + slot;
    if ((std::int32_t(event) | slot) >= 0 && id > 0x62) {
        NoteId(u32(id));
    }
}

std::string DefaultDumpDir() {
    std::string base = bbnet::settings().user_dir;
    if (base.empty()) {
        if (const char* u = std::getenv("BB_GPU_USER_DIR")) {
            base = u;
        } else if (const char* u2 = std::getenv("BB_USER_DIR")) {
            base = u2;
        } else {
            const char* d = std::getenv("BB_DATA_DIR");
            base = std::string(d && d[0] ? d : ".") + "/user";
        }
    }
    return base + "/party";
}

void WriteDumps(Global& g, const FlagStore& store, const GameSnapshot& s) {
    perf::Scope z(perf::Zone::ProgressDump);
    if (g.dump_dir.empty()) {
        g.dump_dir = DefaultDumpDir();
    }
    asyncio::Run([dir = g.dump_dir] {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    });
    BbpfHeader h;
    h.map_id = s.map_id;
    h.load_mode = store.LoadMode();
    h.role = s.session_role;
    h.unix_ms = u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    h.frame = g.frame;
    for (int pass = 0; pass < 2; ++pass) {
        h.label = pass ? (g.label + "-pool").substr(0, 16) : g.label.substr(0, 16);
        // The game's memory is read here (main thread); encoding and the file go to the writer.
        std::vector<BbpfBlock> blocks = pass ? CollectPool(store) : CollectVisible(store);
        char name[96];
        std::snprintf(name, sizeof name, "/flags_%s_%llu.bbpf", h.label.c_str(), static_cast<unsigned long long>(g.frame));
        std::string path = g.dump_dir + name;
        asyncio::Run([path = std::move(path), h, blocks = std::move(blocks)] {
            if (BbpfWriteFile(path, h, blocks)) {
                Log("dump %s (%zu blocks, mode %d)", path.c_str(), blocks.size(), h.load_mode);
            } else {
                Log("dump %s failed", path.c_str());
            }
        });
    }
    g.last_dump = Clock::now();
}

void HostTick(Global& g, const FlagStore& store, const GameSnapshot& s, const Clock::time_point now) {
    perf::Scope z(perf::Zone::ProgressScan);
    std::vector<u32> ids;
    {
        std::lock_guard<std::mutex> lk(g.hook_mu);
        ids.swap(g.hook_ids);
    }
    g.scan_overflow = g.hook_overflow.exchange(false) || g.scan_overflow;
    if (g.round_active && (g.stable != g.round_at_stable + 1 || g.stable == kStableFrames)) {
        g.round_active = false; // a load came between: the next round walks again
    }
    // A scan round: (a walk of the tree in its own frame, then) the candidate blocks in
    // kScanSlices consecutive frames, so no frame pays for all of them.
    const bool start_round = !g.round_active &&
                             (g.scan_overflow || g.scans == 0 ||
                              std::chrono::duration<double, std::milli>(now - g.last_scan).count() >= g.scan_ms ||
                              g.stable == kStableFrames); // right after a load
    bool slice = false;
    if (start_round) {
        const bool walk = !g.scan_refs_ok || g.scan_refs_man != store.manager() || g.scan_refs_pool != store.Pool() ||
                          g.stable == kStableFrames || g.scans_since_walk >= kRefWalkEvery;
        if (walk) {
            // The same blocks the walk used to read directly, in the same (ascending) order.
            const auto& cands = CandidateBlocks();
            g.scan_refs.clear();
            g.scan_refs_ok = store.ForEachBlock([&](const BlockRef& r) {
                if (std::binary_search(cands.begin(), cands.end(), r.block)) {
                    g.scan_refs.push_back(r);
                }
            });
            g.scan_refs_man = store.manager();
            g.scan_refs_pool = store.Pool();
            g.scans_since_walk = 0;
        }
        ++g.scans_since_walk;
        g.scan_overflow = false;
        g.round_active = true;
        g.round_pos = 0;
        g.round_blocks = 0;
        slice = !walk;
    } else if (g.round_active) {
        slice = true;
    }
    g.round_at_stable = g.stable;
    bool round_done = false;
    std::vector<FlagChange> changes;
    std::vector<std::pair<u32, std::array<u8, kBlockBytes>>> blocks;
    if (slice) {
        const std::size_t n = g.scan_refs.size();
        const std::size_t chunk = (n + kScanSlices - 1) / kScanSlices;
        const std::size_t to = std::min(n, g.round_pos + chunk);
        blocks.reserve(to - g.round_pos);
        for (std::size_t i = g.round_pos; i < to; ++i) {
            const BlockRef& r = g.scan_refs[i];
            std::array<u8, kBlockBytes> data;
            if (store.ReadBlockBytes(r, data.data())) {
                blocks.emplace_back(r.block, data);
            } else {
                g.scan_refs_ok = false; // the next round walks the tree again
            }
        }
        g.round_pos = to;
        g.round_blocks += blocks.size();
        round_done = to >= n;
    }
    std::vector<std::pair<u32, bool>> values;
    values.reserve(ids.size());
    for (u32 id : ids) {
        bool v = false;
        if (store.ReadVisible(id, &v)) {
            values.emplace_back(id, v);
        }
    }
    std::unique_lock<std::mutex> lk(g.mu);
    const bool baseline = g.tracker.has_baseline();
    for (const auto& [id, v] : values) {
        g.tracker.ObserveId(id, v, &changes);
    }
    for (const auto& [block, data] : blocks) {
        g.tracker.ObserveBlock(block, data.data(), &changes);
    }
    if (round_done) {
        // The host-world mirror: every mirror group of the own pool (one 150 KB read a scan).
        g.pool_copy.resize(std::size_t(kPoolBlocks) * kBlockBytes);
        if (SafeRead(store.Pool(), g.pool_copy.data(), g.pool_copy.size())) {
            const bool had = g.world.has_baseline();
            const std::size_t before = g.world_out.groups.size();
            g.world.Observe(g.pool_copy.data(), &g.world_out);
            if (!had) {
                Log("host world mirror baseline: %zu non-zero of %zu mirror groups",
                    g.world.Full(0).groups.size(), WorldMirrorKeys().size());
            } else if (g.world_out.groups.size() != before) {
                g.dump_sync = true;
            }
        }
        g.tracker.EndScan();
        g.last_scan = now;
        ++g.scans;
        g.round_active = false;
        if (!baseline) {
            Log("host baseline: %zu candidate blocks in the store (epoch %016llx)", g.round_blocks,
                static_cast<unsigned long long>(g.tracker.epoch()));
        }
    }
    g.map_id = s.map_id;
    if (!changes.empty()) {
        g.host_changes += changes.size();
        for (const FlagChange& c : changes) {
            if (c.cat != Category::item_lot_picked) {
                Log("host flag %u = %d (%s) seq %llu", c.id, int(c.value), CategoryName(c.cat),
                    static_cast<unsigned long long>(c.seq));
            }
        }
        g.outbox.insert(g.outbox.end(), changes.begin(), changes.end());
        g.dump_sync = true;
    }
    lk.unlock();
    if (const ItemFlagObserver obs = g.item_observer.load()) {
        for (const FlagChange& c : changes) {
            if (c.cat == Category::item_lot_picked && c.value) {
                obs(c.id);
            }
        }
    }
}

void GuestTick(Global& g, const FlagStore& store, u32 map_id) {
    perf::Scope z(perf::Zone::ProgressGuest);
    const bool mode0_window = g.stable >= kStableFrames && g.stable < kStableFrames + kMode0Window;
    std::lock_guard<std::mutex> lk(g.mu);
    const std::size_t pending = g.applier.Pending();
    if (!pending) {
        return;
    }
    {
        // Playtest 1 #19: nothing of the host's world goes into a prologue world (this guest's, or
        // while the host is in its own prologue); the changes wait in the queue until both are ready.
        static bool held = false;
        const char* why = nullptr;
        if (coop::PartyPrologueHold(&why)) {
            if (!held) {
                held = true;
                Log("guest: %zu host flag change(s) held: %s (#19: no sync into a prologue world)", pending, why);
            }
            return;
        }
        if (held) {
            held = false;
            Log("guest: prologue over: applying the %zu held host flag change(s)", pending);
        }
    }
    g.policy.covenant_per_player = CovenantPerPlayer();  // the host's rule (party glue)
    const auto before = g.applier.counters();
    const std::size_t held_before = g.applier.Held();
    const std::size_t n = g.applier.Apply(store, g.policy, mode0_window, map_id ? map_id : 0xffffffffu);
    if (n == 0 && g.applier.Pending() == pending && g.applier.Held() == held_before) {
        return;  // kept for later (mode 0 outside the post-load window)
    }
    const auto& c = g.applier.counters();
    Log("guest applied %zu flags in load mode %d (NPC state cleared %llu, already %llu, skipped %llu, failed %llu; "
        "cursor %llu; %zu boss flag(s) of this map held back: written silently once summoned or elsewhere)", n,
        store.LoadMode(), static_cast<unsigned long long>(c.npc_cleared - before.npc_cleared),
        static_cast<unsigned long long>(c.already - before.already),
        static_cast<unsigned long long>(c.skipped - before.skipped),
        static_cast<unsigned long long>(c.failed - before.failed),
        static_cast<unsigned long long>(g.applier.cursor().last_seq()), g.applier.Held());
    if (n > 0 && store.LoadMode() == 1) {
        // Summoned: the pool changed but the game may not save for a long time (seamless play).
        RequestGameSave("C2 host flags in the own pool");
    }
    g.dump_sync = true;
}

/// D4, guest: the item_lot_picked bits of the host-world overlay follow the guest's OWN pool (its
/// pickups), not the host's: full (after the overlay was rebuilt / refilled) = the pool's value;
/// live = set-only from the pool (C3 gives while summoned write the pool), so a pickup the guest's
/// game just made in the overlay is never taken back before GuestLocalWorldChanges saw it.
std::size_t GuestItemOverlaySync(const FlagStore& store, const std::map<u32, u64>& overlay, bool full) {
    if (!InstancedItems()) {
        return 0;
    }
    std::size_t bits = 0;
    for (const auto& [key, buf] : overlay) {
        if (!buf || !WorldMirrorKey(int(key))) {
            continue;
        }
        const GroupBytes& im = WorldItemMask(int(key));
        bool any = false;
        for (u8 b : im) {
            any = any || b;
        }
        const u64 pool = store.PoolBlockAddress(int(key));
        GroupBytes cur, own;
        if (!any || !pool || !store.ops().read(buf, cur.data(), kBlockBytes) ||
            !store.ops().read(pool, own.data(), kBlockBytes)) {
            continue;
        }
        GroupBytes next = cur;
        for (u32 i = 0; i < kBlockBytes; ++i) {
            next[i] = full ? u8((cur[i] & ~im[i]) | (own[i] & im[i])) : u8(cur[i] | (own[i] & im[i]));
        }
        if (next != cur && store.ops().write(buf, next.data(), kBlockBytes)) {
            for (u32 i = 0; i < kBlockBytes; ++i) {
                bits += std::size_t(__builtin_popcount(u8(next[i] ^ cur[i])));
            }
        }
    }
    return bits;
}

/// Guest, main thread, load mode 1: shop stock is per player (ShopStockFlag). The shop reads and
/// writes the host-world overlay (global T7 zone 9, m21_00 T7 zone 1), which the mirror leaves
/// alone for these bits: each frame the overlay's stock bits and the own pool's are merged set-only
/// both ways, so the guest's shop shows the guest's own purchases and a purchase made here is saved.
void GuestShopStockTick(const FlagStore& store) {
    static const std::vector<int> keys = [] {
        std::vector<int> k;
        for (int key : WorldMirrorKeys()) {
            const GroupBytes& m = WorldShopMask(key);
            if (std::any_of(m.begin(), m.end(), [](u8 b) { return b != 0; })) {
                k.push_back(key);
            }
        }
        return k;
    }();
    bool saved = false;
    for (int key : keys) {
        const u32 block = BlockOfPoolKey(key);
        BlockRef r;
        const u64 pool = store.PoolBlockAddress(key);
        if (!pool || !store.FindBlock(block, &r) || r.kind != 2 || !r.data || r.data == pool) {
            continue;  // not this map's overlay group (another map's T7 zone 1: not in the tree)
        }
        GroupBytes cur, own;
        if (!store.ops().read(r.data, cur.data(), kBlockBytes) || !store.ops().read(pool, own.data(), kBlockBytes)) {
            continue;
        }
        const ShopMerge m = MergeShopStock(block, WorldShopMask(key), &cur, &own);
        if (!m.saved.empty() && store.ops().write(pool, own.data(), kBlockBytes)) {
            saved = true;
            for (u32 id : m.saved) {
                Log("guest shop stock: purchase %u in the host's world saved to the own save (per player)", id);
            }
        }
        if (!m.shown.empty() && store.ops().write(r.data, cur.data(), kBlockBytes)) {
            std::string list;
            for (u32 id : m.shown) {
                list += (list.empty() ? "" : " ") + std::to_string(id);
            }
            Log("guest shop stock: %zu own purchase(s) shown in the host's world: %s", m.shown.size(), list.c_str());
        }
    }
    if (saved) {
        RequestGameSave("shop purchase in the host's world");
    }
}

/// Guest, main thread, load mode 1 (0.7.7, covenant2077): covenant outcomes are per player (the rule), so the
/// guest's talk with a covenant NPC in the host's world must read the GUEST's own state (Valtr, Alfred,
/// Annalise, their talk blocks), not the host's: a host who joined the League once left the guest a "you
/// are a member" dialog and no join menu. The overlay's covenant bits (WorldCovenantMask, kept out of the
/// mirror) are the own pool's when an overlay appears; what the guest's game changes in them afterwards
/// (its own talk) goes to the own pool, so it is saved with the guest's progress and is not lost at the leave.
void GuestCovenantTick(const FlagStore& store) {
    static std::map<int, std::pair<u64, GroupBytes>> last;  // per pool key: overlay address, the covenant bits we last saw
    static u64 last_identity = 0;
    static const std::vector<int> keys = [] {
        std::vector<int> k;
        for (int key : WorldMirrorKeys()) {
            const GroupBytes& m = WorldCovenantMask(key);
            if (std::any_of(m.begin(), m.end(), [](u8 b) { return b != 0; })) {
                k.push_back(key);
            }
        }
        return k;
    }();
    if (!CovenantPerPlayer()) {
        last.clear();
        last_identity = 0;
        return;
    }
    const u64 ident = store.OverlayIdentity();
    if (ident != last_identity) {
        last.clear();
        last_identity = ident;
    }
    bool saved = false;
    for (int key : keys) {
        const u32 block = BlockOfPoolKey(key);
        BlockRef r;
        const u64 pool = store.PoolBlockAddress(key);
        if (!pool || !store.FindBlock(block, &r) || r.kind != 2 || !r.data || r.data == pool) {
            continue;
        }
        const GroupBytes& cm = WorldCovenantMask(key);
        GroupBytes cur, own;
        if (!store.ops().read(r.data, cur.data(), kBlockBytes) || !store.ops().read(pool, own.data(), kBlockBytes)) {
            continue;
        }
        auto it = last.find(key);
        if (it == last.end() || it->second.first != r.data) {
            GroupBytes next = cur, seen{};
            std::string bits;
            for (u32 i = 0; i < kBlockBytes; ++i) {
                next[i] = u8((cur[i] & ~cm[i]) | (own[i] & cm[i]));
                seen[i] = u8(own[i] & cm[i]);
                for (u32 b = 0; b < 8 && bits.size() < 160; ++b) {
                    if ((next[i] ^ cur[i]) & 0x80u >> b) {
                        bits += (bits.empty() ? "" : " ") + std::to_string(block * kBitsPerBlock + i * 8 + b);
                    }
                }
            }
            if (next != cur && store.ops().write(r.data, next.data(), kBlockBytes)) {
                Log("guest covenant overlay: the host's covenant state replaced by the guest's own in block %u (bits "
                    "changed: %s)", block, bits.c_str());
            }
            last[key] = {r.data, seen};
            continue;
        }
        GroupBytes& seen = it->second.second;
        GroupBytes next_own = own, next_seen = seen;
        std::string ids;
        for (u32 i = 0; i < kBlockBytes; ++i) {
            const u8 d = u8((cur[i] ^ seen[i]) & cm[i]);  // what the guest's game changed
            if (!d) {
                continue;
            }
            next_own[i] = u8((own[i] & ~d) | (cur[i] & d));
            next_seen[i] = u8((seen[i] & ~d) | (cur[i] & d));
            for (u32 b = 0; b < 8 && ids.size() < 160; ++b) {
                if (d & 0x80u >> b) {
                    const u32 id = block * kBitsPerBlock + i * 8 + b;
                    ids += (ids.empty() ? "" : " ") + std::to_string(id) + "=" + ((cur[i] & 0x80u >> b) ? "1" : "0");
                }
            }
        }
        seen = next_seen;
        if (next_own != own && store.ops().write(pool, next_own.data(), kBlockBytes)) {
            saved = true;
            Log("guest covenant overlay: own covenant flag(s) changed in the host's world kept in the own save "
                "(per player): %s", ids.c_str());
        }
    }
    if (saved) {
        RequestGameSave("covenant state in the host's world");
    }
}

const char* MirrorWhy(bool full, u64 id, u64 last) {
    return !full ? "host changes" : id != last ? "new overlay" : "snapshot applied";
}

/// Guest, main thread: the host's world image into the overlay. Full after the overlay was
/// (re)built by the vanilla snapshot apply, else the bits the host changed since. Any load mode
/// (the overlay is only visible in mode 1; at home it is a stale copy nobody reads).
void GuestMirrorTick(Global& g, bool force_full, const char* why) {
    perf::Scope z(perf::Zone::ProgressGuest);
    static const bool on = EnvOn("BB_PARTY_WORLD_MIRROR", true);
    if (!on) {
        return;
    }
    const FlagStore store(Manager(), GameOps());
    if (!store.Valid()) {
        return;
    }
    const u64 id = store.OverlayIdentity();
    if (!id) {
        return;
    }
    std::lock_guard<std::mutex> lk(g.mu);
    const bool full = force_full || g.mirror_full_due || id != g.overlay_id;
    if (full ? !g.mirror.has_image() : g.mirror.Dirty() == 0) {
        if (full && !g.mirror_full_due) {
            g.mirror_full_due = true;
            Log("world mirror: overlay ready (%s) before the host's world image; filled when it arrives",
                why ? why : "rebuilt");
        }
        return;
    }
    std::map<u32, u64> overlay;
    if (!store.OverlayGroups(&overlay) || overlay.empty()) {
        return;
    }
    GuestWorldMirror::Result r;
    const u64 last = g.overlay_id;
    g.mirror.Apply(overlay, store.ops(), full, &r);
    if (const std::size_t ib = GuestItemOverlaySync(store, overlay, full)) {
        Log("world mirror: %zu item pickup bit(s) of the guest's own save put into the overlay (instanced items)", ib);
    }
    g.overlay_id = id;
    g.mirror_full_due = false;
    if (full) {
        ++g.mirror_full_applies;
    } else {
        g.mirror_live_bits += r.bits;
    }
    if (!r.changed.empty()) {
        // Full: what the vanilla overlay lacked; live: what the host changed. Flag blocks (id / 1000).
        std::string list;
        for (const auto& [key, bits] : r.changed) {
            char b[32];
            std::snprintf(b, sizeof b, "%s%u:%d", list.empty() ? "" : " ", BlockOfPoolKey(key), bits);
            list += b;
            if (list.size() > 400) {
                Log("world mirror: blocks changed by the %s (block:bits): %s", full ? "full copy" : "host", list.c_str());
                list.clear();
            }
        }
        if (!list.empty()) {
            Log("world mirror: blocks changed by the %s (block:bits): %s", full ? "full copy" : "host", list.c_str());
        }
    }
    if (full || r.groups) {
        Log("world mirror: %s (%s, load mode %d): %zu of %zu overlay groups written, %zu bits changed%s%s",
            full ? "full copy" : "live", why ? why : MirrorWhy(full, id, last), store.LoadMode(), r.groups,
            overlay.size(), r.bits, r.missing ? ", host groups the overlay lacks: " : "",
            r.missing ? std::to_string(r.missing).c_str() : "");
        g.dump_sync = true;
    }
}

} // namespace

void OnVanillaSnapshotApplied() {
    Global& g = G();
    if (!g.enabled || CurrentRole() != Role::Guest) {
        return;
    }
    GuestMirrorTick(g, true, "right after the vanilla snapshot apply");
}

void Init() {
    Global& g = G();
    std::call_once(g.init_once, [&g] {
        g.enabled = EnvOn("BB_PARTY_PROGRESS", true);
        g.dump = EnvOn("BB_PARTY_FLAG_DUMP", false);
        if (const char* v = std::getenv("BB_PARTY_PROGRESS_SCAN_MS")) {
            g.scan_ms = std::max(100, std::atoi(v));
        }
        g.policy = Policy::FromEnv();
        g.tracker = HostTracker(NewEpoch());
        if (const char* t = std::getenv("BB_PARTY_PROGRESS_TEST_READ"); t && t[0]) {
            for (const char* p = t; *p;) {
                char* end = nullptr;
                const unsigned long id = std::strtoul(p, &end, 10);
                if (end == p) {
                    break;
                }
                if (id) {
                    g.test_read.push_back(u32(id));
                }
                p = *end ? end + 1 : end;
            }
        }
        if (const char* t = std::getenv("BB_PARTY_PROGRESS_TEST_CLEAR"); t && t[0]) {
            for (const char* p = t; *p;) {
                char* end = nullptr;
                const unsigned long id = std::strtoul(p, &end, 10);
                if (end == p) {
                    break;
                }
                if (id && !NeverWritable(u32(id))) {
                    g.test_clear.push_back(u32(id));
                }
                p = *end ? end + 1 : end;
            }
        }
        if (const char* t = std::getenv("BB_PARTY_PROGRESS_TEST_SET"); t && t[0]) {
            for (const char* p = t; *p;) {
                char* end = nullptr;
                const unsigned long id = std::strtoul(p, &end, 10);
                if (end == p) {
                    break;
                }
                if (id && !NeverWritable(u32(id))) {
                    g.test_set.push_back(u32(id));
                }
                p = *end ? end + 1 : end;
            }
        }
        if (EnvOn("BB_PARTY_RESTARTED", false) && (!g.test_clear.empty() || !g.test_set.empty())) {
            // A crash restart (run.bat) must read what the save kept, not re-apply the fixture.
            Log("test clear / set skipped: restarted after a crash");
            g.test_clear.clear();
            g.test_set.clear();
        }
        if (!g.enabled) {
            Log("off (BB_PARTY_PROGRESS=0)");
            return;
        }
        const auto t0 = Clock::now();
        const std::size_t ranges = CategoryRanges().size();
        const std::size_t blocks = CandidateBlocks().size();
        Log("on: %zu category ranges, %zu candidate blocks (%.0f ms); items via %s, npc %s, mirror %s, dump %s",
            ranges, blocks, std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
            "C3",
            g.policy.mode[int(Category::npc_quest)] != SyncMode::Off ? "on" : "off",
            g.policy.mirror_overlay ? "on" : "off", g.dump ? "on" : "off");
    });
}

bool Enabled() {
    Init();
    return G().enabled;
}

int InstallHooks() {
    if (!Enabled()) {
        return 0;
    }
    int n = 0;
    g_set_flag_hooked = HookPrologue(kSetEventFlag, {0x41, 0x89, 0xd0, 0x8b, 0x4f, 0x1c}, &OnSetEventFlag,
                                     "progress: SetEventFlag");
    n += g_set_flag_hooked ? 1 : 0;
    n += HookPrologue(kSetEventFlagValue, {0x55, 0x41, 0x57, 0x41, 0x56}, &OnSetEventFlagValue,
                      "progress: SetEventFlagValue") ? 1 : 0;
    n += HookPrologue(kBroadcastSetFlag, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &OnBroadcastSetFlag,
                      "progress: CompleteEvent (BroadcastSetFlag)") ? 1 : 0;
    n += HookPrologue(kEmkEndOrRestart, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &OnEmkEndOrRestart,
                      "progress: EMEVD EndOrRestart") ? 1 : 0;
    Log("%d of 4 flag hooks installed%s", n, n < 4 ? " (the periodic scan still catches every change)" : "");
    return n;
}

void SetRole(Role role) {
    Init();
    Global& g = G();
    g.role = int(role);
    g.capture = g.enabled && role == Role::Host;
    Log("role %s", role == Role::Host ? "host" : role == Role::Guest ? "guest" : "none");
}

Role CurrentRole() {
    return Role(G().role.load());
}

void SetDumpDir(const std::string& dir) {
    G().dump_dir = dir;
}

void SessionStarted(const char* label) {
    Global& g = G();
    if (label && label[0]) {
        g.label = label;
    }
    g.dump_session = true;
}

void SessionEnded() {
    Global& g = G();
    g.dump_session = true;
    if (CurrentRole() == Role::Guest) {
        g.dump_home = true;  // the game still shows the host's world (mode 1) for a while
    }
}

void Tick(const GameSnapshot& s) {
    Global& g = G();
    if (!g.enabled) {
        return;
    }
    ++g.frame;
    if (CurrentRole() == Role::Guest) {
        GuestMirrorTick(g, false, nullptr);  // also while loading: before the host-world load reads it
    }
    if (!s.world_up || s.loading) {
        g.stable = 0;
        return;
    }
    const u64 m = Manager();
    const FlagStore store(m, GameOps());
    if (!store.Valid()) {
        g.stable = 0;
        return;
    }
    if (m != g.last_man || store.LoadMode() != g.last_mode) {
        g.last_man = m;
        g.last_mode = store.LoadMode();
        g.stable = 0;
        return;
    }
    if (++g.stable < kStableFrames) {
        return;
    }
    const auto now = Clock::now();
    const Role role = CurrentRole();
    if (g.dump && !g.start_dumped) {
        // What the save held when the world came up (before anything is applied): the restart check.
        g.start_dumped = true;
        const std::string keep = g.label;
        g.label = "start";
        WriteDumps(g, store, s);
        g.label = keep;
    }
    if ((!g.test_clear.empty() || !g.test_set.empty()) && (store.LoadMode() == 0 || store.LoadMode() == 1)) {
        // Test fixture (a guest save that lacks some host progress, or holds a stale NPC state):
        // before anything is applied. Load mode 1 too (smokefix 2026-10-16): a guest whose first
        // world is the host's (title join, summoned at once) wrote it only at its first own-world
        // load, so its C3 ledger was seeded with the fixture's reward flag still set (smoke s2: the
        // Cleric Beast's reward "0 new" on the summoned guest, no item box). Mode 1 writes the pool.
        for (const bool on : {false, true}) {
            std::vector<u32>& ids = on ? g.test_set : g.test_clear;
            if (ids.empty()) {
                continue;
            }
            std::string list;
            for (u32 id : ids) {
                bool before = false, after = false;
                store.ReadOwnSave(id, &before);
                store.WriteOwnSave(id, on);
                store.ReadOwnSave(id, &after);
                list += (list.empty() ? "" : ",") + std::to_string(id) + ":" + std::to_string(int(before)) + "->" +
                        std::to_string(int(after));
            }
            Log("test: %s in the own save (%s): %s", on ? "set" : "cleared", CategoryName(CategoryOf(ids.front())),
                list.c_str());
            ids.clear();
        }
        g.dump_session = true;
    }
    if (g.stable == kStableFrames && !g.test_read.empty()) {
        // Test evidence: what the own save holds at this load (mode 1: the pool, not the overlay).
        std::string list;
        for (u32 id : g.test_read) {
            bool own = false, visible = false;
            const bool ok = store.ReadOwnSave(id, &own);
            const bool vok = store.ReadVisible(id, &visible);
            list += (list.empty() ? "" : " ") + std::to_string(id) + "=" + (ok ? std::to_string(int(own)) : "?") +
                    "/" + (vok ? std::to_string(int(visible)) : "?");
        }
        Log("test read (load mode %d, own save / visible): %s", store.LoadMode(), list.c_str());
    }
    if (g.stable == kStableFrames) {
        if (const BeforeApplyHook hook = g.before_apply.load()) {
            // C3 ledger seeding: before the first host flag lands in this load. Load mode 1 too:
            // with seamless play a guest may be summoned from its first load on (own-pool reads).
            hook();
        }
    }
    if (role == Role::Host && store.LoadMode() == 0) {
        HostTick(g, store, s, now);
    } else if (role == Role::Guest) {
        if (store.LoadMode() == 1) {
            GuestShopStockTick(store);
            GuestCovenantTick(store);
        }
        GuestTick(g, store, s.map_id);
    }
    if (g.dump) {
        if (g.stable == kStableFrames && role != Role::None) {
            g.dump_session = true;  // every steady load in a session (host pool vs guest overlay)
        }
        const bool gap = std::chrono::duration<double>(now - g.last_dump).count() >= kDumpMinGap;
        if (g.dump_session || (g.dump_sync && gap)) {
            WriteDumps(g, store, s);
            g.dump_session = g.dump_sync = false;
        }
        if (g.dump_home && store.LoadMode() == 0) {
            // What the guest's own save holds after the session (the "returned home" check).
            const std::string keep = g.label;
            g.label = "home";
            WriteDumps(g, store, s);
            g.label = keep;
            g.dump_home = false;
        }
    } else {
        g.dump_session = g.dump_sync = g.dump_home = false;
    }
}

void SetItemFlagObserver(ItemFlagObserver fn) {
    G().item_observer = fn;
}

bool AddLocalFlagSetObserver(LocalFlagSetObserver fn) {
    for (auto& slot : g_local_set_extra) {
        LocalFlagSetObserver cur = slot.load();
        if (cur == fn) {
            return true;
        }
        if (!cur && slot.compare_exchange_strong(cur, fn)) {
            return true;
        }
    }
    return false;
}

void SetLocalFlagSetObserver(LocalFlagSetObserver fn) {
    g_local_set_observer = fn;
}

bool LocalFlagSetHooked() {
    return g_set_flag_hooked.load();
}

void SetBeforeApplyHook(BeforeApplyHook fn) {
    G().before_apply = fn;
}

std::vector<FlagChange> PopHostFlagChanges() {
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    std::vector<FlagChange> out;
    out.swap(g.outbox);
    return out;
}

bool FullSnapshot(FlagSnapshot* out) {
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.tracker.has_baseline()) {
        return false;
    }
    *out = g.tracker.Snapshot(g.map_id);
    return true;
}

WorldImage PopHostWorldChanges() {
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    WorldImage out;
    out.groups.swap(g.world_out.groups);
    out.epoch = g.tracker.epoch();
    return out;
}

bool HostWorldImage(WorldImage* out) {
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.world.has_baseline()) {
        return false;
    }
    *out = g.world.Full(g.tracker.epoch());
    return true;
}

u64 HostEpoch() {
    Init();
    std::lock_guard<std::mutex> lk(G().mu);
    return G().tracker.epoch();
}

void ApplyHostFlags(u64 epoch, const std::vector<FlagChange>& changes) {
    Init();
    Global& g = G();
    if (!g.enabled) {
        return;
    }
    if (CovenantPerPlayer()) {
        std::string ids;
        for (const FlagChange& c : changes) {
            if (CategoryOf(c.id) == Category::covenant_outcome && ids.size() < 200) {
                ids += (ids.empty() ? "" : " ") + std::to_string(c.id) + "=" + (c.value ? "1" : "0");
            }
        }
        if (!ids.empty()) {
            Log("guest: covenant outcome change(s) of the host kept out of the own save (per player): %s", ids.c_str());
        }
    }
    std::lock_guard<std::mutex> lk(g.mu);
    g.applier.QueueChanges(epoch, changes);
}

void ApplyHostSnapshot(const FlagSnapshot& snap) {
    Init();
    Global& g = G();
    if (!g.enabled) {
        return;
    }
    Policy policy;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.policy.covenant_per_player = CovenantPerPlayer();
        policy = g.policy;
    }
    // The candidate-bit walk (~ms) here on the link's thread, outside g.mu: the main thread's
    // apply then only writes the planned flags.
    GuestApplier::SnapshotPlan plan = GuestApplier::PlanSnapshot(snap, policy);
    if (policy.covenant_per_player) {
        // 0.7.3: what the host's world has of the covenant outcomes stays there (the world mirror
        // shows it while summoned); the own save keeps its own.
        std::size_t n = 0;
        std::string ids;
        for (const auto& [block, bits] : snap.blocks) {
            for (u32 b : bits) {
                const u32 id = block * kBitsPerBlock + b;
                if (b < kBitsPerBlock && CategoryOf(id) == Category::covenant_outcome) {
                    ++n;
                    if (ids.size() < 200) {
                        ids += (ids.empty() ? "" : " ") + std::to_string(id);
                    }
                }
            }
        }
        Log("guest: covenant outcomes per player: %zu covenant flag(s) set in the host's world not copied into the "
            "own save%s%s", n, n ? ": " : "", ids.c_str());
    }
    std::lock_guard<std::mutex> lk(g.mu);
    g.host_snap = snap;
    g.have_host_snap = true;
    g.applier.QueueSnapshot(snap, std::move(plan));
    Log("host snapshot queued: %zu blocks, seq %llu", snap.blocks.size(), static_cast<unsigned long long>(snap.seq));
}

bool OnLinkEvent(const std::string& name, const std::string& body, std::string* error) {
    if (name == kEventFlags) {
        u64 epoch = 0;
        std::vector<FlagChange> changes;
        if (!ChangesFromJson(body, &epoch, &changes, error)) {
            return false;
        }
        desync::GuestOnChanges(epoch, changes);  // F5: the model of the host's flags
        ApplyHostFlags(epoch, changes);
        return true;
    }
    if (name == kEventWorldGroups) {
        WorldImage img;
        if (!WorldImageFromJson(body, &img, error)) {
            return false;
        }
        Init();
        Global& g = G();
        if (!g.enabled) {
            return true;
        }
        std::lock_guard<std::mutex> lk(g.mu);
        g.mirror.Receive(img);
        if (img.full) {
            Log("world mirror: host image received (%zu non-zero groups)", img.groups.size());
        }
        return true;
    }
    if (name == kEventFlagSnapshot) {
        FlagSnapshot snap;
        if (!SnapshotFromJson(body, &snap, error)) {
            return false;
        }
        desync::GuestOnSnapshot(snap);
        ApplyHostSnapshot(snap);
        return true;
    }
    if (name == desync::kDigestEvent) {  // F5: the host's digest; a difference asks for a resync
        Init();
        Global& g = G();
        bool has_world = false;
        desync::Buckets world{};
        {
            std::lock_guard<std::mutex> lk(g.mu);
            has_world = g.mirror.has_image();
            if (has_world) {
                world = desync::WorldDigestOf([&](int key) { return &g.mirror.Group(key); });
            }
        }
        desync::Checker::Result res;
        bool corrupt = false;
        if (!desync::GuestOnDigest(body, has_world, world, &res, &corrupt, error)) {
            return false;
        }
        using V = desync::Checker::Verdict;
        if (res.verdict == V::Asked) {
            Log("desync: the host's digest differs: %s; resync asked", res.detail.c_str());
        } else if (res.verdict == V::Resolved) {
            Log("desync: resolved: %s", res.detail.c_str());
        } else if (res.verdict == V::Persistent) {
            Log("desync: still differs after %d resyncs (%s); no more requests for now", desync::kMaxStrikes,
                res.detail.c_str());
        } else if (res.verdict == V::Mismatch) {
            Log("desync: differs (%s); resync pending", res.detail.c_str());
        } else if (res.verdict == V::Match) {
            static unsigned matches = 0;  // the first, then every 20th (~5 min): proof the check runs
            if (matches++ % 20 == 0 || bbnet::party_trace()) {
                Log("desync: digest %u matches (world %s, flags %s)", matches, has_world ? "checked" : "-",
                    res.detail.empty() ? "checked" : res.detail.c_str());
            }
        }
        if (corrupt) {
            int key = -1;
            {
                std::lock_guard<std::mutex> lk(g.mu);
                key = g.mirror.TestCorrupt();
            }
            Log("desync test: corrupted mirror group %d and forgot one host flag (BB_PARTY_DESYNC_TEST)", key);
        }
        return true;
    }
    if (error) {
        *error = "not a progress event";
    }
    return false;
}

void SetPolicy(const Policy& p) {
    Init();
    std::lock_guard<std::mutex> lk(G().mu);
    G().policy = p;
}

Policy CurrentPolicy() {
    Init();
    std::lock_guard<std::mutex> lk(G().mu);
    return G().policy;
}

bool ReadFlag(u32 id, bool* value) {
    const FlagStore store(Manager(), GameOps());
    return store.Valid() && store.ReadVisible(id, value);
}

bool WriteFlagOwnSave(u32 id, bool value) {
    const FlagStore store(Manager(), GameOps());
    return store.Valid() && store.WriteOwnSave(id, value);
}

bool WriteFlagVisible(u32 id, bool value) {
    const FlagStore store(Manager(), GameOps());
    return store.Valid() && store.WriteVisible(id, value);
}

bool HostSnapshotFlag(u32 id, bool* value) {
    Init();
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.have_host_snap) {
        return false;
    }
    bool known = false;
    *value = g.host_snap.Get(id, &known);
    return known;
}

bool ReadFlagOwnSave(u32 id, bool* value) {
    const FlagStore store(Manager(), GameOps());
    return store.Valid() && store.ReadOwnSave(id, value);
}

bool HostWorldFlag(u32 id, bool* value) {
    const int key = PoolKeyOfId(id);
    if (key < 0 || !WorldMirrorKey(key)) {
        return false;
    }
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.mirror.has_image()) {
        return false;
    }
    *value = GetBit(g.mirror.Group(key).data(), id % kBitsPerBlock);
    return true;
}

void SetMirrorHold(u32 id, bool hold) {
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    g.mirror.Hold(id, hold);
}

int FlagLoadMode() {
    const FlagStore store(Manager(), GameOps());
    return store.Valid() ? store.LoadMode() : -1;
}

bool GuestLocalWorldChanges(std::vector<std::pair<u32, bool>>* out, std::size_t max) {
    const FlagStore store(Manager(), GameOps());
    if (!store.Valid() || store.LoadMode() != 1) {
        return false;
    }
    std::map<u32, u64> overlay;
    if (!store.OverlayGroups(&overlay) || overlay.empty()) {
        return false;
    }
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.mirror.has_image() || g.overlay_id != store.OverlayIdentity()) {
        return false;  // the overlay was rebuilt and not yet filled: every bit would look local
    }
    *out = g.mirror.LocalChanges(overlay, store.ops(), max);
    if (InstancedItems()) {
        // D4: item pickups the guest's game made in the overlay that its own pool lacks.
        for (const auto& [key, buf] : overlay) {
            const GroupBytes& im = WorldItemMask(int(key));
            const u64 pool = store.PoolBlockAddress(int(key));
            GroupBytes cur, own;
            if (!buf || !pool || !WorldMirrorKey(int(key)) || !store.ops().read(buf, cur.data(), kBlockBytes) ||
                !store.ops().read(pool, own.data(), kBlockBytes)) {
                continue;
            }
            const u32 base = BlockOfPoolKey(int(key)) * kBitsPerBlock;
            for (u32 i = 0; i < kBlockBytes && out->size() < max; ++i) {
                const u8 d = u8(cur[i] & ~own[i] & im[i]);
                for (u32 b = 0; d && b < 8; ++b) {
                    if (d & (0x80u >> b)) {
                        out->emplace_back(base + i * 8 + b, true);
                    }
                }
            }
        }
        GuestItemOverlaySync(store, overlay, false);  // C3's summoned gives (pool) show in the overlay
    }
    return true;
}

Stats GetStats() {
    Global& g = G();
    Stats s;
    s.hook_hits = g.hook_hits.load();
    s.hook_candidates = g.hook_candidates.load();
    std::lock_guard<std::mutex> lk(g.mu);
    s.scans = g.scans;
    s.host_changes = g.host_changes;
    const auto& c = g.applier.counters();
    s.guest_received = c.received;
    s.guest_applied = c.applied;
    s.guest_skipped = c.skipped;
    s.guest_dropped_seq = c.dropped_seq;
    s.guest_pending = g.applier.Pending();
    return s;
}

} // namespace coop::progress
#endif // BB_PARTY_PROGRESS_NO_GAME
