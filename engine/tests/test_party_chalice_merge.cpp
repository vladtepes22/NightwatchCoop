// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for party_chalice_merge.h: where a guest's brought-home dungeon goes (dedupe, own
// dungeons kept) and which flag ids carry its progress.
#include "party/party_chalice_merge.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace coop::chalice;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static SlotRecord Empty() {
    SlotRecord r;
    r.w[1] = 0;
    r.w[2] = 0x80000000u;
    r.w[3] = 0xffffffffu;
    r.w[4] = 0xffffffffu;
    return r;
}
static SlotRecord Dungeon(std::uint64_t channel, std::uint32_t map = 0x1d0a5a00u) {
    SlotRecord r;
    r.w[1] = static_cast<std::uint32_t>(channel);
    r.w[2] = static_cast<std::uint32_t>(channel >> 32);
    r.w[3] = map;
    r.w[4] = 0x17dd;
    return r;
}
static std::array<SlotRecord, 7> AllEmpty() {
    std::array<SlotRecord, 7> a;
    for (SlotRecord& r : a) r = Empty();
    return a;
}

static void test_record() {
    CHECK(!RecordHoldsDungeon(Empty()));
    CHECK(RecordHoldsDungeon(Dungeon(0x3f22b458bd799ull)));
    CHECK(RecordChannel(Dungeon(0x3f22b458bd799ull)) == 0x3f22b458bd799ull);
    CHECK(RecordChannel(Empty()) == 0);
    SlotRecord local = Dungeon(1);   // a local dungeon: top bit set
    local.w[2] = 0x80000001u;
    CHECK(RecordChannel(local) == 0);
}

static void test_plan() {
    const SlotRecord host = Dungeon(0x3f22b458bd799ull);
    auto own = AllEmpty();
    // an empty guest: the first free slot after the network slot
    MergePlan p = PlanMerge(own, host);
    CHECK(p.kind == MergeKind::Added && p.slot == 1);
    // slot 0 (the network slot) holds the host's dungeon after the quick join: never a target
    own[0] = host;
    p = PlanMerge(own, host);
    CHECK(p.kind == MergeKind::Added && p.slot == 1);
    // the guest's own dungeon in slot 1 is kept, the new one goes to slot 2
    own[1] = Dungeon(0xdb68ed64129bull, 0x1d155a00u);
    p = PlanMerge(own, host);
    CHECK(p.kind == MergeKind::Added && p.slot == 2);
    // a hole is filled (slot 3 free, 1 and 2 taken)
    own[2] = Dungeon(77);
    p = PlanMerge(own, host);
    CHECK(p.slot == 3);
    // the same dungeon already at slot 2: no duplicate, only its progress merges there
    own[2] = host;
    p = PlanMerge(own, host);
    CHECK(p.kind == MergeKind::Existing && p.slot == 2);
    // ... even when a free slot is earlier
    own[1] = Empty();
    p = PlanMerge(own, host);
    CHECK(p.kind == MergeKind::Existing && p.slot == 2);
    // all of 1..6 hold other dungeons: nothing is overwritten
    for (int k = 1; k < 7; ++k) own[static_cast<std::size_t>(k)] = Dungeon(1000 + static_cast<std::uint64_t>(k));
    p = PlanMerge(own, host);
    CHECK(p.kind == MergeKind::None && p.slot == -1);
    // not a dungeon / no ChannelId
    CHECK(PlanMerge(AllEmpty(), Empty()).kind == MergeKind::None);
    SlotRecord local = Dungeon(5);
    local.w[2] = 0x80000000u;
    CHECK(PlanMerge(AllEmpty(), local).kind == MergeKind::None);
    // an empty slot's stale ChannelId is not a match (a record without a map holds no dungeon)
    auto stale = AllEmpty();
    stale[1] = Empty();
    stale[1].w[1] = static_cast<std::uint32_t>(0x3f22b458bd799ull);
    stale[1].w[2] = static_cast<std::uint32_t>(0x3f22b458bd799ull >> 32);
    CHECK(PlanMerge(stale, host).kind == MergeKind::Added && PlanMerge(stale, host).slot == 1);
}

static void test_ids() {
    // area 29 -> the slot's area 40 + k groups
    CHECK(SlotFlagId(12907800, 0) == 14007800);
    CHECK(SlotFlagId(12907800, 1) == 14107800);
    CHECK(SlotFlagId(72900202, 1) == 74100202);
    CHECK(SlotFlagId(12907800, 6) == 14607800);
    CHECK(SlotFlagId(12907800, 7) == 0);
    CHECK(SlotFlagId(12807800, 1) == 0);   // not an area-29 id
    CHECK(SlotFlagId(94000000, 1) == 0);

    const std::vector<IdRange> r = {{12900000, 12900000}, {12907800, 12907800}, {12907820, 12907820},
                                    {72900200, 72900208}, {12910300, 12910300},  // block 1: out
                                    {12207800, 12207800},                         // another area: out
                                    {12907820, 12907821}};                        // overlap: deduplicated
    const std::vector<std::uint32_t> ids = ProgressIds(r);
    // the 9 lamps x {registration, lit +10, lit +11, lamp-lit 7290020n} = 36 ids, plus the ranges' extras
    const std::vector<std::uint32_t> lamps = LampIds();
    CHECK(lamps.size() == 36);
    for (std::uint32_t id : lamps) CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    CHECK(std::find(lamps.begin(), lamps.end(), 12907810u) != lamps.end());
    CHECK(std::find(lamps.begin(), lamps.end(), 12907971u) != lamps.end());
    CHECK(std::find(lamps.begin(), lamps.end(), 72900208u) != lamps.end());
    CHECK(std::find(ids.begin(), ids.end(), 12900000u) != ids.end());   // from the ranges
    CHECK(std::find(ids.begin(), ids.end(), 12907821u) != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), 12910300u) == ids.end());   // block 1: out
    CHECK(std::find(ids.begin(), ids.end(), 12207800u) == ids.end());   // another area: out
    CHECK(std::is_sorted(ids.begin(), ids.end()));
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());      // deduplicated
    // a range wider than the dungeon's block is clipped to it
    const std::vector<std::uint32_t> wide = ProgressIds({{12899990, 12900002}}, 4096);
    CHECK(std::find(wide.begin(), wide.end(), 12899999u) == wide.end());
    CHECK(std::find(wide.begin(), wide.end(), 12900002u) != wide.end());
    CHECK(std::find(wide.begin(), wide.end(), 12900003u) == wide.end());
    // a registered lamp brings its other three flags; unregistered ones nothing
    std::vector<std::uint32_t> got = {12907840u, 12907970u, 12900000u};  // registered lamp 2; lit +10 of lamp 8
    ExpandLamps(&got);
    CHECK(got.size() == 3 + 6);
    CHECK(std::find(got.begin(), got.end(), 12907960u) != got.end());
    CHECK(std::find(got.begin(), got.end(), 12907850u) != got.end());
    CHECK(std::find(got.begin(), got.end(), 12907851u) != got.end());
    CHECK(std::find(got.begin(), got.end(), 72900202u) != got.end());
    CHECK(std::find(got.begin(), got.end(), 72900208u) != got.end());
    CHECK(std::find(got.begin(), got.end(), 72900201u) == got.end());
}

int main() {
    test_record();
    test_plan();
    test_ids();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("party-chalice-merge-test: all passed\n");
    return 0;
}
