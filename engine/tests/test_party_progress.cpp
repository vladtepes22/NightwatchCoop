// SPDX-License-Identifier: GPL-3.0-or-later
// Party progress sync (gpu/shim/party/party_progress, C2) against a fake in-memory
// SprjEventFlagMan: group keys, bit order, category lookup (sorted ranges == first-match table),
// policy filters, the flag store walker (mode 0 tree over the pool, mode 1 overlay), own-save
// writes, the host diff, seq de-duplication, JSON round trips, snapshot apply, BBPF files
// (read back here and, when python is available, by tools/party/flag_tool.py dump).
// Build and run: ninja -C out/gpu party-progress-test && out/gpu/party-progress-test.exe
#include "party/party_progress.h"

#include <algorithm>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace coop::progress;

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

// ---- a fake SprjEventFlagMan in host memory ----

static bool MemRead(u64 a, void* o, std::size_t n) {
    if (!a) return false;
    std::memcpy(o, reinterpret_cast<const void*>(a), n);
    return true;
}
static bool MemWrite(u64 a, const void* i, std::size_t n) {
    if (!a) return false;
    std::memcpy(reinterpret_cast<void*>(a), i, n);
    return true;
}

struct FakeNode {  // MSVC std::map node layout (left +0, parent +8, right +0x10, is_nil +0x19,
                   // key +0x20, kind +0x28, storage +0x30)
    u64 left = 0, parent = 0, right = 0;
    u8 color = 0, is_nil = 0;
    u8 pad[6] = {};
    u32 key = 0, pad2 = 0;
    std::int32_t kind = 0, pad3 = 0;
    u64 storage = 0;
};
static_assert(offsetof(FakeNode, is_nil) == 0x19);
static_assert(offsetof(FakeNode, key) == 0x20);
static_assert(offsetof(FakeNode, kind) == 0x28);
static_assert(offsetof(FakeNode, storage) == 0x30);

struct FakeStore {
    std::vector<u8> man = std::vector<u8>(0x100);
    std::vector<u8> pool = std::vector<u8>(kPoolBlocks * kBlockBytes);
    std::map<u32, std::vector<u8>> overlay;  // block -> 125 bytes (mode 1)
    std::vector<std::unique_ptr<FakeNode>> nodes;
    FakeNode head;
    int mode = 0;

    u64 addr() const { return reinterpret_cast<u64>(man.data()); }
    MemOps ops() const {
        MemOps o;
        o.read = MemRead;
        o.write = MemWrite;
        return o;
    }
    FlagStore view() const { return FlagStore(addr(), ops()); }

    // Mode 0: every pool block through the tree (kind 1); mode 1: blocks 6..8 stay in the pool,
    // everything else points at a zeroed overlay buffer (kind 2), as SetLoadMode(1).
    void Build(int load_mode) {
        mode = load_mode;
        std::map<u32, std::pair<int, u64>> entries;
        for (int key = 0; key < int(kPoolBlocks); ++key) {
            const u32 block = BlockOfPoolKey(key);
            if (block == 0xffffffffu) continue;
            if (load_mode == 1 && !(block >= 6 && block <= 8)) {
                auto& buf = overlay[block];
                buf.assign(kBlockBytes, 0);
                entries[block] = {2, reinterpret_cast<u64>(buf.data())};
            } else {
                entries[block] = {1, u64(key)};
            }
        }
        nodes.clear();
        head = FakeNode{};
        head.is_nil = 1;
        std::vector<std::pair<u32, std::pair<int, u64>>> sorted(entries.begin(), entries.end());
        const u64 nil = reinterpret_cast<u64>(&head);
        std::function<u64(int, int, u64)> build = [&](int lo, int hi, u64 parent) -> u64 {
            if (lo > hi) return nil;
            const int mid = (lo + hi) / 2;
            nodes.push_back(std::make_unique<FakeNode>());
            FakeNode* n = nodes.back().get();
            n->key = sorted[mid].first;
            n->kind = sorted[mid].second.first;
            n->storage = sorted[mid].second.second;
            n->parent = parent;
            const u64 self = reinterpret_cast<u64>(n);
            n->left = build(lo, mid - 1, self);
            n->right = build(mid + 1, hi, self);
            return self;
        };
        head.parent = build(0, int(sorted.size()) - 1, nil);
        auto put32 = [&](std::size_t off, u32 v) { std::memcpy(man.data() + off, &v, 4); };
        auto put64 = [&](std::size_t off, u64 v) { std::memcpy(man.data() + off, &v, 8); };
        put32(0x10, 5);
        put32(0x18, 3);
        put32(0x1c, 1000);
        put32(0x20, 125);
        put32(0x24, 1200);
        put64(0x28, reinterpret_cast<u64>(pool.data()));
        put64(0x38, nil);
        put32(0x80, u32(load_mode));
    }
    bool PoolBit(u32 id) const {
        const int key = PoolKeyOfId(id);
        return key >= 0 && GetBit(pool.data() + key * kBlockBytes, id % 1000);
    }
    void SetPoolBit(u32 id, bool v) {
        PutBit(pool.data() + PoolKeyOfId(id) * kBlockBytes, id % 1000, v);
    }
    bool OverlayBit(u32 id) const {
        auto it = overlay.find(id / 1000);
        return it != overlay.end() && GetBit(it->second.data(), id % 1000);
    }
    void SetOverlayBit(u32 id, bool v) { PutBit(overlay[id / 1000].data(), id % 1000, v); }
};

// ---- tests ----

static void TestKeys() {
    for (int z = 0; z < 10; ++z) {
        CHECK(GroupKey(0, 0, 0, z) == 5 * z);
        CHECK(GroupKey(1, 0, 0, z) == 1 + 5 * z);
        CHECK(GroupKey(7, 0, 0, z) == 4 + 5 * z);
    }
    CHECK(GroupKey(9, 0, 0, 0) == -1);           // type 9 has no global group
    CHECK(PoolKeyOfId(12401800) == 115 + 50 + 4);  // T1 m24_00 zone 1 -> 169
    CHECK(PoolKeyOfId(73601999) == 115 + 50 + 23 * 3 + 16);  // T7 m36_00 zone 1 -> 250
    CHECK(PoolKeyOfId(73609000) == 9 * 115 + 50 + 23 * 3 + 16);  // T7 m36_00 zone 9 -> 1170
    CHECK(PoolKeyOfId(73609000) == 1170);
    CHECK(PoolKeyOfId(2200) == 10);               // global zone 2
    CHECK(PoolKeyOfId(10002500) == 1 + 2 * 5);    // T1 global zone 2
    CHECK(PoolKeyOfId(14000000) == 50 + 11);      // area 40 -> map index 11 (shares 29_00)
    CHECK(PoolKeyOfId(14100000) == 50 + 17);      // area 41 -> 17
    CHECK(PoolKeyOfId(14600000) == 50 + 22);      // area 46 -> 22
    CHECK(PoolKeyOfId(14700000) == -1);           // area 47 -> 23: out of range
    CHECK(PoolKeyOfId(19900000) == -1);           // area 99: not pooled
    CHECK(PoolKeyOfId(13000000) == -1);           // m30: not in the table
    CHECK(PoolKeyOfId(12430000) == -1);           // m24_03: not in the table
    CHECK(PoolKeyOfId(10000) == -1);              // block 10: area 0 block 1, type 0
    CHECK(GroupKey(5, 99, 0, 2) == 0x4b0 + 1 + 10);
    // the inverse covers all 1200 keys exactly once and round-trips
    std::set<u32> blocks;
    for (int k = 0; k < int(kPoolBlocks); ++k) {
        const u32 b = BlockOfPoolKey(k);
        CHECK(b != 0xffffffffu);
        CHECK(PoolKeyOfBlock(b) == k);
        blocks.insert(b);
    }
    CHECK(blocks.size() == kPoolBlocks);
    CHECK(BlockOfPoolKey(61) == 12900);  // 29_00 wins over area 40 for key 61
    CHECK(NeverWritable(9020) && NeverWritable(9026) && !NeverWritable(9019) && !NeverWritable(9027));
    CHECK(NeverWritable(12900100) && NeverWritable(14100000) && NeverWritable(19900000));
    CHECK(!NeverWritable(12401800));
}

