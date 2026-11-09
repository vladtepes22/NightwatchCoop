// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the pure rules of the Scaleform HUD guards in bbport_stability.cpp (unit-tested in
// tests/test_stability_guards.cpp). From droogie/bbhost (GPL-3.0-or-later), src/engine/sf_text_layout.h,
// commits 7fea28a, 4b18a86, 46b2f8c, 8551763, 3e609c7; rewritten for this port. bbhost names
// Binary Ninja addresses (image + 0x400000); offsets of structures are the same, code addresses here
// are guest offsets (image + 0).
//
// Scaleform 4's render tree in eboot 1.09: every movie's display list is mirrored in a render context
// (Context, MovieImpl +0x5350). Its nodes are 56-byte entries on 4 KiB pages; a page holds 72 of them
// from +0x38, and its header names the snapshot being built (+0x18), that snapshot's data table (+0x20)
// and the table of the snapshot on display (+0x28). A table lists each entry's node data from +0x28
// (a pointer whose bit 0 tags it).
//
// A text node's data (TreeText::NodeData) gets its layout (+0x98) only in a capture's propagate pass,
// from its document view when its layout-changed flag (+0xa0 bit 0) is set. The render side's text
// cache node (TreeCacheText) reads that layout in its transform update (guest +0x12d5e0) whenever the
// update carries flag bit 0, without a null check. A text changed from another thread after a capture's
// propagate pass is published with its flag set and no layout: a null read at guest +0x12d6c0.
// The other thread is the game's HUD update (+0x1bf7c20), which drives the HUD movie with GFx SetText /
// GotoAndPlay and no lock, while the movie's own jobs (Advance + Capture, NextCapture) hold the movie
// wrapper's lock (+0x38, its scePthreadMutex at +0x40).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace BbScaleform {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kPageMask = ~u64{0xfff};
constexpr u64 kPageEntries = 0x38; // Entry[72], 56 bytes each
constexpr u64 kPageDisplay = 0x28; // the displayed snapshot's table: what the render side reads
constexpr u64 kEntrySize = 56;
constexpr u64 kEntriesPerPage = 72;
constexpr u64 kTableData = 0x28; // table: the entries' data pointers, bit 0 a tag

constexpr u64 kTextLayout = 0x98; // TreeText::NodeData: its layout

constexpr u64 kNodeRoot = 0x18;  // TreeCacheNode: its TreeCacheRoot*
constexpr u64 kNodeEntry = 0x20; // its Entry
constexpr u64 kNodeFlags = 0x44; // update flags; bit 31 pending
constexpr u64 kNodeNext = 0x48;  // the root's pending list link
// TreeCacheRoot: its pending list's head, and the byte set while its tree update runs, when the game
// marks a node into the update's own lists instead.
constexpr u64 kRootPending = 0xa0;
constexpr u64 kRootInUpdate = 0xaa;

/// The transform update's flag that reads the layout, and the node flag the tree update turns into it
/// for a node it updates (the game's own mark for a node whose transform changed).
constexpr u32 kUpdateLayoutPart = 0x1;
constexpr u32 kNodeRetransform = 0x10000;
constexpr u32 kNodePending = 0x80000000u;
/// The change flag "the text's layout changed" HandleChanges gets.
constexpr u32 kChangeTextLayout = 0x400;

inline u64 Rd(u64 a) {
    u64 v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof(v));
    return v;
}
inline u32 Rd32(u64 a) {
    u32 v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof(v));
    return v;
}
inline u8 Rd8(u64 a) { return *reinterpret_cast<const u8*>(static_cast<std::uintptr_t>(a)); }
inline void Wr(u64 a, u64 v) { std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof(v)); }
inline void Wr32(u64 a, u32 v) { std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof(v)); }

/// An entry's index on its page, as the game computes it: the byte offset from the first entry over 8,
/// times the inverse of 7 mod 2^32 (56 = 8 * 7), in 32 bits.
inline u32 EntryIndex(u64 entry) {
    const u64 page = entry & kPageMask;
    const u32 q = static_cast<u32>((entry - (page + kPageEntries)) >> 3);
    return q * 0xb6db6db7u;
}

/// The entry's data in the table at page offset `which`, the tag cleared; 0 when anything on the way
/// cannot be read. `readable(addr, len)` says whether memory there can be read.
template <typename Readable>
u64 EntryData(u64 entry, u64 which, Readable readable) {
    if (!readable(entry, 8)) return 0;
    const u64 page = entry & kPageMask;
    if (entry < page + kPageEntries) return 0;
    const u32 i = EntryIndex(entry);
    if (i >= kEntriesPerPage || !readable(page, kPageEntries)) return 0;
    const u64 table = Rd(page + which);
    const u64 slot = table + kTableData + u64{i} * 8;
    if (!table || !readable(slot, 8)) return 0;
    return Rd(slot) & ~u64{1};
}

/// The guard's decision for the transform update of the text cache node `node` with `flags`.
struct Decision {
    u32 flags;   ///< what the update runs with
    bool skipped; ///< the layout part was taken out
    u64 data;    ///< the displayed data (when skipped)
};
template <typename Readable>
Decision Decide(u64 node, u32 flags, Readable readable) {
    if (!(flags & kUpdateLayoutPart) || !readable(node + kNodeEntry, 8)) return {flags, false, 0};
    const u64 data = EntryData(Rd(node + kNodeEntry), kPageDisplay, readable);
    // Data the game could not read either is left to the game: this guards the missing layout, not a
    // broken tree.
    if (!data || !readable(data + kTextLayout, 8) || Rd(data + kTextLayout) != 0) return {flags, false, 0};
    return {flags & ~kUpdateLayoutPart, true, data};
}

