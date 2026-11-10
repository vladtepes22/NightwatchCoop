// SPDX-License-Identifier: GPL-3.0-or-later
// HUD text and sound crash guards (visy 0.8.0 aed502bfb, from droogie/bbhost, GPL-3.0-or-later: 7fea28a, 4b18a86,
// 46b2f8c, 8551763, 3e609c7, 4d9fb54, f1d4fa3). visy's sections 4 and 5 of his bbport_stability.cpp with the stub
// helpers they use, as a file of their own (our tree has no bbport_stability; as bbport_gx_query_guard.cpp). Each site
// is compared byte for byte with the 1.09 code first; a site that differs is left alone with a log line. Switches
// (default on):
//   BB_SF_MOVIE_LOCK=0     the game's HUD update (+0x1bf7c20) runs without its movie's lock (the game's own)
//   BB_SF_TEXT_GUARD=0     a text shown before its layout exists is not guarded (+0x12d5e0 / +0x12df80)
//   BB_FMOD_RING_GUARD=0   a sound freed while still in FMOD's sound ring is not taken out first (+0xdab3a0)
//   BB_MOVIE_DRAW_WAIT=0   the attract movie's frame draw / close (+0x28fe7c0 / +0x28fe2e0) do not wait for GXRenderThread
#include "bbport_crash_guards.h"
#include "bbport_scaleform_logic.h"
#include "bbport_fmod_logic.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" void* runtime_low_map(size_t size, int prot); // src/runtime_memory.c
extern "C" int runtime_memory_is_mapped(std::uintptr_t address, std::uint64_t size); // src/runtime_memory.c
extern "C" int runtime_guest_mutex_acquire(void** slot, std::uint64_t usec, std::uint64_t* waited_us); // runtime_mutex.c
extern "C" void runtime_guest_mutex_release(void** slot);

#define BB_SYSV __attribute__((sysv_abi))

namespace BbCrashGuards {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;

u8* image = nullptr;
u64 image_size = 0;

bool On(const char* name) {
    const char* e = std::getenv(name);
    return !(e && e[0] == '0');
}

u64 Rd64(u64 address) {
    u64 v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), 8);
    return v;
}
u32 Rd32(u64 address) {
    u32 v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), 4);
    return v;
}

/// The image holds `bytes` at guest offset `off`.
bool Matches(u64 off, std::initializer_list<u8> bytes) {
    if (off + bytes.size() > image_size) {
        return false;
    }
    return std::memcmp(image + off, bytes.begin(), bytes.size()) == 0;
}

/// A `call rel32` at guest offset `off` to guest offset `target`.
bool CallsTo(u64 off, u64 target) {
    if (off + 5 > image_size || image[off] != 0xe8) {
        return false;
    }
    std::int32_t rel;
    std::memcpy(&rel, image + off + 1, 4);
    return off + 5 + std::int64_t(rel) == target;
}

// ---- Stubs next to the image (rel32 reach) ----
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 16384;

u8* MapNear(std::size_t size) {
    const u64 base = reinterpret_cast<u64>(image);
#ifdef _WIN32
    // The image is the first block of the runtime's low guest area, which is reserved for the
    // guest as a whole: the next low block lies right after the image, within rel32 reach (as in
    // bbport_game_menu.cpp). Fixed hints around the image all fall into that reservation.
    if (void* p = runtime_low_map(size, 3 /* read | write; VirtualProtect later */)) {
        const u64 at = reinterpret_cast<u64>(p);
        if (at >= base && at + size <= base + (2ull << 30) - (1ull << 20)) {
            return static_cast<u8*>(p);
        }
    }
#endif
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (16ull << 20), base + image_size + k * (16ull << 20)}) {
#ifdef _WIN32
            void* p = VirtualAlloc(reinterpret_cast<void*>(hint & ~u64(0xffff)), size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
            if (p) {
                return static_cast<u8*>(p);
            }
#else
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
#endif
        }
    }
    return nullptr;
}

bool Rel32(u64 from_next, u64 to, std::int32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::int32_t(d);
    return true;
}

struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    u64 at = 0; ///< where the stub will be placed
    bool ok = true;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            code[size++] = b;
        }
    }
    void U32(u32 v) {
        std::memcpy(&code[size], &v, 4);
        size += 4;
    }
    void U64(u64 v) {
        std::memcpy(&code[size], &v, 8);
        size += 8;
    }
    /// jmp rel32 / jz rel32 to an absolute address (in the image).
    void JmpTo(u64 target) {
        Bytes({0xe9});
        std::int32_t rel = 0;
        ok &= Rel32(at + size + 4, target, rel);
        U32(u32(rel));
    }
    void SkipRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); } // lea rsp, [rsp - 128]
    void BackRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); } // lea rsp, [rsp + 128]
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
    /// jmp [rip]; dq fn - an absolute jump.
    void JmpAbs(const void* fn) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(reinterpret_cast<u64>(fn));
    }
};

/// A new emitter for the next stub in the page.
Emitter NewStub() {
    Emitter e;
    e.at = reinterpret_cast<u64>(stub_page + stub_used);
    e.ok = stub_page != nullptr;
    return e;
}