static void TestBits() {
    u8 d[kBlockBytes] = {};
    PutBit(d, 0, true);
    CHECK(d[0] == 0x80);
    PutBit(d, 9, true);
    CHECK(d[1] == 0x40);
    PutBit(d, 999, true);
    CHECK(d[124] == 0x01);
    CHECK(GetBit(d, 0) && GetBit(d, 9) && GetBit(d, 999) && !GetBit(d, 1));
    PutBit(d, 0, false);
    CHECK(d[0] == 0 && !GetBit(d, 0));
}

static void TestCategories() {
    const auto& r = CategoryRanges();
    CHECK(!r.empty());
    for (std::size_t i = 1; i < r.size(); ++i) {
        CHECK(r[i - 1].last < r[i].first);
    }
    CHECK(CategoryOf(2200) == Category::never_sync);   // area gate (0x47304B0)
    CHECK(CategoryOf(2410) == Category::never_sync);
    CHECK(CategoryOf(2405) == Category::never_sync);   // TBL0 gate
    CHECK(CategoryOf(9801) == Category::time_of_day);
    CHECK(CategoryOf(9180) == Category::never_sync);   // cutscene playing
    CHECK(CategoryOf(72100130) == Category::never_sync);
    CHECK(CategoryOf(12100180) == Category::never_sync);
    CHECK(CategoryOf(12101800) == Category::boss_defeated);
    CHECK(CategoryOf(12101802) == Category::cutscene_seen);
    CHECK(CategoryOf(9021) == Category::never_sync);
    CHECK(CategoryOf(6610) == Category::never_sync);
    CHECK(CategoryOf(21) == Category::never_sync);     // ending
    CHECK(CategoryOf(1005) == Category::npc_quest);
    CHECK(CategoryOf(5000) == Category::item_lot_picked);
    CHECK(CategoryOf(2000) == Category::session_runtime);
    CHECK(CategoryOf(12400150) == Category::shortcut_door);
    CHECK(CategoryOf(99999999) == Category::never_sync);
    // exhaustive against the first-match table: every range edge +-1 and random ids
    std::vector<u32> probe;
    for (const auto& x : r) {
        for (u32 d : {0u, 1u}) {
            probe.push_back(x.first - d);
            probe.push_back(x.first + d);
            probe.push_back(x.last - d);
            probe.push_back(x.last + d);
        }
    }
    std::mt19937 rng(7);
    for (int i = 0; i < 20000; ++i) {
        probe.push_back(rng() % 100000000u);
    }
    for (u32 b = 0; b < 10000; ++b) {
        probe.push_back(b);
    }
    std::size_t mismatches = 0;
    for (u32 id : probe) {
        if (CategoryOf(id) != CategoryOfLinear(id)) {
            if (++mismatches < 5) std::fprintf(stderr, "category mismatch at %u\n", id);
        }
    }
    CHECK(mismatches == 0);
    Category c;
    CHECK(CategoryFromName("lamp_unlocked", &c) && c == Category::lamp_unlocked);
    CHECK(!CategoryFromName("nope", &c));
    std::printf("categories: %zu disjoint ranges, %zu ids probed\n", r.size(), probe.size());
}

static void TestPolicy() {
    Policy p = Policy::Defaults();
    CHECK(ModeFor(p, 12101800) == SyncMode::SetOnly);  // boss_defeated
    CHECK(ModeFor(p, 12101802) == SyncMode::SetOnly);  // cutscene_seen
    CHECK(ModeFor(p, 21) == SyncMode::Off);            // ending: player-owned
    // 0.7.6: the Insight shop's Chalice boss unlocks (5090 Bone Ash, 5091 Madman; no item lot carries them)
    // are shared progress: before, the item-lot range 5000-5299 left them with the host (C3 has no lot to replay)
    CHECK(CategoryOf(5090) == Category::world_state && ModeFor(p, 5090) == SyncMode::SetOnly);
    CHECK(CategoryOf(5091) == Category::world_state && ModeFor(p, 5091) == SyncMode::SetOnly);
    CHECK(CategoryOf(5100) == Category::item_lot_picked && ModeFor(p, 5100) == SyncMode::Off);  // a lot's pickup flag: C3
    CHECK(CategoryOf(5053) == Category::item_lot_picked);                                       // a shop gate WITH a lot: per player
    CHECK(ModeFor(p, 5500) == SyncMode::Off && ModeFor(p, 6000) == SyncMode::Off);              // owns-the-weapon / personal block
    CHECK(ModeFor(p, 9802) == SyncMode::Off);          // time of day: C4 only
    CHECK(!HostCandidate(9800) && !HostCandidate(2410) && !HostCandidate(3601));
    CHECK(ModeFor(p, 12100180) == SyncMode::Off);      // ending event
    CHECK(ModeFor(p, 12400150) == SyncMode::State);    // elevator
    CHECK(ModeFor(p, 12400147) == SyncMode::State);
    // P9 (quest_audit): the one-way doors that close again follow the host both ways (open AND
    // shut), so the guest's own save is not left with a door the host's world has closed.
    CHECK(ModeFor(p, 12400157) == SyncMode::State);
    CHECK(ModeFor(p, 12400177) == SyncMode::State);
    CHECK(ModeFor(p, 12700173) == SyncMode::State);
    CHECK(ModeFor(p, 5000) == SyncMode::Off);          // item lot: C3
    CHECK(ModeFor(p, 1005) == SyncMode::Off);          // npc quest
    CHECK(ModeFor(p, 2000) == SyncMode::Off);          // session runtime
    CHECK(ModeFor(p, 9021) == SyncMode::Off);          // chalice slot
    CHECK(ModeFor(p, 9180) == SyncMode::Off);          // cleared every load
    // Snatcher kidnap trigger: transient (common 9421 moves the respawn point to the Gaol and
    // clears it); in a guest's own save it would send its next solo death to Yahar'gul.
    CHECK(CategoryOf(9420) == Category::never_sync);
    CHECK(ModeFor(p, 9420) == SyncMode::Off);
    CHECK(CategoryOf(9423) == Category::key_event);    // first-sacrifice scene seen: still shared
    CHECK(ModeFor(p, 9423) == SyncMode::SetOnly);
    CHECK(ModeFor(p, 12100505) == SyncMode::Off);      // Doll position (random)
    CHECK(!HostCandidate(2000) && HostCandidate(5000) && HostCandidate(1005) && HostCandidate(12101800));
    p.mode[int(Category::item_lot_picked)] = SyncMode::SetOnly;
    CHECK(ModeFor(p, 5000) == SyncMode::Off);          // C3 owns item pickups, whatever the policy says
    CHECK(!HostCandidate(60009000));                   // C3 ledger flags
    // candidate masks agree with HostCandidate
    const auto& blocks = CandidateBlocks();
    CHECK(!blocks.empty());
    std::size_t bad = 0, bits = 0;
    for (u32 b : blocks) {
        const auto* m = CandidateMask(b);
        for (u32 i = 0; i < 1000; ++i) {
            const bool want = HostCandidate(b * 1000 + i);
            bits += want;
            bad += want != GetBit(m->data(), i);
        }
    }
    CHECK(bad == 0);
    CHECK(CandidateMask(6) == nullptr);  // 6000-6999 personal
    std::printf("policy: %zu candidate blocks, %zu candidate flags\n", blocks.size(), bits);
}

static void TestStore() {
    FakeStore f;
    f.Build(0);
    FlagStore s = f.view();
    CHECK(s.Valid() && s.LoadMode() == 0);
    BlockRef r;
    CHECK(s.FindBlock(12401, &r) && r.kind == 1 && r.data == reinterpret_cast<u64>(f.pool.data()) + 169 * 125);
    CHECK(!s.FindBlock(12430, &r));
    std::size_t n = 0;
    u32 prev = 0;
    bool ordered = true;
    CHECK(s.ForEachBlock([&](const BlockRef& b) {
        if (n && b.block <= prev) ordered = false;
        prev = b.block;
        ++n;
    }));
    CHECK(ordered && n == kPoolBlocks);
    // mode 0: own-save writes go through the tree = the pool
    CHECK(s.WriteOwnSave(12401800, true));
    CHECK(f.PoolBit(12401800));
    bool v = false;
    CHECK(s.ReadVisible(12401800, &v) && v);
    CHECK(s.ReadOwnSave(12401800, &v) && v);
    CHECK(!s.WriteOwnSave(9021, true));  // never
    CHECK(!f.PoolBit(9021));

    // mode 1: the visible tree is the overlay; own-save writes go to the pool only
    FakeStore g;
    g.Build(1);
    FlagStore t = g.view();
    CHECK(t.Valid() && t.LoadMode() == 1);
    CHECK(t.FindBlock(12401, &r) && r.kind == 2);
    CHECK(t.FindBlock(6, &r) && r.kind == 1);
    CHECK(t.WriteOwnSave(12401800, true));
    CHECK(g.PoolBit(12401800) && !g.OverlayBit(12401800));
    CHECK(t.ReadVisible(12401800, &v) && !v);
    CHECK(t.ReadOwnSave(12401800, &v) && v);
    CHECK(t.WriteVisible(2200, true));  // the overlay
    CHECK(g.OverlayBit(2200) && !g.PoolBit(2200));
    CHECK(t.WriteOwnSave(12401800, false) && !g.PoolBit(12401800));
    // a broken header is rejected
    FakeStore h;
    h.Build(0);
    const u32 bad = 999;
    std::memcpy(h.man.data() + 0x20, &bad, 4);
    CHECK(!h.view().Valid());
}

