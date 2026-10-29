// SPDX-License-Identifier: GPL-2.0-or-later
// GX occlusion query guard (visy 0.7.7 CL-9666e3, his port of deadinside28's InstallGxQueryGuard,
// upstream 0fc2921c, PR #343 by whp199, issue #336). Crash 0x2169edc on GXRenderThread, address 0,
// after hours of play: the game's query (0x30 bytes, its handle the first field) is made while the
// device heap is full and keeps a null handle; the render command then begins it (+0xe2f130: state
// [query + 8] 0 -> 2, the callee reads the handle) and ends it (+0xe2f160: state 1). A query without
// a handle is neither begun nor ended: its state stays 0, so its result getter gives 0 (nothing
// visible). visy patches the two functions with stubs; here both are replaced through coop hooks
// (ReplacePrologue), the game's function runs unchanged for every query with a handle.
// BB_GX_QUERY_GUARD=0: off. BB_GX_QUERY_TEST_NULL=N (test aid): the N-th begin of a query with a
// handle sees a null handle (the real handle is put back at the next begin).
#include "bbport_gx_query_guard.h"

#include "party/coop_hooks.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbGxQueryGuard {
namespace {
using u64 = std::uint64_t;
using ull = unsigned long long;
using QueryFn = u64(BB_COOP_SYSV*)(u64 query, u64 context);

constexpr u64 kQueryBegin = 0xe2f130;
constexpr u64 kQueryEnd = 0xe2f160;
QueryFn g_begin = nullptr;
QueryFn g_end = nullptr;
std::atomic<u64> g_skipped{0};
// Test aid (render thread only, like the game's own calls).
u64 g_test_every = 0, g_test_calls = 0, g_test_query = 0, g_test_handle = 0;

u64 Handle(u64 query) { return *reinterpret_cast<const u64*>(query); }
std::uint32_t State(u64 query) { return *reinterpret_cast<const std::uint32_t*>(query + 8); }

void NoteSkip(const char* what) {
    const u64 n = g_skipped.fetch_add(1) + 1;
    if ((n & (n - 1)) == 0) { // the 1st, 2nd, 4th, ...
        std::printf("GX query guard: null handle skipped (%llu, %s)\n", ull(n), what);
    }
}

void TestAid(u64 query) {
    if (g_test_query) { // the handle of the forced begin comes back
        *reinterpret_cast<u64*>(g_test_query) = g_test_handle;
        g_test_query = 0;
    }
    const u64 handle = Handle(query);
    if (!handle || State(query) != 0) return;
    if (g_test_calls == 0 || (g_test_calls + 1) % 1000 == 0) {
        std::printf("GX query guard: test aid saw %llu begins\n", ull(g_test_calls + 1));
    }
    if (++g_test_calls == g_test_every) {
        g_test_query = query;
        g_test_handle = handle;
        *reinterpret_cast<u64*>(query) = 0;
        std::printf("GX query guard: test aid nulls the handle of query %#llx at begin %llu\n", ull(query),
                    ull(g_test_calls));
    }
}

BB_COOP_SYSV u64 Begin(u64 query, u64 context) {
    if (g_test_every) TestAid(query);
    if (State(query) == 0 && Handle(query) == 0) { // the game would begin it and read the null handle
        NoteSkip("begin");
        return 1; // the game's own "begun already" return
    }
    return g_begin(query, context);
}

BB_COOP_SYSV u64 End(u64 query, u64 context) {
    if (Handle(query) == 0) {
        NoteSkip("end");
        return 1; // not ended: the state stays 0
    }
    return g_end(query, context);
}
} // namespace

void Install() {
    const char* off = std::getenv("BB_GX_QUERY_GUARD");
    if (off && std::strcmp(off, "0") == 0) {
        std::printf("GX query guard: off (BB_GX_QUERY_GUARD=0)\n");
        return;
    }
    // The bytes after the replaced prologues: the begin's `jne +0x1d` / `mov dword [rdi + 8], 2`
    // and the end's `mov rax, [rsi + 8]`, plus the shared `mov al, 1` returns.
    if (!coop::Matches(kQueryBegin + 8, {0x75, 0x1d, 0xc7, 0x47, 0x08, 0x02, 0x00, 0x00, 0x00}) ||
        !coop::Matches(kQueryBegin + 0x27, {0xb0, 0x01, 0x5d, 0xc3}) ||
        !coop::Matches(kQueryEnd + 11, {0x48, 0x8b, 0x46, 0x08}) ||
        !coop::Matches(kQueryEnd + 0x21, {0xb0, 0x01, 0x5d, 0xc3})) {
        std::printf("GX query guard: guest +0x%llx / +0x%llx is not the expected code; left alone\n",
                    ull(kQueryBegin), ull(kQueryEnd));
        return;
    }
    if (const char* test = std::getenv("BB_GX_QUERY_TEST_NULL")) {
        g_test_every = std::strtoull(test, nullptr, 10);
    }
    void* begin = nullptr;
    void* end = nullptr;
    // push rbp; mov rbp, rsp; cmp dword [rdi + 8], 0
    if (!coop::ReplacePrologue(kQueryBegin, {0x55, 0x48, 0x89, 0xe5, 0x83, 0x7f, 0x08, 0x00},
                               reinterpret_cast<const void*>(&Begin), &begin, "GX query guard: begin")) {
        return;
    }
    g_begin = reinterpret_cast<QueryFn>(begin);
    // push rbp; mov rbp, rsp; mov dword [rdi + 8], 1
    if (!coop::ReplacePrologue(kQueryEnd, {0x55, 0x48, 0x89, 0xe5, 0xc7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00},
                               reinterpret_cast<const void*>(&End), &end, "GX query guard: end")) {
        std::printf("GX query guard: only the begin is guarded\n");
        return;
    }
    g_end = reinterpret_cast<QueryFn>(end);
    std::printf("GX query guard: on%s\n", g_test_every ? " (test aid)" : "");
}
} // namespace BbGxQueryGuard