/// Copies `e` into the stub page; returns its address, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!e.ok || !stub_page || stub_used + e.size > StubPageSize ||
        e.at != reinterpret_cast<u64>(stub_page + stub_used)) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

/// Writes `jmp stub` (or `call stub`, opcode 0xe8) at guest offset `off`, `nops` more bytes after.
bool WriteBranch(u64 off, u8 opcode, u64 stub, std::size_t pad) {
    std::int32_t rel = 0;
    const u64 address = reinterpret_cast<u64>(image + off);
    if (!stub || !Rel32(address + 5, stub, rel)) {
        return false;
    }
    image[off] = opcode;
    std::memcpy(image + off + 1, &rel, 4);
    // NOPs for the rest of the instructions replaced (never executed: the jump leaves first).
    for (std::size_t i = 0; i < pad; ++i) {
        image[off + 5 + i] = 0x90;
    }
    return true;
}

// ======================================================================================
// 4. Scaleform HUD race and text-layout guard (bbhost src/engine/sf_text_layout.cpp, 7fea28a,
// 4b18a86, 46b2f8c, 8551763, 3e609c7; the rules are in bbport_scaleform_logic.h).
//  a. The game's HUD update (+0x1bf7c20: bars, enemy tags, boss list, talk window, map name, event
//     text) drives the HUD movie through GFx SetText / GotoAndPlay with no lock, beside the movie's
//     own jobs (Advance + Capture +0x1f58e50, NextCapture +0x1f59150), which hold the movie wrapper's
//     lock (+0x38, a recursive mutex over the scePthreadMutex at +0x40). BB_SF_MOVIE_LOCK=0: off.
//     The update now takes that mutex too: a stub in front of it finds the item's movie the way the
//     update itself does first (+0x1adb8a0: the movie handle at +0x20, whose method 0 returns the
//     movie; +0x1cfb0 lets that reference go), the movie's wrapper among the Advance jobs seen, and
//     calls the original under the mutex. A thread that holds it already runs as is; a wait past
//     100 ms runs as the game does, counted. Lock order: the game runs the update itself on the main
//     thread only while holding data_5994200, which NextCapture also takes before +0x38; Advance
//     never takes it.
//  b. A text changed after a capture's propagate pass is published with its layout-changed flag set
//     and no layout, and the text cache node's transform update (+0x12d5e0) reads +0x1c of the null
//     layout (fault at +0x12d6c0). BB_SF_TEXT_GUARD=0: off. A text node whose displayed data has no
//     layout runs the update without flag bit 0 (the part that reads the layout); when a later
//     capture brings the layout (HandleChanges +0x12df80, change 0x400), the node is marked for the
//     update with 0x10000, the flag the tree update turns into bit 0.
// Not ported from bbhost: the stranded-propagate-list repair (restrand / destrand at the capture's
// call sites) and the detector; with the lock in place the race that strands an entry no longer
// happens, and the guard keeps the game up where it does.
// ======================================================================================
constexpr u64 kHudUpdate = 0x1bf7c20;
constexpr u8 kHudUpdatePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                     0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x28, 0x07, 0x00, 0x00};
constexpr u64 kAdvanceJob = 0x1f58e50;
constexpr u8 kAdvanceJobPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                      0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00};
constexpr u64 kUpdateTransform = 0x12d5e0;
constexpr u8 kUpdateTransformPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                           0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff};
constexpr u64 kHandleChanges = 0x12df80;
constexpr u8 kHandleChangesPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                         0x53, 0x50, 0x89, 0xd3, 0x49, 0x89, 0xf6};
constexpr u64 kMovieRelease = 0x1cfb0;
constexpr u8 kMovieReleasePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53};
constexpr u64 kHudItemMovie = 0x20; // the item's movie handle
constexpr u64 kWrapperMovie = 0x18, kWrapperMutex = 0x40;

BbScaleform::WrapperEntry sf_entries[BbScaleform::kWrapperTableSize];
BbScaleform::WrapperTable sf_table{sf_entries, BbScaleform::kWrapperTableSize};
BbScaleform::WaitSet sf_waiting;
std::atomic<u64> sf_runs{0}, sf_waits{0}, sf_wait_us{0}, sf_wait_max{0}, sf_timeouts{0}, sf_no_wrapper{0}, sf_already{0},
    sf_no_mutex{0}, sf_skipped{0}, sf_marked{0}, sf_unmarked{0};
bool sf_lock_on = false, sf_guard_on = false;

bool GameReads(u64 a, std::size_t) { return a >= 0x10000; }
bool Mapped(u64 a, std::size_t n) { return a >= 0x10000 && runtime_memory_is_mapped(std::uintptr_t(a), n) != 0; }
bool LogThis(u64 n) { return n <= 16 || (n & (n - 1)) == 0; }