static std::vector<FlagChange> Scan(HostTracker& tr, const FlagStore& s) {
    std::vector<FlagChange> out;
    s.ForEachBlock([&](const BlockRef& r) {
        u8 d[kBlockBytes];
        if (CandidateMask(r.block) && s.ReadBlockBytes(r, d)) tr.ObserveBlock(r.block, d, &out);
    });
    tr.EndScan();
    return out;
}

static void TestDiffAndDedupe() {
    FakeStore f;
    f.Build(0);
    f.SetPoolBit(12101800, true);  // already dead before the session: baseline, not a change
    FlagStore s = f.view();
    HostTracker tr(0x1234);
    CHECK(Scan(tr, s).empty());
    CHECK(tr.has_baseline());
    f.SetPoolBit(12401800, true);  // boss
    f.SetPoolBit(12400150, true);  // elevator
    f.SetPoolBit(2000, true);      // session runtime: ignored
    f.SetPoolBit(9021, true);      // never: ignored
    auto ch = Scan(tr, s);
    CHECK(ch.size() == 2);
    CHECK(ch.size() == 2 && ch[0].id == 12400150 && ch[0].value && ch[0].seq == 1 &&
          ch[0].cat == Category::shortcut_door);
    CHECK(ch.size() == 2 && ch[1].id == 12401800 && ch[1].seq == 2 && ch[1].cat == Category::boss_defeated);
    CHECK(Scan(tr, s).empty());
    // the hook path: an id confirmed once, the scan then sees no change
    f.SetPoolBit(12400150, false);
    std::vector<FlagChange> hook;
    tr.ObserveId(12400150, false, &hook);
    tr.ObserveId(12400150, false, &hook);
    tr.ObserveId(2000, false, &hook);  // not a candidate
    CHECK(hook.size() == 1 && hook[0].seq == 3 && !hook[0].value);
    CHECK(Scan(tr, s).empty());
    // snapshot = shadow
    FlagSnapshot snap = tr.Snapshot(0x18000000);
    bool known = false;
    CHECK(snap.epoch == 0x1234 && snap.seq == 3 && snap.map_id == 0x18000000);
    CHECK(snap.Get(12401800, &known) && known);
    CHECK(snap.Get(12101800, &known) && known);
    CHECK(!snap.Get(12400150, &known) && known);
    CHECK(!snap.Get(2000, &known));

    // guest cursor: duplicates and replays dropped, a new epoch resets
    GuestCursor c;
    auto a = c.Filter(1, {{1, true, Category::boss_defeated, 1}, {2, true, Category::boss_defeated, 2}});
    CHECK(a.size() == 2 && c.last_seq() == 2);
    a = c.Filter(1, {{2, true, Category::boss_defeated, 2}, {3, true, Category::boss_defeated, 3}});
    CHECK(a.size() == 1 && a[0].seq == 3);
    a = c.Filter(1, {{1, true, Category::boss_defeated, 1}});
    CHECK(a.empty());
    a = c.Filter(2, {{1, true, Category::boss_defeated, 1}});  // host restarted
    CHECK(a.size() == 1 && c.epoch() == 2);
    c.Reset(2, 10);
    CHECK(c.Filter(2, {{5, true, Category::boss_defeated, 9}}).empty());
}

static void TestJson() {
    std::vector<FlagChange> in = {{12401800, true, Category::boss_defeated, 1},
                                  {12400150, false, Category::shortcut_door, 2},
                                  {72400200, true, Category::lamp_unlocked, 9007199254740991ull}};
    const std::string text = ChangesToJson(0xfedcba9876543210ull, in);
    u64 epoch = 0;
    std::vector<FlagChange> out;
    std::string err;
    CHECK(ChangesFromJson(text, &epoch, &out, &err));
    CHECK(epoch == 0xfedcba9876543210ull && out == in);
    CHECK(!ChangesFromJson("{\"v\":1,\"epoch\":\"0x1\",\"c\":[[1,1,\"bogus\",1]]}", &epoch, &out, &err));
    CHECK(!ChangesFromJson("{\"v\":2,\"epoch\":\"0x1\",\"c\":[]}", &epoch, &out, &err));
    CHECK(!ChangesFromJson("not json", &epoch, &out, &err));

    FlagSnapshot s;
    s.epoch = 0x8000000000000001ull;
    s.seq = 42;
    s.map_id = 0x18010000;
    s.blocks[12401] = {800, 801};
    s.blocks[12400] = {};
    s.blocks[2] = {200};
    const std::string st = SnapshotToJson(s);
    FlagSnapshot r;
    CHECK(SnapshotFromJson(st, &r, &err));
    CHECK(r == s);
    CHECK(!SnapshotFromJson("{\"v\":1,\"epoch\":\"0x1\",\"seq\":1,\"map\":0,\"b\":[[1,[1000]]]}", &r, &err));
    std::printf("json: changes %zu bytes, snapshot %zu bytes\n", text.size(), st.size());
}

