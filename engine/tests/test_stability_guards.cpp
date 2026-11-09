// SPDX-License-Identifier: GPL-3.0-or-later
// The pure rules of the Scaleform HUD guards and the FMOD sound-ring guard
// (gpu/shim/bbport_scaleform_logic.h, bbport_fmod_logic.h; sites in bbport_stability.cpp). The game's
// structures are laid out in host memory with synthetic data, as the eboot lays them out.
// Run from test_stability_logic.cpp's main. Build and run: ninja -C out/gpu stability-logic-test &&
// out/gpu/stability-logic-test.exe
#include "bbport_fmod_logic.h"
#include "bbport_scaleform_logic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
static void* AlignedAlloc(std::size_t size, std::size_t align) { return _aligned_malloc(size, align); }
static void AlignedFree(void* p) { _aligned_free(p); }
#else
static void* AlignedAlloc(std::size_t size, std::size_t align) { return std::aligned_alloc(align, size); }
static void AlignedFree(void* p) { std::free(p); }
#endif

namespace {
int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using u64 = std::uint64_t;
using u32 = std::uint32_t;
const auto kAll = [](u64, std::size_t) { return true; };
u64 A(const void* p) { return reinterpret_cast<u64>(p); }

// ---- Scaleform ----------------------------------------------------------------------------
// A render-tree page with one text entry (index `slot`), a text data object with or without a layout, a
// cache node pointing at the entry and a root.
struct Tree {
    unsigned char* page;  // 4 KiB aligned
    unsigned char* table; // the displayed snapshot's data table
    unsigned char* data;  // TreeText::NodeData
    unsigned char* node;  // TreeCacheText
    unsigned char* root;  // TreeCacheRoot
    u64 entry;
    explicit Tree(u32 slot, bool layout) {
        page = static_cast<unsigned char*>(AlignedAlloc(4096, 4096));
        std::memset(page, 0, 4096);
        table = new unsigned char[0x28 + 72 * 8]();
        data = new unsigned char[0x100]();
        node = new unsigned char[0x100]();
        root = new unsigned char[0x100]();
        entry = A(page) + BbScaleform::kPageEntries + u64{slot} * BbScaleform::kEntrySize;
        BbScaleform::Wr(A(page) + BbScaleform::kPageDisplay, A(table));
        BbScaleform::Wr(A(table) + BbScaleform::kTableData + u64{slot} * 8, A(data) | 1); // tagged, as the game keeps it
        BbScaleform::Wr(A(data) + BbScaleform::kTextLayout, layout ? 0x1234567 : 0);
        BbScaleform::Wr(A(node) + BbScaleform::kNodeEntry, entry);
        BbScaleform::Wr(A(node) + BbScaleform::kNodeRoot, A(root));
    }
    ~Tree() {
        AlignedFree(page);
        delete[] table;
        delete[] data;
        delete[] node;
        delete[] root;
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
};

void TestEntryIndex() {
    using namespace BbScaleform;
    Tree t(0, true);
    const u64 page = A(t.page);
    for (u32 i = 0; i < kEntriesPerPage; ++i) {
        CHECK(EntryIndex(page + kPageEntries + u64{i} * kEntrySize) == i);
    }
}

void TestDecide() {
    using namespace BbScaleform;
    for (const u32 slot : {0u, 1u, 33u, 71u}) {
        Tree none(slot, false), laid(slot, true);
        // The update that reads the layout, a text without one: the part is taken out.
        Decision d = Decide(A(none.node), 0x5, kAll);
        CHECK(d.skipped && d.flags == 0x4 && d.data == A(none.data));
        d = Decide(A(none.node), 0x1, kAll);
        CHECK(d.skipped && d.flags == 0);
        // A text with its layout, and an update that does not read it: left alone.
        d = Decide(A(laid.node), 0x1, kAll);
        CHECK(!d.skipped && d.flags == 0x1);
        d = Decide(A(none.node), 0x6, kAll);
        CHECK(!d.skipped && d.flags == 0x6);
    }
    // Data the game could not read either (no table / unreadable memory) is left to the game.
    Tree broken(3, false);
    BbScaleform::Wr(A(broken.page) + kPageDisplay, 0);
    CHECK(!Decide(A(broken.node), 1, kAll).skipped);
    Tree t(3, false);
    CHECK(!Decide(A(t.node), 1, [](u64, std::size_t) { return false; }).skipped);
    // A table entry beyond the page's 72 entries is not read.
    Tree wild(0, false);
    BbScaleform::Wr(A(wild.node) + kNodeEntry, A(wild.page) + 4000);
    CHECK(!Decide(A(wild.node), 1, kAll).skipped);
    // The tag in bit 0 is cleared before the data is read.
    Tree tagged(5, false);
    CHECK(EntryData(tagged.entry, kPageDisplay, kAll) == A(tagged.data));
}

void TestMark() {
    using namespace BbScaleform;
    Tree t(2, true);
    CHECK(MarkForUpdate(A(t.node), kNodeRetransform, kAll));
    CHECK(Rd(A(t.root) + kRootPending) == A(t.node));
    CHECK(Rd32(A(t.node) + kNodeFlags) == (kNodeRetransform | kNodePending));
    // Marked again while pending: flags merge, the list is not pushed twice (that would loop it).
    Wr(A(t.node) + kNodeNext, 0x77);
    CHECK(MarkForUpdate(A(t.node), 0x2, kAll));
    CHECK(Rd(A(t.node) + kNodeNext) == 0x77);
    CHECK(Rd32(A(t.node) + kNodeFlags) == (kNodeRetransform | 0x2 | kNodePending));
    // A second node goes in front of the first.
    Tree u(4, true);
    Wr(A(u.node) + kNodeRoot, A(t.root));
    CHECK(MarkForUpdate(A(u.node), kNodeRetransform, kAll));
    CHECK(Rd(A(t.root) + kRootPending) == A(u.node) && Rd(A(u.node) + kNodeNext) == A(t.node));
    // The root's update under way: nothing is written.
    Tree v(6, true);
    t.root[kRootInUpdate] = 1;
    Wr(A(v.node) + kNodeRoot, A(t.root));
    const u64 head = Rd(A(t.root) + kRootPending);
    CHECK(!MarkForUpdate(A(v.node), kNodeRetransform, kAll));
    CHECK(Rd(A(t.root) + kRootPending) == head && Rd32(A(v.node) + kNodeFlags) == 0);
    // No root.
    Wr(A(v.node) + kNodeRoot, 0);
    CHECK(!MarkForUpdate(A(v.node), kNodeRetransform, kAll));
}

void TestWaitSet() {
    BbScaleform::WaitSet w;
    CHECK(w.Count() == 0 && !w.Take(5));
    w.Add(5);
    w.Add(5);
    CHECK(w.Count() == 1 && w.Take(5) && !w.Take(5) && w.Count() == 0);
    for (u64 i = 1; i <= 200; ++i) {
        w.Add(i); // more than it holds: the game stays up, the count stays bounded
    }
    CHECK(w.Count() == BbScaleform::WaitSet::kSlots);
    CHECK(w.Take(200));
}

// ---- the movie table ----------------------------------------------------------------------
// Wrapper objects in host memory: +0x18 holds the movie while alive, 0 once its destructor ran.
struct Wrappers {
    std::vector<unsigned char*> w;
    u64 Make(u64 movie) {
        auto* p = new unsigned char[0x100]();
        BbScaleform::Wr(A(p) + 0x18, movie);
        w.push_back(p);
        return A(p);
    }
    void Die(u64 wrapper) { BbScaleform::Wr(wrapper + 0x18, 0); } // memory stays, as a freed guest block does
    ~Wrappers() {
        for (auto* p : w) delete[] p;
    }
};

void TestTableChurn() {
    using namespace BbScaleform;
    // bbhost's 64-entry table filled up in a long session: here a session makes and drops thousands of
    // movies, 32 alive at most, and every live movie stays found with the lock's wrapper.
    {
        std::vector<WrapperEntry> store(kWrapperTableSize);
        WrapperTable t{store.data(), kWrapperTableSize};
        Wrappers ws;
        std::vector<std::pair<u64, u64>> live; // movie, wrapper
        u64 next_movie = 0x100000;
        for (int round = 0; round < 6000; ++round) {
            if (live.size() >= 32) { // the oldest one is dropped
                ws.Die(live.front().second);
                live.erase(live.begin());
            }
            const u64 movie = next_movie += 0x5350;
            const u64 w = ws.Make(movie);
            live.emplace_back(movie, w);
            CHECK(TableNote(t, w, movie, kAll) != nullptr);
            for (const auto& [m, wr] : live) {
                WrapperEntry* e = TableFind(t, m);
                CHECK(e && e->wrapper.load() == wr);
            }
        }
        CHECK(t.left_out.load() == 0);
        CHECK(t.reused.load() > 0);
    }
    // A movie made again at the address of a dead one takes its entry over with the new wrapper.
    {
        std::vector<WrapperEntry> store(8);
        WrapperTable t{store.data(), 8};
        Wrappers ws;
        const u64 w1 = ws.Make(0xabc000), w2 = ws.Make(0xabc000);
        CHECK(TableNote(t, w1, 0xabc000, kAll) != nullptr);
        ws.Die(w1);
        WrapperEntry* e = TableNote(t, w2, 0xabc000, kAll);
        CHECK(e && e->wrapper.load() == w2 && TableFind(t, 0xabc000) == e);
    }
    // More live movies than entries: the surplus is left out, counted, and the others stay found.
    {
        std::vector<WrapperEntry> store(4);
        WrapperTable t{store.data(), 4};
        Wrappers ws;
        for (u64 i = 1; i <= 6; ++i) {
            TableNote(t, ws.Make(i * 0x1000), i * 0x1000, kAll);
        }
        CHECK(t.left_out.load() == 2);
        CHECK(TableFind(t, 0x1000) && TableFind(t, 0x4000) && !TableFind(t, 0x5000));
    }
    // 100 live movies all fit in the real table (the old one stopped at 64).
    {
        std::vector<WrapperEntry> store(kWrapperTableSize);
        WrapperTable t{store.data(), kWrapperTableSize};
        Wrappers ws;
        for (u64 i = 1; i <= 100; ++i) {
            CHECK(TableNote(t, ws.Make(i * 0x1000), i * 0x1000, kAll));
        }
        CHECK(t.left_out.load() == 0 && TableFind(t, 100 * 0x1000));
    }
    CHECK(WrapperStale(0x1000, 0, kAll)); // no wrapper: stale
}

void TestLockResult() {
    using namespace BbScaleform;
    CHECK(MustRelease(int(LockResult::kLocked)));
    CHECK(!MustRelease(int(LockResult::kAlreadyHeld)));
    CHECK(!MustRelease(int(LockResult::kTimedOut)));
    CHECK(!MustRelease(int(LockResult::kNoMutex)));
}

// ---- FMOD ---------------------------------------------------------------------------------
struct Sound {
    unsigned char b[0x400];
    Sound() { std::memset(b, 0, sizeof(b)); }
    u64 addr() const { return A(b); }
    u64 node() const { return A(b) + BbFmod::kNode; }
};
// A ring head and its members, linked as FMOD links them: node = {next, prev, the sound}.
struct Ring {
    u64 head[2];
    void Link(Sound& s, u64 vtable) {
        BbFmod::Wr(s.addr(), vtable);
        BbFmod::Wr(s.node() + 16, s.addr());
        const u64 last = head[1];
        BbFmod::Wr(s.node(), A(head));      // next: back to the head
        BbFmod::Wr(s.node() + 8, last);     // prev
        BbFmod::Wr(last, s.node());         // the last one's next
        head[1] = s.node();
    }
};
struct Fresh {
    Ring ring;
    Fresh() {
        ring.head[0] = A(ring.head);
        ring.head[1] = A(ring.head);
    }
};

void TestFmod() {
    using namespace BbFmod;
    const Vtables v{{0x10, 0x20, 0x30, 0x40}};
    Fresh f;
    Sound a, b, c;
    f.ring.Link(a, 0x20);
    f.ring.Link(b, 0x40);
    f.ring.Link(c, 0x20);
    CHECK(IsSound(v, a.addr()) && IsSound(v, b.addr()));
    Sound notsound;
    CHECK(!IsSound(v, notsound.addr()));
    CHECK(Linked(a.addr(), kAll) && Linked(b.addr(), kAll) && Linked(c.addr(), kAll));
    // A sound released as FMOD releases it (node linked to itself) is not linked.
    Sound lone;
    Wr(lone.addr(), 0x20);
    Wr(lone.node(), lone.node());
    Wr(lone.node() + 8, lone.node());
    CHECK(!Linked(lone.addr(), kAll) && !Unlink(lone.addr(), kAll));
    // Taking the middle member out leaves a sound ring: the neighbours point at each other.
    CHECK(Unlink(b.addr(), kAll));
    CHECK(Rd(a.node()) == c.node() && Rd(c.node() + 8) == a.node());
    CHECK(Rd(b.node()) == b.node() && Rd(b.node() + 8) == b.node() && Rd(b.node() + 16) == 0);
    CHECK(!Linked(b.addr(), kAll) && !Unlink(b.addr(), kAll)); // not twice
    // The ends: the first (next of the head) and the last.
    CHECK(Unlink(a.addr(), kAll));
    CHECK(f.ring.head[0] == c.node() && Rd(c.node() + 8) == A(f.ring.head));
    CHECK(Unlink(c.addr(), kAll));
    CHECK(f.ring.head[0] == A(f.ring.head) && f.ring.head[1] == A(f.ring.head));
    // A node whose neighbours do not point back is left alone (nothing written).
    Fresh g;
    Sound x, y;
    g.ring.Link(x, 0x20);
    g.ring.Link(y, 0x20);
    Wr(y.node() + 8, 0x1234); // prev points elsewhere
    const bool unreadable_prev = Unlink(y.addr(), [](u64 a, std::size_t) { return a >= 0x10000; });
    CHECK(!unreadable_prev);
    CHECK(Rd(x.node()) == y.node());
}

void TestFmodRefusing() {
    using namespace BbFmod;
    Sound s;
    Wr(s.addr() + kShared, s.addr()); // a stream's shared subsound names itself
    Wr32(s.addr() + kFlags, 0);
    CHECK(!RefusedShared(s.addr()));
    for (const u32 flag : {0x20u, 0x100u, 0x200u, 0x300u}) {
        Wr32(s.addr() + kFlags, flag);
        CHECK(RefusedShared(s.addr()));
    }
    Wr32(s.addr() + kFlags, 0x1);
    CHECK(!RefusedShared(s.addr()));
    // A sound that is not a shared subsound (its +0xb0 names another) is not a refusing one.
    Wr32(s.addr() + kFlags, 0x200);
    Wr(s.addr() + kShared, 0x9999);
    CHECK(!RefusedShared(s.addr()));
    // What the free does with it.
    CHECK(ChooseFree(false, true, true) == FreeAction::kLeave);
    CHECK(ChooseFree(true, false, true) == FreeAction::kUnlink);
    CHECK(ChooseFree(true, true, false) == FreeAction::kUnlink);
    CHECK(ChooseFree(true, true, true) == FreeAction::kUnlinkAndKeep);
}

void TestFmodOrphan() {
    using namespace BbFmod;
    Sound s;
    Wr(s.addr() + kParent, 0x111);
    Wr(s.addr() + kShared, s.addr());
    Wr(s.addr() + kStreamSample, 0x222);
    Wr32(s.addr() + kOpenState, 0);
    Wr32(s.addr() + kFlags, 0x200);
    Orphan(s.addr());
    CHECK(Rd(s.addr() + kParent) == 0 && Rd(s.addr() + kShared) == 0 && Rd(s.addr() + kStreamSample) == 0);
    CHECK(Rd32(s.addr() + kOpenState) == kOpenStateError);
    CHECK(Rd32(s.addr() + kFlags) == 0x200); // the event still holds it
    CHECK(!RefusedShared(s.addr()));         // and it no longer looks like a shared subsound
    // The bytes it owns: its own block plus the blocks it points at ({size, flags} 8 bytes before each).
    unsigned char blocks[3][0x40] = {};
    Wr(s.addr() + 0x30, A(blocks[0]) + 8);
    Wr32(A(blocks[0]), 24);
    Wr(s.addr() + 0xd0, A(blocks[1]) + 8);
    Wr32(A(blocks[1]), 100);
    CHECK(OwnedBytes(s.addr(), [&](u64 a, std::size_t) { return a >= A(blocks[0]) && a < A(blocks[2]) + 0x40; }) == 124);
}

} // namespace

int RunGuardTests() {
    TestEntryIndex();
    TestDecide();
    TestMark();
    TestWaitSet();
    TestTableChurn();
    TestLockResult();
    TestFmod();
    TestFmodRefusing();
    TestFmodOrphan();
    std::printf("stability guards: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