/// The Advance job's prologue stub: the wrapper (rdi) holds a movie.
BB_SYSV void SfNoteWrapper(u64 w) {
    if (!Mapped(w + kWrapperMovie, 8)) {
        return;
    }
    const u64 movie = Rd64(w + kWrapperMovie);
    if (!movie) {
        return;
    }
    const u64 reused = sf_table.reused.load(std::memory_order_relaxed);
    const u64 left_out = sf_table.left_out.load(std::memory_order_relaxed);
    BbScaleform::TableNote(sf_table, w, movie, Mapped);
    if (sf_table.reused.load(std::memory_order_relaxed) > reused && reused == 0) {
        std::printf("Stability: sf lock: the movie table took its first entry again (movie %#llx)\n", ull(movie));
    }
    if (sf_table.left_out.load(std::memory_order_relaxed) > left_out && left_out == 0) {
        std::printf("Stability: sf lock: the movie table is full of live movies (%d): movie %#llx is left out, the HUD "
                    "lock does not see it\n", sf_table.size, ull(movie));
    }
}

void SfReport();
using GetMovieFn = void(BB_SYSV*)(u64* out, u64 handle);
using ReleaseFn = void(BB_SYSV*)(u64 movie);

/// The item's movie as its handle's method 0 gives it (a Ptr<Movie> through rdi), the reference that
/// call took let go again: the handle keeps its own.
u64 HudMovie(u64 item) {
    if (item < 0x10000) {
        return 0;
    }
    const u64 handle = Rd64(item + kHudItemMovie);
    if (handle < 0x10000) {
        return 0;
    }
    const u64 get = Rd64(Rd64(handle));
    if (get < reinterpret_cast<u64>(image) || get >= reinterpret_cast<u64>(image) + image_size) {
        return 0;
    }
    u64 movie = 0;
    reinterpret_cast<GetMovieFn>(get)(&movie, handle);
    if (movie) {
        reinterpret_cast<ReleaseFn>(reinterpret_cast<u64>(image) + kMovieRelease)(movie);
    }
    return movie;
}

/// The HUD update's stub, before the original: takes the movie wrapper's mutex when it can. Returns the
/// mutex slot to give back afterwards (SfHudUnlock), or 0.
BB_SYSV u64 SfHudLock(u64 item) {
    const u64 movie = HudMovie(item);
    BbScaleform::WrapperEntry* e = movie ? BbScaleform::TableFind(sf_table, movie) : nullptr;
    const u64 w = e ? e->wrapper.load(std::memory_order_acquire) : 0;
    if (!w || !Mapped(w + kWrapperMovie, 8) || Rd64(w + kWrapperMovie) != movie) {
        if (sf_no_wrapper.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::printf("Stability: sf lock: the HUD update ran before its movie's Advance job was seen (movie %#llx): "
                        "it runs as the game has it until then\n", ull(movie));
        }
        return 0;
    }
    void** slot = reinterpret_cast<void**>(w + kWrapperMutex);
    std::uint64_t us = 0;
    const int rc = runtime_guest_mutex_acquire(slot, BbScaleform::kLockWaitUs, &us);
    if (us) {
        sf_waits.fetch_add(1, std::memory_order_relaxed);
        sf_wait_us.fetch_add(us, std::memory_order_relaxed);
        u64 cur = sf_wait_max.load(std::memory_order_relaxed);
        while (us > cur && !sf_wait_max.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
        }
    }
    switch (BbScaleform::LockResult(rc)) {
    case BbScaleform::LockResult::kLocked:
        {
            const u64 n = sf_runs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1) {
                std::printf("Stability: sf lock: the HUD update runs under the lock of movie %#llx (wrapper %#llx)\n",
                            ull(movie), ull(w));
            } else if (n == 10000 || n == 100000 || n % 1000000 == 0) {
                SfReport(); // the game is often ended without a normal exit: a running tally
            }
        }
        break;
    case BbScaleform::LockResult::kAlreadyHeld:
        sf_already.fetch_add(1, std::memory_order_relaxed);
        break;
    case BbScaleform::LockResult::kTimedOut: {
        const u64 n = sf_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (LogThis(n)) {
            std::printf("Stability: sf lock: the HUD update waited %llu us for its movie's lock (wrapper %#llx) and ran "
                        "without it, as the game has it (%llu so far)\n", ull(us), ull(w), ull(n));
        }
        break;
    }
    default:
        sf_no_mutex.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    return BbScaleform::MustRelease(rc) ? reinterpret_cast<u64>(slot) : 0;
}

BB_SYSV void SfHudUnlock(u64 slot) {
    if (slot) {
        runtime_guest_mutex_release(reinterpret_cast<void**>(slot));
    }
}

/// The transform update's stub (rdi: the text cache node, esi: its flags in r8d): the flags to run with.
BB_SYSV u64 SfTextFlags(u64 node, u64 flags) {
    const BbScaleform::Decision d = BbScaleform::Decide(node, u32(flags), GameReads);
    if (!d.skipped) {
        return flags;
    }
    sf_waiting.Add(node);
    const u64 n = sf_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (LogThis(n)) {
        std::printf("Stability: sf text: a text is shown before its layout exists (cache node %#llx, data %#llx): its "
                    "transform update runs without the layout's part, done again once the layout arrives (%llu so "
                    "far)\n", ull(node), ull(d.data), ull(n));
    }
    return d.flags;
}

