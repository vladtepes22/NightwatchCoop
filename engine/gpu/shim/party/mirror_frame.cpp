// SPDX-License-Identifier: GPL-3.0-or-later
// The mirror's appearance editor in character creation's frame: see mirror_frame.h.
// Ported from droogie/bbhost (GPL-3.0-or-later), src/engine/change_appearance.cpp / menu_steps.cpp.
#include "mirror_frame.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_b2.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::mirror {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Offsets in our image (bbhost's minus 0x400000).
constexpr u64 kStepCtor = 0x1c1cce0;       // menu step constructor (step, ctx, type, name, factory, ...)
constexpr u64 kOpenWindow = 0x1ad8150;     // (descriptor)
constexpr u64 kCloseWindow = 0x1ad8e60;    // (descriptor): closes the window, unloads the movie
constexpr u64 kMenuHeap = 0x5540418;       // the menus' allocator: +0x58 alloc(size, align), +0x70 free(p)
constexpr u64 kWorldChrMan = 0x553e878;    // +0x60 the local player, its +0x3f8 the block id
constexpr u64 kDescVtable = 0x5336b30, kDescOwnerVtable = 0x5336b70, kDescListVtable = 0x53310d0;
constexpr u64 kFactoryVtable = 0x533ee10;  // the functor character creation stores ...
constexpr u64 kFrameFactory = 0x1b08010;   // ... around this factory: 0x1B06B10's controller
constexpr u64 kFrameName = 0x4998486;      // L"ChrMake_BG"
constexpr u64 kFrameType = 6;
constexpr u64 kListType = 7;
constexpr char16_t kListName[] = u"ChrMakeCommandList";
constexpr std::size_t kDescSize = 0xc0;

std::atomic<u64> g_made{0};  // the list's step, made since the last tick
bool g_on = false;
u64 g_list = 0;   // the list's step while we hold a reference
u64 g_desc = 0;   // our descriptor while ChrMake_BG is open
alignas(8) std::uint8_t g_active = 1;  // the update's argument (a flag byte it reads)
int g_opened = 0;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party: mirror frame: ") + line + "\n");
}

u64 Rd(u64 a) {
    u64 v = 0;
    return SafeGet(a, &v) ? v : 0;
}

BB_COOP_SYSV void StepCtorEntry(u64 step, u64, u64 type, u64 name, u64, u64) {
    if (type != kListType || !name || !step) return;
    char16_t n[sizeof kListName / sizeof kListName[0]] = {};
    if (!SafeRead(name, n, sizeof n)) return;
    if (std::memcmp(n, kListName, sizeof n) == 0) g_made.store(step, std::memory_order_release);
}

u64 HeapAlloc(std::size_t n) {
    const u64 heap = Rd(Guest(kMenuHeap));
    const u64 fn = heap ? Rd(Rd(heap) + 0x58) : 0;
    if (!fn) return 0;
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64)>(fn)(heap, n, 0x10);
}

void HeapFree(u64 p) {
    const u64 heap = Rd(Guest(kMenuHeap));
    const u64 fn = heap ? Rd(Rd(heap) + 0x70) : 0;
    if (fn && p) reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(fn)(heap, p);
}

void W64(u64 a, u64 v) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof v);
}

bool InWorld() {
    const u64 chr = Rd(Rd(Guest(kWorldChrMan)) + 0x60);
    u32 block = 0xffffffffu;
    return chr && SafeGet(chr + 0x3f8, &block) && block != 0xffffffffu;
}

bool OpenFrame() {
    const u64 d = HeapAlloc(kDescSize);
    if (!d) return false;
    std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d)), 0, kDescSize);
    W64(d + 0x00, Guest(kDescVtable));
    W64(d + 0x08, Guest(kDescOwnerVtable));
    W64(d + 0x18, kFrameType);
    W64(d + 0x20, Guest(kFrameName));
    W64(d + 0x30, Guest(kFactoryVtable));  // the factory functor, in the descriptor's own storage
    W64(d + 0x38, Guest(kFrameFactory));
    W64(d + 0x50, d + 0x30);
    W64(d + 0x68, Guest(kDescListVtable));
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(Guest(kOpenWindow))(d);
    if (!Rd(d + 0x60)) {  // no window: nothing to keep
        reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(Guest(kCloseWindow))(d);
        HeapFree(d);
        return false;
    }
    g_desc = d;
    return true;
}

void CloseFrame() {
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(Guest(kCloseWindow))(g_desc);
    HeapFree(g_desc);
    g_desc = 0;
}

std::int32_t* Count(u64 step) {
    return reinterpret_cast<std::int32_t*>(static_cast<std::uintptr_t>(step + 8));
}

void Release(u64 step) {
    if (--*Count(step) == 0) reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(Rd(Rd(step)))(step);
}

}  // namespace

void Install() {
    if (const char* e = std::getenv("BB_MIRROR_FRAME"); e && e[0] == '0') {
        Log("off (BB_MIRROR_FRAME=0): the mirror's editor opens without character creation's frame");
        return;
    }
    // Every function and table used must be the 1.09 code bbhost read.
    const bool code = Matches(kOpenWindow, {0x55, 0x48, 0x89, 0xe5}) && Matches(kCloseWindow, {0x55, 0x48, 0x89, 0xe5}) &&
                      Matches(kFrameFactory, {0x55, 0x48, 0x89, 0xe5});
    char16_t name[11] = {};
    const bool str = SafeRead(Guest(kFrameName), name, sizeof name) && std::memcmp(name, u"ChrMake_BG", sizeof name) == 0;
    g_on = code && str &&
           HookPrologue(kStepCtor, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &StepCtorEntry,
                        "mirror: menu step watch (ChrMakeCommandList)");
    Log("%s", g_on ? "on: the mirror's appearance editor opens in character creation's frame (ChrMake_BG)"
                   : "NOT installed: the code differs from 1.09");
}

void Tick() {
    if (!g_on) return;
    if (const u64 made = g_made.exchange(0, std::memory_order_acq_rel)) {
        if (!g_list && !g_desc && InWorld() && *Count(made) >= 1 && OpenFrame()) {
            ++*Count(made);  // ours: when it is the last, the list has closed
            g_list = made;
            if (g_opened++ < 8) Log("the editor's frame opened (ChrMake_BG: the book, the hunter's preview)");
        }
    }
    if (g_desc) {
        const u64 ctl = Rd(g_desc + 0x60);
        if (ctl) reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(Rd(Rd(ctl) + 0x18))(ctl, reinterpret_cast<u64>(&g_active));
    }
    if (g_list && *Count(g_list) == 1) {
        CloseFrame();
        Release(g_list);
        g_list = 0;
        Log("the editor closed: its frame closed");
        b2::NoteOwnAppearanceChanged();  // the other players rebuild this hunter's chr (H5)
    }
}

}  // namespace coop::mirror
