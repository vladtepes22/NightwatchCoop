// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/engine/lua_events.cpp @8f2746c
// Named Lua events, raised on the main thread (see lua_events.h).
//
// LuaEvent_DispatchByName (0x1339870, bbhost 0x1739870) is (ctx, const char* name): it walks
// ctx+8's chain of handler tables for the name. The game's own callers pass
// ctx = *(*(SprjLuaEventMan slot 0x553b0c8) + 8): e.g. 0x13de97a "OnFailedCreateSession" and
// 0x13162b0 "OnIrregularLeaveSession"; the SOS path 0x130ca7c dispatches "OnEvent_Call_SOS".
// It returns 0 when no handler table has the name, else the Lua call's result (0x2749180) - a
// bool in al: only the low byte means anything (1 = a handler ran).
#include "lua_events.h"
#include "party_async_io.h"

#include "coop_hooks.h"
#include "game_state.h"

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace coop {
namespace {

constexpr std::uint64_t kDispatchByName = 0x1339870;
constexpr std::uint64_t kLuaEventMan = 0x553b0c8;
constexpr std::size_t kQueueMax = 64;

bool g_ok = false;
std::mutex g_mu;
std::deque<std::string> g_queue; // under g_mu
std::uint64_t g_raised = 0, g_dropped = 0;

using DispatchFn = std::int64_t(BB_COOP_SYSV*)(std::uint64_t ctx, const char* name);

} // namespace

bool LuaEventsInit() {
    // push rbp; mov rbp, rsp; push r15..rbx; sub rsp, 0x5d8; mov r12, rsi; mov rbx, rdi - and its
    // `lea r14, [rip -> 0x553b0c8]` (the event manager it asserts on).
    g_ok = Matches(kDispatchByName, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                     0x48, 0x81, 0xec, 0xd8, 0x05, 0x00, 0x00, 0x49, 0x89, 0xf4, 0x48, 0x89, 0xfb}) &&
           Matches(0x13398a1, {0x4c, 0x8d, 0x35, 0x20, 0x18, 0x20, 0x04});
    if (!g_ok) {
        std::printf("Party: Lua events: guest +0x%llx is not LuaEvent_DispatchByName; events off\n",
                    static_cast<unsigned long long>(kDispatchByName));
    }
    return g_ok;
}

bool LuaEventQueue(const char* name) {
    if (!g_ok || !name || !name[0]) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_queue.size() >= kQueueMax) {
        ++g_dropped;
        return false;
    }
    g_queue.emplace_back(name);
    return true;
}

bool LuaEventRaiseNow(const char* name, std::int64_t* result) {
    if (!g_ok) {
        return false;
    }
    std::uint64_t man = 0, ctx = 0;
    if (!SafeGet(Guest(kLuaEventMan), &man) || !man || !SafeGet(man + 8, &ctx) || !ctx) {
        return false;
    }
    const std::int64_t r = reinterpret_cast<DispatchFn>(Guest(kDispatchByName))(ctx, name) & 0xff;
    ++g_raised;
    if (result) {
        *result = r;
    }
    return true;
}

void LuaEventsTick() {
    std::deque<std::string> now;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_queue.empty()) {
            return;
        }
        now.swap(g_queue);
    }
    for (const std::string& name : now) {
        std::int64_t r = 0;
        if (LuaEventRaiseNow(name.c_str(), &r)) {
            coop::asyncio::Print("Party: Lua event " + name + " raised -> " + std::to_string(r) + " (1: a handler ran)\n");
        } else {
            coop::asyncio::Print("Party: Lua event " + name + " dropped: no event manager yet\n");
        }
    }
}

std::uint64_t LuaEventsRaised() {
    return g_raised;
}

} // namespace coop