/// HandleChanges' stub (rdi: the node, esi: the change flags).
BB_SYSV void SfTextChanged(u64 node, u64 changes) {
    if (!(u32(changes) & BbScaleform::kChangeTextLayout) || sf_waiting.Count() == 0) {
        return;
    }
    const u64 data = BbScaleform::EntryData(Rd64(node + BbScaleform::kNodeEntry), BbScaleform::kPageDisplay, GameReads);
    if (!data || Rd64(data + BbScaleform::kTextLayout) == 0 || !sf_waiting.Take(node)) {
        return;
    }
    // Its layout is on display now: the part the guard took out runs in this capture's tree update.
    const bool marked = BbScaleform::MarkForUpdate(node, BbScaleform::kNodeRetransform, GameReads);
    const u64 n = (marked ? sf_marked : sf_unmarked).fetch_add(1, std::memory_order_relaxed) + 1;
    if (LogThis(n)) {
        std::printf("Stability: sf text: cache node %#llx has its layout now - %s (%llu so far)\n", ull(node),
                    marked ? "marked for its transform update" : "not marked (no root, or its update under way)", ull(n));
    }
}

void SfReport() {
    if (sf_lock_on && (sf_runs || sf_timeouts || sf_no_wrapper || sf_already)) {
        std::printf("Stability: sf lock: the HUD update ran %llu time(s) under its movie's lock; %llu waited (%llu us in "
                    "all, longest %llu us), %llu waited past %llu ms and ran without it; %llu before its movie's Advance "
                    "job was seen, %llu with the lock held already, %llu without a usable mutex; movie table: %llu "
                    "entries taken again, %llu movies left out\n",
                    ull(sf_runs), ull(sf_waits), ull(sf_wait_us), ull(sf_wait_max), ull(sf_timeouts),
                    ull(BbScaleform::kLockWaitUs / 1000), ull(sf_no_wrapper), ull(sf_already), ull(sf_no_mutex),
                    ull(sf_table.reused.load()), ull(sf_table.left_out.load()));
    }
    if (sf_guard_on && (sf_skipped || sf_marked || sf_unmarked)) {
        std::printf("Stability: sf text: %llu text update(s) ran without their layout's part; %llu node(s) marked again "
                    "when it arrived, %llu not marked, %d still waiting\n",
                    ull(sf_skipped), ull(sf_marked), ull(sf_unmarked), sf_waiting.Count());
    }
    std::fflush(stdout);
}

/// The image holds `n` bytes `bytes` at guest offset `off`.
bool MatchesBytes(u64 off, const u8* bytes, std::size_t n) {
    return off + n <= image_size && std::memcmp(image + off, bytes, n) == 0;
}

/// The moved prologue of the function at `off` and a jump back past it: the original, callable.
u64 PlaceTrampoline(u64 off, const u8* prologue, std::size_t n) {
    Emitter t = NewStub();
    for (std::size_t i = 0; i < n; ++i) {
        t.Bytes({prologue[i]});
    }
    t.JmpTo(reinterpret_cast<u64>(image) + off + n);
    return PlaceStub(t);
}

