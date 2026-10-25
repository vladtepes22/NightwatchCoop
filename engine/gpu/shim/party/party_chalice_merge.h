// SPDX-License-Identifier: GPL-3.0-or-later
// Chalice Dungeon progress that comes home with a guest (0.7.6, docs/party/chalice.md 9).
//
// A summoned guest plays the host's dungeon in the host's world (load mode 1); its own save never
// learns about it. When the guest is back in its own world the party layer adds the dungeon to the
// guest's altar (a free slot 1..6 of the flag store) and sets the lamps / shortcuts the party
// reached in that slot's flag groups. Only the pure parts live here (unit-tested): which slot gets
// the dungeon, and which flag ids carry the progress. The game side is party_chalice.cpp.
//
// Header only, no game or party dependencies.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace coop::chalice {

constexpr int kRecordDwords = 43;                 ///< 1376 bits: ChannelId .. the creator's name
constexpr std::uint32_t kDungeonMapMask = 0xff000000u;
constexpr std::uint32_t kDungeonMapArea = 0x1d000000u;  ///< a slot record's map (+96) in area 29

struct SlotRecord {
    std::array<std::uint32_t, kRecordDwords> w{};   ///< w[1] / w[2] ChannelId lo / hi, w[3] the map
};

/// An empty slot reads {0, lo 0, hi 0x80000000, map 0xffffffff, ...}.
inline bool RecordHoldsDungeon(const SlotRecord& r) {
    return (r.w[3] & kDungeonMapMask) == kDungeonMapArea;
}
/// The record's ChannelId; 0 when it has none (top bit set, the game's "never uploaded").
inline std::uint64_t RecordChannel(const SlotRecord& r) {
    return (r.w[2] & 0x80000000u) ? 0 : (static_cast<std::uint64_t>(r.w[2]) << 32 | r.w[1]);
}

enum class MergeKind {
    None,      ///< nothing to do (not a dungeon, no ChannelId, or no free slot)
    Added,     ///< the dungeon goes into the free slot `slot`
    Existing,  ///< the guest already has this dungeon in `slot`: only its progress is merged
};
struct MergePlan {
    MergeKind kind = MergeKind::None;
    int slot = -1;
    const char* why = "";
};

/// Where `joined` (the host's dungeon, as the quick join wrote it) goes in a guest whose own slot
/// records are `own` (index = slot). Slot 0 is the network slot (the join itself writes it) and is
/// never a target; a slot that holds a dungeon is never overwritten; a dungeon with the same
/// ChannelId is never added twice.
inline MergePlan PlanMerge(const std::array<SlotRecord, 7>& own, const SlotRecord& joined) {
    MergePlan p;
    if (!RecordHoldsDungeon(joined)) {
        p.why = "the joined record holds no dungeon";
        return p;
    }
    const std::uint64_t ch = RecordChannel(joined);
    if (!ch) {
        p.why = "the joined dungeon has no ChannelId";
        return p;
    }
    for (int k = 1; k < 7; ++k) {
        if (RecordHoldsDungeon(own[k]) && RecordChannel(own[k]) == ch) {
            p.kind = MergeKind::Existing;
            p.slot = k;
            p.why = "the guest already has this dungeon";
            return p;
        }
    }
    for (int k = 1; k < 7; ++k) {
        if (!RecordHoldsDungeon(own[k])) {
            p.kind = MergeKind::Added;
            p.slot = k;
            p.why = "free slot";
            return p;
        }
    }
    p.why = "all slots 1..6 hold dungeons";
    return p;
}

// ---- the progress flag ids ----
// A dungeon's flags use area 29 ids (T1 12900000.., the lamps' lit flags T7 72900200..) while it is
// the current slot; slot k's own groups are the same ids with area 40 + k (docs/party/chalice.md 1).
constexpr std::uint32_t kAreaStride = 100000;
constexpr std::uint32_t kDungeonT1First = 12900000u, kDungeonT1Last = 12909999u;  // block 0, zones 0..9
constexpr std::uint32_t kDungeonT7First = 72900000u, kDungeonT7Last = 72909999u;

/// The id that slot `slot` (0..6) stores for the area-29 id `id`; 0 when `id` is not an area-29 id.
inline std::uint32_t SlotFlagId(std::uint32_t id, int slot) {
    if (slot < 0 || slot > 6 || (id / kAreaStride) % 100 != 29) return 0;
    return id + static_cast<std::uint32_t>(11 + slot) * kAreaStride;
}

struct IdRange {
    std::uint32_t first, last;
};

/// The flags of the dungeon's 9 lamps (return points 2902950..58, lamp n): the lamp object's
/// registration flag 12907800 + 20 n and its lit flags +10 / +11, and the lamp-lit flag 72900200 + n
/// (the same layout as every map's lamps, party_guestvals_rules.h lampshare::LitFlags with block 290).
inline std::vector<std::uint32_t> LampIds() {
    std::vector<std::uint32_t> out;
    for (std::uint32_t n = 0; n < 9; ++n) {
        const std::uint32_t base = 12907800u + 20u * n;
        out.push_back(base);
        out.push_back(base + 10u);
        out.push_back(base + 11u);
        out.push_back(72900200u + n);
    }
    return out;
}

/// The ids a dungeon's party progress is made of: the lamps (LampIds) and, from `ranges` (the
/// shortcut / lamp categories of the party flag table), the parts inside the dungeon's block 0 of T1
/// and T7. Ascending, deduplicated.
inline std::vector<std::uint32_t> ProgressIds(const std::vector<IdRange>& ranges, std::size_t cap = 4096) {
    std::vector<std::uint32_t> out = LampIds();
    for (const IdRange& r : ranges) {
        for (const IdRange w : {IdRange{kDungeonT1First, kDungeonT1Last}, IdRange{kDungeonT7First, kDungeonT7Last}}) {
            const std::uint32_t a = r.first > w.first ? r.first : w.first;
            const std::uint32_t b = r.last < w.last ? r.last : w.last;
            for (std::uint64_t id = a; id <= b && out.size() < cap; ++id) out.push_back(static_cast<std::uint32_t>(id));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// A lamp counts as reached when its lit flag +10 (the one the host's world syncs to a guest, live
/// 2026-10-25) or its registration flag is set; the other flags of it (registration, +11, the lamp-lit
/// flag 7290020n) are not all synced, so they are added here. Result ascending, deduplicated.
inline void ExpandLamps(std::vector<std::uint32_t>* ids) {
    std::vector<std::uint32_t> extra;
    for (std::uint32_t n = 0; n < 9; ++n) {
        const std::uint32_t base = 12907800u + 20u * n;
        if (std::find(ids->begin(), ids->end(), base) != ids->end() ||
            std::find(ids->begin(), ids->end(), base + 10u) != ids->end()) {
            extra.push_back(base);
            extra.push_back(base + 10u);
            extra.push_back(base + 11u);
            extra.push_back(72900200u + n);
        }
    }
    ids->insert(ids->end(), extra.begin(), extra.end());
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
}

} // namespace coop::chalice