static void TestApplier() {
    // host world -> snapshot + changes -> a guest in load mode 1
    FakeStore host;
    host.Build(0);
    host.SetPoolBit(12401800, true);  // boss
    host.SetPoolBit(12400151, true);  // elevator up
    host.SetPoolBit(72400200, true);  // lamp lit (lamp_unlocked)
    host.SetPoolBit(5000, true);      // item lot (off by default)
    FlagStore hs = host.view();
    HostTracker tr(77);
    Scan(tr, hs);
    const FlagSnapshot snap = tr.Snapshot(0);

    FakeStore guest;
    guest.Build(1);
    guest.SetPoolBit(12400150, true);  // guest's own elevator state: follows the host (0)
    guest.SetPoolBit(12200125, true);  // another elevator, 0 on the host
    guest.SetPoolBit(2200, true);      // guest killed something the host has not: set-only keeps it
    FlagStore gs = guest.view();
    GuestApplier ap;
    // round trip through JSON as the link would
    FlagSnapshot wire;
    std::string err;
    CHECK(SnapshotFromJson(SnapshotToJson(snap), &wire, &err));
    ap.QueueSnapshot(wire);
    Policy p = Policy::Defaults();
    CHECK(ap.Apply(gs, p, false) > 0);
    CHECK(guest.PoolBit(12401800));
    CHECK(guest.PoolBit(12400151) && !guest.PoolBit(12400150));
    CHECK(!guest.PoolBit(12200125));
    CHECK(guest.PoolBit(2200));
    CHECK(!guest.PoolBit(5000));                                  // items off
    CHECK(!guest.OverlayBit(12401800));                           // no mirror by default
    CHECK(ModeFor(p, 72400200) == SyncMode::SetOnly && guest.PoolBit(72400200));
    CHECK(ap.cursor().epoch() == 77 && ap.cursor().last_seq() == snap.seq);

    // The precomputed plan (link thread) writes exactly what the bit walk writes.
    {
        auto run = [&](int how) {  // 0 no plan, 1 plan, 2 plan for another policy (falls back)
            FakeStore g2;
            g2.Build(1);
            g2.SetPoolBit(12400150, true);
            g2.SetPoolBit(12200125, true);
            g2.SetPoolBit(2200, true);
            FlagStore v = g2.view();
            GuestApplier a;
            const Policy pol = Policy::Defaults();
            if (how == 0) {
                a.QueueSnapshot(wire);
            } else {
                Policy other = pol;
                other.mirror_overlay = !other.mirror_overlay;
                a.QueueSnapshot(wire, GuestApplier::PlanSnapshot(wire, how == 1 ? pol : other));
            }
            const std::size_t n = a.Apply(v, pol, false);
            return std::make_pair(std::make_pair(n, g2.pool),
                                  std::make_pair(a.counters().already, a.counters().skipped));
        };
        const auto plain = run(0), planned = run(1), stale = run(2);
        CHECK(plain.first.first > 0);
        CHECK(planned == plain);
        CHECK(stale == plain);
        const auto plan = GuestApplier::PlanSnapshot(wire, Policy::Defaults());
        CHECK(plan.valid && !plan.flags.empty());
    }

    // live changes, with a replayed duplicate; mirror on
    p.mirror_overlay = true;
    host.SetPoolBit(12411800, true);
    auto ch = Scan(tr, hs);
    CHECK(ch.size() == 1);
    ap.QueueChanges(77, ch);
    ap.QueueChanges(77, ch);  // replay: dropped
    CHECK(ap.counters().dropped_seq == ch.size());
    CHECK(ap.Apply(gs, p, false) == 1);
    CHECK(guest.PoolBit(12411800) && guest.OverlayBit(12411800));
    // set-only: a host clear does not clear the guest
    host.SetPoolBit(12411800, false);
    ch = Scan(tr, hs);
    CHECK(ch.size() == 1 && !ch[0].value);
    ap.QueueChanges(77, ch);
    ap.Apply(gs, p, false);
    CHECK(guest.PoolBit(12411800));

    // load mode 0: kept until the post-load window
    FakeStore own;
    own.Build(0);
    FlagStore os = own.view();
    GuestApplier ap0;
    ap0.QueueChanges(5, {{12401800, true, Category::boss_defeated, 1}});
    CHECK(ap0.Apply(os, Policy::Defaults(), false) == 0 && ap0.Pending() == 1 && !own.PoolBit(12401800));
    CHECK(ap0.Apply(os, Policy::Defaults(), true) == 1 && ap0.Pending() == 0 && own.PoolBit(12401800));
    // A boss flag of the map the guest stands in (m24_01 Cleric Beast 12411800 in Central
    // Yharnam) is held back in mode 0: the running boss event would play the kill (banner,
    // music, rewards). Another map's boss flag goes in; the held one follows in mode 1 (pool).
    CHECK(HoldBossFlagInMode0(12411800, 0x18010000) && !HoldBossFlagInMode0(12411800, 0x18000000) &&
          !HoldBossFlagInMode0(12417810, 0x18010000) && !HoldBossFlagInMode0(12411800, 0xffffffffu));
    {
        FakeStore home;
        home.Build(0);
        FlagStore hs = home.view();
        GuestApplier h;
        h.QueueChanges(6, {{12411800, true, Category::boss_defeated, 1}, {12401800, true, Category::boss_defeated, 2}});
        CHECK(h.Apply(hs, Policy::Defaults(), true, 0x18010000) == 1);
        CHECK(home.PoolBit(12401800) && !home.PoolBit(12411800) && h.Held() == 1 && h.Pending() == 1);
        CHECK(h.Apply(hs, Policy::Defaults(), true, 0x18010000) == 0 && h.Held() == 1); // same map: still held
        FakeStore away;
        away.Build(1);
        FlagStore as = away.view();
        CHECK(h.Apply(as, Policy::Defaults(), false, 0x18010000) == 1 && away.PoolBit(12411800) && h.Pending() == 0);
    }
    // never-writable ids are refused even when a (bad) host sends them
    ap0.QueueChanges(5, {{9021, true, Category::world_state, 2}});
    ap0.Apply(os, Policy::Defaults(), true);
    CHECK(!own.PoolBit(9021));
}

// D8 shared NPC quest choices (Policy::FromEnv: npc_quest Follow). The host's quest step (Iosefka:
// 1141 off, 1143 on) reaches the guest's own save both ways; a catch-up snapshot copies each
// 1000-1999 NPC state machine the host has any bit of, and sets only elsewhere.
static void TestNpcFollow() {
    CHECK(NpcStateGroup(999) == -1 && NpcStateGroup(1000) == 0 && NpcStateGroup(1141) == 7 &&
          NpcStateGroup(1159) == 7 && NpcStateGroup(1999) == 49 && NpcStateGroup(2000) == -1);
    const Policy p = Policy::FromEnv();
    CHECK(ModeFor(p, 1141) == SyncMode::Follow);
    CHECK(ModeFor(Policy::Defaults(), 1141) == SyncMode::Off);

    FakeStore host;
    host.Build(0);
    host.SetPoolBit(1143, true);      // host: Iosefka moved on (1141 cleared)
    host.SetPoolBit(72410337, true);  // per-map NPC state
    FlagStore hs = host.view();
    HostTracker tr(9);
    Scan(tr, hs);
    FlagSnapshot wire;
    std::string err;
    CHECK(SnapshotFromJson(SnapshotToJson(tr.Snapshot(0)), &wire, &err));

    FakeStore guest;
    guest.Build(1);
    guest.SetPoolBit(1141, true);      // stale state of the same NPC: cleared by the catch-up
    guest.SetPoolBit(1205, true);      // another NPC the host never met: kept
    guest.SetPoolBit(72410338, true);  // per-map NPC flag outside the state machines: kept (set-only)
    FlagStore gs = guest.view();
    GuestApplier ap;
    ap.QueueSnapshot(wire, GuestApplier::PlanSnapshot(wire, p));
    CHECK(ap.Apply(gs, p, false) > 0);
    CHECK(guest.PoolBit(1143) && !guest.PoolBit(1141));
    CHECK(guest.PoolBit(1205) && guest.PoolBit(72410337) && guest.PoolBit(72410338));
    CHECK(ap.counters().npc_cleared == 1);
    // same result without the precomputed plan
    {
        FakeStore g2;
        g2.Build(1);
        g2.SetPoolBit(1141, true);
        g2.SetPoolBit(1205, true);
        FlagStore v = g2.view();
        GuestApplier a;
        a.QueueSnapshot(wire);
        a.Apply(v, p, false);
        CHECK(g2.PoolBit(1143) && !g2.PoolBit(1141) && g2.PoolBit(1205));
    }

    // live: the host's next step clears 1143 and sets 1144 -> both reach the guest
    host.SetPoolBit(1143, false);
    host.SetPoolBit(1144, true);
    host.SetPoolBit(72410337, false);
    auto ch = Scan(tr, hs);
    CHECK(ch.size() == 3);
    ap.QueueChanges(9, ch);
    ap.Apply(gs, p, false);
    CHECK(!guest.PoolBit(1143) && guest.PoolBit(1144) && !guest.PoolBit(72410337));
    CHECK(ap.counters().npc_cleared == 3);
    // other categories stay set-only under the same policy
    host.SetPoolBit(12411800, true);
    ap.QueueChanges(9, Scan(tr, hs));
    ap.Apply(gs, p, false);
    host.SetPoolBit(12411800, false);
    ap.QueueChanges(9, Scan(tr, hs));
    ap.Apply(gs, p, false);
    CHECK(guest.PoolBit(12411800));
}

// The host-world overlay map (SprjEventFlagMan +0x88/+0x90: key +0x20, buffer +0x28) over the
// fake store's mode-1 overlay buffers (the visible tree's kind-2 nodes point at the same buffers).
struct OvNode {
    u64 left = 0, parent = 0, right = 0;
    u8 color = 0, is_nil = 0;
    u8 pad[6] = {};
    u32 key = 0, pad2 = 0;
    u64 buffer = 0;
};
static_assert(offsetof(OvNode, key) == 0x20 && offsetof(OvNode, buffer) == 0x28);

struct FakeOverlayMap {
    std::vector<std::unique_ptr<OvNode>> nodes;
    std::unique_ptr<OvNode> head = std::make_unique<OvNode>();
    void Build(FakeStore& fs) {
        std::vector<std::pair<u32, u64>> sorted;
        for (int key = 0; key < int(kPoolBlocks); ++key) {
            const u32 block = BlockOfPoolKey(key);
            if (block >= 6 && block <= 8) continue;
            auto it = fs.overlay.find(block);
            if (it == fs.overlay.end()) continue;
            sorted.emplace_back(u32(key), reinterpret_cast<u64>(it->second.data()));
        }
        nodes.clear();
        head = std::make_unique<OvNode>();
        head->is_nil = 1;
        const u64 nil = reinterpret_cast<u64>(head.get());
        std::function<u64(int, int, u64)> build = [&](int lo, int hi, u64 parent) -> u64 {
            if (lo > hi) return nil;
            const int mid = (lo + hi) / 2;
            nodes.push_back(std::make_unique<OvNode>());
            OvNode* n = nodes.back().get();
            n->key = sorted[mid].first;
            n->buffer = sorted[mid].second;
            n->parent = parent;
            const u64 self = reinterpret_cast<u64>(n);
            n->left = build(lo, mid - 1, self);
            n->right = build(mid + 1, hi, self);
            return self;
        };
        head->parent = build(0, int(sorted.size()) - 1, nil);
        u64 leftmost = head->parent;
        while (leftmost != nil && reinterpret_cast<OvNode*>(leftmost)->left != nil) {
            leftmost = reinterpret_cast<OvNode*>(leftmost)->left;
        }
        head->left = leftmost;
        std::memcpy(fs.man.data() + 0x90, &nil, 8);
    }
};