bool InstallHudLock() {
    const std::size_t n = sizeof(kHudUpdatePrologue);
    if (!MatchesBytes(kHudUpdate, kHudUpdatePrologue, n) ||
        !MatchesBytes(kAdvanceJob, kAdvanceJobPrologue, sizeof(kAdvanceJobPrologue)) ||
        !MatchesBytes(kMovieRelease, kMovieReleasePrologue, sizeof(kMovieReleasePrologue))) {
        std::printf("Stability: sf lock: guest +0x%llx / +0x%llx is not the expected code; left alone\n", ull(kHudUpdate),
                    ull(kAdvanceJob));
        return false;
    }
    const u64 base = reinterpret_cast<u64>(image);
    // The Advance job notes its wrapper, then runs as it was.
    Emitter a = NewStub();
    a.SaveVolatile();
    a.CallAbs(reinterpret_cast<const void*>(&SfNoteWrapper)); // rdi: the wrapper
    a.RestoreVolatile();
    for (const u8 b : kAdvanceJobPrologue) {
        a.Bytes({b});
    }
    a.JmpTo(base + kAdvanceJob + sizeof(kAdvanceJobPrologue));
    const u64 advance_stub = PlaceStub(a);
    const u64 original = PlaceTrampoline(kHudUpdate, kHudUpdatePrologue, n);
    if (!advance_stub || !original) {
        std::printf("Stability: sf lock: no stub space; left alone\n");
        return false;
    }
    // The HUD update: item (rdi), state (rsi), dt (xmm0), run under the wrapper's mutex. Frame: 56 bytes
    // (xmm0 at 0, the mutex slot at 16, rax at 24) under the six saved argument registers.
    Emitter e = NewStub();
    e.Bytes({0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51}); // push rdi, rsi, rdx, rcx, r8, r9
    e.Bytes({0x48, 0x83, 0xec, 0x38});                         // sub rsp, 56
    e.Bytes({0x0f, 0x11, 0x04, 0x24});                         // movups [rsp], xmm0
    e.CallAbs(reinterpret_cast<const void*>(&SfHudLock));      // rdi: the item
    e.Bytes({0x48, 0x89, 0x44, 0x24, 0x10});                   // mov [rsp + 16], rax
    e.Bytes({0x48, 0x8b, 0x7c, 0x24, 0x60});                   // mov rdi, [rsp + 96]
    e.Bytes({0x48, 0x8b, 0x74, 0x24, 0x58});                   // mov rsi, [rsp + 88]
    e.Bytes({0x48, 0x8b, 0x54, 0x24, 0x50});                   // mov rdx, [rsp + 80]
    e.Bytes({0x48, 0x8b, 0x4c, 0x24, 0x48});                   // mov rcx, [rsp + 72]
    e.Bytes({0x4c, 0x8b, 0x44, 0x24, 0x40});                   // mov r8, [rsp + 64]
    e.Bytes({0x4c, 0x8b, 0x4c, 0x24, 0x38});                   // mov r9, [rsp + 56]
    e.Bytes({0x0f, 0x10, 0x04, 0x24});                         // movups xmm0, [rsp]
    e.Bytes({0xe8});                                           // call the original
    std::int32_t rel = 0;
    e.ok &= Rel32(e.at + e.size + 4, original, rel);
    e.U32(u32(rel));
    e.Bytes({0x48, 0x89, 0x44, 0x24, 0x18});                   // mov [rsp + 24], rax
    e.Bytes({0x0f, 0x11, 0x04, 0x24});                         // movups [rsp], xmm0
    e.Bytes({0x48, 0x8b, 0x7c, 0x24, 0x10});                   // mov rdi, [rsp + 16]
    e.CallAbs(reinterpret_cast<const void*>(&SfHudUnlock));
    e.Bytes({0x48, 0x8b, 0x44, 0x24, 0x18});                   // mov rax, [rsp + 24]
    e.Bytes({0x0f, 0x10, 0x04, 0x24});                         // movups xmm0, [rsp]
    e.Bytes({0x48, 0x83, 0xc4, 0x68});                         // add rsp, 104 (frame and the six saved registers)
    e.Bytes({0xc3});                                           // ret
    const u64 hud_stub = PlaceStub(e);
    if (!hud_stub) {
        std::printf("Stability: sf lock: no stub space; left alone\n");
        return false;
    }
    return WriteBranch(kAdvanceJob, 0xe9, advance_stub, sizeof(kAdvanceJobPrologue) - 5) &&
           WriteBranch(kHudUpdate, 0xe9, hud_stub, n - 5);
}

bool InstallTextGuard() {
    if (!MatchesBytes(kUpdateTransform, kUpdateTransformPrologue, sizeof(kUpdateTransformPrologue)) ||
        !MatchesBytes(kHandleChanges, kHandleChangesPrologue, sizeof(kHandleChangesPrologue))) {
        std::printf("Stability: sf text: guest +0x%llx / +0x%llx is not the expected code; left alone\n",
                    ull(kUpdateTransform), ull(kHandleChanges));
        return false;
    }
    const u64 base = reinterpret_cast<u64>(image);
    // UpdateTransform(node = rdi, ..., flags = r8d): the flags come back in the saved r8's place.
    Emitter u = NewStub();
    u.SaveVolatile();
    u.Bytes({0x44, 0x89, 0xc6}); // mov esi, r8d
    u.CallAbs(reinterpret_cast<const void*>(&SfTextFlags));
    u.Bytes({0x49, 0x89, 0x44, 0x24, 0x20}); // mov [r12 + 32], rax (the saved r8)
    u.RestoreVolatile();
    for (const u8 b : kUpdateTransformPrologue) {
        u.Bytes({b});
    }
    u.JmpTo(base + kUpdateTransform + sizeof(kUpdateTransformPrologue));
    const u64 update_stub = PlaceStub(u);
    // HandleChanges(node = rdi, root, changes = edx).
    Emitter h = NewStub();
    h.SaveVolatile();
    h.Bytes({0x89, 0xd6}); // mov esi, edx
    h.CallAbs(reinterpret_cast<const void*>(&SfTextChanged));
    h.RestoreVolatile();
    for (const u8 b : kHandleChangesPrologue) {
        h.Bytes({b});
    }
    h.JmpTo(base + kHandleChanges + sizeof(kHandleChangesPrologue));
    const u64 changes_stub = PlaceStub(h);
    if (!update_stub || !changes_stub) {
        std::printf("Stability: sf text: no stub space; left alone\n");
        return false;
    }
    return WriteBranch(kUpdateTransform, 0xe9, update_stub, sizeof(kUpdateTransformPrologue) - 5) &&
           WriteBranch(kHandleChanges, 0xe9, changes_stub, sizeof(kHandleChangesPrologue) - 5);
}

