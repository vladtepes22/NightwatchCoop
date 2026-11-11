// SPDX-License-Identifier: GPL-3.0-or-later
// Map-parts budget on the loading screen: see bbport_parts_budget.h. Offsets are ours (bbhost - 0x400000).
#include "bbport_parts_budget.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "party/coop_hooks.h"
#include "party/game_state.h"

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" void runtime_win_set_thread_name(const char* name);

namespace BbPartsBudget {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// 0x15934D0 (MapInsBlock update, called by the world-ready check 0x19E8190 every frame of a load and by the
// in-world block update): `budget = 0x14` is loaded once before the first pass and once inside it; each part
// inside the block's load / draw masks costs one (a 0x1587ED0 backread request), the call returns when the budget
// is spent and the two cursors (+0xB8 / +0xBC) continue next frame. The body is 0x15934D0..0x1593E4D.
constexpr u64 kFn = 0x15934d0, kFnEnd = 0x1593e4d;
// NowLoading requested (*0x556286B, u8): the loading screen is up (the party director's "loading" condition
// without the in-game frame part: raised at InGameStep 2, cleared by InGameStart).
constexpr u64 kNowLoadingRequested = 0x556286b;
constexpr u32 kGame = 0x14;

std::vector<u64> g_sites;  // offsets of the imm32 of each `mov r32, 0x14`
u32 g_value = 0;
std::atomic<bool> g_raised{false};

/// The budget while the screen is up. Unset: the default (4096: more than any block's parts, so a pass is one call;
/// measured 4.40 s vs 4.70 s at 400 and 7.21 s at the game's 20 for a Cathedral Ward warp at 60 fps). 0 / off /
/// false: nothing. N: that many (20..1000000). The MapInsUpdateHelper queue the requests go to holds one entry
/// per part of the block (capacity 4048 for m24_00) and 0x1587ED0 never queues a part twice (+0xC bit 0), so no
/// budget can overflow it (its assert MapInsUpdateHelper.cpp(0xcc)).
u32 Parse(const char* v) {
    constexpr u32 kDefault = 4096;
    if (!v || !*v) return kDefault;
    if (!std::strcmp(v, "0") || !std::strcmp(v, "off") || !std::strcmp(v, "false")) return 0;
    const long n = std::strtol(v, nullptr, 10);
    return n <= 0 ? 0 : u32(std::min<long>(std::max<long>(n, 20), 1000000));
}

// The two budget loads, `mov r14d, 0x14` (41 BE 14 00 00 00) at +0x1593764 and +0x1593773 (a scan of the body for
// `mov r32, 0x14` finds these two and no other). The imm32 is the last four bytes of each.
constexpr u64 kSite1 = 0x1593764, kSite2 = 0x1593773;
constexpr u8 kLoad[] = {0x41, 0xbe, 0x14, 0x00, 0x00, 0x00};

/// Both sites hold the expected instruction and the function holds no third `mov r32, 0x14`.
bool CheckSites() {
    if (!coop::Matches(kSite1, kLoad, sizeof kLoad) || !coop::Matches(kSite2, kLoad, sizeof kLoad)) return false;
    const u8* img = coop::Image();
    int n = 0;
    for (u64 p = kFn; p + 5 <= kFnEnd; ++p) {
        const u8 b = img[p];
        if (b >= 0xb8 && b <= 0xbf && img[p + 1] == 0x14 && img[p + 2] == 0 && img[p + 3] == 0 && img[p + 4] == 0) ++n;
    }
    if (n != 2) return false;
    g_sites = {kSite1 + 2, kSite2 + 2};
    return true;
}

void WriteImm(u64 off, u32 v) {
#ifdef _WIN32
    u8* at = coop::Image() + off;
    DWORD old = 0;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(at, &v, 4);  // one aligned-enough dword: a torn fetch mixes bytes of 20 and of N, both valid budgets
    VirtualProtect(at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
#else
    (void)off;
    (void)v;
#endif
}

void Poller() {
    runtime_win_set_thread_name("bb:PartsBudget");
    bool up = false;
    int logged = 0;
    for (;;) {
#ifdef _WIN32
        Sleep(4);
#endif
        u8 requested = 0;
        coop::SafeGet(coop::Guest(kNowLoadingRequested), &requested);
        const bool now = requested != 0;
        if (now == up) continue;
        up = now;
        for (const u64 s : g_sites) WriteImm(s, up ? g_value : kGame);
        g_raised.store(up, std::memory_order_relaxed);
        if (logged < 4) {
            ++logged;
            std::printf("Parts budget: loading screen %s: budget %u\n", up ? "up" : "gone", up ? g_value : kGame);
            std::fflush(stdout);
        }
    }
}

} // namespace

void Install() {
    if (!coop::Image()) return;
    g_value = Parse(std::getenv("BB_LOAD_PARTS_BUDGET"));
    if (g_value == 0) {
        std::printf("Parts budget: the game's 20 parts per frame (BB_LOAD_PARTS_BUDGET=0)\n");
        return;
    }
    if (!CheckSites()) {
        std::printf("Parts budget: NOT applied: the budget loads of 0x15934d0 are not the expected code (another game version?)\n");
        return;
    }
    std::printf("Parts budget: %u map parts per frame while the loading screen is up (the game's 20 in the world; "
                "BB_LOAD_PARTS_BUDGET=0 off): sites +0x%llx +0x%llx\n",
                g_value, static_cast<unsigned long long>(g_sites[0]), static_cast<unsigned long long>(g_sites[1]));
    std::thread(Poller).detach();
}

} // namespace BbPartsBudget