static void TestWorldMirror() {
    // the mirror set
    CHECK(WorldMirrorKey(PoolKeyOfId(72410201)));   // lamp lit, T7 m24_01 zone 0
    CHECK(WorldMirrorKey(PoolKeyOfId(12411780)));   // lamp object, T1 zone 7
    CHECK(WorldMirrorKey(PoolKeyOfId(52410100)));   // item pickup
    CHECK(WorldMirrorKey(PoolKeyOfId(12411000)));   // T1 zone 1 (doors)
    CHECK(WorldMirrorKey(PoolKeyOfId(2200)));       // boss / area gates
    CHECK(WorldMirrorKey(PoolKeyOfId(9401)));
    CHECK(WorldMirrorKey(PoolKeyOfId(70000030)));   // T7 global
    CHECK(!WorldMirrorKey(PoolKeyOfId(12404800)));  // boss-fight state (zone 4)
    CHECK(!WorldMirrorKey(PoolKeyOfId(12415000)));  // enemy zones 5, 6
    CHECK(!WorldMirrorKey(PoolKeyOfId(12416000)));
    CHECK(!WorldMirrorKey(PoolKeyOfId(6100)) && !WorldMirrorKey(PoolKeyOfId(7600)) &&
          !WorldMirrorKey(PoolKeyOfId(8100)));       // the own pool in mode 1
    CHECK(!WorldMirrorKey(PoolKeyOfId(60009000)));  // C3's ledger
    CHECK(!WorldMirrorKey(PoolKeyOfId(92410000)));  // type 9
    CHECK(!WorldMirrorKey(PoolKeyOfId(12900100)) && !WorldMirrorKey(PoolKeyOfId(14100000)));  // chalice
    CHECK(!WorldMirrorKey(-1) && !WorldMirrorKey(1200));
    const GroupBytes& m2 = WorldMirrorMask(PoolKeyOfId(2000));
    CHECK(!GetBit(m2.data(), 0) && !GetBit(m2.data(), 99) && GetBit(m2.data(), 100) && GetBit(m2.data(), 200));
    const GroupBytes& m9 = WorldMirrorMask(PoolKeyOfId(9000));
    CHECK(GetBit(m9.data(), 19) && !GetBit(m9.data(), 20) && !GetBit(m9.data(), 26) && GetBit(m9.data(), 27));
    CHECK(WorldMirrorKeys().size() == 46 + 16 * 37);  // globals; 16 maps x (T1 7 zones + T5/6/7)
    std::printf("world mirror: %zu mirror groups\n", WorldMirrorKeys().size());

    // host tracker
    FakeStore host;
    host.Build(0);
    HostWorldTracker ht;
    WorldImage out;
    host.SetPoolBit(72410201, true);
    host.SetPoolBit(2200, true);
    host.SetPoolBit(2006, true);      // session flag: sent (the guest's mask drops it)
    host.SetPoolBit(9025, true);      // chalice slot: idem
    host.SetPoolBit(12404800, true);  // not a mirror group: never sent
    ht.Observe(host.pool.data(), &out);
    CHECK(ht.has_baseline() && out.groups.empty());
    WorldImage full = ht.Full(7);
    CHECK(full.full && full.epoch == 7 && full.groups.size() == 3);
    CHECK(full.groups.count(u32(PoolKeyOfId(72410201))) && !full.groups.count(u32(PoolKeyOfId(12404800))));

    // JSON
    WorldImage back;
    std::string err;
    CHECK(WorldImageFromJson(WorldImageToJson(full), &back, &err) && back == full);
    CHECK(!WorldImageFromJson("{\"v\":1,\"epoch\":\"0x1\",\"full\":1,\"g\":[[30,\"" + std::string(250, '0') + "\"]]}",
                              &back, &err));  // block 6: not a mirror group
    CHECK(!WorldImageFromJson("{\"v\":1,\"epoch\":\"0x1\",\"full\":0,\"g\":[[0,\"00\"]]}", &back, &err));
    CHECK(!WorldImageFromJson("{\"v\":1,\"epoch\":\"0x1\",\"full\":0,\"g\":[[0,\"" + std::string(250, 'x') + "\"]]}",
                              &back, &err));

    // guest overlay (mode 1)
    FakeStore guest;
    guest.Build(1);
    {
        CHECK(guest.view().OverlayIdentity() == 0);  // no overlay map
        OvNode empty;
        empty.is_nil = 1;
        empty.parent = empty.left = empty.right = reinterpret_cast<u64>(&empty);
        const u64 h = reinterpret_cast<u64>(&empty);
        std::memcpy(guest.man.data() + 0x90, &h, 8);
        std::map<u32, u64> none;
        CHECK(guest.view().OverlayIdentity() == 0);  // empty: nothing to fill yet
        CHECK(guest.view().OverlayGroups(&none) && none.empty());
    }
    FakeOverlayMap ovm;
    ovm.Build(guest);
    const FlagStore gs = guest.view();
    CHECK(gs.Valid() && gs.LoadMode() == 1);
    std::map<u32, u64> ov;
    CHECK(gs.OverlayGroups(&ov) && ov.size() == kPoolBlocks - 3);
    const u64 id1 = gs.OverlayIdentity();
    CHECK(id1 != 0);
    guest.SetOverlayBit(9020, true);     // the host's chalice slot bit, set by SetLoadMode(1)
    guest.SetOverlayBit(2005, true);     // the guest's session flag
    guest.SetOverlayBit(72410050, true); // stale: the full copy makes it the host's (0)
    guest.SetPoolBit(72410201, false);   // own save untouched throughout
    GuestWorldMirror gm;
    GuestWorldMirror::Result r;
    CHECK(!gm.Apply(ov, guest.ops(), true, &r));  // no image yet
    gm.Receive(back.full ? full : full);
    CHECK(gm.has_image());
    CHECK(gm.Apply(ov, guest.ops(), true, &r));
    bool v = false;
    CHECK(gs.ReadVisible(72410201, &v) && v);
    CHECK(gs.ReadVisible(2200, &v) && v);
    CHECK(gs.ReadVisible(9020, &v) && v);     // kept
    CHECK(gs.ReadVisible(9025, &v) && !v);    // never written
    CHECK(gs.ReadVisible(2005, &v) && v);     // kept
    CHECK(gs.ReadVisible(2006, &v) && !v);    // never written
    CHECK(gs.ReadVisible(72410050, &v) && !v);
    CHECK(!guest.PoolBit(72410201) && !guest.PoolBit(2200));
    CHECK(r.groups == 2 && r.changed.size() == 2);  // T7 m24_01 zone 0 and block 2 (the 9xxx group: only masked bits)

    // live: only the bits the host changed; the guest's own runtime bits stay
    guest.SetOverlayBit(72410060, true);
    host.SetPoolBit(72410202, true);
    host.SetPoolBit(72410201, false);
    host.SetPoolBit(52410100, true);
    out = WorldImage{};
    ht.Observe(host.pool.data(), &out);
    CHECK(!out.full && out.groups.size() == 2);
    CHECK(WorldImageFromJson(WorldImageToJson(out), &back, &err) && back == out);
    gm.Receive(back);
    CHECK(gm.Dirty() == 2);
    CHECK(gm.Apply(ov, guest.ops(), false, &r) && r.groups == 2 && r.bits == 3);
    CHECK(gs.ReadVisible(72410202, &v) && v);
    CHECK(gs.ReadVisible(72410201, &v) && !v);
    CHECK(gs.ReadVisible(52410100, &v) && v);
    CHECK(gs.ReadVisible(72410060, &v) && v);  // kept
    CHECK(gm.Dirty() == 0 && gm.Apply(ov, guest.ops(), false, &r) && r.groups == 0);

    // liftfix: a held flag (a lift's position flag while the guest's own copy rides) keeps the
    // overlay's value through live and full applies and is no local change meanwhile; at the release
    // the overlay's value stands (the copy's own end; the image may be stale) and only a later host
    // change goes in
    gm.Hold(72410202, true);
    CHECK(gm.Held(72410202) && !gm.Held(72410203));
    host.SetPoolBit(72410202, false);
    host.SetPoolBit(72410203, true);
    out = WorldImage{};
    ht.Observe(host.pool.data(), &out);
    gm.Receive(out);
    CHECK(gm.Apply(ov, guest.ops(), false, &r) && r.bits == 1);
    CHECK(gs.ReadVisible(72410202, &v) && v);   // held: the guest's copy reads its own end
    CHECK(gs.ReadVisible(72410203, &v) && v);   // the rest of the group follows the host
    for (const auto& [id, val] : gm.LocalChanges(ov, guest.ops(), 64)) {
        CHECK(id != 72410202u && id != 72410203u);
        (void)val;
    }
    CHECK(gm.Apply(ov, guest.ops(), true, &r));  // a full copy keeps it too
    CHECK(gs.ReadVisible(72410202, &v) && v);
    gm.Hold(72410202, false);
    CHECK(gm.Dirty() == 0 && gm.Apply(ov, guest.ops(), false, &r) && r.bits == 0);
    CHECK(gs.ReadVisible(72410202, &v) && v);   // released: the copy's own end stands
    bool local = false;
    for (const auto& [id, val] : gm.LocalChanges(ov, guest.ops(), 64)) {
        local = local || (id == 72410202u && val);
    }
    CHECK(local);                                // ... and differs from the host's: the guest reports it
    host.SetPoolBit(72410202, true);             // the host's next change goes in as usual
    out = WorldImage{};
    ht.Observe(host.pool.data(), &out);
    gm.Receive(out);
    CHECK(gm.Apply(ov, guest.ops(), false, &r) && gs.ReadVisible(72410202, &v) && v);
    host.SetPoolBit(72410202, false);
    out = WorldImage{};
    ht.Observe(host.pool.data(), &out);
    gm.Receive(out);
    CHECK(gm.Apply(ov, guest.ops(), false, &r) && r.bits == 1 && gs.ReadVisible(72410202, &v) && !v);
    host.SetPoolBit(72410203, false);  // back as it was for the checks below
    host.SetPoolBit(72410202, true);
    out = WorldImage{};
    ht.Observe(host.pool.data(), &out);
    gm.Receive(out);
    CHECK(gm.Apply(ov, guest.ops(), false, &r) && gs.ReadVisible(72410202, &v) && v);

    // a rebuilt overlay (new buffers, zeroed): new identity, the full copy restores the host's world
    guest.Build(1);
    ovm.Build(guest);
    const FlagStore gs2 = guest.view();
    CHECK(gs2.OverlayIdentity() != id1);
    CHECK(gs2.OverlayGroups(&ov));
    CHECK(gm.Apply(ov, guest.ops(), true, &r) && r.groups == 3);
    CHECK(gs2.ReadVisible(72410202, &v) && v);
    CHECK(gs2.ReadVisible(52410100, &v) && v);
    CHECK(gs2.ReadVisible(2200, &v) && v);
    // a full image replaces: groups absent from it become zero
    WorldImage only_lamp;
    only_lamp.full = true;
    only_lamp.groups[u32(PoolKeyOfId(72410202))] = gm.Group(PoolKeyOfId(72410202));
    gm.Receive(only_lamp);
    CHECK(gm.Apply(ov, guest.ops(), false, &r));
    CHECK(gs2.ReadVisible(2200, &v) && !v);
    CHECK(gs2.ReadVisible(52410100, &v) && !v);
    CHECK(gs2.ReadVisible(72410202, &v) && v);

    // D3: the guest's own changes in the overlay are its local changes (not the host's)
    CHECK(gm.LocalChanges(ov, guest.ops(), 64).empty());
    guest.SetOverlayBit(12410200, true);   // the guest pulled a lever
    guest.SetOverlayBit(72410202, false);  // and something it cleared
    auto lc = gm.LocalChanges(ov, guest.ops(), 64);
    CHECK(lc.size() == 2);
    CHECK(std::find(lc.begin(), lc.end(), std::make_pair(u32(12410200), true)) != lc.end());
    CHECK(std::find(lc.begin(), lc.end(), std::make_pair(u32(72410202), false)) != lc.end());
    CHECK(gm.LocalChanges(ov, guest.ops(), 1).size() == 1);
    guest.SetOverlayBit(2005, true);  // a session flag is never a local change (not masked)
    CHECK(gm.LocalChanges(ov, guest.ops(), 64).size() == 2);
    // a host change not yet applied: that group is skipped (its difference may be the host's)
    WorldImage lever;
    lever.groups[u32(PoolKeyOfId(12410200))] = gm.Group(PoolKeyOfId(12410200));
    PutBit(lever.groups[u32(PoolKeyOfId(12410200))].data(), 12410200 % kBitsPerBlock, true);
    gm.Receive(lever);
    lc = gm.LocalChanges(ov, guest.ops(), 64);
    CHECK(lc.size() == 1 && lc[0].first == 72410202u);
    CHECK(gm.Apply(ov, guest.ops(), false, &r));
    lc = gm.LocalChanges(ov, guest.ops(), 64);
    CHECK(lc.size() == 1 && lc[0].first == 72410202u);  // the host took the lever: no longer local

    // D4: instanced items: the mirror leaves item pickup bits alone; they are no local changes
    CHECK(CategoryOf(52410100) == Category::item_lot_picked);
    CHECK(GetBit(WorldItemMask(PoolKeyOfId(52410100)).data(), 52410100 % kBitsPerBlock));
    CHECK(!GetBit(WorldItemMask(PoolKeyOfId(12410200)).data(), 12410200 % kBitsPerBlock));
    SetInstancedItems(true);
    guest.SetOverlayBit(52410100, true);  // the guest picked it up
    WorldImage items;
    items.groups[u32(PoolKeyOfId(52410101))] = gm.Group(PoolKeyOfId(52410101));
    PutBit(items.groups[u32(PoolKeyOfId(52410101))].data(), 52410101 % kBitsPerBlock, true);  // host took another
    gm.Receive(items);
    CHECK(gm.Apply(ov, guest.ops(), false, &r));
    CHECK(gs2.ReadVisible(52410101, &v) && !v);  // the host's pickup is not the guest's
    CHECK(gs2.ReadVisible(52410100, &v) && v);   // the guest's own stays
    lc = gm.LocalChanges(ov, guest.ops(), 64);
    CHECK(lc.size() == 1 && lc[0].first == 72410202u);  // item bits are reported by the store glue (own pool)
    SetInstancedItems(false);
    CHECK(gm.Apply(ov, guest.ops(), true, &r));
    CHECK(gs2.ReadVisible(52410101, &v) && v);  // instancing off: the host's world again
}