// ======================================================================================
// 5. FMOD sound-ring guard (bbhost src/engine/fmod_ring.cpp, 4d9fb54, f1d4fa3; the rules are in
// bbport_fmod_logic.h). BB_FMOD_RING_GUARD=0: off. FMOD's MemPool free (+0xdab3a0) first takes a sound
// that is still linked out of the master sound group's ring, under FMOD's global critical section (the
// slot behind the pointer at +0x546d260). At the stream's free of a refusing shared subsound (the call
// at +0xdbe682) it also keeps the object, as a sound of its own whose open state is ERROR, so that the
// event or the release still holding it does not use freed memory; it is never played again.
// ======================================================================================
constexpr u64 kMemPoolFree = 0xdab3a0;
constexpr u8 kMemPoolFreePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                       0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr u64 kSharedFreeReturn = 0xdbe687; // after the stream's call of the free
constexpr u64 kFmodCrit = 0x546d260;        // pointer to FMOD's critical section mutex slot
constexpr u64 kSoundVtables[4] = {0x52d7640, 0x52d7840, 0x52d7a30, 0x52d7c30};
BbFmod::Vtables fmod_vt{};
std::atomic<u64> fmod_unlinked{0}, fmod_kept{0}, fmod_kept_bytes{0};

/// MemPool::free(pool = rdi, ptr = rsi, ...): ret is the free's return address. Non-zero: the free is
/// not run (its caller ignores the result).
BB_SYSV u64 FmodFreeGuard(u64 /*pool*/, u64 p, u64 ret) {
    if (p < 0x10000 || !BbFmod::IsSound(fmod_vt, p)) {
        return 0;
    }
    const bool linked_now = BbFmod::Linked(p, GameReads);
    if (!linked_now) {
        return 0;
    }
    const BbFmod::FreeAction action = BbFmod::ChooseFree(
        linked_now, ret == reinterpret_cast<u64>(image) + kSharedFreeReturn, BbFmod::RefusedShared(p));
    void** slot = nullptr;
    if (const u64 c = Rd64(reinterpret_cast<u64>(image) + kFmodCrit)) {
        slot = reinterpret_cast<void**>(c);
    }
    std::uint64_t waited = 0;
    const int rc = slot ? runtime_guest_mutex_acquire(slot, 5000000, &waited) : 3;
    if (rc == int(BbScaleform::LockResult::kTimedOut) || rc == int(BbScaleform::LockResult::kNoMutex)) {
        return 0; // the critical section cannot be taken: the game's own free
    }
    const bool done = BbFmod::Unlink(p, GameReads);
    const bool keep = done && action == BbFmod::FreeAction::kUnlinkAndKeep;
    if (keep) {
        BbFmod::Orphan(p);
    }
    if (BbScaleform::MustRelease(rc)) {
        runtime_guest_mutex_release(slot);
    }
    if (!done) {
        return 0;
    }
    const u64 n = fmod_unlinked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (keep) {
        fmod_kept.fetch_add(1, std::memory_order_relaxed);
        fmod_kept_bytes.fetch_add(BbFmod::OwnedBytes(p, GameReads), std::memory_order_relaxed);
    }
    if (LogThis(n)) {
        std::printf("Stability: fmod ring: a sound was freed while still in its sound group (sound %#llx, flags %#x): "
                    "%s (%llu so far, %llu kept)\n", ull(p), BbFmod::Rd32(p + BbFmod::kFlags),
                    keep ? "taken out of the ring and kept as a sound of its own that nothing plays again"
                         : "taken out of the ring first",
                    ull(n), ull(fmod_kept.load(std::memory_order_relaxed)));
    }
    return keep ? 1 : 0;
}

void FmodReport() {
    if (fmod_unlinked) {
        std::printf("Stability: fmod ring: the guard took %llu freed sound(s) out of the ring; kept %llu refusing shared "
                    "subsound(s) (about %llu bytes never freed)\n",
                    ull(fmod_unlinked), ull(fmod_kept), ull(fmod_kept_bytes));
        std::fflush(stdout);
    }
}

bool InstallFmodGuard() {
    if (!MatchesBytes(kMemPoolFree, kMemPoolFreePrologue, sizeof(kMemPoolFreePrologue)) ||
        !CallsTo(kSharedFreeReturn - 5, kMemPoolFree)) {
        std::printf("Stability: fmod ring: guest +0x%llx is not the expected code; left alone\n", ull(kMemPoolFree));
        return false;
    }
    const u64 base = reinterpret_cast<u64>(image);
    for (int i = 0; i < 4; ++i) {
        fmod_vt.vt[i] = base + kSoundVtables[i];
    }
    Emitter e = NewStub();
    e.SaveVolatile();
    e.Bytes({0x49, 0x8b, 0x54, 0x24, 0x50}); // mov rdx, [r12 + 80] (the return address)
    e.CallAbs(reinterpret_cast<const void*>(&FmodFreeGuard));
    e.Bytes({0x49, 0x89, 0x44, 0x24, 0x48}); // mov [r12 + 72], rax (the saved rax)
    e.RestoreVolatile();
    e.Bytes({0x85, 0xc0});                   // test eax, eax
    e.Bytes({0x74, 0x01});                   // jz run
    e.Bytes({0xc3});                         // ret (the free is not run)
    for (const u8 b : kMemPoolFreePrologue) {
        e.Bytes({b});
    }
    e.JmpTo(base + kMemPoolFree + sizeof(kMemPoolFreePrologue));
    return WriteBranch(kMemPoolFree, 0xe9, PlaceStub(e), sizeof(kMemPoolFreePrologue) - 5);
}

