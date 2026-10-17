// SPDX-License-Identifier: GPL-3.0-or-later
// The FROM client's "drop offline" routine 0x1ea5ce0(SprjNetworkClientMan*, int msgId), logged
// with its reason (docs/party/from_api_schema.md §1.1). It clears the client's UserId (+0x60) and
// SessionId (+0x70), queues msgId into FrpgNetMan+0xa30 and sets FrpgNetMan+0xa50 (server
// offline): from then on every request builder (summon_messenger/create, get, request ...)
// refuses to send. Its only callers are in the response dispatcher 0x1e7f240:
//   0x1e88815  WanderingGhostGet (API 0x24) failed 3 times            msg 0xfa1
//   0x1e8924d  ss.info: <ss> != 0 (0x1131 / 0x1132) or 4 failures     msg 0x1133
//   0x1e8965e  ResKind 0x10010b (msg 0x10cd) / 0x10010a (msg 0x1069)
// The dispatcher's frame holds the API id at [rsp+0xd5c] and the computed result at
// [rsp+0xd60] (low 32 bits the code, bit 32 the bad-shape flag); both are logged with the
// return addresses up the frame-pointer chain.
#include "coop_hooks.h"
#include "party_online.h"
#include "../net/bbnet_internal.h"

#include <cstdint>
#include <cstdio>

namespace coop {

namespace {

constexpr u64 kDropOffline = 0x1ea5ce0;
constexpr u64 kDispatcher = 0x1e7f240, kDispatcherEnd = 0x1e896e0;
using DropFn = void(BB_COOP_SYSV*)(u64, int);
DropFn g_drop_original = nullptr;

template <class T>
bool Read(u64 addr, T* out) {
    return bbnet::guest_read(static_cast<std::uintptr_t>(addr), out, sizeof(T));
}

const char* Why(u64 ret_off, int msg) {
    if (ret_off == 0x1e8881a) return "WanderingGhostGet failed 3 times";
    if (ret_off == 0x1e89252) return msg == 0x1133 ? "ss.info failed 4 times" : "ss.info <ss> is not 0";
    if (ret_off == 0x1e89663) return msg == 0x10cd ? "ResKind 0x10010b" : "ResKind 0x10010a";
    return "unknown caller";
}

__attribute__((noinline)) BB_COOP_SYSV void DropOfflineHook(u64 mgr, int msg) {
    const u64 base = Guest(0);
    auto* frame = static_cast<u64*>(__builtin_frame_address(0));
    u64 ret = 0;
    Read(reinterpret_cast<u64>(frame) + 8, &ret);
    const u64 ret_off = ret - base;
    // The dispatcher's rsp at the call: our entry rsp (the return address) + 8.
    std::uint32_t api = 0xffffffff;
    std::uint64_t result = 0;
    if (ret_off > kDispatcher && ret_off < kDispatcherEnd) {
        const u64 rsp = reinterpret_cast<u64>(frame) + 16;
        Read(rsp + 0xd5c, &api);
        Read(rsp + 0xd60, &result);
    }
    char chain[256];
    int n = 0;
    u64 fp = 0;
    Read(reinterpret_cast<u64>(frame), &fp);  // the dispatcher's rbp
    for (int i = 0; i < 6 && fp; ++i) {
        u64 r = 0, next = 0;
        if (!Read(fp + 8, &r) || !Read(fp, &next)) break;
        n += std::snprintf(chain + n, sizeof chain - n, " +0x%llx", static_cast<unsigned long long>(r - base));
        if (n >= static_cast<int>(sizeof chain) - 24 || next <= fp) break;
        fp = next;
    }
    chain[n] = 0;
    std::printf("Party online: the FROM client drops offline: msg 0x%x (%s) from +0x%llx, API 0x%x, result 0x%llx; "
                "callers%s\n",
                msg, Why(ret_off, msg), static_cast<unsigned long long>(ret_off), api,
                static_cast<unsigned long long>(result), chain);
    std::fflush(stdout);
    restore_guest_fs();
    g_drop_original(mgr, msg);
}

}  // namespace

void OnlineDiagInit() {
    void* original = nullptr;
    if (ReplacePrologue(kDropOffline, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                        reinterpret_cast<const void*>(&DropOfflineHook), &original,
                        "FROM client drop-offline log (0x1ea5ce0)")) {
        g_drop_original = reinterpret_cast<DropFn>(original);
    }
}

}  // namespace coop