// Shop stock is per player (player report 2026-10-20: the host bought the Hunter set and it was
// sold out for the guest, who never had it): which flags, the mirror never carries them, and the
// guest's overlay / own pool merge.
static void TestShopStock() {
    // the id rule: every one-time ShopLineupParam row's stock flag (party_flags.inc "shop stock row")
    for (u32 id = 70009000; id <= 70009490; id += 10) {
        CHECK(ShopStockFlag(id));  // Bath Messengers' armor / emblem / ardeo, Insight shop sets, chalices
    }
    CHECK(ShopStockFlag(70009000) && ShopStockFlag(70009030));  // Yharnam Hunter Cap .. Trousers
    CHECK(ShopStockFlag(72101000) && ShopStockFlag(72101002) && ShopStockFlag(72101010) && ShopStockFlag(72101011));
    CHECK(ShopStockFlag(6610) && ShopStockFlag(6650) && ShopStockFlag(6660));  // bells, chalice (own pool anyway)
    CHECK(!ShopStockFlag(70009001) && !ShopStockFlag(70009495) && !ShopStockFlag(70009500));
    CHECK(!ShopStockFlag(72101003) && !ShopStockFlag(72101012) && !ShopStockFlag(72100100));  // Doll talk
    CHECK(!ShopStockFlag(70000030) && !ShopStockFlag(12401800) && !ShopStockFlag(6611) && !ShopStockFlag(0));
    CHECK(CategoryOf(70009000) == Category::never_sync && !HostCandidate(70009000));

    // the mirror never writes them; the shop mask holds them
    const int k9 = PoolKeyOfId(70009000), k21 = PoolKeyOfId(72101000);
    CHECK(WorldMirrorKey(k9) && WorldMirrorKey(k21));
    CHECK(!GetBit(WorldMirrorMask(k9).data(), 0) && GetBit(WorldShopMask(k9).data(), 0));
    CHECK(GetBit(WorldMirrorMask(k9).data(), 1) && !GetBit(WorldShopMask(k9).data(), 1));  // 70009001: mirrored
    CHECK(!GetBit(WorldMirrorMask(k21).data(), 0) && GetBit(WorldShopMask(k21).data(), 11));
    std::size_t shop_bits = 0;
    for (int key : WorldMirrorKeys()) {
        const GroupBytes &m = WorldMirrorMask(key), &sm = WorldShopMask(key);
        for (u32 i = 0; i < kBlockBytes; ++i) {
            CHECK((m[i] & sm[i]) == 0);
            shop_bits += std::size_t(__builtin_popcount(sm[i]));
        }
    }
    CHECK(shop_bits == 50 + 5);  // 70009000-70009490, 72101000-002 / 010-011 (66xx: block 6, own pool)
    const GroupBytes& own6 = WorldShopMask(PoolKeyOfId(6610));
    CHECK(std::none_of(own6.begin(), own6.end(), [](u8 b) { return b != 0; }));  // not a mirror key

    // the host bought the Yharnam Hunter Cap: its world image reaches the guest's overlay without it
    FakeStore host;
    host.Build(0);
    host.SetPoolBit(70009000, true);
    host.SetPoolBit(70009001, true);  // a non-shop bit of the same group still mirrors
    host.SetPoolBit(72101000, true);
    HostWorldTracker ht;
    ht.Observe(host.pool.data(), nullptr);
    FakeStore guest;
    guest.Build(1);
    guest.SetPoolBit(70009040, true);  // the guest bought Gehrman's Hunter Cap at home earlier
    FakeOverlayMap ovm;
    ovm.Build(guest);
    std::map<u32, u64> ov;
    CHECK(guest.view().OverlayGroups(&ov));
    GuestWorldMirror gm;
    gm.Receive(ht.Full(1));
    GuestWorldMirror::Result r;
    CHECK(gm.Apply(ov, guest.ops(), true, &r));
    CHECK(!guest.OverlayBit(70009000) && guest.OverlayBit(70009001) && !guest.OverlayBit(72101000));

    // merge: the guest's own stock shows, its purchase here is saved, nothing else is touched
    GroupBytes o{}, own{}, mask = WorldShopMask(k9);
    std::memcpy(o.data(), guest.overlay[70009].data(), kBlockBytes);
    std::memcpy(own.data(), guest.pool.data() + k9 * kBlockBytes, kBlockBytes);
    PutBit(own.data(), 5, true);  // 70009005: not a shop bit, never copied
    ShopMerge sm = MergeShopStock(70009, mask, &o, &own);
    CHECK(sm.saved.empty() && sm.shown == std::vector<u32>{70009040u});
    CHECK(GetBit(o.data(), 40) && !GetBit(o.data(), 5) && GetBit(o.data(), 1));
    PutBit(o.data(), 10, true);  // the guest buys the Yharnam Hunter Garb in the host's world
    sm = MergeShopStock(70009, mask, &o, &own);
    CHECK(sm.saved == std::vector<u32>{70009010u} && sm.shown.empty());
    CHECK(GetBit(own.data(), 10) && GetBit(own.data(), 40) && !GetBit(own.data(), 1) && !GetBit(own.data(), 0));
    sm = MergeShopStock(70009, mask, &o, &own);
    CHECK(sm.saved.empty() && sm.shown.empty());  // settled
    PutBit(o.data(), 10, false);  // a rebuilt overlay (zeroed): set-only, the purchase comes back
    sm = MergeShopStock(70009, mask, &o, &own);
    CHECK(sm.shown == std::vector<u32>{70009010u} && GetBit(own.data(), 10));

    // D3: a guest purchase is never a local change the host would take
    guest.SetOverlayBit(70009020, true);
    CHECK(gm.LocalChanges(ov, guest.ops(), 64).empty());
}