// ======================================================================================
// The title's attract movie waits for GXRenderThread (visy f54d3c8e0; bbhost 41d725b, droogie/bbhost, GPL-3.0;
// ported by intent). BB_MOVIE_DRAW_WAIT=0: the game's own calls. The movie player's frame draw
// (+0x28fe7c0, called every frame by CSMovieIns::STEP_Wait_Play on the main thread with GX's
// immediate context) and its close (+0x28fe2e0, from STEP_Finish_Play) use that context while
// GXRenderThread still works on it (its last batch's pending state, the rename map's resolve): a
// null read in GX's commit (+0x26c01c3) or a null write in the map's resolve (+0x26ae211) a minute
// or two into the attract movie. The game's other main-thread code waits for the render thread's
// idle event first; these two do not. Now they do: until the event is set, bounded at 5 s (then the
// game's call goes ahead, logged). From the render thread itself nothing is waited for. Ours polls the
// event's flag (yields, then 1 ms sleeps) where visy's calls the event's own timed wait (vtable slot +0x18):
// the Live FPS clock patch set (Bloodborne.xml, 60 FPS++ Deltatime) writes a ret over that wait's timed
// call (+0x2083ec1), which the game never takes (it waits without a timeout), so a timed wait from here
// returned into its saved registers (host fault at 0x5 at the title movie). GX manager +0x348 = GXRenderThread; its idle event at +0x68
// (set once a batch and its flip are done); the thread object at +8, its pthread at thread +0x18.
// ======================================================================================
constexpr u64 kMovieDraw = 0x28fe7c0;
constexpr u64 kMovieClose = 0x28fe2e0;
constexpr u8 kMovieDrawPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                     0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff};
constexpr u8 kMovieClosePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xfb};
constexpr u64 kGxManager = 0x55406c8;      // pointer to GX's manager
constexpr u64 kGxImmediateWrapper = 0x10;  // its immediate context's wrapper; the context at +8
constexpr u64 kGxRenderThread = 0x348;
constexpr u64 kRtThread = 0x8, kThreadPthread = 0x18, kRtIdleEvent = 0x68;
constexpr u64 kEventFlags = 0x18;
constexpr u32 kEventSet = 2;
constexpr u64 kEventVtable = 0x52b97a0;    // the Dantelion2 event's vtable
constexpr u64 kPthreadSelf = 0x208ba90;    // j_scePthreadSelf
constexpr auto kMovieWaitSpin = std::chrono::milliseconds(2);  // yield this long, then sleep 1 ms steps
constexpr auto kMovieWaitBound = std::chrono::seconds(5);

using MovieDrawFn = BB_SYSV u64 (*)(u64, u64, u64);
using MovieCloseFn = BB_SYSV u64 (*)(u64);
using SelfFn = BB_SYSV u64 (*)();
bool movie_wait_on = false;
MovieDrawFn movie_draw_orig = nullptr;
MovieCloseFn movie_close_orig = nullptr;
std::atomic<u64> movie_draws{0}, movie_immediate{0}, movie_busy{0}, movie_closes{0}, movie_closes_busy{0},
    movie_bounded{0}, movie_waited_us{0}, movie_longest_us{0};

