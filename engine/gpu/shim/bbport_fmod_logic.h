// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the pure rules of the FMOD sound-ring guard in bbport_stability.cpp (unit-tested in
// tests/test_stability_guards.cpp). From droogie/bbhost (GPL-3.0-or-later), src/engine/fmod_ring.h,
// commits 4d9fb54 and f1d4fa3; rewritten for this port.
//
// FMOD Ex 4.44.50 in the eboot keeps every sound it has made (a bank's samples, a stream and its
// subsound) in the master sound group's ring: SoundI +0x1d0 is a node (next, prev, the sound).
// setSoundGroup links a sound in and SoundI::release (guest +0xdbe120) takes it out before freeing it,
// both under FMOD's global critical section. A sound whose memory is freed while still linked leaves a
// dead member in the ring: the next sound linked or unlinked beside it writes through it into whatever
// the heap has put there since (a crash on arriving at Rom's lake: a render item's listener node was
// overwritten).
//
// FMOD's release unlinks a sound before freeing it, with one exception: a stream of several subsounds
// (a voice bank's) shares one subsound object (+0xb0, whose +0xb0 names itself), and the stream's
// release asks it to release itself, ignores the answer and frees it (guest +0xdbe682). The object
// refuses, and so stays linked, while its flags say 0x200 (an event's programmer sound in use), 0x100
// (preloaded) or 0x20 (a release under way).
#pragma once

#include <cstdint>
#include <cstring>

namespace BbFmod {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kNode = 0x1d0;        // the ring node: next, prev, the sound
constexpr u64 kFlags = 0x50;        // 0x20 being released, 0x100 preloaded, 0x200 programmer sound in use
constexpr u64 kShared = 0xb0;       // the subsound object a stream of several subsounds shares
constexpr u64 kParent = 0xc0;
constexpr u64 kOpenState = 0x1c0;   // FMOD_OPENSTATE
constexpr u64 kStreamSample = 0x238; // a stream's sample buffer, which its subsound object shares
constexpr u32 kOpenStateError = 2;
/// The blocks SoundI::release frees besides the object: name, subsound table, two sync-point buffers,
/// the sync-point list (their {size, flags} header sits 8 bytes before each block).
constexpr u64 kOwnedBlocks[] = {0x30, 0xd0, 0x198, 0x1a0, 0x1b0};

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
inline void Wr(u64 a, u64 v) { std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof(v)); }
inline void Wr32(u64 a, u32 v) { std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof(v)); }

/// The four SoundI classes' vtables as the objects hold them (SampleSoftware, SoundI, bank container,
/// Stream), runtime addresses.
struct Vtables {
    u64 vt[4];
};
inline bool IsSound(const Vtables& v, u64 obj) {
    const u64 t = Rd(obj);
    for (const u64 x : v.vt) {
        if (t == x) return true;
    }
    return false;
}

/// In a ring: the node names the sound and both neighbours point back at it.
template <typename Readable>
bool Linked(u64 obj, Readable readable) {
    const u64 node = obj + kNode;
    const u64 next = Rd(node), prev = Rd(node + 8);
    if (Rd(node + 16) != obj || next == node || prev == node) return false;
    if (!readable(next, 16) || !readable(prev, 8)) return false;
    return Rd(next + 8) == node && Rd(prev) == node;
}

/// Takes a linked sound out of its ring and leaves the node as SoundI::release does (linked to itself,
/// no data). False when it was not linked or its neighbours do not point back at it (nothing written).
template <typename Readable>
bool Unlink(u64 obj, Readable readable) {
    if (!Linked(obj, readable)) return false;
    const u64 node = obj + kNode, next = Rd(node), prev = Rd(node + 8);
    Wr(prev, next);
    Wr(next + 8, prev);
    Wr(node, node);
    Wr(node + 8, node);
    Wr(node + 16, 0);
    return true;
}

/// A stream's shared subsound (its +0xb0 names itself) that refused its own release when the stream
/// released it.
constexpr u32 kRefusing = 0x20 | 0x100 | 0x200;
inline bool RefusedShared(u64 obj) { return Rd(obj + kShared) == obj && (Rd32(obj + kFlags) & kRefusing) != 0; }

/// What the guard makes of such a subsound instead of letting its stream free it: a sound of its own
/// (no parent, no shared block, no stream sample) whose open state is FMOD_OPENSTATE_ERROR, so that
/// nothing plays it again (an event that starts its programmer sound once more gets the same object
/// back, and would read through its cleared codec). An event's programmer sound kept so is never freed:
/// a leak of a few hundred bytes each time. One whose release was under way on another thread (0x20) is
/// freed by that release, which now finds no shared block.
inline void Orphan(u64 obj) {
    Wr(obj + kParent, 0);
    Wr(obj + kShared, 0);
    Wr(obj + kStreamSample, 0);
    Wr32(obj + kOpenState, kOpenStateError);
}

/// The bytes FMOD's allocator recorded for a sound and the blocks it owns.
template <typename Readable>
u64 OwnedBytes(u64 obj, Readable readable) {
    u64 n = readable(obj - 8, 4) ? Rd32(obj - 8) : 0;
    for (const u64 off : kOwnedBlocks) {
        const u64 b = Rd(obj + off);
        if (b && readable(b - 8, 4)) n += Rd32(b - 8);
    }
    return n;
}

/// What the MemPool::free hook does with a block about to be freed: nothing, take it out of the ring
/// first, or take it out and keep it (skip the free).
enum class FreeAction { kLeave, kUnlink, kUnlinkAndKeep };
/// `linked_now`: the block is a linked sound; `from_shared_free`: the free's caller is the stream's
/// free of its shared subsound.
inline FreeAction ChooseFree(bool linked_now, bool from_shared_free, bool refused_shared) {
    if (!linked_now) return FreeAction::kLeave;
    return from_shared_free && refused_shared ? FreeAction::kUnlinkAndKeep : FreeAction::kUnlink;
}

} // namespace BbFmod