static void TestBbpf(const char* source_dir) {
    FakeStore f;
    f.Build(1);
    f.SetPoolBit(12401800, true);
    f.SetPoolBit(6100, true);  // personal block 6 is pooled in mode 1
    f.SetOverlayBit(2200, true);
    f.SetOverlayBit(12400999, true);
    FlagStore s = f.view();
    BbpfHeader h;
    h.map_id = 0x18000000;
    h.load_mode = 1;
    h.role = 6;
    h.unix_ms = 1234567890123ull;
    h.frame = 77;
    h.label = "guest1";
    const auto vis = CollectVisible(s);
    const auto pool = CollectPool(s);
    CHECK(vis.size() == kPoolBlocks && pool.size() == kPoolBlocks);
    const auto bytes = BbpfEncode(h, vis);
    CHECK(bytes.size() == 64 + kPoolBlocks * (8 + 125));
    BbpfHeader h2;
    std::vector<BbpfBlock> back;
    CHECK(BbpfDecode(bytes, &h2, &back));
    CHECK(h2.map_id == h.map_id && h2.load_mode == 1 && h2.role == 6 && h2.unix_ms == h.unix_ms && h2.frame == 77 &&
          h2.label == "guest1" && back.size() == vis.size());
    bool kinds_ok = true;
    for (const auto& b : back) {
        kinds_ok &= (b.block >= 6 && b.block <= 8) ? b.kind == 1 : b.kind == 2;
    }
    CHECK(kinds_ok);

    char path[512], ppath[512];
    std::snprintf(path, sizeof path, "%s/out/party_progress_test.bbpf", source_dir);
    std::snprintf(ppath, sizeof ppath, "%s/out/party_progress_test_pool.bbpf", source_dir);
    CHECK(BbpfWriteFile(path, h, vis));
    h.label = "guest1-pool";
    CHECK(BbpfWriteFile(ppath, h, pool));
    // tools/party/flag_tool.py dump must read them
    char cmd[1600];
    std::snprintf(cmd, sizeof cmd, "python -I \"%s/tools/party/flag_tool.py\" dump \"%s\" --set-only 2>&1", source_dir,
                  path);
    std::FILE* p = popen(cmd, "r");
    if (!p) {
        std::printf("bbpf: python not run (popen failed); the files are %s, %s\n", path, ppath);
        return;
    }
    std::string text;
    char line[512];
    while (std::fgets(line, sizeof line, p)) text += line;
    const int rc = pclose(p);
    if (text.find("not recognized") != std::string::npos || text.find("not found") != std::string::npos) {
        std::printf("bbpf: python not available; skipped the flag_tool check\n");
        return;
    }
    std::printf("bbpf: flag_tool.py dump (rc %d):\n%s", rc, text.c_str());
    CHECK(rc == 0);
    CHECK(text.find("label='guest1'") != std::string::npos);
    CHECK(text.find("load_mode=1") != std::string::npos && text.find("role=6") != std::string::npos);
    CHECK(text.find("\n2200\n") != std::string::npos);
    CHECK(text.find("\n6100\n") != std::string::npos);
    CHECK(text.find("\n12400999\n") != std::string::npos);
    CHECK(text.find("12401800") == std::string::npos);  // in the pool, not the visible tree
    std::snprintf(cmd, sizeof cmd, "python -I \"%s/tools/party/flag_tool.py\" dump \"%s\" --set-only 2>&1", source_dir,
                  ppath);
    p = popen(cmd, "r");
    text.clear();
    while (p && std::fgets(line, sizeof line, p)) text += line;
    CHECK(p && pclose(p) == 0);
    CHECK(text.find("label='guest1-pool'") != std::string::npos);
    CHECK(text.find("\n12401800\n") != std::string::npos && text.find("\n2200\n") == std::string::npos);
}