/// Marks a cache node for its root's next tree update, as the game's own mark does while it takes a
/// capture's changes: onto the root's pending list unless it is pending already, with `flags` and the
/// pending bit. False (nothing written) when the node has no root, or the root's update is under way
/// (a node left pending after it could be destroyed before the next one drains the list).
template <typename Readable>
bool MarkForUpdate(u64 node, u32 flags, Readable readable) {
    if (!readable(node + kNodeRoot, 8)) return false;
    const u64 root = Rd(node + kNodeRoot);
    if (!root || !readable(root + kRootPending, 16) || !readable(node + kNodeFlags, 12)) return false;
    if (Rd8(root + kRootInUpdate) != 0) return false;
    const u32 f = Rd32(node + kNodeFlags);
    if (!(f & kNodePending)) {
        Wr(node + kNodeNext, Rd(root + kRootPending));
        Wr(root + kRootPending, node);
    }
    Wr32(node + kNodeFlags, f | flags | kNodePending);
    return true;
}

/// A movie wrapper seen for `movie` that no longer holds it: gone, its destructor ran (it clears the
/// movie, +0x18, first), or its memory went to something else. Its table entry may go to another movie.
template <typename Readable>
bool WrapperStale(u64 movie, u64 wrapper, Readable readable) {
    return !wrapper || !readable(wrapper + 0x18, 8) || Rd(wrapper + 0x18) != movie;
}

/// The table of movie wrappers seen (the Advance job names them): an entry per movie, found by the
/// movie. A new movie takes a free entry; when none is free, one whose wrapper no longer holds its movie
/// (a long session makes and drops far more movies than are alive at once; bbhost's 64 entries filled
/// up and the lock silently stopped working); with neither it is left out, counted. The game's own list
/// of live wrappers holds 32, so the size leaves room for the dead ones not yet taken again.
constexpr int kWrapperTableSize = 512;
struct WrapperEntry {
    std::atomic<u64> movie{0}, wrapper{0};
};
struct WrapperTable {
    WrapperEntry* entries;
    int size; ///< in use of the entries (a test shrinks it)
    std::atomic<u64> reused{0}, left_out{0};
};

inline WrapperEntry* TableFind(WrapperTable& t, u64 movie) {
    for (int i = 0; i < t.size; ++i) {
        if (t.entries[i].movie.load(std::memory_order_acquire) == movie) return &t.entries[i];
    }
    return nullptr;
}

/// Wrapper `w`, which holds `movie` (the caller read it): the movie's entry, made or taken again;
/// nullptr when the table has no room (counted).
template <typename Readable>
WrapperEntry* TableNote(WrapperTable& t, u64 w, u64 movie, Readable readable) {
    if (WrapperEntry* e = TableFind(t, movie)) {
        if (e->wrapper.load(std::memory_order_relaxed) != w) e->wrapper.store(w, std::memory_order_release);
        return e;
    }
    for (int i = 0; i < t.size; ++i) {
        WrapperEntry& e = t.entries[i];
        u64 expected = 0;
        if (e.movie.load(std::memory_order_relaxed) == 0 && e.movie.compare_exchange_strong(expected, movie)) {
            e.wrapper.store(w, std::memory_order_release);
            return &e;
        }
    }
    for (int i = 0; i < t.size; ++i) {
        WrapperEntry& e = t.entries[i];
        u64 old = e.movie.load(std::memory_order_acquire);
        if (!old || !WrapperStale(old, e.wrapper.load(std::memory_order_acquire), readable)) continue;
        if (!e.movie.compare_exchange_strong(old, movie)) continue;
        e.wrapper.store(w, std::memory_order_release);
        t.reused.fetch_add(1, std::memory_order_relaxed);
        return &e;
    }
    t.left_out.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

/// Text cache nodes whose layout part was taken out, until their layout arrives (bbhost: 64). Only
/// compared with the node HandleChanges is given (a live one), never followed, so a node destroyed
/// meanwhile costs nothing; one made at the same address later is marked once for nothing. When full,
/// the oldest-by-count slot is reused (the guard still keeps the game up; the node just is not marked).
class WaitSet {
public:
    static constexpr int kSlots = 64;
    void Add(u64 node) {
        std::lock_guard<std::mutex> lk(mu_);
        int free_slot = -1;
        for (int i = 0; i < kSlots; ++i) {
            if (slot_[i] == node) return;
            if (!slot_[i] && free_slot < 0) free_slot = i;
        }
        if (free_slot < 0) {
            free_slot = int(evict_++ % kSlots);
        } else {
            n_.fetch_add(1, std::memory_order_relaxed);
        }
        slot_[free_slot] = node;
    }
    bool Take(u64 node) {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < kSlots; ++i) {
            if (slot_[i] == node) {
                slot_[i] = 0;
                n_.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }
    int Count() const { return n_.load(std::memory_order_relaxed); }

private:
    std::mutex mu_;
    u64 slot_[kSlots] = {};
    u64 evict_ = 0;
    std::atomic<int> n_{0};
};

/// The lock decision of a HUD update: what the acquire of the wrapper's mutex gave.
enum class LockResult { kLocked = 0, kAlreadyHeld = 1, kTimedOut = 2, kNoMutex = 3 };
/// Whether the update must give the mutex back afterwards.
inline bool MustRelease(int acquire_result) { return acquire_result == int(LockResult::kLocked); }

/// A wait on the wrapper's lock longer than this runs the update as the game has it (a guard against a
/// lock order the game never takes), counted.
constexpr u64 kLockWaitUs = 100000;

} // namespace BbScaleform