/// Waits until GXRenderThread (object `rt`) is idle; no wait from the render thread itself. `busy` counts
/// the call when it had to wait. Bounded; the game's call goes ahead after the bound (logged).
void MovieWaitIdle(u64 rt, const char* what, u64 n, std::atomic<u64>& busy) {
    const u64 event = rt + kRtIdleEvent;
    if ((Rd32(event + kEventFlags) & kEventSet) != 0) {
        return;
    }
    const u64 base = reinterpret_cast<u64>(image);
    const u64 thread = Rd64(rt + kRtThread);
    if (thread >= 0x10000 && Rd64(thread + kThreadPthread) == reinterpret_cast<SelfFn>(image + kPthreadSelf)()) {
        return; // the render thread itself
    }
    if (Rd64(event) != base + kEventVtable) {
        return; // not the event we know
    }
    const u64 k = busy.fetch_add(1, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    bool done = true;
    while ((Rd32(event + kEventFlags) & kEventSet) == 0) {
        const auto waited = std::chrono::steady_clock::now() - t0;
        if (waited > kMovieWaitBound) {
            done = false;
            break;
        }
        if (waited < kMovieWaitSpin) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const u64 us = u64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    movie_waited_us.fetch_add(us, std::memory_order_relaxed);
    u64 longest = movie_longest_us.load(std::memory_order_relaxed);
    while (us > longest && !movie_longest_us.compare_exchange_weak(longest, us, std::memory_order_relaxed)) {
    }
    if (!done) {
        if (movie_bounded.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("Stability: movie: the %s %llu waited %llu ms for GXRenderThread to go idle and goes ahead without it\n",
                        what, ull(n), ull(us / 1000));
        }
    } else if (k == 0) {
        std::printf("Stability: movie: the %s %llu waited %llu us for GXRenderThread to finish its frame first\n", what, ull(n),
                    ull(us));
    }
}

/// GX's render thread object, or 0 before GX is up; `immediate` = GX's immediate context.
u64 MovieRenderThread(u64* immediate) {
    const u64 gx = Rd64(reinterpret_cast<u64>(image) + kGxManager);
    if (gx < 0x10000) {
        return 0;
    }
    const u64 wrapper = Rd64(gx + kGxImmediateWrapper);
    *immediate = wrapper >= 0x10000 ? Rd64(wrapper + 8) : 0;
    return wrapper >= 0x10000 ? Rd64(gx + kGxRenderThread) : 0;
}

BB_SYSV u64 MovieDraw(u64 player, u64 ctx, u64 out) {
    const u64 n = movie_draws.fetch_add(1, std::memory_order_relaxed);
    u64 immediate = 0;
    const u64 rt = MovieRenderThread(&immediate);
    if (rt >= 0x10000 && ctx && ctx == immediate) {
        movie_immediate.fetch_add(1, std::memory_order_relaxed);
        MovieWaitIdle(rt, "draw", n, movie_busy);
    }
    return movie_draw_orig(player, ctx, out);
}

BB_SYSV u64 MovieClose(u64 player) {
    const u64 n = movie_closes.fetch_add(1, std::memory_order_relaxed);
    u64 immediate = 0;
    const u64 rt = MovieRenderThread(&immediate);
    if (rt >= 0x10000) {
        MovieWaitIdle(rt, "close", n, movie_closes_busy);
    }
    return movie_close_orig(player);
}

void MovieReport() {
    if (movie_draws || movie_closes) {
        std::printf("Stability: movie: %llu frame draws (%llu on the immediate context, %llu waited for GXRenderThread), "
                    "%llu closes (%llu waited); waited %llu ms in all, longest %llu ms, %llu went ahead after the bound\n",
                    ull(movie_draws), ull(movie_immediate), ull(movie_busy), ull(movie_closes), ull(movie_closes_busy),
                    ull(movie_waited_us / 1000), ull(movie_longest_us / 1000), ull(movie_bounded));
        std::fflush(stdout);
    }
}

bool InstallMovieWait() {
    if (!MatchesBytes(kMovieDraw, kMovieDrawPrologue, sizeof(kMovieDrawPrologue)) ||
        !MatchesBytes(kMovieClose, kMovieClosePrologue, sizeof(kMovieClosePrologue))) {
        std::printf("Stability: movie: guest +0x%llx / +0x%llx is not the expected code; left alone\n", ull(kMovieDraw),
                    ull(kMovieClose));
        return false;
    }
    const u64 draw_orig = PlaceTrampoline(kMovieDraw, kMovieDrawPrologue, sizeof(kMovieDrawPrologue));
    const u64 close_orig = PlaceTrampoline(kMovieClose, kMovieClosePrologue, sizeof(kMovieClosePrologue));
    Emitter d = NewStub();
    d.JmpAbs(reinterpret_cast<const void*>(&MovieDraw));
    const u64 draw_stub = PlaceStub(d);
    Emitter c = NewStub();
    c.JmpAbs(reinterpret_cast<const void*>(&MovieClose));
    const u64 close_stub = PlaceStub(c);
    if (!draw_orig || !close_orig || !draw_stub || !close_stub) {
        std::printf("Stability: movie: no stub space; left alone\n");
        return false;
    }
    movie_draw_orig = reinterpret_cast<MovieDrawFn>(draw_orig);
    movie_close_orig = reinterpret_cast<MovieCloseFn>(close_orig);
    return WriteBranch(kMovieDraw, 0xe9, draw_stub, sizeof(kMovieDrawPrologue) - 5) &&
           WriteBranch(kMovieClose, 0xe9, close_stub, sizeof(kMovieClosePrologue) - 5);
}

} // namespace

void PatchImage(unsigned char* guest_image, std::uint64_t size) {
    image = guest_image;
    image_size = size;
    stub_page = MapNear(StubPageSize);
    std::string active;
    auto note = [&](const std::string& s) { active += active.empty() ? s : ", " + s; };
    sf_lock_on = On("BB_SF_MOVIE_LOCK") && stub_page && InstallHudLock();
    note(std::string("sf-movie-lock ") + (sf_lock_on ? "on" : "off"));
    sf_guard_on = On("BB_SF_TEXT_GUARD") && stub_page && InstallTextGuard();
    note(std::string("sf-text-guard ") + (sf_guard_on ? "on" : "off"));
    const bool fmod_ring = On("BB_FMOD_RING_GUARD") && stub_page && InstallFmodGuard();
    note(std::string("fmod-ring-guard ") + (fmod_ring ? "on" : "off"));
    movie_wait_on = On("BB_MOVIE_DRAW_WAIT") && stub_page && InstallMovieWait();
    note(std::string("movie-wait ") + (movie_wait_on ? "on" : "off"));
    if (sf_lock_on || sf_guard_on || fmod_ring || movie_wait_on) {
        std::atexit([] {
            SfReport();
            FmodReport();
            MovieReport();
        });
    }
    if (stub_page) {
#ifdef _WIN32
        DWORD unused = 0;
        VirtualProtect(stub_page, StubPageSize, PAGE_EXECUTE_READ, &unused);
        FlushInstructionCache(GetCurrentProcess(), stub_page, StubPageSize);
#else
        mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
#endif
    }
    std::printf("Crash guards: %s\n", active.c_str());
    std::fflush(stdout);
}

} // namespace BbCrashGuards