// 0.7.3 (user decision 2026-10-22): covenant outcomes are per player (flag_tool.py COVENANT_ROWS).
static void TestCovenantOutcomes() {
    // The classification: Annalise, Alfred and Valtr's quest state, their events and talk blocks.
    for (u32 id : {1040u, 1041u, 1042u, 1059u, 1340u, 1347u, 1348u, 1350u, 1359u, 1790u, 1791u, 1800u, 1809u, 9770u,
                   12400590u, 12400595u, 12420500u, 12500010u, 12500015u, 12500020u, 12500021u, 12500050u,
                   12500054u, 12700901u, 12700903u, 12700905u, 12700910u, 72400461u, 72400465u, 72400479u,
                   72500304u, 72500326u, 72500328u, 72500339u, 72500357u, 72500359u, 72700304u, 72700320u,
                   72700321u, 72700322u, 72700330u}) {
        if (CategoryOf(id) != Category::covenant_outcome) {
            std::fprintf(stderr, "  %u: %s\n", id, CategoryName(CategoryOf(id)));
        }
        CHECK(CategoryOf(id) == Category::covenant_outcome);
        CHECK(CategoryOf(id) == CategoryOfLinear(id));
        CHECK(BaseCategoryOf(id) != Category::covenant_outcome);
    }
    // What they were before (and are again while the host's rule is off).
    CHECK(BaseCategoryOf(1042) == Category::npc_quest && BaseCategoryOf(1350) == Category::npc_quest);
    CHECK(BaseCategoryOf(1790) == Category::npc_quest && BaseCategoryOf(9770) == Category::npc_quest);
    CHECK(BaseCategoryOf(12420500) == Category::cutscene_seen);
    CHECK(BaseCategoryOf(12500020) == Category::world_state);
    CHECK(BaseCategoryOf(12500050) == Category::npc_quest && BaseCategoryOf(12700901) == Category::npc_quest);
    CHECK(BaseCategoryOf(72500328) == Category::npc_quest && BaseCategoryOf(72700321) == Category::npc_quest);
    CHECK(BaseCategoryOf(12401800) == CategoryOf(12401800));  // every other id: the same
    CHECK(EffectiveCategory(1042, true) == Category::covenant_outcome);
    CHECK(EffectiveCategory(1042, false) == Category::npc_quest);
    CHECK(EffectiveCategory(12401800, true) == Category::boss_defeated);
    // Neighbours stay shared: the other NPC state machines (Eileen 1360, the priest 1780), the
    // gatekeeper / Arianna talk blocks, other Cainhurst / Forbidden Woods flags.
    for (u32 id : {1039u, 1060u, 1339u, 1360u, 1379u, 1780u, 1789u, 1810u, 9769u, 9771u, 72400459u, 72400480u,
                   72500340u, 72700340u, 72700361u, 12500022u, 12500049u}) {
        CHECK(CategoryOf(id) != Category::covenant_outcome);
    }
    CHECK(CategoryOf(12501800) == Category::boss_defeated);   // Martyr Logarius: shared
    CHECK(CategoryOf(12700902) == Category::item_lot_picked); // Valtr's successor award: C3
    CHECK(CategoryOf(12700906) == Category::item_lot_picked); // the rune award event: C3 / own gift
    CHECK(CategoryOf(9440) == Category::never_sync && CategoryOf(9451) == Category::never_sync);  // runes
    CHECK(!HostCandidate(6676) && !HostCandidate(6813) && CategoryOf(6676) != Category::covenant_outcome);  // personal
    CHECK(std::string(CategoryName(Category::covenant_outcome)) == "covenant_outcome");
    Category back = Category::never_sync;
    CHECK(CategoryFromName("covenant_outcome", &back) && back == Category::covenant_outcome);

    // Policy: per player while the rule is on, the base category's mode while it is off.
    Policy on = Policy::FromEnv(), off = Policy::FromEnv();
    off.covenant_per_player = false;
    CHECK(on.covenant_per_player && on != off);
    CHECK(ModeFor(on, 1042) == SyncMode::Off && ModeFor(on, 72500328) == SyncMode::Off);
    CHECK(ModeFor(on, 12420500) == SyncMode::Off && ModeFor(on, 12500020) == SyncMode::Off);
    CHECK(ModeFor(off, 1042) == SyncMode::Follow && ModeFor(off, 72500328) == SyncMode::Follow);
    CHECK(ModeFor(off, 12420500) == SyncMode::SetOnly && ModeFor(off, 12500020) == SyncMode::SetOnly);
    CHECK(ModeFor(on, 1141) == SyncMode::Follow && ModeFor(on, 12401800) == SyncMode::SetOnly);  // the rest: shared
    // The host keeps tracking them (a guest of a host with the rule off still gets them).
    CHECK(HostCandidate(1042) && HostCandidate(72700321) && HostCandidate(12420500));
    CHECK(CandidateMask(1) && GetBit(CandidateMask(1)->data(), 42));

    // The fixture of the live check (smoke cv): Alfred killed the queen in the host's world (1042
    // the flesh, Alfred 1348 in the castle, Valtr dead 1790), a boss the guest has not killed; the
    // guest's save has Annalise alive (1040), Alfred at the chapel (1344), Valtr's first state (1800).
    FakeStore host;
    host.Build(0);
    for (u32 id : {1042u, 1348u, 1790u, 72500328u, 12401800u, 1143u}) host.SetPoolBit(id, true);
    FlagStore hs = host.view();
    HostTracker tr(31);
    Scan(tr, hs);
    FlagSnapshot wire;
    std::string err;
    CHECK(SnapshotFromJson(SnapshotToJson(tr.Snapshot(0)), &wire, &err));
    for (const bool rule : {true, false}) {
        const Policy p = rule ? on : off;
        FakeStore guest;
        guest.Build(1);
        for (u32 id : {1040u, 1344u, 1800u, 1141u}) guest.SetPoolBit(id, true);
        FlagStore gs = guest.view();
        GuestApplier ap;
        ap.QueueSnapshot(wire, GuestApplier::PlanSnapshot(wire, p));
        CHECK(ap.Apply(gs, p, false) > 0);
        CHECK(guest.PoolBit(12401800) && guest.PoolBit(1143) && !guest.PoolBit(1141));  // shared progress
        if (rule) {
            CHECK(guest.PoolBit(1040) && !guest.PoolBit(1042));   // Annalise alive in the guest's save
            CHECK(guest.PoolBit(1344) && !guest.PoolBit(1348));   // its Alfred never went to the castle
            CHECK(guest.PoolBit(1800) && !guest.PoolBit(1790));   // its Valtr alive
            CHECK(!guest.PoolBit(72500328));
        } else {
            CHECK(!guest.PoolBit(1040) && guest.PoolBit(1042));   // as before 0.7.3: the host's copied
            CHECK(!guest.PoolBit(1344) && guest.PoolBit(1348));
            // (1790 and 1800 sit in two 20-flag groups of NpcStateGroup: the host's 1790 does not clear
            // the guest's 1800, as before)
            CHECK(guest.PoolBit(1790) && guest.PoolBit(72500328));
        }
        // Live changes: the host's Valtr turns hostile (1790 off, 1805 on): per player with the rule.
        ap.QueueChanges(31, {{1790, false, Category::covenant_outcome, wire.seq + 1},
                             {1805, true, Category::covenant_outcome, wire.seq + 2},
                             {12411700, true, Category::boss_defeated, wire.seq + 3}});
        ap.Apply(gs, p, false);
        CHECK(guest.PoolBit(12411700));
        CHECK(guest.PoolBit(1805) == !rule);
        // The rule switched after the snapshot was planned: Apply re-plans with the policy in force.
        FakeStore g2;
        g2.Build(1);
        g2.SetPoolBit(1040, true);
        FlagStore v2 = g2.view();
        GuestApplier a2;
        a2.QueueSnapshot(wire, GuestApplier::PlanSnapshot(wire, rule ? off : on));
        a2.Apply(v2, p, false);
        CHECK(g2.PoolBit(1042) == !rule && g2.PoolBit(1040) == rule);
    }
    // JSON: the category name travels.
    std::vector<FlagChange> ch{{1042, true, Category::covenant_outcome, 3}}, ch2;
    u64 ep = 0;
    CHECK(ChangesFromJson(ChangesToJson(5, ch), &ep, &ch2, &err) && ch2 == ch);
    // The rule's global (the party glue sets it each tick; default on).
    CHECK(CovenantPerPlayer());
    SetCovenantPerPlayer(false);
    CHECK(!CovenantPerPlayer());
    SetCovenantPerPlayer(true);
}

int main(int argc, char** argv) {
    const char* source_dir = argc > 1 ? argv[1] : BB_SOURCE_DIR;
    TestKeys();
    TestBits();
    TestCategories();
    TestPolicy();
    TestStore();
    TestDiffAndDedupe();
    TestJson();
    TestApplier();
    TestNpcFollow();
    TestWorldMirror();
    TestShopStock();
    TestCovenantOutcomes();
    TestBbpf(source_dir);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
