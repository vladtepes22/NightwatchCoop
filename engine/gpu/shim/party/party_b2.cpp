// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel B2 prototype (see party_b2.h; the RE is docs/party/b2_seamless.md).
//
// Every site was disassembled from the 1.09 eboot (capstone + Ghidra) and is compared byte for
// byte by coop_hooks before anything is written. Offsets are ours (raw ELF VA). The game's code
// is System V: every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_b2.h"
#include "party_async_io.h"
#include "party_fourp.h"
#include "party_link.h"
#include "party_runtime.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_phantom.h"
#include "party_progress.h"
#include "party_travel.h"
#include "party_story.h"
#include "party_ids.h"
#include "lua_events.h"
#include "seamless_rules.h"
#include "../../party_env.h"
#include "../net/from_api.h"
#include "../net/bbnet_internal.h"
#include "../net/np_hle.h"
#include "party_nr_drop.h"
#include "party_dsummon.h"
#include "party_status_bridge.h"
#include "party_spectate.h"
#include "party_director.h"
#include "party_death_rules.h"
#include "party_hud.h"
#include "party_status.h"
#include "party_guestvals_rules.h"
#include "party_ping.h"
#include "party_flow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace coop::b2 {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;

// ---- Globals (singleton slots) ----
constexpr u64 kSlotWorldChrMan = 0x553e878;  // +0x30 player set {int n; entries @+0x38 (0x38)}, +0x60 local
constexpr u64 kSlotFlow = 0x553d6d0;         // network flow: +0x84 state, +0x68 members, +0x108 timer
constexpr u64 kSlotMultiPlay = 0x5540230;    // CSMultiPlayMan: human tasks +0x10/+0x18, NPC +0x38/+0x40
constexpr u64 kSlotSession = 0x5540290;      // SprjSessionManager +0x124 role
constexpr u64 kSlotGameData = 0x553b130;     // GameDataMan +0x68 (packet 0x19 payload)
constexpr u64 kSessionTypeDesc = 0x553d750;   // + st * 0x80: +0x15 net chr type (party_npc_test)
constexpr u64 kSlotGsm = 0x5556678;          // WorldTransitionState: +0x16f4 presentation slot byte, +0x16f8 slots
constexpr u64 kSlotBullet = 0x553e8d0;       // SprjBulletManager
constexpr u64 kSlotNetSyncArg = 0x553b148;   // *(+0x20) third argument of 0x18F0DB0 / 0x18F0E80

// ---- Functions ----
constexpr u64 kBeginLeave = 0x1ed07a0;   // bool (SprjSessionManager*)
constexpr u64 kMultiPlayReset = 0x1e54830; // void (CSMultiPlayMan*)
constexpr u64 kDropPresentation = 0x15bed10; // void (slots*)
constexpr u64 kJoinStartLua = 0x1317780; // void (SprjLuaEventMan*)
constexpr u64 kIdEquals = 0xca1c70;      // bool (const PlayerId*, const PlayerId*)
constexpr u64 kSendPacket = 0x17894e0;   // int (flow, const PlayerId*, type, const void*, size)
constexpr u64 kSendFlags = 0x1786050;    // bool (unused, const PlayerId*) packet 0x10
constexpr u64 kSendWorld = 0x1786360;    // bool (unused, const PlayerId*) packet 0x11
constexpr u64 kSendBullets = 0x1a04190;  // void (SprjBulletManager*, const PlayerId*)
constexpr u64 kNetSyncJoin = 0x18f0db0;  // void (WorldChrMan+0xa40 obj, const PlayerId*, u32)
constexpr u64 kNetSyncData = 0x18f0e80;  // same signature, the packet-0xa queue
constexpr u64 kSendNpcSummons = 0x15bd0e0; // void (slots*, const PlayerId*)

// ---- Sites ----
constexpr u64 kDtorLeaveCall = 0x19471b1;   // e8 ea 95 58 00 -> 0x1ed07a0 (XML retarget: e8 ea 8e 58 00)
constexpr u64 kDtorResetCall = 0x1947207;   // e8 24 d6 50 00 -> 0x1e54830
constexpr u64 kUidLeaveCall = 0x193c657;    // e8 44 41 59 00 -> 0x1ed07a0
constexpr u64 kUidDropCall = 0x193c689;     // e8 82 26 c8 ff -> 0x15bed10
constexpr u64 kMatchCheckCall = 0x13809b7;  // e8 e4 fd b4 00 -> 0x1ed07a0
constexpr u64 kJoinStartCall = 0x178524f;   // e8 2c 25 b9 ff -> 0x1317780
constexpr u64 kFunnelLeaveJne = 0x13cdeae;  // 75 6c (cmp [GSM+0x1592],0 ; jne skip-leave)
constexpr u64 kHostDeathReturnCall = 0x18c4d7d; // e8 2e 00 59 00 -> 0x1e54db0 (ChrIns_OnDeath, role 3)
constexpr u64 kRequestReturn = 0x1e54db0;       // void (CSMultiPlayMan*, const PlayerId*)
constexpr u64 kHostDead1Call = 0x1381a93;       // HostDead_1's send-home (B1 SendHomeHook site)
constexpr u64 kTaskUpdate = 0x1e52fe0;          // CSMultiPlayerInsTask update (vfunc +0xd0)

// Task (CSMultiPlayerInsTask, 0x100 bytes, vtable 0x534F650) fields.
constexpr u64 kTaskStep = 0x50, kTaskNext = 0x54, kTaskPeer = 0xc8, kTaskMode = 0xe8, kTaskSlot = 0xec,
              kTaskFlags = 0xed;
constexpr int kStepCreate = 4, kStepUpdate = 6;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party B2: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && !(v[0] == '0' && !v[1]);
}
bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <class T>
T Field(u64 base, u64 off, T fallback = T{}) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

u64 Slot(u64 off) {
    u64 v = 0;
    const u64 at = Guest(off);
    return at && SafeGet(at, &v) ? v : 0;
}

int Role() {
    return Field<int>(Slot(kSlotSession), 0x124, -1);
}

bool SessionUp() {
    const int r = Role();
    return r == RoleHost || r == RoleClient;
}

std::atomic<bool> g_on{false};
std::atomic<bool> g_force{false};
std::atomic<bool> g_rejoin{false};  // a B2 re-join is in flight (K5 / K6 armed)
std::atomic<bool> g_k1_xml{false};  // the XML retarget already keeps the room at 0x19471B1
std::atomic<int> g_apart_k5{0};     // members in another area (split travel): K5 stands aside
std::atomic<double> g_split_change_at{-100.0};  // the last split chr removal / meet rewind (Now())
std::atomic<double> g_title_served_at{-1e9};  // host: the last title-join room sent (Now()), holds lamp travel

bool IdEquals(u64 a, u64 b) {
    using Fn = bool(BB_COOP_SYSV*)(u64, u64);
    return reinterpret_cast<Fn>(Guest(kIdEquals))(a, b);
}

// ---- Reads ----

struct TaskInfo {
    u64 task = 0;
    int step = -1, next = -1, mode = -1;
    u8 slot = 0, flags = 0;
};

std::vector<TaskInfo> Tasks(bool npc) {
    std::vector<TaskInfo> out;
    const u64 man = Slot(kSlotMultiPlay);
    const u64 begin = Field<u64>(man, npc ? 0x38 : 0x10), end = Field<u64>(man, npc ? 0x40 : 0x18);
    if (!begin || end < begin || end - begin > 64 * 8) {
        return out;
    }
    for (u64 p = begin; p < end; p += 8) {
        TaskInfo t;
        t.task = Field<u64>(p, 0);
        if (!t.task) {
            continue;
        }
        t.step = Field<int>(t.task, kTaskStep, -1);
        t.next = Field<int>(t.task, kTaskNext, -1);
        t.mode = npc ? -1 : Field<int>(t.task, kTaskMode, -1);
        t.slot = npc ? 0 : Field<u8>(t.task, kTaskSlot);
        t.flags = npc ? Field<u8>(t.task, 0x128) : Field<u8>(t.task, kTaskFlags);
        out.push_back(t);
    }
    return out;
}

/// The player ChrSet entry whose peer id (+0x3D8) equals `id` (main thread: calls the game).
u64 PlayerForPeer(u64 id) {
    const u64 wcm = Slot(kSlotWorldChrMan);
    const int n = Field<int>(wcm, 0x30, 0);
    const u64 entries = Field<u64>(wcm, 0x38);
    if (!entries || n <= 0 || n > 8) {
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
        if (chr && IdEquals(chr + 0x3d8, id)) {
            return chr;
        }
    }
    return 0;
}

/// The flow's member list (+0x68: std::list of {PlayerId @+0x10, int slot @+0x30}): the PlayerId
/// addresses of the other session members.
std::vector<u64> Members() {
    std::vector<u64> out;
    const u64 flow = Slot(kSlotFlow);
    const u64 head = Field<u64>(flow, 0x68);
    if (!head) {
        return out;
    }
    u64 node = Field<u64>(head, 0);
    for (int guard = 0; node && node != head && guard < 8; ++guard) {
        out.push_back(node + 0x10);
        node = Field<u64>(node, 0);
    }
    return out;
}

bool ChrPos(u64 chr, float out[3]) {
    const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
    return a && SafeRead(a + 0x1e0, out, 12);
}

std::string DescribeChr(u64 chr) {
    float p[3] = {0, 0, 0};
    char b[192];
    if (!chr || !ChrPos(chr, p)) {
        return "-";
    }
    float drawn[4] = {};
    SafeRead(chr + 0xc0, drawn, 16);
    // +0x78 chr type (0 own body, 1 white phantom, ...); vfunc +0x558 is what the cooperator count
    // 0x15BDC20 tests (counted when in {1, 5, 7, 19}).
    using TypeFn = u32(BB_COOP_SYSV*)(u64);
    const u64 vt = Field<u64>(chr, 0);
    const u64 fn = vt ? Field<u64>(vt, 0x558) : 0;
    const u32 pres = fn ? reinterpret_cast<TypeFn>(fn)(chr) : ~0u;
    std::snprintf(b, sizeof b, "stage %#x type %d/%d pos (%.1f, %.1f, %.1f) drawn (%.1f, %.1f, %.1f) hp %d",
                  Field<u32>(chr, 0x3f8), Field<int>(chr, 0x78, -1), int(pres), p[0], p[1], p[2], drawn[0], drawn[1],
                  drawn[2],
                  Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0xf8, -1));
    return b;
}

std::string DescribeRender(u64 chr);

std::string DescribeTasks() {
    std::string s;
    char b[192];
    for (const TaskInfo& t : Tasks(false)) {
        std::string chrs;
        if (t.mode == 1 || t.mode == 2) {
            const u64 c = PlayerForPeer(t.task + kTaskPeer);
            char h[48];
            std::snprintf(h, sizeof h, " handle %08x at %#llx", c ? Field<u32>(c, 8, ~0u) : ~0u, ull(c));
            chrs = c ? " chr " + DescribeChr(c) + h + DescribeRender(c) : " NO-chr";
        }
        const char* chr = chrs.c_str();
        std::snprintf(b, sizeof b, " [human mode %d step %d next %d slot %u flags 0x%02x%s]", t.mode, t.step, t.next,
                      unsigned(t.slot), unsigned(t.flags), chr);
        s += b;
    }
    for (const TaskInfo& t : Tasks(true)) {
        std::snprintf(b, sizeof b, " [npc step %d next %d life 0x%02x]", t.step, t.next, unsigned(t.flags));
        s += b;
    }
    return s.empty() ? std::string(" none") : s;
}

/// The presentation slots (GSM +0x16F8 table: +0x14 used, +0x18 count, 5 entries of 0x14 bytes
/// from +0x1C: {chr handle, kind, state, a, b}) - what the cooperator count 0x15BDC20 reads (a
/// handle counts when it resolves to a live chr in WorldChrMan +0x850).
std::string DescribePresentation() {
    const u64 t = Field<u64>(Slot(kSlotGsm), 0x16f8);
    if (!t) {
        return "pres -";
    }
    char b[96];
    std::snprintf(b, sizeof b, "pres used %d count %d max %d:", Field<int>(t, 0x14, -1), Field<int>(t, 0x18, -1),
                  Field<int>(Slot(kSlotFlow), 0xc, -1));
    std::string s = b;
    for (int i = 0; i < 5; ++i) {
        const u64 e = t + 0x1c + 0x14 * u64(i);
        const u32 h = Field<u32>(e, 0, ~0u);
        if (h == ~0u) {
            continue;
        }
        std::snprintf(b, sizeof b, " [%d %08x k%d s%d]", i, h, Field<int>(e, 4), Field<int>(e, 8));
        s += b;
    }
    return s;
}

// ---- Hooks ----

// K1: the stage payload dtor's BeginLeaveSession.
BB_COOP_SYSV u64 DtorLeaveHook(u64 session) {
    if (g_on.load() && SessionUp()) {
        Log("K1: stage unload with a session (role %d): room kept", Role());
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

/// The NPC part of 0x1E54830: destroy and free every NPC task (vfunc +8, then the owning heap's
/// free, +0x70 - the reset's own sequence), empty the vector.
void DestroyNpcTasks(u64 man) {
    using HeapOf = u64(BB_COOP_SYSV*)(u64);
    using Dtor = void(BB_COOP_SYSV*)(u64);
    using Free = void(BB_COOP_SYSV*)(u64, u64);
    const u64 begin = Field<u64>(man, 0x38), end = Field<u64>(man, 0x40);
    if (!begin || end < begin) {
        return;
    }
    int n = 0;
    for (u64 p = begin; p < end; p += 8) {
        const u64 task = Field<u64>(p, 0);
        if (!task) {
            continue;
        }
        const u64 heap = reinterpret_cast<HeapOf>(Guest(0x207b720))(task);
        const u64 vt = Field<u64>(task, 0);
        reinterpret_cast<Dtor>(Field<u64>(vt, 8))(task);
        reinterpret_cast<Free>(Field<u64>(Field<u64>(heap, 0), 0x70))(heap, task);
        ++n;
    }
    SafeWrite(man + 0x40, &begin, 8);
    if (n) {
        Log("K2: %d NPC task(s) destroyed as the reset would", n);
    }
}

// K2: the stage payload dtor's CSMultiPlayMan reset.
BB_COOP_SYSV void ResetHook(u64 man) {
    if (g_on.load() && (SessionUp() || g_force.load())) {
        Log("K2: CSMultiPlayMan reset skipped (role %d); tasks:%s", Role(), DescribeTasks().c_str());
        DestroyNpcTasks(man);
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(kMultiPlayReset))(man);
}

std::atomic<double> g_k3_at{-100.0};  // the last K3 (Now()), for the hit watch

// K3: in-game stage update, stage-record UID changed (the host walked into another streamed block,
// e.g. Great Bridge m24_01 <-> Cathedral Ward m24_00): the host's BeginLeaveSession. The room is
// kept; the log names how far the farthest member chr is. A member left in a block this game then
// streams out is handled by the stale map hit guard (ChrPreStepHook below): before it, the host
// crashed ~0.8 s after such a change in the remote chr's physics pre-step (0x18CB16A..0x18CB17D),
// also when the vanilla leave ran at the change (docs/party/travel.md 7).
BB_COOP_SYSV u64 UidLeaveHook(u64 session) {
    g_k3_at = Now();
    if (g_on.load() && SessionUp()) {
        const u64 me = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
        float mp[3] = {};
        const bool have_me = me && ChrPos(me, mp);
        float farthest = 0.0f;
        int members = 0;
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode != 1 && t.mode != 2) {
                continue;
            }
            const u64 c = PlayerForPeer(t.task + kTaskPeer);
            float p[3] = {};
            if (!c || !have_me || !ChrPos(c, p)) {
                continue;
            }
            ++members;
            const float dx = p[0] - mp[0], dy = p[1] - mp[1], dz = p[2] - mp[2];
            farthest = std::max(farthest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        Log("K3: stage-record change with a session (stage %#x, %d member chr(s), farthest %.1f m): no leave",
            me ? Field<u32>(me, 0x3f8) : 0u, members, farthest);
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

// K4: the 0x15BED10 that follows it (every role).
BB_COOP_SYSV void UidDropHook(u64 slots) {
    if (g_on.load() && SessionUp()) {
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(kDropPresentation))(slots);
}

// K8: ChrIns_OnDeath (local player, role 3) asks every human task to return: the host's death
// would end the party. With B2 the host respawns and the members follow (its travel event).
BB_COOP_SYSV void HostDeathReturnHook(u64 man, u64 id) {
    if (g_on.load() && Role() == RoleHost) {
        Log("K8: host death: member task kept (no return request)");
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64, u64);
    reinterpret_cast<Fn>(Guest(kRequestReturn))(man, id);
}

/// Guest: HostDead_1's send-home is dropped (the host's death travel brings the guest along).
bool SplitHostReachable();
std::atomic<bool> g_force_load_warp{false};
std::atomic<bool> g_title_continuing{false};  // the director's AutoContinue is pressing Continue  // the next lamp warp loads (title-join home)
std::atomic<bool> g_title_joined{false};  // this guest came in by a title join (no own world loaded)
std::atomic<bool> g_title_home_now{false};

bool SendHomeVeto(u64 site) {
    const bool own_leave = SendHomeVetoForOwnLeave();
    // Split travel: OnLeave_Limit (0x138A51C: the room-gone / irregular-leave path) sent a guest
    // home 5 s after the host travelled alone (live split4) although the room and the party link
    // were up. While the host is apart (or just left alone) and its link is fresh, it is dropped -
    // also for a title-joined guest (B5, live parab2 b5e: it left the host's world 6 s after the
    // host's alone travel and loaded its own map). The player's own Leave is never dropped here.
    if (g_on.load() && Role() == RoleClient && site == 0x138a51c && !own_leave && SplitHostReachable()) {
        Log("guest: send-home at +0x138a51c (OnLeave_Limit) dropped: split travel, the host is in another area and "
            "connected (session %d, flow %d, flow +0xF4 %.1f, session +0x2E0 %.1f +0x2F0 %.1f%s)",
            Role(), Field<int>(Slot(kSlotFlow), 0x84, -1), double(Field<float>(Slot(kSlotFlow), 0xf4)),
            double(Field<float>(Slot(kSlotSession), 0x2e0)), double(Field<float>(Slot(kSlotSession), 0x2f0)),
            g_title_joined.load() ? "; a title-joined guest" : "");
        return true;
    }
    // A title-joined guest has no own-world return data (no snapshot: its own world was never
    // loaded): the game's return load fell back to the title (live run 18 / 19). Leave the session
    // here; TitleJoinGuestTick takes it home with a lamp warp to its own last lamp.
    if (g_on.load() && g_title_joined.load() && Role() == RoleClient && site == 0x138a51c) {
        using Fn = u8(BB_COOP_SYSV*)(u64);
        reinterpret_cast<Fn>(Guest(kBeginLeave))(Slot(kSlotSession));
        g_title_home_now = true;
        Log("title join: send-home at +0x138a51c replaced: session left, home by a lamp warp next");
        return true;
    }
    if (g_on.load() && Role() == RoleClient &&
        (site == kHostDead1Call || site == 0x13856f4 || site == 0x13859df)) {
        Log("guest: send-home at +0x%llx dropped (B2: %s)", ull(site),
            site == kHostDead1Call ? "the host's respawn travel follows" : "the party stays after the boss");
        return true;
    }
    return false;
}

// K10: the boss-clear Lua states' BeginLeaveSession (0x13858F5 in BlockClear2_1 0x13858A0,
// 0x1385A95 in BlockClear2_3 0x13859F0): the party stays in the session after the boss. Both
// roles run these states after a kill (live run r3: the host left 12 s after the kill, which
// ended the guest's session although its own leave was dropped), so both are kept.
std::atomic<double> g_k10_at{0};  // when K10 last kept the party (BossClearInvincibleTick)
std::atomic<bool> g_k10_this_load{false};  // K10 ran since the last load (DeadAfterClearTick)
BB_COOP_SYSV u64 BossClearLeaveHook(u64 session) {
    const int role = Role();
    if (g_on.load() && (role == RoleClient || role == RoleHost)) {
        Log("K10: boss-clear leave suppressed (%s; party stays)", role == RoleHost ? "host" : "guest");
        g_k10_at = Now();
        g_k10_this_load = true;
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

// K10b: BlockClear2 also locks the session (LockSession 0x178D7B0 via 0x1384B89: SprjSessionManager
// +0x118 = 1). With K10 the session outlives the clear, so the lock stays until the session ends,
// and the host's session update 0x1ED0B20 (event 0xE, member joined) kicks every later joiner
// with reason 0xFF000019 (the guest's OnJoinClearedRoom / OnBeKickOut): a guest that left after a
// boss and pressed Rejoin was kicked 0.9 s after each join (parx2 smoke3, live). The party lets
// its members join a cleared area, so that call (0x1ED14FB -> 0xC8EE30) is skipped while the party
// is on; other kicks (invaders, the party's own Kick) use other paths.
constexpr u64 kLockKickCall = 0x1ed14fb;
constexpr u64 kSessionKick = 0xc8ee30;  // (SprjSessionManager +0x18, npid): KickoutRoomMember 0xFF000019
BB_COOP_SYSV u64 LockKickHook(u64 matching, u64 npid) {
    if (g_on.load() && Role() == RoleHost) {
        (void)npid;
        // The lock also skips the joiner's registration in that update (0x1ED1245: the member
        // tables +0x170 / +0x178), so the joiner never gets the world data (K10d). A lock taken
        // anyway (another path than 0x1384B89) is lifted here for the next join.
        u64 sm = 0;
        if (SafeGet(Guest(kSlotSession), &sm) && sm) {
            const u8 zero = 0;
            SafeWrite(sm + 0x118, &zero, 1);
        }
        Log("K10b: the session lock after a boss clear would kick a joining member (0xFF000019): skipped, the "
            "party joins it");
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64, u64);
    return reinterpret_cast<Fn>(Guest(kSessionKick))(matching, npid);
}

// K10d (confid, post-merge smoke 2026-10-15): BlockClear2's LockSession (0x178D7B0, called from
// 0x1384B89) sets SprjSessionManager +0x118 = 1 and kicks members still joining. While the session
// is locked, the host's session update 0x1ED0B20 not only kicks a new joiner (K10b) but also skips
// its registration (0x1ED121E jne -> 0x1ED14F1 jumps past 0x1ECC970 / 0x1ECC130): a guest that
// title-joined after a boss kill (a crash rejoin) got no world data ("title join: FAILED (no snapshot
// within 12 s)", twice in smoke s2) and the host's game later kicked ANOTHER member with 0xFF000020
// (its stale-member timeout, 0xC99FA2), ~40 s after each crash. The party lets members join a
// cleared area, so the lock is not taken while the party is on (both roles; vanilla otherwise).
constexpr u64 kLockSessionCall = 0x1384b89;
constexpr u64 kLockSession = 0x178d7b0;  // (obj): SprjSessionManager +0x118 = 1, kicks pending joiners
BB_COOP_SYSV void LockSessionHook(u64 obj) {
    if (g_on.load() && (Role() == RoleHost || Role() == RoleClient)) {
        Log("K10d: boss-clear session lock skipped (%s; members may still join)", Role() == RoleHost ? "host" : "guest");
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(kLockSession))(obj);
}

// K10c: the boss-clear Lua state 0x1385470 schedules, for a summoned cooperator (local chr type 1),
// the Lua event 0xFF9 "RecallMenuEvent_MisssionClear_forNormalCoop" (call 0x13855DB -> 0x132B1C0):
// the message "The final battle has ended. It is time to bid farewell." that precedes the vanilla
// send-home. With K10 the guest stays in the party (its send-home is dropped), so the message only
// told it something false (live g1x, parlast: a Chalice layer boss killed by the host 90 m away;
// the box sat on the guest's screen). Skipped while the party is on (`BB_PARTY_BOSS_FAREWELL=1`:
// the vanilla message); the next state BlockClear2_2 is scheduled as before.
constexpr u64 kFarewellCall = 0x13855db, kLuaEventSchedule = 0x132b1c0;
BB_COOP_SYSV u64 BossFarewellHook(float t, u64 state, u64 id, u64 name, u64 a, u64 b, u64 c) {
    static const bool vanilla = std::getenv("BB_PARTY_BOSS_FAREWELL") && std::getenv("BB_PARTY_BOSS_FAREWELL")[0] == '1';
    if (g_on.load() && Role() == RoleClient && !vanilla) {
        Log("K10c: the boss-clear farewell message for a summoned cooperator skipped (the party stays)");
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(float, u64, u64, u64, u64, u64, u64);
    return reinterpret_cast<Fn>(Guest(kLuaEventSchedule))(t, state, id, name, a, b, c);
}

// After K10 the party stays. In case the boss-clear states left the players invincible for the
// clear animation (Lua EnableInvincible 0x1332EF0: chr +0x1E1 bit 14) with no leave to end it,
// the local player's flag is cleared 10 s after K10 (live runs: it was never still on).
void BossClearInvincibleTick(const GameSnapshot& s, double now) {
    const double at = g_k10_at.load();
    if (at <= 0 || now - at < 10.0 || !s.world_up || s.loading || !s.player) {
        return;
    }
    g_k10_at = 0;
    std::uint32_t f = 0;
    if (!SafeRead(s.player + 0x1e1, &f, 4)) {
        return;
    }
    if (f & (1u << 14)) {
        f &= ~(1u << 14);
        SafeWrite(s.player + 0x1e1, &f, 4);
        Log("K10: the boss-clear invincibility was still on 10 s after the clear; cleared (the party stays)");
    } else {
        Log("K10: no invincibility left 10 s after the clear");
    }
}

// ---- Remote player chrs on a streamed-out block (host and guest) ----
// A chr keeps the map hit (collision part) it stands on in +0x288 (set by 0x18BBA70; +0x350 a
// second one the respawn path 0x18BB670 re-applies). A map's own chrs die with their block and the
// local player's block never unloads under it, but a *remote* player chr standing in a block this
// game streams out (the other player far away, past the confinement walls the party opens) keeps
// a pointer into the freed hit array: the physics pre-step 0x18CB0E0 then reads
// [[hit+0x18]+0x88] and the game faults at 0x18CB16A / 0x18CB17D (live, edgetravel 2026-10-11).
// Before that pre-step, a remote player chr's hit that is no longer in the hit registry
// (*0x553E850: +0x20 block count, +0x28 block records of 0xB8: +0x78 hit count, +0x80 hits of
// 0x138 - the table 0x13C9FB0 resolves hits from) is cleared; only pointers are compared, the
// stale hit is never read. The chr then has no ground hit until it stands on loaded collision
// again (its position keeps coming from its owner).
constexpr u64 kSlotHitMan = 0x553e850;
std::atomic<u32> g_stale_hits{0};
bool HitLive(u64 hit) {
    const u64 man = *reinterpret_cast<const u64*>(Guest(kSlotHitMan));
    if (!man) {
        return false;
    }
    const int blocks = *reinterpret_cast<const int*>(man + 0x20);
    const u64 recs = *reinterpret_cast<const u64*>(man + 0x28);
    if (!recs || blocks <= 0 || blocks > 4096) {
        return false;
    }
    for (int i = 0; i < blocks; ++i) {
        const u64 rec = recs + u64(i) * 0xb8;
        const int n = *reinterpret_cast<const int*>(rec + 0x78);
        const u64 base = *reinterpret_cast<const u64*>(rec + 0x80);
        if (base && n > 0 && hit >= base && hit < base + u64(n) * 0x138) {
            return (hit - base) % 0x138 == 0;
        }
    }
    return false;
}
/// Diagnostics: the hit of a chr and where the registry holds it.
std::string DescribeHit(u64 chr) {
    const u64 hit = Field<u64>(chr, 0x288);
    char b[200];
    if (!hit) {
        return "hit 0";
    }
    const u64 man = Slot(kSlotHitMan);
    const int blocks = Field<int>(man, 0x20);
    const u64 recs = Field<u64>(man, 0x28);
    int at = -1;
    for (int i = 0; recs && i < blocks && i < 4096; ++i) {
        const u64 rec = recs + u64(i) * 0xb8;
        const int n = Field<int>(rec, 0x78);
        const u64 base = Field<u64>(rec, 0x80);
        if (base && n > 0 && hit >= base && hit < base + u64(n) * 0x138) {
            at = i;
        }
    }
    const u64 r18 = Field<u64>(hit, 0x18);
    std::snprintf(b, sizeof b, "hit %#llx block #%d/%d +0xc0 %02x +0x18 %#llx (+0x344 %#x) +0x20 %#llx",
                  ull(hit), at, blocks, unsigned(Field<u8>(hit, 0xc0)), ull(r18), Field<u32>(r18, 0x344),
                  ull(Field<u64>(hit, 0x20)));
    std::string out = b;
    if (at >= 0) {
        const u64 rec = recs + u64(at) * 0xb8;
        std::snprintf(b, sizeof b, " rec[+0..+0x20] %llx %llx %llx %llx %llx", ull(Field<u64>(rec, 0)),
                      ull(Field<u64>(rec, 8)), ull(Field<u64>(rec, 0x10)), ull(Field<u64>(rec, 0x18)),
                      ull(Field<u64>(rec, 0x20)));
        out += b;
    }
    return out;
}

bool RemotePlayerChr(u64 chr) {
    const u64 wcm = *reinterpret_cast<const u64*>(Guest(kSlotWorldChrMan));
    if (!wcm || chr == *reinterpret_cast<const u64*>(wcm + 0x60)) {
        return false;
    }
    const int n = *reinterpret_cast<const int*>(wcm + 0x30);
    const u64 entries = *reinterpret_cast<const u64*>(wcm + 0x38);
    if (!entries || n <= 0 || n > 8) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (*reinterpret_cast<const u64*>(entries + u64(i) * 0x38) == chr) {
            return true;
        }
    }
    return false;
}
BB_COOP_SYSV void ChrPreStepHook(u64 chr, u64, u64, u64, u64, u64) {  // rdi = the chr (mov rbx, rdi)
    if (!chr) {
        return;
    }
    u64* hit = reinterpret_cast<u64*>(chr + 0x288);
    u64* hit2 = reinterpret_cast<u64*>(chr + 0x350);
    if ((!*hit || HitLive(*hit)) && (!*hit2 || HitLive(*hit2))) {
        return;
    }
    if (!RemotePlayerChr(chr)) {
        return;
    }
    if (*hit && !HitLive(*hit)) {
        *hit = 0;
    }
    if (*hit2 && !HitLive(*hit2)) {
        *hit2 = 0;
    }
    g_stale_hits.fetch_add(1, std::memory_order_relaxed);
}

// BeginLeaveSession entry (ReplacePrologue: jumped to, so the return address is the caller's):
// logs which call site ends a B2 session; the call itself is unchanged.
void* g_leave_orig = nullptr;
std::atomic<bool> g_host_irregular_leave{false};
__attribute__((noinline)) BB_COOP_SYSV u64 LeaveEntryHook(u64 session) {
    const u64 ret = reinterpret_cast<u64>(__builtin_return_address(0));
    const int role = Role();
    if (g_on.load() && (role == RoleClient || role == RoleHost)) {
        const u64 base = Guest(0);
        if (ret > base + 5 && ret - base < 0x6000000) {
            const u64 site = ret - base - 5;
            if (role == RoleHost && (site == 0x178980d || site == 0x1789cb4)) g_host_irregular_leave.store(true);
            Log("leave: BeginLeaveSession called from +0x%llx (role %d); session event handler state: flow +0x84 %d, "
                "GSM +0x1592 %u, flow +0xF4 %.1f",
                ull(site), role, Field<int>(Slot(kSlotFlow), 0x84, -1), unsigned(Field<u8>(Slot(kSlotGsm), 0x1592)),
                double(Field<float>(Slot(kSlotFlow), 0xf4)));
        } else {
            Log("leave: BeginLeaveSession called through a party hook (role %d)", role);
        }
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(g_leave_orig)(session);
}

// K11: the packet-0x10 apply 0x13BECA0(SprjEventFlagMan, blob) at 0x17851CE. At a vanilla join
// the guest is still in its own world (load mode 0) when it runs; a B2 re-entry receives it in
// load mode 1 (the host-world overlay active), and the result read back all zero (runs 18-20).
// So for a B2 re-entry the store is put back to load mode 0 (SetLoadMode 0x13BB590) first, as at
// a join; the guest's MoveMapStep sets mode 1 again from the fresh overlay a moment later.
BB_COOP_SYSV u64 FlagApplyHook(u64 man, u64 blob) {
    using Apply = u64(BB_COOP_SYSV*)(u64, u64);
    using Mode = void(BB_COOP_SYSV*)(u64, u32);
    const int mode = Field<int>(man, 0x80, -1);
    if (g_on.load() && g_rejoin.load() && Role() == RoleClient && mode == 1) {
        reinterpret_cast<Mode>(Guest(0x13bb590))(man, 0);
        const u64 r = reinterpret_cast<Apply>(Guest(0x13beca0))(man, blob);
        Log("K11: host flag snapshot applied in load mode 0 (was 1) -> %llu", ull(r));
        coop::progress::OnVanillaSnapshotApplied();  // the host's whole world into the new overlay
        return r;
    }
    const u64 r = reinterpret_cast<Apply>(Guest(0x13beca0))(man, blob);
    coop::progress::OnVanillaSnapshotApplied();  // the vanilla snapshot has 12 groups: the mirror adds the rest
    return r;
}

// K5: Lua OnMatchingCheck: a member's chr is missing (it is loading).
BB_COOP_SYSV u64 MatchCheckHook(u64 session) {
    // Split travel: a member apart (no chr here, by design), or a chr removed / re-created in the
    // last 15 s (live split1: the leave came 50 ms after a meet's rewind, the chr not built yet).
    const bool split = g_apart_k5.load() > 0 || Now() - g_split_change_at.load() < 15.0;
    if (g_on.load() && (g_rejoin.load() || split)) {
        Log("K5: OnMatchingCheck leave suppressed (%s)", g_rejoin.load() ? "re-join in flight" : "split travel: members apart");
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

// A5: right after a cross-map join, until the arrival re-join starts (1 s): the game's own
// summon load (Lua OnBeJoinStart, K6) is held - it built the guest's own map as the host's world.
std::atomic<bool> g_carry_hold{false};
// K6: Lua OnBeJoinStart_* after the flag snapshot.
// A4: the last Lua join start the game ran (JoinStartHook), for the flow-2 watchdog in Tick.
std::atomic<u64> g_join_lua{0};
std::atomic<double> g_join_start_at{-1.0};
std::atomic<int> g_join_type{0};
BB_COOP_SYSV void JoinStartHook(u64 lua) {
    {
        if (lua) g_join_lua = lua;
        g_join_start_at = Now();
        if (const int t = Field<int>(Slot(kSlotGsm), 0x16f0, 0)) g_join_type = t;
        // A4: what the Lua join start sees (its event comes from GSM +0x16F0, 0 = none; the Lua's
        // SummonReloadStart 0x1389440 loads only with presentation count (+0x16F8 +0x18) >= 2 and
        // the own chr alive, otherwise it clears +0x16F0 and the join sits in flow 2 with no load).
        const u64 gsm = Slot(kSlotGsm);
        const u64 pres = Field<u64>(gsm, 0x16f8);
        Log("K6: OnBeJoinStart: type %d, slot %u, presentation count %d used %d, role %d, flow %d%s",
            Field<int>(gsm, 0x16f0, 0), unsigned(Field<u8>(gsm, 0x16f4)), Field<int>(pres, 0x18, -1),
            Field<int>(pres, 0x14, -1), Role(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            g_on.load() && (g_rejoin.load() || g_carry_hold.load()) ? "" : ": the game's own load starts");
    }
    // Test aid (BB_PARTY_TEST_JOIN_STALL=N): the N-th game join start of this process ends as the
    // 0.5 s bell timer stall did - SummonReloadStart cleared the summon type and nothing loads - so
    // the flow-2 watchdog in Tick is checked live.
    {
        static const int stall_at = std::getenv("BB_PARTY_TEST_JOIN_STALL") ? std::atoi(std::getenv("BB_PARTY_TEST_JOIN_STALL")) : 0;
        static int starts = 0;
        if (stall_at > 0 && !(g_on.load() && (g_rejoin.load() || g_carry_hold.load())) && ++starts == stall_at) {
            const int zero = 0;
            SafeWrite(Slot(kSlotGsm) + 0x16f0, &zero, 4);
            Log("test: join stall: join start %d swallowed, summon type cleared (BB_PARTY_TEST_JOIN_STALL)", starts);
            return;
        }
    }
    if (g_on.load() && (g_rejoin.load() || g_carry_hold.load())) {
        Log("K6: OnBeJoinStart suppressed (%s)", g_carry_hold.load() ? "A5: the arrival re-join after a cross-map join loads the host's map"
                                                                    : "B2 re-join drives the reload");
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(kJoinStartLua))(lua);
}

// K7: scoped write of the funnel's "own world" branch (main thread, around our own call).
bool WriteCode(u64 off, const u8* bytes, std::size_t n) {
    unsigned char* at = Image() + off;
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(at, bytes, n);
    DWORD unused = 0;
    VirtualProtect(at, n, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    std::memcpy(at, bytes, n);
#endif
    return true;
}

// The guest's loading screen from the host's travel event on (one continuous screen): what the
// map-load request at 0x1929007..0x192902B writes inline into the NowLoading object
// (*0x5562878): +0x808 screen kind 5, request byte 0x556286B = 1, +0x17C0 = 0, +0x804 = 1 unless 2.
// The guest's own MoveMapStep clears it at InGameStart (0x176D3C0 from 0x193A755).
constexpr u64 kSlotNowLoading = 0x5562878, kNowLoadingByte = 0x556286b, kNowLoadingClear = 0x176d3c0;
bool g_early_screen = false;

void RaiseLoadingScreen() {
    const u64 nl = Slot(kSlotNowLoading);
    if (!nl) {
        return;
    }
    const int kind = 5, one = 1;
    const u8 req = 1;
    const float zero = 0.0f;
    SafeWrite(nl + 0x808, &kind, 4);
    SafeWrite(Guest(kNowLoadingByte), &req, 1);
    SafeWrite(nl + 0x17c0, &zero, 4);
    if (Field<int>(nl, 0x804) != 2) {
        SafeWrite(nl + 0x804, &one, 4);
    }
    g_early_screen = true;
    Log("guest: loading screen raised at the host's travel");
}

void DropLoadingScreen(const char* why) {
    if (!g_early_screen) {
        return;
    }
    g_early_screen = false;
    using Fn = u64(BB_COOP_SYSV*)();
    reinterpret_cast<Fn>(Guest(kNowLoadingClear))();
    Log("guest: early loading screen dropped (%s)", why);
}

struct FunnelKeepRoom {
    bool armed = false;
    FunnelKeepRoom() {
        const u8 jmp[2] = {0xeb, 0x6c};
        armed = Matches(kFunnelLeaveJne, {0x75, 0x6c}) && WriteCode(kFunnelLeaveJne, jmp, 2);
    }
    ~FunnelKeepRoom() {
        if (armed) {
            const u8 jne[2] = {0x75, 0x6c};
            WriteCode(kFunnelLeaveJne, jne, 2);
        }
    }
};

// ---- Driver ----

/// Rewinds every remote task (mode 1 = guest member, 2 = session owner) whose PlayerIns is gone
/// to STEP_Create with the create bit. Returns how many.
// A remote phantom is created hidden: STEP_Create 0x1E50090 clears chr +0x1E2 bit 4 (chr +0x1E0
// bit 20, the draw bit) for chr types 1 / 2 / 12 (0x1E502C3), and the game shows it either with
// the summon presentation or at the end of a load: InGameStart 0x193A3E0 sets the bit
// (0x193A93C) and calls chr vfunc +0x610(1) for every player chr whose vfunc +0x1B8 is 0. A chr our
// rewind re-creates in a running world gets neither (the presentation entry of that member is the
// old one), so it stayed invisible with its plate up (split24 / hd22 / hd26). Every task we rewind
// is remembered; once its chr exists it gets InGameStart's two steps.
std::map<u64, double> g_rewound;  // task -> when rewound (main thread)

void ShowLikeInGameStart(u64 c, const char* why) {
    using Bool = u8(BB_COOP_SYSV*)(u64);
    using Set = void(BB_COOP_SYSV*)(u64, u32);
    const u64 vt = Field<u64>(c, 0);
    const u64 f1b8 = Field<u64>(vt, 0x1b8), f610 = Field<u64>(vt, 0x610);
    if (!f1b8 || !f610 || reinterpret_cast<Bool>(f1b8)(c)) {
        return;
    }
    const u8 v = Field<u8>(c, 0x1e2);
    const u8 w = u8(v | 0x10);
    SafeWrite(c + 0x1e2, &w, 1);
    reinterpret_cast<Set>(f610)(c, 1);
    Log("remote chr %#llx (handle %x) shown as InGameStart does (%s; +0x1E2 %#x -> %#x)", ull(c), Field<u32>(c, 8), why,
        unsigned(v), unsigned(w));
}

std::atomic<double> g_rejoin_banner_at{0};  // Now() of the last B2 re-join (or a member chr rebuilt by a rewind)

int RewindRemoteTasks(const char* why) {
    int n = 0;
    // A member's chr rebuilt here (a revive in place, a new look, the host's revive): its task's
    // STEP_Create shows the join banner ("The bell's resonance has beckoned ...") again although
    // nobody joined (live parx s0b: on the host after a guest's revive at its lamp). The banner
    // filter treats the next seconds like a re-join's.
    struct BannerMark {
        ~BannerMark() { if (n_ref > 0) g_rejoin_banner_at = Now(); }
        int& n_ref;
    } mark{n};
    for (const TaskInfo& t : Tasks(false)) {
        if ((t.mode != 1 && t.mode != 2) || t.step < kStepCreate || (t.flags & 2)) {
            continue; // not remote, not created yet (the join itself is still running), or leaving
        }
        if (PlayerForPeer(t.task + kTaskPeer)) {
            continue;
        }
        const int next = kStepCreate;
        const u8 flags = u8(t.flags | 1);
        SafeWrite(t.task + kTaskFlags, &flags, 1);
        SafeWrite(t.task + kTaskNext, &next, 4);
        ++n;
        g_rewound[t.task] = Now();
        Log("%s: task mode %d (step %d) rewound to STEP_Create", why, t.mode, t.step);
    }
    return n;
}

/// Host: the join-time snapshot again, to one member (after the host's own load).
void LogIntroFlags(const char* when);

// F1 (internet profile, 2026-10-15): with 100-150 ms one-way and 2-3 % loss the game's flag /
// world sends (packets 0x10 / 0x11) can fail right after the host's own load (its reliable
// channel to the member still busy); the member then never got its snapshot, fell back to B1 45 s
// later and the host's game ended the session ("first sync never completed"). A failed re-send is
// retried every 0.5 s for up to 15 s (ResyncRetryTick).
struct ResyncRetry {
    u64 id = 0;
    double next = 0, until = 0;
    int tries = 0;
};
std::vector<ResyncRetry> g_resync_retry;  // main thread
bool ResyncMember(u64 id);
void ResyncRetryTick(const GameSnapshot& s, double now) {
    if (g_resync_retry.empty()) {
        return;
    }
    const std::vector<u64> members = Members();
    for (auto it = g_resync_retry.begin(); it != g_resync_retry.end();) {
        const bool here = std::find(members.begin(), members.end(), it->id) != members.end();
        if (!here || now > it->until || !s.world_up || s.loading) {
            if (now > it->until || !here) {
                Log("host: join snapshot re-send %s after %d retr%s", here ? "still FAILING" : "dropped (member gone)",
                    it->tries, it->tries == 1 ? "y" : "ies");
                it = g_resync_retry.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        if (now < it->next) {
            ++it;
            continue;
        }
        ++it->tries;
        it->next = now + 0.5;
        if (ResyncMember(it->id)) {
            Log("host: join snapshot re-sent on retry %d (%.1f s after the first try)", it->tries,
                now - (it->until - 15.0));
            it = g_resync_retry.erase(it);
            continue;
        }
        ++it;
    }
}

bool ResyncMember(u64 id) {
    LogIntroFlags("host at the re-send");
    const u64 flow = Slot(kSlotFlow);
    using Send = int(BB_COOP_SYSV*)(u64, u64, u32, const void*, u32);
    using Snap = u64(BB_COOP_SYSV*)(u64, u64);
    using Two = void(BB_COOP_SYSV*)(u64, u64);
    using Three = void(BB_COOP_SYSV*)(u64, u64, u32);
    alignas(8) u8 p19[0x44] = {};
    const u32 gd68 = Field<u32>(Slot(kSlotGameData), 0x68);
    std::memcpy(p19, &gd68, 4);
    reinterpret_cast<Send>(Guest(kSendPacket))(flow, id, 0x19, p19, 0x44);
    const bool flags = reinterpret_cast<Snap>(Guest(kSendFlags))(0, id) & 1;
    const bool world = reinterpret_cast<Snap>(Guest(kSendWorld))(0, id) & 1;
    if (const u64 bullets = Slot(kSlotBullet)) {
        reinterpret_cast<Two>(Guest(kSendBullets))(bullets, id);
    }
    const u64 wcm = Slot(kSlotWorldChrMan);
    const u64 netsync = Field<u64>(wcm, 0xa40);
    const u64 arg = Slot(kSlotNetSyncArg);
    if (netsync && arg) {
        const u32 a = Field<u32>(arg, 0x20);
        reinterpret_cast<Three>(Guest(kNetSyncJoin))(netsync, id, a);
        reinterpret_cast<Three>(Guest(kNetSyncData))(netsync, id, a);
    }
    if (const u64 slots = Field<u64>(Slot(kSlotGsm), 0x16f8)) {
        reinterpret_cast<Two>(Guest(kSendNpcSummons))(slots, id);
    }
    const u8 pslot = Field<u8>(Slot(kSlotGsm), 0x16f4);
    reinterpret_cast<Send>(Guest(kSendPacket))(flow, id, 0x42, &pslot, 1);
    Log("host: join snapshot re-sent (0x19, flags %s, world %s, bullets, net sync, NPC, 0x42)",
        flags ? "ok" : "FAILED", world ? "ok" : "FAILED");
    if (!(flags && world)) {
        bool queued = false;
        for (const ResyncRetry& r : g_resync_retry) {
            queued = queued || r.id == id;
        }
        if (!queued) {
            ResyncRetry r;
            r.id = id;
            r.next = Now() + 0.5;
            r.until = Now() + 15.0;
            g_resync_retry.push_back(r);
            Log("host: the snapshot send failed: retrying every 0.5 s for up to 15 s");
        }
        return false;
    }
    return true;
}

/// The chr behind a WorldChrMan handle ((set << 14) | index; table +0x850, entries 0x38), as the
/// game resolves it (0x15BDC20), with every read checked. 0 when stale.
u64 ChrOfHandle(u32 h) {
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm || h == 0xffffffffu) {
        return 0;
    }
    const u32 set = h >> 14 & 0x3f, idx = h & 0x3fff;
    if (int(set) >= Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2) {
        return 0;
    }
    const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
    if (!tab || int(idx) >= Field<int>(tab, 0, 0)) {
        return 0;
    }
    return Field<u64>(Field<u64>(tab, 8) + u64(idx) * 0x38, 0);
}

/// A boss fight is on in this world: a boss health bar is up. EMEVD 2003[11] (DisplayBossHealthBar,
/// in the bank-2003 handler 0x17C0420) writes the HUD object *0x5562878 +0x1658 + slot * 0x14
/// {name id, chr handle, ...} for slots 0..2 and resets them all to -1 when the bar goes. Counted
/// while the handle resolves to a living chr. `who` (optional): the boss's handle and HP.
bool BossFightActive(std::string* who = nullptr) {
    const u64 hud = Slot(0x5562878);
    if (!hud) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        const u32 h = Field<u32>(hud, 0x165c + u64(i) * 0x14, ~0u);
        if (h == ~0u) {
            continue;
        }
        const u64 chr = ChrOfHandle(h);
        const int hp = Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0xf8, -1);
        if (chr && hp > 0) {
            if (who) {
                char b[96];
                std::snprintf(b, sizeof b, "boss bar %d: name %d chr %08x hp %d", i,
                              Field<int>(hud, 0x1658 + u64(i) * 0x14, -1), h, hp);
                *who = b;
            }
            return true;
        }
    }
    return false;
}

/// The presentation slot table (GSM +0x16F8): 5 entries at +0x1C (stride 0x14: chr handle, kind,
/// state, a, b), the counts +0x8/+0xC/+0x10/+0x14, the handle list +0x80..+0x8C (+0x90 count)
/// and the max (flow +0xC). The game's cooperator count (0x15BDC20) walks the entries.
std::string DescribePresence(bool deep = false) {
    const u64 t = Field<u64>(Slot(kSlotGsm), 0x16f8);
    if (!t) {
        return "none";
    }
    std::string out;
    char b[160];
    std::snprintf(b, sizeof b, "count %d (h %d n %d e %d) max %d; local %x;", Field<int>(t, 0x14, -1),
                  Field<int>(t, 0x8, -1), Field<int>(t, 0xc, -1), Field<int>(t, 0x10, -1),
                  Field<int>(Slot(kSlotFlow), 0xc, -1), Field<u32>(Field<u64>(Slot(kSlotWorldChrMan), 0x60), 8, ~0u));
    out += b;
    for (int i = 0; i < 5; ++i) {
        const u64 e = t + 0x1c + u64(i) * 0x14;
        const u32 h = Field<u32>(e, 0, ~0u);
        if (h == 0xffffffffu) {
            continue;
        }
        const u64 chr = ChrOfHandle(h);
        std::snprintf(b, sizeof b, " [%d h %x kind %d st %d %d %d chr %s", i, h, Field<int>(e, 4), Field<int>(e, 8),
                      Field<int>(e, 0xc), Field<int>(e, 0x10),
                      !chr ? "STALE" : Field<u32>(chr, 8, ~0u) == h ? "ok" : "other");
        out += b;
        if (deep && chr && Field<u32>(chr, 8, ~0u) == h) {
            // What the cooperator count asks (vfunc +0x558: the net chr type, counted when in
            // {1,5,7,19}) and where it comes from: GameData (vfunc +0x1C8) +0x5D8 session type ->
            // SessionTypeDesc 0x553D750 + st * 0x80 +0x15; +0xA4 chr type.
            using Get = u64(BB_COOP_SYSV*)(u64);
            const u64 vt = Field<u64>(chr, 0);
            const u64 f558 = Field<u64>(vt, 0x558), f1c8 = Field<u64>(vt, 0x1c8);
            const int net = f558 ? int(u32(reinterpret_cast<Get>(f558)(chr))) : -1;
            const u64 gd = f1c8 ? reinterpret_cast<Get>(f1c8)(chr) : 0;
            const int st = Field<int>(gd, 0x5d8, -1);
            std::snprintf(b, sizeof b, " net %d (fn +%llx) gd st %d desc %d a4 %d", net,
                          ull(f558 ? f558 - Guest(0) : 0), st,
                          st >= 0 && st < 64 ? int(Field<u8>(Guest(kSessionTypeDesc + u64(st) * 0x80), 0x15, 0xff)) : -1,
                          Field<int>(gd, 0xa4, -1));
            out += b;
        }
        out += "]";
    }
    std::snprintf(b, sizeof b, " list %x %x %x %x (%d)", Field<u32>(t, 0x80), Field<u32>(t, 0x84), Field<u32>(t, 0x88),
                  Field<u32>(t, 0x8c), Field<int>(t, 0x90, -1));
    out += b;
    return out;
}

enum class GuestPhase { Idle, AwaitSnapshot, Loading, Rewinding, AwayLoading, InWorldFollow };
enum class HostPhase { Idle, Loading, Resynced };

struct State {
    std::mutex mu;
    // guest
    GuestPhase gphase = GuestPhase::Idle;
    bool have_intent = false;
    TravelIntent intent;  // under mu
    double since = 0;
    bool load_seen = false;
    bool screen_done = false;
    int stable = 0;
    // host
    HostPhase hphase = HostPhase::Idle;
    double hsince = 0;
    bool alone_load = false;  // host: this own load is an alone travel (split travel)
    bool arrived = false;  // a guest said it arrived (under mu)
    bool rejoined_logged = false; // host: an "arrived" came since the resync
    bool respawn_request = false;  // a guest died alone and reloads this world (under mu)
    bool respawn_live = false;     // ... or it travels alone into this world ("rejoin": not dead)
    bool away_request = false;     // a guest leaves this area alone ("away"): drop its chr (under mu)
    bool away_have_pos = false;
    float away_pos[3] = {};
    bool enter_state1 = false;     // guest: the follow's flow state 1 is entered by the main thread
    // guest: the running re-join is this guest's own travel (split travel): the destination is
    // its own (no transform beside the host, no summoned placement); the GSM +0x1521 it cleared.
    bool solo = false;
    bool blobs_saved = false;  // guest: this re-join's host world blobs were copied
    int saved_1521 = -1;
    u32 warp_lamp = 0;      // guest: the host's in-world party move to follow (main thread)
    double inworld_since = 0;  // guest: an in-world follow of a host load into this block
    bool respawn_have_pos = false;  // where it died (its member chr here stands there)
    float respawn_pos[3] = {};
    bool have_dest = false; // guest: the host's post-load transform (under mu)
    int hp_before = -1;      // guest: its HP at the host's travel event (kept through the B2 load)
    std::vector<std::pair<int, float>> buffs_before;  // guest: timed SpEffects (id, s left) at the travel
    u32 dest_stage = 0;
    float dest_pos[4] = {}, dest_rot[4] = {};
    // probe
    bool was_loading = false;
    u64 player_before = 0;
    std::deque<std::pair<int, std::string>> out;  // under mu
};
State& S() {
    static State s;
    return s;
}

void Send(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().out.emplace_back(slot, body);
}

std::string DescribeBlobs();
void ProbeTick(const GameSnapshot& s) {
    State& st = S();
    const bool loading = s.loading || !s.world_up;
    if (loading && !st.was_loading) {
        st.player_before = s.player;
        Log("load started: blobs %s", DescribeBlobs().c_str());
        Log("load started: local PlayerIns %#llx, role %d, flow %d; tasks:%s; presence %s", ull(s.player), Role(),
            Field<int>(Slot(kSlotFlow), 0x84, -1), DescribeTasks().c_str(), DescribePresence().c_str());
    } else if (!loading && st.was_loading) {
        Log("B2 load report: map %s, local PlayerIns %#llx -> %#llx (%s), player set %d, role %d, flow %d; tasks:%s",
            MapName(s.map_id).c_str(), ull(st.player_before), ull(s.player),
            st.player_before == s.player ? "SAME object" : "re-created",
            Field<int>(Slot(kSlotWorldChrMan), 0x30, -1), Role(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            DescribeTasks().c_str());
        Log("B2 load report: presence %s", DescribePresence().c_str());
        const u64 gsm = Slot(kSlotGsm);
        Log("B2 load report: GSM +0x1593 %u +0x1594 %#x +0x1598 %u +0x159c %#x, GameDataMan +0x68 %#x, +0x0c %#x",
            unsigned(Field<u8>(gsm, 0x1593)), Field<u32>(gsm, 0x1594), unsigned(Field<u8>(gsm, 0x1598)),
            Field<u32>(gsm, 0x159c), Field<u32>(Slot(kSlotGameData), 0x68), Field<u32>(gsm, 0x0c));
        Log("B2 load report: blobs %s", DescribeBlobs().c_str());
    }
    st.was_loading = loading;
}

std::atomic<int> g_local_slot{-1};
std::atomic<bool> g_fell_back{false};  // a B1 fallback ran: clean the network flow once idle

// ---- Split travel (docs/party/split_travel.md): members in different areas ----
// Where every member is, from the party link's vitals (the director, every tick): map (0xAABBCCDD)
// and position as that member's own game reads them.
std::vector<Where> g_where;  // main thread
std::atomic<int> g_apart{0};  // members whose area differs from ours (K5, the host watchdog)
std::atomic<bool> g_host_alone_next{false};  // host: the next own load is an alone travel
std::atomic<double> g_host_alone_at{0};
std::atomic<bool> g_host_left{false};
bool g_alone_spot_valid = false;  // guest: an alone load's lamp spot to stand on after the arrival
float g_alone_spot[4] = {};
float g_alone_spot_yaw = 0;  // guest: the host travelled alone; drop its chr here
std::atomic<double> g_host_alone_seen{0};  // guest: when the host's last alone travel arrived (Now())
// A member that just travelled alone still reports its old map for a moment (its vitals stay
// until its load ends): no meet with it until its vitals show another area, or 30 s passed
// (live split3: the host re-created a guest's chr right after its "away").
std::atomic<double> g_away_until[8] = {};
void NoteAway(int slot) {
    if (slot >= 0 && slot < 8) {
        g_away_until[slot] = Now() + 30.0;
    }
}

void SendDeadEnemies(const GameSnapshot& s, const char* why);
std::string DescribeEnterLeave();
std::string DescribeRender(u64 chr);
double g_meet_rewind_at = 0;
std::atomic<bool> g_record_request{false};
double g_arrived_at = 0;  // guest: send "arrived" then (after an in-world follow)  // a member asked for our record (EVENT b2 record)  // main thread: the meet's re-creation, after the records arrived
/// This player's record to every member (0x1781AC0, what InGameStart runs after a load). Main thread.
void RebroadcastOwnRecord(const char* why) {
    reinterpret_cast<void(BB_COOP_SYSV*)()>(Guest(0x1781ac0))();
    Log("split: own player record re-broadcast (%s)", why);
}
u32 AreaOf(u32 map) {
    return (map == 0 || map == 0xffffffffu) ? 0u : map >> 24;
}

const Where* WhereOf(int slot) {
    for (const Where& w : g_where) {
        if (w.slot == slot) {
            return &w;
        }
    }
    return nullptr;
}

/// The host's current map as the vitals say (guest side); 0 unknown.
u32 HostMap() {
    const Where* w = WhereOf(0);
    return w && w->age_ms < 5000 ? w->map : 0u;
}

/// Removes a remote player chr the way its task's STEP_Delete 0x1E50880 / STEP_DeleteWait
/// 0x1E50A70 do, the task kept (rewound to STEP_Create when the two meet again): first the
/// EnterleaveDirector leave (0x19704B0(director *0x553E8B0, chr handle, presentation slot task
/// +0xEC) marks the entry leaving, +0xFD = 1; chr data +0x3B0 -> +8 +0x1BC |= 2), then, once the
/// entry's leave has played (+0x135) or 2 s later, the presentation slot release 0x15BC080 and the
/// WorldChrMan remove 0x19186E0. Without the leave the director kept the old entry (it is keyed by
/// the member's PlayerId): the re-created chr found it (find-or-create 0x19700A0) and stayed
/// invisible while its name plate hung at the old spot (user reports split24 / hd22). Main thread.
struct PendingRemoval {
    u64 chr = 0, entry = 0;
    double since = 0;
    std::string why;
    bool member_gone = false;  // A7: the member left the session (no task holds the chr any more)
};
std::vector<PendingRemoval> g_removals;

/// The EnterleaveDirector's entries (*0x553E8B0 +0xF0: list sentinel, node [3] = entry; entry
/// +0xD0 key, +0xF8 handle?, +0xFC presentation slot, +0xFD leaving, +0x134 / +0x135 flags).
std::string DescribeEnterLeave() {
    const u64 d = Slot(0x553e8b0);
    const u64 head = Field<u64>(d, 0xf0);
    std::string out;
    u64 node = Field<u64>(head, 0);
    for (int guard = 0; head && node && node != head && guard < 16; ++guard) {
        const u64 e = Field<u64>(node, 0x18);
        char b[128];
        std::snprintf(b, sizeof b, " [%#llx key %x h %x slot %d leave %u f134 %u f135 %u]", ull(e), Field<u32>(e, 0xd0),
                      Field<u32>(e, 0xf8), int(int8_t(Field<u8>(e, 0xfc))), unsigned(Field<u8>(e, 0xfd)),
                      unsigned(Field<u8>(e, 0x134)), unsigned(Field<u8>(e, 0x135)));
        out += b;
        node = Field<u64>(node, 0);
    }
    return out.empty() ? " none" : out;
}

void FinishRemoval(const PendingRemoval& r, const char* how) {
    using Release = u64(BB_COOP_SYSV*)(u64, u32);
    using Remove = void(BB_COOP_SYSV*)(u64, u64);
    bool still = false;
    for (const TaskInfo& t : Tasks(false)) {
        still = still || ((t.mode == 1 || t.mode == 2) && PlayerForPeer(t.task + kTaskPeer) == r.chr);
    }
    if (r.member_gone) {
        // No task holds it: still there if the ChrSet still lists it.
        const u64 wcm = Slot(kSlotWorldChrMan);
        const int n = Field<int>(wcm, 0x30, 0);
        const u64 entries = Field<u64>(wcm, 0x38);
        for (int i = 0; entries && i < n && i <= 8; ++i) {
            still = still || Field<u64>(entries + u64(i) * 0x38, 0) == r.chr;
        }
    }
    if (!still) {
        Log("split: remote chr %#llx already gone (%s)", ull(r.chr), r.why.c_str());
        return;
    }
    float p[3] = {};
    ChrPos(r.chr, p);
    reinterpret_cast<Release>(Guest(0x15bc080))(Field<u64>(Slot(kSlotGsm), 0x16f8), Field<u32>(r.chr, 8));
    reinterpret_cast<Remove>(Guest(0x19186e0))(Slot(kSlotWorldChrMan), r.chr);
    g_split_change_at = Now();
    Log("split: remote chr %#llx at (%.1f, %.1f, %.1f) removed (%s; leave %s); %s", ull(r.chr), p[0], p[1], p[2],
        r.why.c_str(), how, r.member_gone ? "the member left the session" : "its task is kept");
}

void RemoveRemoteChr(u64 chr, const char* why, bool member_gone = false) {
    for (const PendingRemoval& r : g_removals) {
        if (r.chr == chr) {
            return;  // already leaving
        }
    }
    u8 pslot = member_gone ? 7 : 0;  // a remote member's presentation slot (live: 7 on every guest's task)
    for (const TaskInfo& t : Tasks(false)) {
        if ((t.mode == 1 || t.mode == 2) && PlayerForPeer(t.task + kTaskPeer) == chr) {
            pslot = t.slot;
        }
    }
    PendingRemoval r;
    r.chr = chr;
    r.since = Now();
    r.why = why;
    r.member_gone = member_gone;
    const u64 director = Slot(0x553e8b0);
    if (director) {
        using Leave = u64(BB_COOP_SYSV*)(u64, u32, u64);
        const u64 e = reinterpret_cast<Leave>(Guest(0x19704b0))(director, Field<u32>(chr, 8), u64(int(int8_t(pslot))));
        r.entry = e == ~0ull ? 0 : e;
        const u64 cd = Field<u64>(Field<u64>(chr, 0x3b0), 8);
        if (cd) {
            const u8 v = u8(Field<u8>(cd, 0x1bc) | 2);
            SafeWrite(cd + 0x1bc, &v, 1);
        }
    }
    g_split_change_at = Now();
    Log("split: remote chr %#llx (handle %x) leaves (%s): presentation entry %#llx; enterleave%s", ull(chr),
        Field<u32>(chr, 8), why, ull(r.entry), DescribeEnterLeave().c_str());
    g_removals.push_back(r);
}

/// Main thread, every frame: removals whose leave has played (or timed out).
void RemovalTick(double now) {
    for (std::size_t i = 0; i < g_removals.size();) {
        const PendingRemoval& r = g_removals[i];
        const bool played = r.entry && Field<u8>(r.entry, 0x135) != 0;
        if (played || now - r.since >= 2.0 || !SessionUp()) {
            const PendingRemoval done = r;
            g_removals.erase(g_removals.begin() + std::ptrdiff_t(i));
            if (SessionUp()) {
                FinishRemoval(done, played ? "played" : "timed out");
            }
            continue;
        }
        ++i;
    }
}

/// A7 (parab4 a7b, live, 3 players): when a guest leaves the session, the other guests' games end
/// its remote player task but keep its PlayerIns in the ChrSet (cooperators stay 2, the presence
/// entry stays). When it rejoins, the kept PlayerIns comes back (its PlayerId matches the member
/// again) next to the new one: two chrs read the member's type-6 packet queue from two chr threads
/// (the 4-player crash), the cooperator count is one too high, and a 3rd rejoin crashed a task step
/// (0x1E50090). A guest now removes, the way the host's game does, a player chr whose member has no
/// peer entry any more (SprjNetMan +0x28, 0x1789380) and that no task holds, 2 s after it saw it.
/// Main thread, client role, steady world.
struct StaleChr {
    u64 chr = 0;
    double since = 0;
};
std::vector<StaleChr> g_stale_chrs;
double g_stale_scan_at = 0;
void StaleMemberChrTick(double now) {
    if (Role() != RoleClient || now - g_stale_scan_at < 0.5) {
        return;
    }
    g_stale_scan_at = now;
    using EntryFn = u64(BB_COOP_SYSV*)(u64, u64);
    const u64 netman = Slot(0x553d6d0);  // SprjNetMan: +0x28 the peer entries
    const u64 head = Field<u64>(netman, 0x28);
    if (!netman || !head || Field<u64>(head, 0) == head) {
        g_stale_chrs.clear();  // no peer at all (a load, a leave): nothing to judge
        return;
    }
    const u64 wcm = Slot(kSlotWorldChrMan);
    const int n = Field<int>(wcm, 0x30, 0);
    const u64 entries = Field<u64>(wcm, 0x38);
    const u64 local = Field<u64>(wcm, 0x60);
    std::vector<StaleChr> seen;
    const std::vector<TaskInfo> tasks = Tasks(false);
    // C6 on a bystander (confid 2026-10-16, repeat_party c6 --players 3): a member whose game
    // CRASHED and came back keeps its PlayerId, so its old chr still matches a peer entry (the new
    // one) and the check below never removed it: one more frozen copy of that hunter on the other
    // guest's screen per crash (its cooperators 2 -> 3 -> 4), two chrs reading one peer queue (the
    // A7 safety net skipped 3921 stale reads) and, after 16 crashes, that guest's own game crashed.
    // A chr that no task holds while another chr of the same peer entry is held is removed too.
    struct Row {
        u64 chr, entry;
        bool held;
    };
    std::vector<Row> rows;
    for (int i = 0; entries && i < n && i <= 8; ++i) {
        const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
        if (!chr || chr == local || !Field<u64>(chr, 0x3d8)) {
            continue;
        }
        bool held = false;
        for (const TaskInfo& t : tasks) {
            held = held || ((t.mode == 1 || t.mode == 2) && PlayerForPeer(t.task + kTaskPeer) == chr);
        }
        rows.push_back({chr, reinterpret_cast<EntryFn>(Guest(0x1789380))(netman, chr + 0x3d8), held});
    }
    for (const Row& row : rows) {
        if (row.held) {
            continue;
        }
        const char* why = "A7: its member left the session; no task holds it";
        if (row.entry) {
            bool newer = false;
            for (const Row& o : rows) newer = newer || (o.chr != row.chr && o.entry == row.entry && o.held);
            if (!newer) {
                continue;  // its member is still a peer (and this is its only chr)
            }
            why = "C6: its member's game restarted; the new chr of that member holds its task";
        }
        double since = now;
        for (const StaleChr& o : g_stale_chrs) {
            if (o.chr == row.chr) since = o.since;
        }
        seen.push_back({row.chr, since});
        if (now - since >= 2.0) {
            RemoveRemoteChr(row.chr, why, true);
        }
    }
    g_stale_chrs = std::move(seen);
}

/// The member slot whose reported position is nearest to `p` (within `max_m`), -1 none.
int SlotNear(const float p[3], float max_m) {
    int best = -1;
    float bd = max_m * max_m;
    for (const Where& w : g_where) {
        if (w.slot == g_local_slot.load() || !w.has_pos || w.age_ms > 3000) {
            continue;
        }
        const float d = (w.pos[0] - p[0]) * (w.pos[0] - p[0]) + (w.pos[1] - p[1]) * (w.pos[1] - p[1]) +
                        (w.pos[2] - p[2]) * (w.pos[2] - p[2]);
        if (d < bd) {
            bd = d;
            best = w.slot;
        }
    }
    return best;
}

/// The remote chr standing nearest `p` (within 6 m), 0 none.
u64 ChrNear(const float p[3]) {
    u64 victim = 0;
    float best = 36.0f;
    for (const TaskInfo& t : Tasks(false)) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        float q[3];
        if (c && ChrPos(c, q)) {
            const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]);
            if (d < best) {
                best = d;
                victim = c;
            }
        }
    }
    return victim;
}

/// Guest, main thread, before any flow state 1: the host's join-snapshot packets (0x19, 0x11,
/// 0x10) that arrived while this guest did not follow wait in the receive queue (0x1783160 pops
/// them only in state 1) and would be applied first - another map's, older flags. Popped and
/// dropped with the game's own receive 0x17893E0(flow, type, buf, size) (role 3/6 only).
void DrainSnapshotQueue(const char* why) {
    static std::vector<u8> buf(0x10000);
    using Recv = int(BB_COOP_SYSV*)(u64, u32, void*, u32);
    const u64 flow = Slot(kSlotFlow);
    if (!flow || Role() != RoleClient) {
        return;
    }
    std::string got;
    for (u32 type : {0x19u, 0x11u, 0x10u}) {
        int n = 0;
        for (int i = 0; i < 16; ++i) {
            const int r = reinterpret_cast<Recv>(Guest(0x17893e0))(flow, type, buf.data(), u32(buf.size()));
            if (r <= 0) {
                break;
            }
            ++n;
        }
        if (n) {
            char b[32];
            std::snprintf(b, sizeof b, " 0x%x x%d", type, n);
            got += b;
        }
    }
    if (!got.empty()) {
        Log("split: stale snapshot packets dropped before flow state 1 (%s):%s", why, got.c_str());
    }
}

/// Host: the per-peer net sync of this world (WorldChrMan +0xA40) for every member, without the
/// snapshot packets (a meet without a load: nobody is in flow state 1).
void NetSyncMembers() {
    using Three = void(BB_COOP_SYSV*)(u64, u64, u32);
    const u64 netsync = Field<u64>(Slot(kSlotWorldChrMan), 0xa40);
    const u64 arg = Slot(kSlotNetSyncArg);
    if (!netsync || !arg) {
        return;
    }
    const u32 a = Field<u32>(arg, 0x20);
    for (u64 id : Members()) {
        reinterpret_cast<Three>(Guest(kNetSyncJoin))(netsync, id, a);
        reinterpret_cast<Three>(Guest(kNetSyncData))(netsync, id, a);
    }
}

/// Both roles, main thread, steady world, no re-join running. (1) A remote chr whose owner (the
/// member reporting a position next to it) is in another area is removed; (2) members in our
/// area whose chr is missing here are built again (rewind), at most every 10 s: a meet without a
/// load (the host also re-registers its net sync); (3) the count of members apart.
void SplitTick(const GameSnapshot& s, double now) {
    static double last_meet = 0;
    static std::map<int, std::pair<u32, double>> seen;  // slot -> (area, since)
    if (!SessionUp() || !s.world_up || s.loading || !s.player) {
        return;
    }
    const u32 mine = AreaOf(s.map_id);
    if (mine == 0) {
        return;  // our own map not known yet (right after a load)
    }
    int apart = 0;
    bool meet = false;
    for (const Where& w : g_where) {
        if (w.slot == g_local_slot.load() || !w.connected) {
            continue;
        }
        const u32 a = w.age_ms < 5000 ? AreaOf(w.map) : 0u;
        auto& sn = seen[w.slot];
        if (sn.first != a) {
            sn = {a, now};
        }
        if (a == 0) {
            continue;  // loading / unknown: no decision
        }
        if (a != mine) {
            ++apart;
            if (w.slot >= 0 && w.slot < 8) {
                g_away_until[w.slot] = 0;  // elsewhere now: a later return to our area is a meet
            }
            continue;
        }
        // In our area for 2 s: its chr should be here.
        const float p[3] = {w.pos[0], w.pos[1], w.pos[2]};
        if (w.slot >= 0 && w.slot < 8 && g_away_until[w.slot].load() > now) {
            continue;  // left alone a moment ago; its vitals still show this area
        }
        if (now - sn.second >= 2.0 && w.has_pos && w.age_ms < 1500 && !ChrNear(p)) {
            meet = true;
        }
    }
    if (apart != g_apart.load()) {
        Log("split: %d member(s) in another area than ours (%s)", apart, MapName(s.map_id).c_str());
    }
    g_apart = apart;
    g_apart_k5 = apart;
    if (g_host_left.exchange(false) && Role() == RoleClient) {
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode == 2) {
                if (const u64 c = PlayerForPeer(t.task + kTaskPeer)) {
                    RemoveRemoteChr(c, "the host travelled alone");
                }
            }
        }
    }
    // (1) chrs of members that are elsewhere.
    for (const TaskInfo& t : Tasks(false)) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        float p[3];
        if (!c || !ChrPos(c, p)) {
            continue;
        }
        const int slot = SlotNear(p, 5.0f);
        const Where* w = slot >= 0 ? WhereOf(slot) : nullptr;
        if (w && AreaOf(w->map) != 0 && AreaOf(w->map) != mine) {
            char why[96];
            std::snprintf(why, sizeof why, "slot %d is in %s, we are in %s", slot, MapName(w->map).c_str(),
                          MapName(s.map_id).c_str());
            RemoveRemoteChr(c, why);
        }
    }
    // (2) a meet without a load.
    if (meet && now - last_meet >= 10.0) {
        bool chrless = false;
        for (const TaskInfo& t : Tasks(false)) {
            chrless = chrless || ((t.mode == 1 || t.mode == 2) && t.step >= kStepCreate && !(t.flags & 2) &&
                                  !PlayerForPeer(t.task + kTaskPeer));
        }
        if (chrless) {
            last_meet = now;
            if (Role() == RoleClient) {
                DrainSnapshotQueue("meet");
            } else {
                NetSyncMembers();
            }
            g_split_change_at = now;
            SendDeadEnemies(s, "meet without a load");
            // The member's player record (GameData row: 0xB / 8 / 0x3B / 0x3C / 0xD) is what
            // STEP_Create builds its chr from. After a load each player re-broadcasts it at
            // InGameStart (0x1781AC0); the side that did NOT load holds a stale row and built an
            // invisible chr whose name plate hung at an old spot (user report, split24: the host saw
            // "Co-operator Hunter1" floating, no body). So both sides re-broadcast now (and ask the
            // other to), and the rewind waits for the records.
            RebroadcastOwnRecord("meet");
            Send(Role() == RoleHost ? -1 : 0, "{\"op\":\"record\"}");
            g_meet_rewind_at = now + 1.5;
            Log("split: a member is in our area (%s) without its chr here: records exchanged, re-creation in 1.5 s "
                "(no load)", MapName(s.map_id).c_str());
        }
    }
    if (g_meet_rewind_at > 0 && now >= g_meet_rewind_at) {
        g_meet_rewind_at = 0;
        Log("split: meet: enterleave before the re-creation:%s", DescribeEnterLeave().c_str());
        const int n = RewindRemoteTasks("split meet");
        Log("split: meet: %d task(s) rewound to STEP_Create", n);
    }
    // Self-heal: a remote chr standing > 4 m from every member's reported position for 3 s,
    // while all of them report positions in our area, is a stale one: removed, the meet builds it
    // again (with fresh records).
    static std::map<u64, double> far_since;
    static const bool heal = std::getenv("BB_PARTY_SPLIT_SELFHEAL") && std::getenv("BB_PARTY_SPLIT_SELFHEAL")[0] == '1';
    for (const TaskInfo& t : heal ? Tasks(false) : std::vector<TaskInfo>{}) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        float p[3];
        if (!c || !ChrPos(c, p)) {
            continue;
        }
        bool all_known = true;
        for (const Where& w : g_where) {
            if (w.slot != g_local_slot.load() && w.connected && (!w.has_pos || w.age_ms > 700 || AreaOf(w.map) != mine)) {
                all_known = false;
            }
        }
        if (!all_known || SlotNear(p, 4.0f) >= 0) {
            far_since.erase(c);
            continue;
        }
        auto it = far_since.emplace(c, now).first;
        if (now - it->second >= 3.0) {
            far_since.erase(c);
            RemoveRemoteChr(c, "no member reports a position within 4 m of it for 3 s (stale)");
            last_meet = 0;  // the meet re-creates it at once
        }
    }
}

/// Guest: the host is apart (another area, or it travelled alone in the last 60 s) and its party
/// link entry is connected and fresh.
bool SplitHostReachable() {
    const Where* h = WhereOf(0);
    const bool apart = g_apart.load() > 0 || Now() - g_host_alone_seen.load() < 60.0;
    return apart && h && h->connected && h->age_ms < 5000;
}

// ---- The host's world blobs of packet 0x11 (GSM +0x16C0/+0x16C8 decals, +0x16A0/+0x16A8 map
// items, +0x1684/+0x1688 CHR), kept for an alone load. A host-world load into a map the host has
// not loaded (no snapshot) built no world (live split5/split7: map "-", the player falling through
// nothing - the user's "grey screen"). The map items blob is what the host-world loader reads
// (getter 0x156C600: +0x16B0 own world / +0x16A0 host world); copies of the last ones the host sent
// are put back (game heap, *0x5540420 vfunc +0x58) when they are empty before an alone load.
struct Blob {
    u64 size_off, ptr_off;
    std::vector<u8> data;
};
Blob g_blobs[3] = {{0x16c0, 0x16c8, {}}, {0x16a0, 0x16a8, {}}, {0x1684, 0x1688, {}}};

std::string DescribeBlobs() {
    const u64 gsm = Slot(kSlotGsm);
    char b[160];
    std::snprintf(b, sizeof b, "decals %d, map items %d, chr %d (own: %d / %d / %d)", Field<int>(gsm, 0x16c0),
                  Field<int>(gsm, 0x16a0), Field<int>(gsm, 0x1684), Field<int>(gsm, 0x16d0), Field<int>(gsm, 0x16b0),
                  Field<int>(gsm, 0x1690));
    return b;
}

void SaveHostBlobs() {
    const u64 gsm = Slot(kSlotGsm);
    for (Blob& bl : g_blobs) {
        const int n = Field<int>(gsm, bl.size_off);
        const u64 p = Field<u64>(gsm, bl.ptr_off);
        if (n > 0 && n < (1 << 22) && p) {
            bl.data.resize(std::size_t(n));
            if (!SafeRead(p, bl.data.data(), bl.data.size())) {
                bl.data.clear();
            }
        }
    }
    Log("guest: host world blobs kept for alone loads (%s)", DescribeBlobs().c_str());
}

void RestoreHostBlobs() {
    const u64 gsm = Slot(kSlotGsm);
    const u64 heap = Slot(0x5540420);
    if (!gsm || !heap) {
        return;
    }
    using Alloc = u64(BB_COOP_SYSV*)(u64, u64, u64);
    const u64 alloc = Field<u64>(Field<u64>(heap, 0), 0x58);
    std::string put;
    for (Blob& bl : g_blobs) {
        if (bl.data.empty() || Field<int>(gsm, bl.size_off) > 0 || Field<u64>(gsm, bl.ptr_off) || !alloc) {
            continue;
        }
        const u64 p = reinterpret_cast<Alloc>(alloc)(heap, bl.data.size(), 1);
        if (!p || !SafeWrite(p, bl.data.data(), bl.data.size())) {
            continue;
        }
        const int n = int(bl.data.size());
        SafeWrite(gsm + bl.ptr_off, &p, 8);
        SafeWrite(gsm + bl.size_off, &n, 4);
        char b[32];
        std::snprintf(b, sizeof b, " +0x%llx (%d B)", ull(bl.size_off), n);
        put += b;
    }
    Log("guest: alone load: host world blobs before (%s); put back:%s", DescribeBlobs().c_str(),
        put.empty() ? " none" : put.c_str());
}

// ---- In-world warps: a move inside the loaded map needs no load (no loading screen) ----
void BesideHost(float pos[4], const float rot[4], int slot);
/// The local player's current map (packed), 0xffffffff unknown.
u32 LocalMap() {
    const GameSnapshot g = ReadGameState(false);
    return g.world_up && !g.loading ? g.map_id : 0xffffffffu;
}

/// `map` (packed) is the block the local player stands in: area and block equal.
bool InLoadedBlock(u32 map) {
    const u32 cur = LocalMap();
    return cur != 0xffffffffu && map != 0 && map != 0xffffffffu && (map >> 16) == (cur >> 16);
}

/// BB_PARTY_WARP_REFRESH (default on; 0 = the raw chr warp): in-world moves of the local player use
/// the game's own in-world player warp instead of the bare chr warp vfunc +0x5F8.
bool WarpRefreshOn() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_WARP_REFRESH");
        return !(v && v[0] == '0');
    }();
    return on;
}

/// The game's own warp of the local player inside the loaded world: 0x154B110(FieldArea
/// *0x553B148, u32* block, float4* pos, float4* rot). The remo end action of a cutscene warp
/// (EMEVD 2002[2/4/6/8], 0x1CEC340 / 0x1CEC3D0) and the stage's spot warps (0x1548740,
/// 0x154CC50, 0x154D5E0, 0x154EA30) move the player with it. Around the same chr warp +0x5F8 it
/// does what the bare call leaves out: the block change (PlayerIns vfunc +0x428 when the block
/// differs from chr +0x3F8), the navmesh / ground state (0x18BBA70), the chr reset (0x18BCF30(chr,
/// 1), +0x1E4 |= 2) and its ChrCtrl refresh (ctrl[3] +0x1334, vfunc +0x40), the warp record the
/// world, the camera and the map's draw groups follow (FieldArea +0x68 -> +0x18: pos, +0x50 = 1;
/// FieldArea +0xC8 = 0x202; [FieldArea +0x28] +0x80 = 1), and the network stage record.
/// Main thread only. False when a pointer is missing or the code differs from 1.09.
constexpr u64 kFieldArea = 0x553b148;
constexpr u64 kWorldWarp = 0x154b110;
bool NativeWarpOk() {
    static const bool ok = MatchesOriginal(kWorldWarp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                        0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28});
    return ok;
}
bool NativeWarpLocalImpl(u64 player, const float* p16, const float* r16, const char* why) {
    const u64 field_area = Slot(kFieldArea);
    if (!player || !field_area || player != Field<u64>(Slot(kSlotWorldChrMan), 0x60) || !NativeWarpOk()) {
        return false;
    }
    // The block the player stands in (chr +0x3F8, what the function compares): an in-world move
    // never changes it (InLoadedBlock), so no block change runs.
    u32 block = Field<u32>(player, 0x3f8, 0xffffffffu);
    if (block == 0xffffffffu) {
        return false;
    }
    alignas(16) float pos[4] = {p16[0], p16[1], p16[2], 1.0f};
    alignas(16) float rot[4] = {r16[0], r16[1], r16[2], r16[3]};
    using Fn = void(BB_COOP_SYSV*)(u64, u32*, float*, float*);
    reinterpret_cast<Fn>(Guest(kWorldWarp))(field_area, &block, pos, rot);
    static int logged = 0;
    if (logged++ < 20) {
        Log("in-world warp (%s): the game's own player warp 0x154b110 (block %#x)", why, block);
    }
    return true;
}

/// The local player's in-world warp: the game's own (NativeWarpLocal) unless BB_PARTY_WARP_REFRESH=0
/// or it is unavailable, else the bare chr warp +0x5F8 (0x18CD600(chr, pos, rot, 1)). Main thread.
bool LocalChrWarpImpl(u64 player, const float* p16, const float* r16, const char* why) {
    if (WarpRefreshOn() && NativeWarpLocalImpl(player, p16, r16, why)) {
        return true;
    }
    const u64 fn = player ? Field<u64>(Field<u64>(player, 0), 0x5f8) : 0;
    if (!fn) {
        return false;
    }
    using Warp = void(BB_COOP_SYSV*)(u64, const float*, const float*, u32);
    reinterpret_cast<Warp>(fn)(player, p16, r16, 1);
    return true;
}

/// The local player to a spot of the loaded block (LocalChrWarp: the game's own in-world warp, or
/// the chr warp vfunc +0x5F8): same map, no load. Main thread.
bool WarpLocal(const float pos[3], float yaw, const char* why) {
    const u64 player = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    if (!player) {
        return false;
    }
    alignas(16) float p16[4] = {pos[0], pos[1], pos[2], 1.0f};
    alignas(16) float r16[4] = {0.0f, yaw, 0.0f, 0.0f};
    float before[3] = {};
    ChrPos(player, before);
    if (!LocalChrWarpImpl(player, p16, r16, why)) {
        return false;
    }
    // The camera stayed where it was and swung through the walls after a long in-world warp
    // (live vis6: the host looked out of the geometry 3 s after a 68 m move): the Lua CamReset
    // 0x132F5A0(ctx, 10000 = the local player, 1) puts it behind the player.
    using CamReset = u64(BB_COOP_SYSV*)(u64, u32, u32);
    reinterpret_cast<CamReset>(Guest(0x132f5a0))(0, 10000, 1);
    Log("in-world warp (%s): (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f) in %s, no load", why, before[0], before[1], before[2],
        pos[0], pos[1], pos[2], MapName(LocalMap()).c_str());
    return true;
}

/// Lamp / return point `id` is in the loaded block and its spot is known.
bool LampInLoadedBlock(u32 id, float spot[4]) {
    return id != 0 && id != kTravelNone && id / 100000 != 29 && ids::ReturnSpot(id, spot) &&
           InLoadedBlock(ids::EntityMap(id));
}

/// To lamp `id`'s spot without a load; beside it by party slot when `beside` (the spacing of
/// BesideHost: guests never stand inside each other).
bool WarpToLampInWorld(u32 id, bool beside, const char* why) {
    float spot[4];
    if (!LampInLoadedBlock(id, spot)) {
        return false;
    }
    float pos[4] = {spot[0], spot[1], spot[2], 1.0f};
    const float rot[4] = {0.0f, spot[3], 0.0f, 0.0f};
    if (beside && g_local_slot.load() > 0) {
        BesideHost(pos, rot, g_local_slot.load());
    }
    char b[96];
    std::snprintf(b, sizeof b, "%s, lamp %u", why, id);
    return WarpLocal(pos, spot[3], b);
}

/// An alone travel to a world lamp makes it this player's own last lamp (GSM +0x1528 {return
/// point entity, anim}: what the Hunter's Mark and a death go to), as the lamp warp 0x13CDF30
/// would have written it (our travel replaces that warp with a transform / an in-world move).
/// The Hunter's Dream is never a last lamp.
void SetOwnLastLamp(const TravelIntent& t) {
    const u32 id = t.lamp_id != kTravelNone && t.lamp_id ? t.lamp_id : 0;
    if (!id || id / 100000 == 21 || t.last_lamp == ~0ull || !t.last_lamp) {
        return;
    }
    const u64 gsm = Slot(kSlotGsm);
    const u64 was = Field<u64>(gsm, 0x1528);
    if (gsm && was != t.last_lamp && SafeWrite(gsm + 0x1528, &t.last_lamp, 8)) {
        Log("own last lamp %#llx -> %#llx (lamp %u, an alone travel)", ull(was), ull(t.last_lamp), id);
    }
}

// ---- Enemies killed while apart (reconciled when players meet again without a load) ----
// Each game simulates its own copies of a map's enemies while the players are apart (a map the
// host is not in, or the host away while a guest stays). Meeting again without a load (SplitTick,
// the in-world follow) each side sends the enemies of the shared block that are dead in its world
// (EVENT b2 {"op":"dead"}); the other side kills those that still live in its own (HP 0: the
// game's own death of the chr). A kill on either side stays a kill (the union); bosses (entity
// AAB08xx) stay with their defeat flags (D1 / D3), never by this.
template <class F>
void ForEachMapChr(F&& fn) {
    // The chr tables WorldChrMan +0x850 + set * 8 ({int n; entries @+8, stride 0x38, [0] = ChrIns};
    // sets up to [+8]+0x18 + 2, as ChrOfHandle resolves handles); a chr's entity id is +0x1F0
    // (live split19: Central Yharnam's enemies 2410700, 2410740, ... in set 5).
    const u64 wcm = Slot(kSlotWorldChrMan);
    const int sets = Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2;
    for (int set = 0; wcm && set < sets && set < 64; ++set) {
        const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
        const int n = Field<int>(tab, 0);
        const u64 entries = Field<u64>(tab, 8);
        if (!tab || !entries || n <= 0 || n > 4096) {
            continue;
        }
        for (int k = 0; k < n; ++k) {
            const u64 chr = Field<u64>(entries + u64(k) * 0x38, 0);
            const u32 id = chr ? Field<u32>(chr, 0x1f0) : 0;
            if (chr && id) {
                fn(id, chr);
            }
        }
    }
}

bool EnemyReconcilable(u32 id, u32 block_map) {
    return id >= 1000000 && ids::EntityMap(id) == (block_map & 0xffff0000u) && id % 10000 / 100 != 8;
}

// Kills seen while members are apart: an enemy of our block that was alive (HP > 0, max HP > 0)
// and is dead now. HP 0 alone means nothing: the game keeps unspawned / far chrs at HP 0 (live
// split20: 119 chrs of m24_01, 7 alive).
std::map<u32, bool> g_enemy_alive;  // entity -> alive at the last scan (main thread)
std::atomic<u32> g_watch_enemy{0};      // test: an enemy to log for a few seconds
std::atomic<double> g_watch_until{0};
std::vector<u32> g_killed_apart;
u32 g_killed_map = 0;

void EnemyWatchTick(const GameSnapshot& s, double now) {
    static double next = 0;
    if (now < next || !s.world_up || s.loading || s.map_id == 0xffffffffu) {
        return;
    }
    next = now + 0.5;
    if (g_killed_map && (g_killed_map >> 16) != (s.map_id >> 16)) {
        g_killed_apart.clear();  // another map: those kills are gone with the load
        g_enemy_alive.clear();
    }
    g_killed_map = s.map_id;
    const bool apart = g_apart.load() > 0;
    std::map<u32, bool> seen;
    ForEachMapChr([&](u32 id, u64 chr) {
        seen[id] = true;
        if (!EnemyReconcilable(id, s.map_id)) {
            return;
        }
        const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
        const int hp = Field<int>(dm, 0x128, 0), mx = Field<int>(dm, 0x12c, 0);
        const bool alive = dm && mx > 0 && hp > 0;
        if (id == g_watch_enemy.load() && now < g_watch_until.load()) {
            Log("split: watched enemy %u hp %d / %d (alive %d, apart %d, seen alive %d)", id, hp, mx, int(alive), int(apart),
                g_enemy_alive.count(id) ? int(g_enemy_alive[id]) : -1);
        }
        auto it = g_enemy_alive.find(id);
        if (it != g_enemy_alive.end() && it->second && !alive && mx > 0 && apart && g_killed_apart.size() < 128) {
            g_killed_apart.push_back(id);
            Log("split: enemy %u killed while the party is apart (sent at the next meet)", id);
        }
        g_enemy_alive[id] = alive;
    });
    // A chr that was alive and is gone from the tables now (live split26: the dead enemy left the
    // tables before the next scan) died too.
    for (auto it = g_enemy_alive.begin(); it != g_enemy_alive.end();) {
        if (!seen.count(it->first)) {
            if (it->second && apart && g_killed_apart.size() < 128) {
                g_killed_apart.push_back(it->first);
                Log("split: enemy %u gone while the party is apart (a kill; sent at the next meet)", it->first);
            }
            it = g_enemy_alive.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<u32> DeadEnemiesHere(u32 map) {
    std::vector<u32> out;
    if (g_killed_map && (g_killed_map >> 16) == (map >> 16)) {
        out = g_killed_apart;
    }
    g_killed_apart.clear();
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// Sends the dead enemies of our block to the others (host: everyone; guest: the host, which
/// relays nothing - every guest sends its own).
/// BB_PARTY_SPLIT_ENEMIES=1: enemies killed while apart are sent / applied at a meet (not yet
/// verified live; off by default for 0.5.2).
bool SplitEnemiesOn() {
    static const bool on = std::getenv("BB_PARTY_SPLIT_ENEMIES") && std::getenv("BB_PARTY_SPLIT_ENEMIES")[0] == '1';
    return on;
}

void SendDeadEnemies(const GameSnapshot& s, const char* why) {
    if (!SplitEnemiesOn()) {
        return;
    }
    const std::vector<u32> dead = DeadEnemiesHere(s.map_id);
    std::string body = "{\"op\":\"dead\",\"map\":" + std::to_string(s.map_id) + ",\"ids\":[";
    for (std::size_t i = 0; i < dead.size(); ++i) {
        body += (i ? "," : "") + std::to_string(dead[i]);
    }
    body += "]}";
    Send(Role() == RoleHost ? -1 : 0, body);
    Log("split: %zu dead enemies of %s sent (%s)", dead.size(), MapName(s.map_id).c_str(), why);
}

std::mutex g_enemy_dead_mu;
std::vector<std::pair<u32, std::vector<u32>>> g_dead_in;  // (map, ids) received

void ApplyDeadEnemies(const GameSnapshot& s) {
    std::vector<std::pair<u32, std::vector<u32>>> in;
    {
        std::lock_guard<std::mutex> lk(g_enemy_dead_mu);
        in.swap(g_dead_in);
    }
    for (const auto& [map, list] : in) {
        if ((map >> 16) != (s.map_id >> 16)) {
            Log("split: dead enemies of %s ignored (we are in %s)", MapName(map).c_str(), MapName(s.map_id).c_str());
            continue;
        }
        int killed = 0, already = 0;
        ForEachMapChr([&](u32 id, u64 chr) {
            if (!EnemyReconcilable(id, s.map_id) || !std::binary_search(list.begin(), list.end(), id)) {
                return;
            }
            const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
            const int hp = Field<int>(dm, 0x128, 0);
            if (dm && hp > 0) {
                const int zero = 0;
                SafeWrite(dm + 0x128, &zero, 4);
                ++killed;
            } else {
                ++already;
            }
        });
        Log("split: enemies killed by the other side while apart: %d here now dead too (%d already dead, %zu sent)",
            killed, already, list.size());
    }
}

/// Test aid: the nearest living enemy of our block within `max_m` (entity id, 0 none).
u32 NearestEnemy(const GameSnapshot& s, float max_m) {
    float me[3] = {};
    if (!ChrPos(s.player, me)) {
        return 0;
    }
    u32 best = 0;
    float bd = max_m * max_m;
    ForEachMapChr([&](u32 id, u64 chr) {
        const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
        float p[3];
        if (!EnemyReconcilable(id, s.map_id) || Field<int>(dm, 0x128, 0) <= 0 || !ChrPos(chr, p)) {
            return;
        }
        const float d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d < bd) {
            bd = d;
            best = id;
        }
    });
    return best;
}

/// Every remote player chr in its task's Update step is drawn: the leave flag the task's
/// STEP_Delete sets (chr +0x3B0 -> +8, byte +0x1BC bit 1) is cleared if a re-created chr carries
/// it (user reports split24 / hd22 / hd26: "I see the health bar but no model" on the host after
/// a re-creation - the bar is the chr's, the model is not drawn). Logs every change. Both roles.
void RemoteVisibleTick(double now) {
    static double next = 0;
    if (now < next) {
        return;
    }
    next = now + 0.25;
    for (const TaskInfo& t : Tasks(false)) {
        if ((t.mode != 1 && t.mode != 2) || t.step != kStepUpdate || (t.flags & 2)) {
            continue;
        }
        const u64 c = PlayerForPeer(t.task + kTaskPeer);
        const u64 cd = c ? Field<u64>(Field<u64>(c, 0x3b0), 8) : 0;
        if (!cd) {
            continue;
        }
        const u8 v = Field<u8>(cd, 0x1bc);
        (void)v;  // live vis1: the game sets +0x1BC bit 1 on visible remote chrs too: not the draw switch
        // chr +0x1E0 bit 20: set on every drawn player chr (local and remote, both games), clear on
        // the invisible re-created one ("the health bar but no model", live vis3 render probe:
        // 0x00009800 vs 0x00109800). Put back.
        // A chr of a task we rewound: shown as at a load's end (the root cause, above).
        if (auto it = g_rewound.find(t.task); it != g_rewound.end()) {
            g_rewound.erase(it);
            ShowLikeInGameStart(c, "re-created by a rewind");
            continue;
        }
        // Safety net: any other remote chr without the draw bit 4 s after we first saw it so.
        static std::map<u64, double> hidden_since;
        const u32 f = Field<u32>(c, 0x1e0);
        if (f && !(f & 0x100000u)) {
            auto h = hidden_since.emplace(c, now).first;
            if (now - h->second >= 4.0) {
                hidden_since.erase(h);
                Log("remote chr %#llx (mode %d): no draw bit for 4 s (safety net); enterleave%s", ull(c), t.mode,
                    DescribeEnterLeave().c_str());
                ShowLikeInGameStart(c, "safety net");
            }
        } else {
            hidden_since.erase(c);
        }
    }
    for (auto it = g_rewound.begin(); it != g_rewound.end();) {
        it = now - it->second > 30.0 ? g_rewound.erase(it) : std::next(it);
    }
}

std::string DescribeRender(u64 chr) {
    const u64 cd = Field<u64>(Field<u64>(chr, 0x3b0), 8);
    char b[160];
    std::snprintf(b, sizeof b, " render[+0x1bc %#x cd %#llx +0x1b8 %08x +0x1c0 %08x chr+0x1e0 %08x chr+0x524 %08x]",
                  unsigned(Field<u8>(cd, 0x1bc)), ull(cd), Field<u32>(cd, 0x1b8), Field<u32>(cd, 0x1c0), Field<u32>(chr, 0x1e0),
                  Field<u32>(chr, 0x524));
    return b;
}

std::string DescribeWhere() {
    std::string out;
    for (const Where& w : g_where) {
        char b[96];
        std::snprintf(b, sizeof b, " [slot %d %s %s (%.0f, %.0f, %.0f) %u ms]", w.slot, w.connected ? "" : "lost",
                      MapName(w.map).c_str(), w.pos[0], w.pos[1], w.pos[2], w.age_ms);
        out += b;
    }
    return out;
}

/// The summoned placement (GSM +0x14C4 stage, +0x14D0 position, +0x14E0 rotation). Written for
/// every host-world load, also with GSM +0x1521 clear: a host-world load takes its STAGE from
/// +0x14C4 whatever the forced transform says (split travel 4 / split10: a stale +0x14C4 loaded the
/// Dream at Central Yharnam's coordinates - the guest fell through a grey world; it came back in
/// split31 when this write was made conditional). The bytes overlap the guest's return spot
/// (+0x14A0..); that was found not to be the cause of the Continue drift (hd32, b2_seamless.md 6.13).
void WriteSummonedPlacement(u64 gsm, const void* stage, const void* pos, const void* rot) {
    if (!gsm) {
        return;
    }
    SafeWrite(gsm + 0x14c4, stage, 4);
    SafeWrite(gsm + 0x14d0, pos, 16);
    SafeWrite(gsm + 0x14e0, rot, 16);
}

/// A guest's spot beside the host (never on it: character collision shoves the host): BB_PARTY_SUMMON_OFFSET
/// (default 0.5 m, 0 = off), side by side in a line: right (party slot 1), left (2), 2x right (3+) of
/// the host's facing, same height and facing. Wider offsets (2.2 m) put guests into walls at narrow
/// lamp spots (Central Yharnam).
/// rot[1] is the yaw (radians); forward = (sin, 0, cos), right = (cos, 0, -sin).
float SummonOffset() {
    static const float d = [] {
        const char* v = std::getenv("BB_PARTY_SUMMON_OFFSET");
        return v && v[0] ? float(std::atof(v)) : 0.5f;
    }();
    return d;
}

// `spot` 1..3 beside the host (coop::fourp::SpotOffset: a line along the host's right axis, right,
// left, 2x right; a party slot is its own spot), neighbours SummonOffset() apart (A7, host + 3).
void BesideHost(float pos[4], const float rot[4], int spot) {
    const float yaw = rot[1];
    const float fx = std::sin(yaw), fz = std::cos(yaw), rx = std::cos(yaw), rz = -std::sin(yaw);
    float right = 0, forward = 0;
    coop::fourp::SpotOffset(spot, SummonOffset(), &right, &forward);
    // A free spot (coop::fourp::PickFreeSpot): not within 1.0 m of another player's chr in this world
    // or reported position (VITALS, fresh), so a later joiner never lands on a guest who moved onto
    // its spot (title join: the in-world re-placement on arrival).
    if (party::PartyLink* link = party::runtime_link(); link && SummonOffset() > 0.0f) {
        party::MemberVitals v[party::kMaxVitals];
        const int n = link->vitals(v, party::kMaxVitals);
        float others[2 * party::kMaxVitals + 8][2];
        int k = 0;
        // The other players' chrs in this world (known as soon as it is up; VITALS can lag).
        const u64 wcm = Slot(kSlotWorldChrMan);
        const u64 me = Field<u64>(wcm, 0x60);
        const int pn = Field<int>(wcm, 0x30, 0);
        const u64 entries = Field<u64>(wcm, 0x38);
        for (int i = 0; entries && pn > 0 && pn <= 8 && i < pn; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            float cp[3] = {};
            if (!chr || chr == me || !ChrPos(chr, cp)) continue;
            const float hx = cp[0] - pos[0], hz = cp[2] - pos[2];
            if (hx * hx + hz * hz < 0.25f) continue;  // the host itself (the line's centre)
            others[k][0] = cp[0];
            others[k][1] = cp[2];
            ++k;
        }
        for (int i = 0; i < n; ++i) {
            if (v[i].slot == link->local_slot() || v[i].slot == 0 || !(v[i].flags & party::kVitalsHasPos) ||
                v[i].age_ms > 3000 || k >= 2 * party::kMaxVitals + 8) {
                continue;  // ourselves; the host stands at the line's centre anyway
            }
            others[k][0] = v[i].pos[0];
            others[k][1] = v[i].pos[2];
            ++k;
        }
        const int pick = coop::fourp::PickFreeSpot(spot, SummonOffset(), pos[0], pos[2], yaw, others, k, 1.0f);
        const float r = coop::fourp::FreeSpotRight(pick, spot, SummonOffset());
        if (pick >= 0) {
            Log("beside the host: slot %d's spot is taken; free spot %.1f m along the line", spot, r);
        }
        right = r;
        forward = 0;
    }
    pos[0] += rx * right + fx * forward;
    pos[2] += rz * right + fz * forward;
}

/// Guest, after any host-world arrival (a bell join or a B2 travel: flow 3 -> 4): once the host's chr
/// stands in this world, a guest closer than the offset - 0.5 m is moved beside it (by party slot,
/// the host's facing and height) with the chr warp vfunc +0x5F8 (0x18CD600(chr, pos, rot, 1), as
/// the NPC summon step 0x1E4B559 calls it). The game's own placement (the sign spot, usually the
/// host's lamp spot) is otherwise on top of the host. No ground probe yet: the host's height.
// ---- Host-world flag overlay guard (B2 re-entry) ----
// SprjEventFlagMan (*0x553B100) +0x88/+0x90: std::map<u32 group key, u8* 125-byte buffer>, the
// host-world overlay of load mode 1 (nodes: left +0, parent +8, right +0x10, is-nil +0x19,
// key +0x20, buffer +0x28). Run 18 showed the overlay all zero after the B2 re-sent snapshot was
// applied (9401 / 12410000 = 0 where the join's snapshot had 1), so the client replayed the
// opening cutscene. The guest keeps a copy of the overlay it had in the host's world and, after
// the new snapshot, restores every group that came back all zero.
std::map<u32, std::vector<u8>> g_overlay_saved;

template <class F>
void ForEachOverlayGroup(F&& fn) {
    const u64 man = Slot(0x553b100);
    const u64 head = Field<u64>(man, 0x90);
    if (!head) {
        return;
    }
    std::vector<u64> stack;
    const u64 root = Field<u64>(head, 8);
    if (root && root != head) {
        stack.push_back(root);
    }
    int guard = 0;
    while (!stack.empty() && guard++ < 4096) {
        const u64 n = stack.back();
        stack.pop_back();
        if (!n || n == head || Field<u8>(n, 0x19)) {
            continue;
        }
        fn(Field<u32>(n, 0x20), Field<u64>(n, 0x28));
        stack.push_back(Field<u64>(n, 0));
        stack.push_back(Field<u64>(n, 0x10));
    }
}

void SaveOverlay() {
    g_overlay_saved.clear();
    int nonzero = 0;
    ForEachOverlayGroup([&](u32 key, u64 buf) {
        std::vector<u8> v(125);
        if (buf && SafeRead(buf, v.data(), v.size())) {
            bool any = false;
            for (u8 b : v) {
                any = any || b;
            }
            nonzero += any ? 1 : 0;
            g_overlay_saved[key] = std::move(v);
        }
    });
    Log("overlay guard: %zu host-world groups saved (%d non-zero)", g_overlay_saved.size(), nonzero);
}

void RestoreZeroedOverlay() {
    int zero = 0, restored = 0, total = 0;
    ForEachOverlayGroup([&](u32 key, u64 buf) {
        ++total;
        u8 v[125];
        if (!buf || !SafeRead(buf, v, sizeof v)) {
            return;
        }
        bool any = false;
        for (u8 b : v) {
            any = any || b;
        }
        if (any) {
            return;
        }
        ++zero;
        auto it = g_overlay_saved.find(key);
        if (it == g_overlay_saved.end()) {
            return;
        }
        bool saved_any = false;
        for (u8 b : it->second) {
            saved_any = saved_any || b;
        }
        if (saved_any && SafeWrite(buf, it->second.data(), it->second.size())) {
            ++restored;
        }
    });
    Log("overlay guard: %d groups, %d all-zero after the new snapshot, %d restored from the saved overlay", total,
        zero, restored);
}

/// The flags the opening cutscene depends on, as the store answers now (overlay in load mode 1).
/// BB_PARTY_B2_TEST_SPEFFECT=<id>[,<id>...]: SpEffects the guest applies to itself at the host's
/// travel event (as a buff item would), to see what a B2 load does to them.
std::vector<int> TestSpEffects() {
    std::vector<int> ids;
    if (const char* v = std::getenv("BB_PARTY_B2_TEST_SPEFFECT"); v && v[0]) {
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const long id = std::strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            if (id > 0) {
                ids.push_back(int(id));
            }
            p = *end ? end + 1 : end;
        }
    }
    return ids;
}

/// The active SpEffects of a chr (ChrIns +0x1C8 -> +8 first node; node: param id +0x40, next
/// +0x58; party_director HasSpEffect; +0x00 s left, +0x04 duration (f32), runtime-checked with a
/// freshly applied 60 s buff: 60/60).
std::string DescribeSpEffects(u64 chr) {
    std::string out;
    u64 node = Field<u64>(Field<u64>(chr, 0x1c8), 8);
    int n = 0;
    for (int guard = 0; node && guard < 256; ++guard, ++n) {
        const int id = Field<int>(node, 0x40, -1);
        char b[48];
        const float left = Field<float>(node, 0), total = Field<float>(node, 4);
        if (total > 0.f) {
            std::snprintf(b, sizeof b, " %d(%.1f/%.0f s)", id, double(left), double(total));
        } else {
            std::snprintf(b, sizeof b, " %d", id);
        }
        out += b;
        node = Field<u64>(node, 0x58);
    }
    return std::to_string(n) + ":" + out;
}

/// Timed SpEffects (a duration, time left) of a chr: buffs a load drops. The multiplayer / bell
/// system effects (9000-9199) are left to the game.
std::vector<std::pair<int, float>> TimedSpEffects(u64 chr) {
    std::vector<std::pair<int, float>> out;
    u64 node = Field<u64>(Field<u64>(chr, 0x1c8), 8);
    for (int guard = 0; node && guard < 256; ++guard) {
        const int id = Field<int>(node, 0x40, -1);
        const float left = Field<float>(node, 0), total = Field<float>(node, 4);
        if (id > 0 && !(id >= 9000 && id < 9200) && total > 0.f && left > 0.f && left <= total) {
            out.emplace_back(id, left);
        }
        node = Field<u64>(node, 0x58);
    }
    return out;
}

/// Guest after a B2 load: the timed buffs it had at the host's travel event and lost in the load,
/// applied again (chr vfunc +0x3F0) with the time they had left then (the guest's load is a pause
/// for it, as for its HP). BB_PARTY_B2_KEEP_BUFFS=0: off.
void RestoreBuffs(u64 player, const std::vector<std::pair<int, float>>& before) {
    static const bool on = !(std::getenv("BB_PARTY_B2_KEEP_BUFFS") && std::getenv("BB_PARTY_B2_KEEP_BUFFS")[0] == '0');
    if (!on || before.empty() || !player) {
        return;
    }
    u64 vt = 0, fn = 0;
    if (!SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) {
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, float, float, float, float, float);
    std::string done;
    for (const auto& [id, left] : before) {
        bool have = false;
        for (u64 node = Field<u64>(Field<u64>(player, 0x1c8), 8); node && !have; node = Field<u64>(node, 0x58)) {
            have = Field<int>(node, 0x40, -1) == id;
        }
        if (have) {
            continue;
        }
        reinterpret_cast<Fn>(fn)(player, u64(id), player, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        for (u64 node = Field<u64>(Field<u64>(player, 0x1c8), 8); node; node = Field<u64>(node, 0x58)) {
            if (Field<int>(node, 0x40, -1) == id) {
                if (left < Field<float>(node, 4)) {
                    SafeWrite(node, &left, 4);
                }
                char b[32];
                std::snprintf(b, sizeof b, " %d(%.1f s)", id, double(left));
                done += b;
                break;
            }
        }
    }
    if (!done.empty()) {
        Log("guest: buffs lost in the load restored:%s", done.c_str());
    }
}

/// Guest: the test SpEffects on itself (chr vfunc +0x3F0, the call shape of party_director's bell).
void ApplyTestSpEffects(u64 player) {
    u64 vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) {
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, float, float, float, float, float);
    for (int id : TestSpEffects()) {
        reinterpret_cast<Fn>(fn)(player, u64(id), player, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        Log("test speffect: %d applied to the guest", id);
    }
}

/// The guest's consumables (goods counts), HP / stamina and SpEffects, for the "state kept" check.
void LogGuestState(const GameSnapshot& s, const char* when) {
    const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
    std::string g;
    for (u32 id : {900u, 1000u, 1010u, 300u, 310u, 200u, 205u}) {
        char b[32];
        std::snprintf(b, sizeof b, " %u:%d", id, GoodsCount(id));
        g += b;
    }
    Log("guest state %s: HP %d/%d stamina %d/%d echoes %d insight %d; goods%s; speffects %s", when, Field<int>(dm, 0xf8, -1),
        Field<int>(dm, 0xfc, -1), Field<int>(dm, 0x134, -1), Field<int>(dm, 0x138, -1), s.echoes, s.insight, g.c_str(),
        DescribeSpEffects(s.player).c_str());
}

void LogIntroFlags(const char* when) {
    const u64 man = Slot(0x553b100);
    if (!man) {
        return;
    }
    using Get = u32(BB_COOP_SYSV*)(u64, u32, u32);
    const auto f = [&](u32 id) { return reinterpret_cast<Get>(Guest(0x13cfd80))(man, id, 1); };
    Log("flags %s (load mode %d, GSM+0x1592 %u): 12410000=%u 9401=%u 9402=%u 12417810=%u 9180=%u 12410105=%u "
        "12411800=%u 2410=%u",
        when, Field<int>(man, 0x80, -1), unsigned(Field<u8>(Slot(kSlotGsm), 0x1592)), f(12410000), f(9401), f(9402),
        f(12417810), f(9180), f(12410105), f(12411800), f(2410));
}

/// A remote member's net chr type (what the game's cooperator count 0x15BDC20 and the EMEVD
/// co-op client count ask: chr vfunc +0x558 = SessionTypeDesc[GameData +0x5D8] +0x15, counted
/// when in {1,5,7,19}) is remembered per member task while it counts; if a B2 re-creation leaves
/// the member's GameData with another session type, the remembered one is put back. A guest's own
/// player is checked the same way (key 0): its B2 host-world load sets its own +0x5D8 to 0 (live
/// r6: st 7 -> 0, so its record re-broadcast at InGameStart carried 0 to the host as well).
void MemberSessionTypeTick(const GameSnapshot& s) {
    static std::map<u64, int> good;  // task (0: the guest itself) -> session type while counted
    if (!SessionUp()) {
        good.clear();
        return;
    }
    if (!s.world_up || s.loading) {
        return;
    }
    using Get = u64(BB_COOP_SYSV*)(u64);
    std::vector<std::pair<u64, u64>> members;  // (key, chr)
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode == 1 || t.mode == 2) {
            members.emplace_back(t.task, PlayerForPeer(t.task + kTaskPeer));
        }
    }
    if (Role() == RoleClient) {
        members.emplace_back(0, s.player);
    }
    for (const auto& [key, chr] : members) {
        const u64 f1c8 = chr ? Field<u64>(Field<u64>(chr, 0), 0x1c8) : 0;
        const u64 gd = f1c8 ? reinterpret_cast<Get>(f1c8)(chr) : 0;
        if (!gd) {
            continue;
        }
        const int st = Field<int>(gd, 0x5d8, -1);
        const auto counted = [](int type) {
            if (type < 0 || type >= 64) {
                return false;
            }
            const u8 net = Field<u8>(Guest(kSessionTypeDesc + u64(type) * 0x80), 0x15, 0xff);
            return net < 20 && ((0x800a2u >> net) & 1);
        };
        if (counted(st)) {
            good[key] = st;
        } else if (const auto it = good.find(key); it != good.end() && it->second != st) {
            SafeWrite(gd + 0x5d8, &it->second, 4);
            Log("%s: session type %d after the load / re-creation (not a cooperator) -> %d as before",
                key ? "member task" : "guest (own player)", st, it->second);
        }
    }
}

/// After any host-world arrival (flow 3 -> 4): the local and the remote players' positions every
/// 0.5 s for 3 s, on both instances (placement stability; nothing is written after a load).
void ArrivalWatchTick(const GameSnapshot& s, double now) {
    static int last_flow = -1;
    static double until = 0, next = 0;
    const int fs = Field<int>(Slot(kSlotFlow), 0x84, -1);
    if (last_flow == 3 && fs == 4) {
        next = -1; // armed: starts when the world is up
    }
    last_flow = fs;
    if (next == 0 || !s.world_up || s.loading || !s.player) {
        return;
    }
    if (next < 0) {
        next = now;
        until = now + 3.0;
        LogIntroFlags("after the host-world load");
    }
    if (now < next) {
        return;
    }
    next = now + 0.5;
    const auto pos = [](u64 chr, float out[4]) {
        const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
        return a && SafeRead(a + 0x1e0, out, 16);
    };
    float me[4] = {};
    pos(s.player, me);
    std::string others;
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode != 1 && t.mode != 2) {
            continue;
        }
        float o[4] = {};
        char b[128];
        const u64 c = PlayerForPeer(t.task + kTaskPeer);
        if (c && pos(c, o)) {
            std::snprintf(b, sizeof b, " [mode %d (%.2f, %.2f, %.2f) %.2f m]", t.mode, o[0], o[1], o[2],
                          std::sqrt((o[0] - me[0]) * (o[0] - me[0]) + (o[2] - me[2]) * (o[2] - me[2])));
        } else {
            std::snprintf(b, sizeof b, " [mode %d no chr]", t.mode);
        }
        others += b;
    }
    Log("arrival watch: me (%.2f, %.2f, %.2f)%s", me[0], me[1], me[2], others.c_str());
    if (now >= until) {
        next = 0;
    }
}

/// Host: the same 3 s watch when a member's chr appears (a join or a B2 re-creation).
void HostMemberWatchTick(const GameSnapshot& s, double now) {
    static int had = 0;
    static double until = 0, next = 0;
    int have = 0;
    for (const TaskInfo& t : Tasks(false)) {
        have += (t.mode == 1 && PlayerForPeer(t.task + kTaskPeer)) ? 1 : 0;
    }
    if (Role() == RoleHost && have > had) {
        next = now;
        until = now + 3.0;
    }
    had = have;
    if (next == 0 || now < next || !s.player) {
        return;
    }
    next = now + 0.5;
    const auto pos = [](u64 chr, float out[4]) {
        const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
        return a && SafeRead(a + 0x1e0, out, 16);
    };
    float me[4] = {};
    pos(s.player, me);
    std::string others;
    for (const TaskInfo& t : Tasks(false)) {
        float o[4] = {};
        const u64 c = t.mode == 1 ? PlayerForPeer(t.task + kTaskPeer) : 0;
        if (c && pos(c, o)) {
            char b[128];
            std::snprintf(b, sizeof b, " [guest (%.2f, %.2f, %.2f) %.2f m]", o[0], o[1], o[2],
                          std::sqrt((o[0] - me[0]) * (o[0] - me[0]) + (o[2] - me[2]) * (o[2] - me[2])));
            others += b;
        }
    }
    Log("host watch: me (%.2f, %.2f, %.2f)%s", me[0], me[1], me[2], others.c_str());
    if (now >= until) {
        next = 0;
    }
}

/// Before a guest rings (its own world, before any load): when it stands in the host's map it steps
/// BB_PARTY_SUMMON_OFFSET aside in its own world - right / left / behind by slot, each try checked
/// 30 ticks later (reached, same height +-1.5 m: walkable), else the next side, else back to the
/// start. The sign is placed where it stands, so the join placement is beside the host once,
/// before the load; nothing moves it afterwards. True while it is still moving (no ring yet).
bool PreRingSpacing(const GameSnapshot& s, u32 host_map) {
    static int attempt = -1, wait = 0;
    static std::chrono::steady_clock::time_point moved_at{};
    static u64 done_for = 0;  // the PlayerIns it was done for (a new one after every load)
    alignas(16) static float start[4], target[4], start_rot[4];
    const float d = SummonOffset();
    if (d <= 0.0f || !s.player || !s.world_up || s.loading || done_for == s.player ||
        (host_map >> 16) != (s.map_id >> 16)) {
        return false;
    }
    const u64 pm = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
    alignas(16) float me[4], rot[4];
    if (!pm || !SafeRead(pm + 0x1e0, me, 16) || !SafeRead(pm + 0x1d0, rot, 16)) {
        return false;
    }
    using Warp = void(BB_COOP_SYSV*)(u64, const float*, const float*, u32);
    if (attempt >= 0) {
        // 0.8 s after the warp (a fall off a ledge has lost > 1.5 m by then): 30 ticks were up to
        // 3.4 s per side at a slow frame rate (A4: the guest's first bell came 6.9 s late).
        (void)wait;
        if (std::chrono::steady_clock::now() - moved_at < std::chrono::milliseconds(800)) {
            return true;
        }
        const float off =
            std::sqrt((me[0] - target[0]) * (me[0] - target[0]) + (me[2] - target[2]) * (me[2] - target[2]));
        const bool ok = off < 0.8f && std::fabs(me[1] - start[1]) < 1.5f;
        Log("pre-ring spacing: side try %d %s: at (%.2f, %.2f, %.2f), %.2f m from the target", attempt,
            ok ? "OK" : "blocked", me[0], me[1], me[2], off);
        if (ok || attempt >= 2) {
            if (!ok) {
                reinterpret_cast<Warp>(Field<u64>(Field<u64>(s.player, 0), 0x5f8))(s.player, start, start_rot, 1);
                Log("pre-ring spacing: no side free; back to the start spot");
            }
            done_for = s.player;
            attempt = -1;
            return false;
        }
    } else {
        std::memcpy(start, me, sizeof me);
        std::memcpy(start_rot, rot, sizeof rot);
    }
    ++attempt;
    wait = 0;
    moved_at = std::chrono::steady_clock::now();
    const int side = coop::fourp::SpotForTry(g_local_slot.load(), attempt);
    std::memcpy(target, start, sizeof start);
    BesideHost(target, start_rot, side);
    reinterpret_cast<Warp>(Field<u64>(Field<u64>(s.player, 0), 0x5f8))(s.player, target, start_rot, 1);
    Log("pre-ring spacing: own world, from (%.2f, %.2f, %.2f) to side %d (%.2f, %.2f, %.2f), %.1f m", start[0],
        start[1], start[2], side, target[0], target[1], target[2], d);
    return true;
}

/// Safety net: a client in the host's world (GSM+0x1592 == 0) never plays the opening cutscene
/// (event 12410000's EMEVD 2002 remo): its flags come from the host's overlay, which may lack them.
bool ClientIntroSkip(int event_id, int bank, int id) {
    // D10: on by default (BB_PARTY_STORY_DEDUPE=0 off): the guest's own copy of a mirrored event's
    // 2002 would play the scene a second time (soak run 5f).
    static const bool dedupe = StoryDedupeEnabled(std::getenv("BB_PARTY_STORY_DEDUPE"));
    if (event_id > 0 && bank == 2002 &&
        StoryDedupeSkip(dedupe, bank, id, event_id, Role() == RoleClient,
                        StoryMirroredEvent(static_cast<std::uint32_t>(event_id)))) {
        // The live mirror (party_story) already played this event's scene with the host.
        Log("client: event %d's 2002[%d] skipped: its cutscene already played as the live mirror", event_id, id);
        return true;
    }
    if (bank != 2002 || event_id != 12410000 || !g_on.load() || Role() != RoleClient ||
        Field<u8>(Slot(kSlotGsm), 0x1592) != 0) {
        return false;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 3) {
        Log("client: opening cutscene (event 12410000, 2002[%d]) skipped in the host's world", id);
    }
    return true;
}

/// Recovery net (any role, own world only): a player that keeps falling for 6 s (an endless fall
/// out of the map after a bad saved position - the game's own death plane did not catch it) is
/// warped to its own last lamp (0x13CDF30 with the low dword of WorldTransitionState +0x1528).
/// A summoned guest (in the host's world) that keeps falling for 6 s (live, 3 players: a guest fell
/// through the host's m24_01 from y -28 to -1500 after the second guest arrived and was never
/// recovered - own-world recovery only) is put back where it last stood still, in place (the chr
/// warp +0x5F8, no load).
void HostWorldFallTick(const GameSnapshot& s, double now) {
    static float stable[4] = {0, 0, 0, 0}, last_y = 0;
    static double last_t = 0, falling_since = 0, still_since = 0;
    static u64 last_player = 0;
    static bool have_stable = false;
    if (!s.world_up || s.loading || !s.player || Field<u8>(Slot(kSlotGsm), 0x1592) != 0) {
        falling_since = still_since = 0;
        have_stable = false;
        last_player = 0;
        return;
    }
    if (now - last_t < 0.5) {
        return;
    }
    const u64 pm = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
    alignas(16) float p[4];
    if (!pm || !SafeRead(pm + 0x1e0, p, 16)) {
        return;
    }
    const float dy = last_player == s.player ? last_y - p[1] : 0.0f;
    last_y = p[1];
    last_t = now;
    if (last_player != s.player) {
        have_stable = false;
    }
    last_player = s.player;
    if (std::fabs(dy) < 0.05f) {  // standing (or walking on the level): a known good spot
        if (still_since == 0) still_since = now;
        if (now - still_since >= 1.0) {
            std::memcpy(stable, p, sizeof p);
            have_stable = true;
        }
        falling_since = 0;
        return;
    }
    still_since = 0;
    if (dy < 0.7f) {  // 0.5 s: more than 0.7 m down is a fall
        falling_since = 0;
        return;
    }
    if (falling_since == 0) {
        falling_since = now;
        return;
    }
    if (now - falling_since < 6.0 || !have_stable) {
        return;
    }
    falling_since = 0;
    alignas(16) float to[4] = {stable[0], stable[1] + 0.3f, stable[2], 1.0f};
    alignas(16) float rot[4] = {0, 0, 0, 0};
    SafeRead(pm + 0x1d0, rot, 16);
    using Warp = void(BB_COOP_SYSV*)(u64, const float*, const float*, u32);
    reinterpret_cast<Warp>(Field<u64>(Field<u64>(s.player, 0), 0x5f8))(s.player, to, rot, 1);
    Log("recovery: falling for 6 s in the host's world at (%.1f, %.1f, %.1f); back to (%.1f, %.1f, %.1f) where it last stood",
        p[0], p[1], p[2], to[0], to[1], to[2]);
}

void FallRecoveryTick(const GameSnapshot& s, double now) {
    HostWorldFallTick(s, now);
    static float last_y = 0;
    static double last_t = 0, falling_since = 0;
    static u64 last_player = 0;
    if (!s.world_up || s.loading || !s.player || Field<u8>(Slot(kSlotGsm), 0x1592) != 1) {
        falling_since = 0;
        last_player = 0;
        return;
    }
    if (now - last_t < 1.0) {
        return;
    }
    const u64 pm = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
    float p[4];
    if (!pm || !SafeRead(pm + 0x1e0, p, 16)) {
        return;
    }
    const float dy = last_player == s.player ? last_y - p[1] : 0.0f;
    last_y = p[1];
    last_t = now;
    last_player = s.player;
    if (dy > 1.5f) {
        if (falling_since == 0) {
            falling_since = now;
        } else if (now - falling_since >= 6.0) {
            falling_since = 0;
            // A stage request already pending (a death redirect's lamp warp): that one wins.
            if (Field<u8>(Slot(kSlotGsm), 8) != 0) {
                Log("recovery: falling at (%.1f, %.1f, %.1f), a stage request is pending; left to it", p[0], p[1], p[2]);
                return;
            }
            // A party guest goes to the host's last lamp (the gather / death-redirect lamp): its own
            // last lamp is often the Hunter's Dream, and a bell rung there joins the host's world at
            // Dream coordinates (live r5: fell in the arena, recovered to the Dream, re-summoned
            // into the host's m24_01 with stage 0x15000000 placement).
            const u32 own = Field<u32>(Slot(kSlotGsm), 0x1528);
            const u32 host = GuestDeathRedirectLamp();
            const bool use_host = bb_party_enabled() && host != kTravelNone && host != 0;
            const u32 lamp = use_host ? host : own;
            Log("recovery: falling for 6 s at (%.1f, %.1f, %.1f); warping to %s last lamp %u", p[0], p[1], p[2],
                use_host ? "the host's" : "the own", lamp);
            if (lamp && lamp != 0xffffffffu && WarpToLampInWorld(lamp, false, "fall recovery")) {
                return;  // the lamp is in this map: no load
            }
            if (lamp && lamp != 0xffffffffu) {
                RunLampWarpNotRest(lamp, "fall recovery");
            }
        }
    } else {
        falling_since = 0;
    }
}

/// Any failed or timed-out B2 step on a guest: hand the host's travel to B1 (party_travel): the
/// guest warps to the destination in its own world (the funnel leaves the session then, as
/// vanilla) and the director's bells re-summon it. Never leaves the guest stranded.
extern std::atomic<bool> g_dead_follow;
extern std::atomic<bool> g_solo_respawn;
bool BeginRejoin(const TravelIntent& t, bool main_thread, bool solo);

void FallbackToB1(const char* why) {
    State& st = S();
    TravelIntent t;
    bool have;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        have = st.have_intent;
        t = st.intent;
        st.gphase = GuestPhase::Idle;
        st.solo = false;
        st.enter_state1 = false;
    }
    if (st.saved_1521 >= 0) {
        const u8 v = u8(st.saved_1521);
        SafeWrite(Slot(kSlotGsm) + 0x1521, &v, 1);
        st.saved_1521 = -1;
    }
    DropLoadingScreen(why);
    g_rejoin = false;
    const u64 flow = Slot(kSlotFlow);
    const int fs = Field<int>(flow, 0x84, -1);
    if (fs >= 1 && fs <= 3) {
        const int four = 4;
        SafeWrite(flow + 0x84, &four, 4);
    }
    g_fell_back = true;
    if (g_solo_respawn.exchange(false)) {
        // A lone death's respawn that failed (no snapshot): the old death redirect - the host's
        // last lamp in the own world, a leave, the bell (live r20: a plain B1 replay of the
        // synthetic intent was dropped as "not newer" and the guest stayed dead in the host world).
        g_dead_follow = false;
        Log("guest: FALLBACK (%s; flow was %d): the lone death's respawn failed; the usual death redirect runs", why,
            fs);
        GuestDeathRedirectNow();
        return;
    }
    // par_c6 (live): the host crashed during its lamp travel; the guest followed that travel alone
    // into Central Yharnam, while the restarted host Continued in the Dream (its save never got
    // there): a second load back (gather). With the host's link down the travel is not followed;
    // the restarted host's world decides where the guest goes (the gather before its ring).
    party::PartyLink* link = party::runtime_link();
    const bool host_gone = have && link && link->state() != party::LinkState::Connected;
    Log("guest: FALLBACK to B1 (%s; flow was %d)%s", why, fs,
        !have ? " - no travel to replay" : host_gone ? " - the host's link is down: its travel is not followed" : "");
    if (have && !host_gone) {
        RequestGuestTravel(t);
    }
}

/// After a B1 fallback the guest leaves the room through the funnel, but the network flow keeps the
/// old member list (+0x68 with the host's GameData slot): the next join's member event 0xE then
/// finds the host "already known" and creates no task, and the join is left at once (run 26).
/// Once the session is idle in the world, run the flow's own end-of-session cleanup 0x178A900
/// (member list, GameData rows, task returns) once.
void FallbackCleanupTick(const GameSnapshot& s) {
    if (!g_fell_back.load() || Role() != RoleIdle || !s.world_up || s.loading) {
        return;
    }
    g_fell_back = false;
    const u64 flow = Slot(kSlotFlow);
    u64 n = 0;
    const u64 head = Field<u64>(flow, 0x68);
    for (u64 node = Field<u64>(head, 0); head && node && node != head && n < 8; node = Field<u64>(node, 0)) {
        ++n;
    }
    const u8 zero = 0;
    SafeWrite(flow + 0xc5, &zero, 1);  // set by every MoveMapStep ctor; 0x178A900 defers while it is set
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(0x178a900))(flow);
    Log("guest: after the fallback, flow cleanup 0x178A900 ran (%llu stale member(s), flow now %d); tasks:%s", ull(n),
        Field<int>(flow, 0x84, -1), DescribeTasks().c_str());
}

// A guest's own death in the host's world (PartyGhostDeath_2's funnel call) is held: when the
// host dies too (a wipe, a fall) the host's travel event follows within seconds and B2 brings
// the guest along with it - one loading screen, still in the session. Before this hold the
// guest's death went first (redirect to the host's lamp in its own world = a leave), the room
// was gone and the bell had to re-summon it (live r5). With no host travel within the grace
// (BB_PARTY_B2_DEATH_GRACE s, default 8; 0 = no hold) the usual redirect runs.
std::atomic<double> g_death_held_at{0};
std::atomic<double> g_last_hold_at{0};  // the last held death (not cleared when the hold is consumed)
// The guest is dead and follows the host's travel: its load is a death load, which goes to its
// own respawn records (GSM +0x1528 last lamp, +0x1538 mode, +0x153C respawn: often the Hunter's
// Dream) whatever the forced placement says (live r11: loaded m21_00 in host-world mode, 142 m
// from the host). The host's records are written for that load and the own ones put back after.
std::atomic<bool> g_dead_follow{false};
std::atomic<bool> g_solo_respawn{false};  // the dead follow is a lone death's respawn (stay at the lamp)
struct OwnRespawn {
    bool saved = false;
    u64 last_lamp = 0, respawn = 0;
    u32 mode = 0;
} g_own_respawn;

double DeathGrace() {
    static const double g = [] {
        const char* v = std::getenv("BB_PARTY_B2_DEATH_GRACE");
        return v && v[0] ? std::atof(v) : 8.0;
    }();
    return g;
}

// ---- Finding 1 (statsaudit): a guest's bloodstain recoverable in the host's world ----
// The game's stain record (GameDataMan +0x28 flag, +0x30 record: position +0x00 / +0x18, echoes
// +0x38, map +0x3C, Chalice index +0x40) builds a stain object only in the world that owns it, so a
// guest that died while summoned found nothing in the host's world, and its next death replaced the
// record: the echoes were gone. The party stain keeps the same record (map, spot, echoes, and the
// raw record bytes) on the guest's side, marks it in the overlay while the guest is in the host's
// world on that map, and pays it (AddSoul, PlayerIns vfunc +0x3D0) when the guest walks onto it;
// the own record is then cleared too. At home the game's own stain does the work; when a summoned
// load dropped the own record's flag (pard2 c10: `own bloodstain taken ... own echoes` unchanged)
// the record is put back. One stain per character (save slot), user\party_stain.txt.
// BB_PARTY_GUEST_STAIN=0: off (the own world's stain only).
struct PartyStain {
    gueststain::Stain s;
    u8 raw[0x48] = {};
    bool have_raw = false;
    bool armed = false;
    double made_at = 0;
};
std::mutex g_pstain_mu;
PartyStain g_pstain;  // under g_pstain_mu
int g_pstain_slot = -2;  // the save slot the file was read for

bool PartyStainOn() {
    static const bool on = !EnvOff("BB_PARTY_GUEST_STAIN") && !EnvOff("BB_PARTY_GUEST_BLOODSTAIN");
    return on;
}

std::string StainFilePath() {
    std::string dir = bbnet::settings().user_dir;
    if (dir.empty()) {
        const char* u = std::getenv("BB_GPU_USER_DIR");
        if (!u) u = std::getenv("BB_USER_DIR");
        dir = u ? u : std::string(std::getenv("BB_DATA_DIR") ? std::getenv("BB_DATA_DIR") : ".") + "/user";
    }
    return dir + "/party_stain.txt";
}

std::vector<std::string> ReadStainLines() {
    std::vector<std::string> out;
    if (FILE* f = std::fopen(StainFilePath().c_str(), "rb")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            out.emplace_back(line);
        }
        std::fclose(f);
    }
    return out;
}

/// Caller holds g_pstain_mu: the file's line for slot `slot` becomes `g_pstain` (or goes).
void WriteStainLocked(int slot) {
    std::vector<std::string> keep;
    for (const std::string& l : ReadStainLines()) {
        gueststain::Stain o;
        if (gueststain::Parse(l, &o) && o.slot != slot) {
            keep.push_back(l);
        }
    }
    if (g_pstain.s.live && g_pstain.s.slot == slot) {
        std::string line = gueststain::Format(g_pstain.s);
        if (g_pstain.have_raw) {
            line += ' ';
            static const char* hex = "0123456789abcdef";
            for (u8 b : g_pstain.raw) {
                line += hex[b >> 4];
                line += hex[b & 15];
            }
        }
        keep.push_back(line + "\n");
    }
    const std::string path = StainFilePath(), tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        for (const std::string& l : keep) {
            std::fputs(l.c_str(), f);
        }
        std::fclose(f);
        std::remove(path.c_str());
        std::rename(tmp.c_str(), path.c_str());
    }
}

/// Caller holds g_pstain_mu: this character's stain from the file.
void LoadStainLocked(int slot) {
    g_pstain = PartyStain{};
    for (const std::string& l : ReadStainLines()) {
        gueststain::Stain o;
        std::string rest;
        if (gueststain::Parse(l, &o, &rest) && o.slot == slot) {
            g_pstain.s = o;
            g_pstain.made_at = Now();
            if (rest.size() == 2 * sizeof g_pstain.raw) {
                auto nib = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
                for (std::size_t i = 0; i < sizeof g_pstain.raw; ++i) {
                    g_pstain.raw[i] = u8(nib(rest[2 * i]) << 4 | nib(rest[2 * i + 1]));
                }
                g_pstain.have_raw = true;
            }
            Log("party stain: %d Blood Echoes at (%.1f, %.1f, %.1f) map %#x (save slot %d, from party_stain.txt)",
                o.echoes, double(o.pos[0]), double(o.pos[1]), double(o.pos[2]), o.map, slot);
        }
    }
}

/// Caller holds g_pstain_mu: the party stain becomes the game's own record (a guest's death, or the
/// own stain found at the first summoned moment).
void StainFromRecordLocked(int slot, const char* why) {
    const u64 gdm = Slot(kSlotGameData);
    const u64 rec = Field<u64>(gdm, 0x30);
    if (!rec) {
        return;
    }
    PartyStain p;
    p.s.live = true;
    p.s.slot = slot;
    p.s.map = Field<u32>(rec, 0x3c);
    p.s.echoes = Field<int>(rec, 0x38);
    SafeRead(rec + 0x18, p.s.pos, 12);
    p.have_raw = SafeRead(rec, p.raw, sizeof p.raw);
    p.made_at = Now();
    if (p.s.echoes <= 0) {
        return;
    }
    const bool same = g_pstain.s.live && g_pstain.s.echoes == p.s.echoes && g_pstain.s.map == p.s.map &&
                      gueststain::Horizontal(g_pstain.s.pos, p.s.pos) < 0.05f;
    if (same) {
        return;
    }
    if (g_pstain.s.live) {
        Log("party stain: the old one (%d Blood Echoes at (%.1f, %.1f, %.1f)) is replaced: its echoes are lost",
            g_pstain.s.echoes, double(g_pstain.s.pos[0]), double(g_pstain.s.pos[1]), double(g_pstain.s.pos[2]));
    }
    g_pstain = p;
    Log("party stain (%s): %d Blood Echoes at (%.1f, %.1f, %.1f) map %#x (save slot %d)", why, p.s.echoes,
        double(p.s.pos[0]), double(p.s.pos[1]), double(p.s.pos[2]), p.s.map, slot);
    WriteStainLocked(slot);
}

/// ERSC / single-player rule (design decision 3): a guest dying in the host's world drops its echoes
/// into a bloodstain, as the host does. The game's own record 0x13D2910(SprjEventMan +0x80, chr, 1)
/// (what SoloPlayDeath_2 calls) is used; in host-world mode (GSM +0x1592 == 0) it would place the
/// stain at the guest's pre-summon spot (forced placement +0x1500), so the call runs with +0x1592 = 1
/// (the chr's own position and map) and +0x1770 = 1 (the record gate), both put back after.
/// BB_PARTY_GUEST_BLOODSTAIN=0: off (a phantom keeps its echoes, vanilla).
void GuestBloodstain() {
    static const bool on = !(std::getenv("BB_PARTY_GUEST_BLOODSTAIN") && std::getenv("BB_PARTY_GUEST_BLOODSTAIN")[0] == '0');
    const u64 gsm = Slot(kSlotGsm), player = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    const u64 pgd = Field<u64>(Slot(kSlotGameData), 8);
    const u64 events = Field<u64>(Slot(0x553b108), 0x80);
    const int echoes = Field<int>(pgd, 0x94, -1);
    if (!on || !gsm || !player || !pgd || !events || echoes <= 0) {
        if (on) {
            Log("guest: no bloodstain (echoes %d)", echoes);
        }
        return;
    }
    const u8 own = Field<u8>(gsm, 0x1592), gate = Field<u8>(gsm, 0x1770), one = 1;
    SafeWrite(gsm + 0x1592, &one, 1);
    SafeWrite(gsm + 0x1770, &one, 1);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u32)>(Guest(0x13d2910))(events, player, 1);
    SafeWrite(gsm + 0x1592, &own, 1);
    SafeWrite(gsm + 0x1770, &gate, 1);
    const u64 rec = Field<u64>(Slot(kSlotGameData), 0x30);
    float pos[3] = {};
    SafeRead(rec + 0x18, pos, 12);
    Log("guest: bloodstain recorded: echoes %d -> %d, stain %d echoes at (%.1f, %.1f, %.1f) map %#x (gate was %u)", echoes,
        Field<int>(pgd, 0x94, -1), Field<int>(rec, 0x38, -1), double(pos[0]), double(pos[1]), double(pos[2]),
        Field<u32>(rec, 0x3c), unsigned(gate));
    if (PartyStainOn()) {
        const int slot = party::flow::CurrentSaveSlot();
        if (slot >= 0) {
            std::lock_guard<std::mutex> lk(g_pstain_mu);
            if (g_pstain_slot != slot) {
                LoadStainLocked(slot);
                g_pstain_slot = slot;
            }
            StainFromRecordLocked(slot, "death in the host's world");
        }
    }
}

bool HoldGuestDeath() {
    if (!g_on.load() || Role() != RoleClient || DeathGrace() <= 0) {
        return false;
    }
    GuestPhase phase;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        phase = S().gphase;
    }
    g_death_held_at = Now();
    g_last_hold_at = g_death_held_at.load();
    GuestBloodstain();
    Send(0, "{\"op\":\"member_down\"}");  // the host counts its members down (C2: a wipe while it is down)
    if (phase != GuestPhase::Idle) {
        g_dead_follow = true;
    }
    Log("guest: own death in the host's world held (%s)",
        phase != GuestPhase::Idle ? "the host's travel is in flight: it takes the guest along"
                                  : "waiting for a host travel");
    return true;
}

/// The host's lamp as a travel intent: ReturnPointParam row via 0x1F26B40(out, id) (out +0x10 =
/// row: +0 area, +1 block, +4 return point entity, +8 aux), the record 0x13CDF30 writes to
/// GSM +0x153C ({+4, +8}).
bool LampIntent(u32 id, TravelIntent* t) {
    alignas(16) u8 out[0x20] = {};
    reinterpret_cast<void(BB_COOP_SYSV*)(void*, u32)>(Guest(0x1f26b40))(out, id);
    u64 row = 0;
    std::memcpy(&row, out + 0x10, 8);
    if (!row) {
        return false;
    }
    const u8 area = Field<u8>(row, 0), block = Field<u8>(row, 1);
    const u64 rec = u64(Field<u32>(row, 4)) | (u64(Field<u32>(row, 8)) << 32);
    t->kind = TravelKind::Lamp;
    t->lamp_id = id;
    (void)block;
    // Lamp ids are AABnnnn (2412952: m24_01): the row's own map bytes did not read back right.
    t->packed_map = (id / 100000) << 24 | ((id / 10000) % 10) << 16;
    t->respawn_mode = id / 100000 == 21 ? 2 : 1;
    (void)area;
    t->respawn_record = rec;
    t->last_lamp = rec;
    return true;
}

/// ER-Seamless-style respawn of a guest that died alone (BB_PARTY_B2_SOLO_RESPAWN, default on):
/// still a client, it asks the host for a fresh snapshot ("respawn") and loads the host's world
/// again as a dead follow (the host's last-lamp records written before the death load), i.e. it
/// respawns at the host's last lamp in the host's world. No leave, no bell. False: not possible
/// (no session, no host lamp, off) - the caller runs the old redirect.
/// A lit lamp of the map the guest stands in (the host's world): the ReturnPointParam ids AAB295k
/// of that map whose lamp flag 1AAB7800 + 20k (2009[5] registration, party_flags.inc) is set in
/// the host-world overlay. The first one (lowest k); 0 if none.
u32 LitLampInMap(u32 map) {
    const u64 man = Slot(0x553b100);
    if (!man) {
        return 0;
    }
    using Get = u32(BB_COOP_SYSV*)(u64, u32, u32);
    u32 first = 0;
    std::string seen;
    for (u32 id : ids::kPartyReturnPoints) {
        if (ids::EntityMap(id) != (map & 0xffff0000u) || id % 10000 / 10 != 295) {
            continue;
        }
        // "Lamp lit": common event 7100's flag for lamp object AAB195k is 7AAB0200 + k (m24_01:
        // 72410200..203 for 2411950..953, from the EMEVD args), a T7 group the host-world mirror
        // (party_progress world_groups) copies into the guest's overlay. (The 2009[5] registration
        // flags 1AAB7800 + 20k read 0 here: T1 zone 7 is not mirrored.)
        const u32 flag = 70000000u + id / 10000 * 10000 + 200 + (id % 10);
        const u32 lit = reinterpret_cast<Get>(Guest(0x13cfd80))(man, flag, 1);
        seen += " " + std::to_string(id) + (lit ? ":lit" : ":-");
        if (lit) {
            Log("guest: lamps of %s:%s", MapName(map).c_str(), seen.c_str());
            return id;
        }
        if (!first) {
            first = id;
        }
    }
    // None reads lit here (the host-world overlay may not carry these flags): a lamp of this map
    // all the same - any ReturnPointParam row of the map is a valid respawn spot in it.
    Log("guest: lamps of %s:%s; none read lit, using %u", MapName(map).c_str(), seen.c_str(), first);
    return first;
}

bool SoloRespawn(double held_for, u32 map_now) {
    static const bool on = !(std::getenv("BB_PARTY_B2_SOLO_RESPAWN") && std::getenv("BB_PARTY_B2_SOLO_RESPAWN")[0] == '0');
    u32 lamp = GuestDeathRedirectLamp();
    if (on && lamp != kTravelNone && lamp != 0 && map_now != 0xffffffffu &&
        ids::EntityMap(lamp) != (map_now & 0xffff0000u)) {
        // The host's last lamp is in another map than the one the party is in (a stage warp, a
        // walk into the next area): a respawn there would load another map in host-world mode
        // (the Dream case of 6.8). A lit lamp of the current map instead.
        const u32 here = LitLampInMap(map_now);
        Log("guest: the host's last lamp %u is not in %s; lit lamp here: %u", lamp, MapName(map_now).c_str(), here);
        lamp = here;
    }
    TravelIntent t;
    if (!on || Role() != RoleClient || lamp == kTravelNone || lamp == 0 || !LampIntent(lamp, &t)) {
        return false;
    }
    g_dead_follow = true;
    g_solo_respawn = true;
    if (!BeginRejoin(t, true, false)) {
        g_dead_follow = false;
        g_solo_respawn = false;
        return false;
    }
    {
        // Where it died: the host removes the member chr standing there (the remote chrs' HP does
        // not tell the dead one apart: live r19 read hp 1 on both guests and removed both).
        float p[3] = {0, 0, 0};
        ChrPos(Field<u64>(Slot(kSlotWorldChrMan), 0x60), p);
        char b[128];
        std::snprintf(b, sizeof b, "{\"op\":\"respawn\",\"pos\":[%.3f,%.3f,%.3f]}", p[0], p[1], p[2]);
        Send(0, b);
    }
    Log("guest: died alone (no host travel in %.1f s): respawn at the host's lamp %u in the host's world (%s); "
        "asked the host for a snapshot",
        held_for, lamp, DescribeTravel(t).c_str());
    return true;
}

/// Main thread: a held guest death goes with the host's travel, or runs after the grace.
// Guest: a death after a boss clear never reached PartyGhostDeath (live 2026-10-11: after the
// Cleric Beast, ForceDead and a fall into the void both set HP 0, but no guest_died funnel, no
// respawn, the guest stayed dead in the host's world). The boss-clear Lua state BlockClear2_3 ends
// in the send-home tail jump K10 drops, so the phantom's state machine never leaves it and never
// runs PartyGhostDeath. A death in the arena during the fight stayed unprocessed too (17 s until
// the kill). Watchdog: HP 0 for 12 s (after a K10 on this load) or 15 s, with no death funnel or
// hold (the normal path fires ~8.6 s after the death) -> the guest death redirect runs from here.
/// BB_PARTY_B2_DEAD_WATCHDOG=redirect: the old watchdog (a leave and a re-summon).
bool DeadWatchdogHolds() {
    static const bool holds = !(std::getenv("BB_PARTY_B2_DEAD_WATCHDOG") &&
                                std::strcmp(std::getenv("BB_PARTY_B2_DEAD_WATCHDOG"), "redirect") == 0);
    return holds;
}
void DeadAfterClearTick(const GameSnapshot& s, double now) {
    static double dead_since = 0;
    static bool fired = false;
    if (!g_on.load() || Role() != RoleClient || !s.world_up || s.loading || !s.player) {
        if (s.loading) g_k10_this_load = false;
        dead_since = 0;
        fired = false;
        return;
    }
    const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
    const int hp = Field<int>(dm, 0xf8, -1), mx = Field<int>(dm, 0xfc, -1);
    if (hp != 0 || mx <= 1) {
        dead_since = 0;
        fired = false;
        return;
    }
    // Regression run 0.6.1 (live wipe): the guest died 10 s before the host's respawn travel came;
    // this watchdog fired 4.5 s into the follow (still before its load), ended the session and the
    // guest re-joined ~50 s later. A guest following the host's travel is revived by that load.
    bool following = false;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        following = S().gphase != GuestPhase::Idle;
    }
    if (!deathrules::DeadWatchdogFires(1e9, 0, false, following)) {
        dead_since = 0;
        return;
    }
    if (dead_since <= 0) dead_since = now;
    // 12 s after a K10 on this load (the normal path fires ~8.6 s after the death), 15 s otherwise
    // (live: a death in the Cleric Beast's arena during the fight was not processed either).
    const double limit = g_k10_this_load.load() ? 12.0 : 11.0;
    // The game's death state did run when B2 held the death (HoldGuestDeath) - also once the hold
    // was consumed by a lone respawn or a dead follow that is still loading (hostdeath hd22: this
    // watchdog redirected a guest 8 s into its lone respawn, which ended the session).
    const bool held = g_death_held_at.load() >= dead_since || g_last_hold_at.load() >= dead_since ||
                      g_dead_follow.load() || g_solo_respawn.load();
    if (fired || !deathrules::DeadWatchdogFires(now - dead_since, limit, held, false)) {
        return;
    }
    fired = true;
    // 0.6.2 (parceh2 s1, live): a guest revived in place (no load) never reached PartyGhostDeath on
    // its NEXT death either (the phantom's death state does not run again until a load), so its
    // second death ended the session here (redirect = leave, re-summon). The death is now held
    // as if the funnel had run: HeldDeathTick revives it at its own lamp / limbo / the host's lamp
    // as usual (its grace already spent); the redirect stays the fallback there.
    if (DeadWatchdogHolds() && HoldGuestDeath()) {
        g_death_held_at = now - DeathGrace();
        Log("guest: dead for %.0f s and the game's death state never ran (%s): the death is taken as held (no leave)",
            now - dead_since, g_k10_this_load.load() ? "after a boss clear: BlockClear state kept by K10" : "no PartyGhostDeath");
        return;
    }
    Log("guest: dead for %.0f s and the game's death state never ran (%s): the guest death redirect runs now",
        now - dead_since, g_k10_this_load.load() ? "after a boss clear: BlockClear state kept by K10" : "no PartyGhostDeath");
    GuestDeathRedirectNow();
}

bool GuestReviveInPlace(const GameSnapshot& s, u32 lamp, const char* why);
int ChrHp(u64 chr);
void SpectateBegin(const char* who);
void SpectateEnd();
bool GuestDeathAloneToOwnLamp(const GameSnapshot& s, u32 own);
void HeldDeathTick(const GameSnapshot& s, double now) {
    if (g_dead_follow.load() && !SessionUp()) {
        // The follow ended without an arrival (B1 fallback): the host's records stay, which is
        // where the fallback and the gather take the guest anyway.
        g_dead_follow = false;
        g_solo_respawn = false;
        g_own_respawn.saved = false;
    }
    const double at = g_death_held_at.load();
    if (at <= 0) {
        return;
    }
    GuestPhase phase;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        phase = S().gphase;
    }
    if (phase != GuestPhase::Idle) {
        g_death_held_at = 0;
        g_dead_follow = true;
        Log("guest: held death goes with the host's travel (%.1f s after it); no redirect, no leave", now - at);
        return;
    }
    if (now - at < DeathGrace() || s.loading) {
        return;
    }
    static double limbo_logged = 0;
    {
        // C3: the host is down too: its death load (a wipe: one load for the whole party) takes
        // this guest along. Live parx deaths (3 players): the guest died 11 s before the host and
        // was revived in place while the host lay dead, 4 s before the party's respawn load.
        static double host_down_logged = 0;
        for (const TaskInfo& t : Tasks(false)) {
            const u64 c = t.mode == 2 ? PlayerForPeer(t.task + kTaskPeer) : 0;
            // At most 30 s (a stale host chr must not hold the respawn for good).
            if (c && ChrHp(c) == 0 && now - at < 30.0) {
                if (host_down_logged != at) {
                    host_down_logged = at;
                    Log("guest: dead while the host is down too: no revive; the host's death load takes it along");
                }
                return;
            }
        }
    }
    {
        // ERSC: a death in a boss fight waits (limbo) until the boss is gone, the party wipes (the
        // host's death travel takes the guest along, above) or someone rests (a travel, same).
        std::string boss;
        if (BossFightActive(&boss)) {
            if (limbo_logged != at) {
                limbo_logged = at;
                Log("guest: dead in a boss fight (%s): limbo until the boss is gone, a wipe or a rest", boss.c_str());
                SpectateBegin("guest");
            }
            return;
        }
        if (limbo_logged == at) {
            Log("guest: the boss fight is over (%.1f s after the death); respawning", now - at);
            SpectateEnd();
        }
    }
    g_death_held_at = 0;
    // Design decisions (ERSC_PARITY.md): outside a boss fight a dead player respawns at its OWN last
    // lamp (split travel: a re-join when that lamp is in the host's area, else an alone load);
    // after a boss fight everyone uses the party's lamp, the host's last lamp (SoloRespawn).
    static const bool own_lamp = !(std::getenv("BB_PARTY_GUEST_OWN_LAMP") && std::getenv("BB_PARTY_GUEST_OWN_LAMP")[0] == '0');
    {
        // The respawn lamp in the loaded map: revived where it lies and put on the lamp, no load
        // for anyone (own last lamp outside a boss fight, the host's after one). Own lamp in
        // another map: split travel loads it for this guest alone; if it cannot, the host's lamp.
        const u32 own = Field<u32>(Slot(kSlotGsm), 0x1528);
        const u32 party = GuestDeathRedirectLamp();
        const bool boss = limbo_logged == at;
        if (!boss && own_lamp) {
            if (GuestReviveInPlace(s, own, "its own last lamp")) {
                return;
            }
            // BB_PARTY_GUEST_DEATH_ALONE (default on since 2026-10-15; =0 off): split travel refuses a dead
            // traveller's alone load, so the guest is first revived where it lies (no warp) and
            // then travels alone to its own lamp like a living player (one load, this guest only).
            if (GuestDeathAloneToOwnLamp(s, own)) {
                return;
            }
            if (RespawnAloneAtOwnLamp()) {
                Log("guest: died alone (no host travel in %.1f s): its own last lamp %u is in another map: a load for "
                    "this guest only", now - at, own);
                return;
            }
        }
        if (GuestReviveInPlace(s, party, "the host's last lamp")) {
            return;
        }
    }
    if (SoloRespawn(now - at, s.map_id)) {
        return;
    }
    Log("guest: no host travel %.1f s after its death; the usual death redirect runs", now - at);
    GuestDeathRedirectNow();
}

// ---- C2: the host dies alone (docs/party/b2_seamless.md 6.13) ----
// Vanilla: SoloPlayDeath_2 0x1381DE0 selects the last lamp (0x1332BC0), calls the funnel
// 0x13CDE30 (stage request -> MoveMapStep: the whole world, and with it every member chr, is
// rebuilt; with B2 the guests follow in one load), then records the bloodstain 0x13D2910 (echoes
// -> GameDataMan +0x30, PlayerGameData +0x94 = 0). With a member in the world the host's funnel
// call is taken instead (party_travel SetHostDeathIntercept): no stage request, no broadcast; the
// rest of SoloPlayDeath_2 runs as usual (bloodstain, echoes lost). A moment later the host's chr is
// revived where it lies and put on its lamp's spawn spot in the loaded map; the guests never load.
// Falls back to the vanilla party move when the lamp is not in the loaded map (or its spawn spot
// was never seen), and runs the vanilla funnel when every guest is down too (a wipe, 6.8).
using u16 = std::uint16_t;
struct LampSpot {
    bool valid = false;
    u32 lamp = 0, map = 0;
    alignas(16) float pos[4] = {};
    alignas(16) float rot[4] = {};
};
LampSpot g_lamp_spot;  // main thread
std::atomic<bool> g_host_down{false};    // the host's death was taken; not revived yet
std::atomic<double> g_host_down_at{0};
std::atomic<int> g_revivals{0};
std::atomic<bool> g_host_limbo{false};   // the host's death came in a boss fight: it waits for the end
std::mutex g_dead_mu;
std::map<int, double> g_dead_members;    // host: party slot -> when it said it died (under g_dead_mu)
std::atomic<bool> g_host_revived_note{false};  // guest: the host was revived in place (re-create its chr)

// What a chr death writes (ChrIns_OnDeath 0x18C49F0 and its followers 0x18C4F90 / 0x18C5070, the
// per-frame trigger 0x18BD9D0), snapshot while the host is alive.
struct DeathFields {
    u64 f1e1 = 0;          // ChrIns +0x1E1, 5 bytes of flags (0x10000 dying, 0x22000 / 0x40000 death drops done)
    u16 ctrl114 = 0;       // ChrCtrl (+0x58) +0x114 (bit 0 set on death)
    u8 ctrl38_48 = 0;      // [ChrCtrl +0x38] +0x48 (bit 0 set on death)
    int m8_1b4 = 0, m8_1b8 = 0;  // [+0x3B0] +8: +0x1B4 / +0x1B8 (death kind 2/9, 2)
    u16 m20_200 = 0;       // HP module [+0x3B0] +0x20 +0x200 (dead mode / no-dead bits)
    int type = 0;          // +0x78
    float alpha = 0, alpha_rate = 0;  // +0x20C draw alpha, +0x210 its rate (ChrFadeIn / ChrFadeOut 0x1339010 / 0x13390B0)
    int upd_state = 0;     // [+0x18] +0x10 (4: disabled, IsDisable 0x1337E70), +0x20 bits
    u8 upd_bits = 0;
};
bool ReadDeathFields(u64 chr, DeathFields* d) {
    if (!chr) {
        return false;
    }
    const u64 ctrl = Field<u64>(chr, 0x58), m8 = Field<u64>(Field<u64>(chr, 0x3b0), 8),
              m20 = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
    d->f1e1 = 0;
    SafeRead(chr + 0x1e1, &d->f1e1, 5);
    d->ctrl114 = Field<u16>(ctrl, 0x114);
    d->ctrl38_48 = Field<u8>(Field<u64>(ctrl, 0x38), 0x48);
    d->m8_1b4 = Field<int>(m8, 0x1b4);
    d->m8_1b8 = Field<int>(m8, 0x1b8);
    d->m20_200 = Field<u16>(m20, 0x200);
    d->type = Field<int>(chr, 0x78);
    d->alpha = Field<float>(chr, 0x20c);
    d->alpha_rate = Field<float>(chr, 0x210);
    d->upd_state = Field<int>(Field<u64>(chr, 0x18), 0x10);
    d->upd_bits = Field<u8>(Field<u64>(chr, 0x18), 0x20);
    return ctrl && m8 && m20;
}
std::string DescribeDeathFields(const DeathFields& d) {
    char b[260];
    std::snprintf(b, sizeof b,
                  "+0x1e1 %010llx ctrl+0x114 %04x ctrl38+0x48 %02x m8 %d/%d hp+0x200 %04x type %d alpha %.2f (%+.2f/s) "
                  "upd %d/%02x",
                  ull(d.f1e1), unsigned(d.ctrl114), unsigned(d.ctrl38_48), d.m8_1b4, d.m8_1b8, unsigned(d.m20_200),
                  d.type, double(d.alpha), double(d.alpha_rate), d.upd_state, unsigned(d.upd_bits));
    return b;
}
DeathFields g_alive_fields;  // main thread: the host's, last seen alive
bool g_seen_dying = false;   // main thread: the host's chr went down (until the revive or a load)
// The revive cleared the death bits of ChrCtrl +0x114 / [+0x38] +0x48 (the chr acts again). With
// them clear a later stage request (lamp warp, death, Hunter's Mark) stayed armed and no load began
// (live hd11, as with the full alive copy in hd6/hd7); with them set it loads (hd10). So once a
// stage request is armed (GSM +8) they are set again for the load (which re-creates the chr).
u64 g_ctrl_cleared_for = 0;

// Diagnostics (BB_PARTY_B2_REVIVE_DIFF=1): raw copies of the host's chr and its parts while alive,
// diffed against the dead chr at the revive (which fields a death changes).
struct RawPart {
    const char* name;
    u64 base = 0;
    std::vector<u8> bytes;
};
std::vector<RawPart> g_alive_raw;
std::vector<RawPart> SnapshotRaw(u64 chr) {
    std::vector<RawPart> out;
    auto add = [&](const char* name, u64 base, std::size_t n) {
        RawPart r{name, base, std::vector<u8>(n)};
        if (base && SafeRead(base, r.bytes.data(), n)) {
            out.push_back(std::move(r));
        }
    };
    add("chr", chr, 0x800);
    add("ctrl", Field<u64>(chr, 0x58), 0x400);
    add("ctrl38", Field<u64>(Field<u64>(chr, 0x58), 0x38), 0x100);
    add("upd18", Field<u64>(chr, 0x18), 0x80);
    static const char* names[] = {"mod+0", "mod+8", "mod+10", "mod+18", "mod+20", "mod+28", "mod+30", "mod+38",
                                  "mod+40", "mod+48", "mod+50", "mod+58", "mod+60", "mod+68", "mod+70", "mod+78",
                                  "mod+80", "mod+88", "mod+90", "mod+98"};
    const u64 bag = Field<u64>(chr, 0x3b0);
    for (int i = 0; i < 20; ++i) {
        add(names[i], Field<u64>(bag, u64(i) * 8), 0x300);
    }
    return out;
}
/// What a death changes besides HP (live diff of the host's chr alive vs dead, run hd5), put back
/// from the alive copy: PlayerIns +0x208 (draw factor 1 -> 0: the dissolve) and +0x7E0;
/// ChrCtrl +0x114 (0 -> 0x10001) and +0x288; the action-request block ChrCtrl +0x370 (+0x3C, +0x40,
/// +0x48, +0x58, +0x64, +0x68: the death request); module [+0x3B0] +8 (+0x10, +0x188, +0x190,
/// +0x1BC, +0x204, +0x208). Only dwords that differ are written; positions and animation slots are
/// left alone.
struct RestoreSpec {
    const char* part;
    std::vector<u32> offs;  // empty: every differing dword of the part
};
int RestoreAliveFields(u64 chr) {
    static const RestoreSpec specs[] = {
        {"chr", {0x208, 0x7e0}},
        {"ctrl", {0x114, 0x288}},
        {"ctrl38", {}},
        {"mod+8", {}},
    };
    const std::vector<RawPart> now = SnapshotRaw(chr);
    int n = 0;
    for (const RestoreSpec& sp : specs) {
        for (const RawPart& a : g_alive_raw) {
            if (std::strcmp(a.name, sp.part) != 0) {
                continue;
            }
            for (const RawPart& b : now) {
                if (std::strcmp(b.name, sp.part) != 0 || a.base != b.base) {
                    continue;
                }
                auto put = [&](std::size_t o) {
                    if (o + 4 <= a.bytes.size() && std::memcmp(&a.bytes[o], &b.bytes[o], 4) != 0) {
                        SafeWrite(a.base + o, &a.bytes[o], 4);
                        ++n;
                    }
                };
                if (sp.offs.empty()) {
                    for (std::size_t o = 0; o + 4 <= a.bytes.size(); o += 4) {
                        put(o);
                    }
                } else {
                    for (u32 o : sp.offs) {
                        put(o);
                    }
                }
            }
        }
    }
    return n;
}

void LogRawDiff(u64 chr) {
    const std::vector<RawPart> now = SnapshotRaw(chr);
    int shown = 0;
    for (const RawPart& a : g_alive_raw) {
        for (const RawPart& b : now) {
            if (std::strcmp(a.name, b.name) != 0 || a.base != b.base) {
                continue;
            }
            std::string line;
            for (std::size_t o = 0; o + 4 <= a.bytes.size() && shown < 400; o += 4) {
                u32 x, y;
                std::memcpy(&x, &a.bytes[o], 4);
                std::memcpy(&y, &b.bytes[o], 4);
                if (x == y) {
                    continue;
                }
                float fx, fy;
                std::memcpy(&fx, &x, 4);
                std::memcpy(&fy, &y, 4);
                char t[96];
                std::snprintf(t, sizeof t, " +%zx:%08x->%08x(%g->%g)", o, x, y, double(fx), double(fy));
                line += t;
                ++shown;
            }
            if (!line.empty()) {
                Log("revive diff %s (%#llx):%s", a.name, ull(a.base), line.c_str());
            }
        }
    }
}
bool g_alive_fields_ok = false;

/// HP module ([+0x3B0] +0x20): +0xF8 HP, +0xFC max HP (ChrIns_OnDeath 0x18C49F0 reads them; the
/// live diff of a death showed +0xF8 going to 0).
int ChrHp(u64 chr) {
    return Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0xf8, -1);
}

bool ChrTransform(u64 chr, float pos[4], float rot[4]) {
    const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
    return a && SafeRead(a + 0x1e0, pos, 16) && SafeRead(a + 0x1d0, rot, 16);
}

/// BB_PARTY_B2_HOST_REVIVE=1 (opt-in since playtest 1): the host's death is a revive in place at
/// its lamp in the loaded map (no load, no world reset). Default: the vanilla respawn load for the
/// whole party (the chr warp broke occlusion, left the guests' HostDead state half-done until their
/// next load - blocked pickups and prompts - and reset nothing; triage bugs 6, 7, 12, 13).
bool HostReviveOn() {
    static const bool on = deathrules::OptIn(std::getenv("BB_PARTY_B2_HOST_REVIVE"));
    return on;
}

/// BB_PARTY_B2_REVIVE_RECIPE (diagnostics): which revive steps run (comma list).
bool RecipeHas(const char* step) {
    // '|' separates the recipes of successive revivals (the last one repeats): one run tries several.
    static const std::vector<std::string> list = [] {
        const char* v = std::getenv("BB_PARTY_B2_REVIVE_RECIPE");
        std::string all = v && v[0] ? v : "recover,flags,ctrl,fade,reset,idle,warp,gsm,rearm";
        std::vector<std::string> out;
        for (std::size_t a = 0;;) {
            const std::size_t b = all.find('|', a);
            out.push_back(all.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) {
                break;
            }
            a = b + 1;
        }
        return out;
    }();
    const std::string& r = list[std::min<std::size_t>(std::size_t(g_revivals.load()), list.size() - 1)];
    return ("," + r + ",").find(std::string(",") + step + ",") != std::string::npos;
}

/// Where the player stands right after a load that put it on its lamp (a lamp warp, a death, a
/// Hunter's Mark: GSM +0x1538 respawn mode 1/2 with a +0x153C record, or +0x1524 "at the last
/// lamp" from the selector 0x1332BC0): that lamp's spawn spot in this map (the game's own
/// placement; where a revive puts the host). Own world only.
void LampSpotTick(const GameSnapshot& s) {
    static bool was_loading = false;
    static bool lamp_load = false;
    static int stable = 0;
    const bool loading = s.loading || !s.world_up;
    const u64 gsm = Slot(kSlotGsm);
    if (loading && !was_loading) {
        const u32 mode = Field<u32>(gsm, 0x1538);
        const u32 rec = Field<u32>(gsm, 0x153c);
        lamp_load = ((mode == 1 || mode == 2) && rec != 0xffffffffu && rec != 0) || Field<u8>(gsm, 0x1524) != 0;
        stable = 0;
        Log("lamp spot: load starts (respawn mode %u, record %u, at-last-lamp %u, last lamp %u)%s", mode, rec,
            unsigned(Field<u8>(gsm, 0x1524)), Field<u32>(gsm, 0x1528), lamp_load ? ": a lamp arrival" : "");
    }
    was_loading = loading;
    if (loading || !lamp_load || !s.player) {
        return;
    }
    if (++stable < 30) {
        return;
    }
    lamp_load = false;
    if (Field<u8>(gsm, 0x1592) != 1) {
        return;  // a guest in the host's world: not its own lamp
    }
    LampSpot sp;
    sp.lamp = Field<u32>(gsm, 0x1528);
    sp.map = s.map_id;
    if (sp.lamp == 0 || sp.lamp == 0xffffffffu || !ChrTransform(s.player, sp.pos, sp.rot)) {
        return;
    }
    sp.valid = true;
    g_lamp_spot = sp;
    Log("lamp spot: lamp %u in %s at (%.2f, %.2f, %.2f) yaw %.2f (the host's revive spot)", sp.lamp,
        MapName(sp.map).c_str(), double(sp.pos[0]), double(sp.pos[1]), double(sp.pos[2]), double(sp.rot[1]));
}

/// Host: how many members said they are down ("dead", until they arrive again; 90 s at most).
int DeadMemberCount() {
    std::lock_guard<std::mutex> lk(g_dead_mu);
    const double now = Now();
    for (auto it = g_dead_members.begin(); it != g_dead_members.end();) {
        it = now - it->second > 90.0 ? g_dead_members.erase(it) : std::next(it);
    }
    return int(g_dead_members.size());
}

int MemberTaskCount() {
    int n = 0;
    for (const TaskInfo& t : Tasks(false)) {
        n += t.mode == 1 ? 1 : 0;
    }
    return n;
}

/// The host's death funnel (game thread, SoloPlayDeath_2): taken when the host can be revived in
/// this map with a member in the world.
/// BB_PARTY_B2_HOST_REVIVE_SOLO=1 (test fixture, one instance): the revive also without a session
/// or a member (several deaths in one run, BB_PARTY_B2_REVIVE_RECIPE a|b|c).
bool SoloReviveTest() {
    static const bool on = std::getenv("BB_PARTY_B2_HOST_REVIVE_SOLO") && std::getenv("BB_PARTY_B2_HOST_REVIVE_SOLO")[0] == '1';
    return on;
}

bool HostDeathIntercept() {
    if (!g_on.load() || (Role() != RoleHost && !SoloReviveTest())) {
        return false;
    }
    if (g_host_down.load()) {
        return true;  // the same death's funnel again (a taken death: limbo or revive pending)
    }
    const int members = MemberTaskCount(), dead = DeadMemberCount();
    const u64 gsm = Slot(kSlotGsm);
    const u32 lamp = Field<u32>(gsm, 0x1528);
    const u32 stage = Field<u32>(Field<u64>(Slot(kSlotWorldChrMan), 0x60), 0x3f8);  // the player's stage
    // The lamp's spawn spot: the one seen at the last lamp arrival, else the MSB revive point of the
    // lamp from the game data (party_ids ReturnSpot, split_travel.md 3a), when it is in this map.
    if (HostReviveOn() && !(g_lamp_spot.valid && g_lamp_spot.lamp == lamp)) {
        float rs[4];
        if (lamp != 0 && lamp != kTravelNone && (ids::EntityMap(lamp) >> 16) == (stage >> 16) && ids::ReturnSpot(lamp, rs)) {
            LampSpot d;
            d.valid = true;
            d.lamp = lamp;
            d.map = ids::EntityMap(lamp);
            d.pos[0] = rs[0], d.pos[1] = rs[1], d.pos[2] = rs[2], d.pos[3] = 1.0f;
            d.rot[1] = rs[3] * 3.14159265f / 180.0f;
            g_lamp_spot = d;
            Log("lamp spot: lamp %u from the game data (%.2f, %.2f, %.2f) yaw %.0f deg", lamp, double(rs[0]),
                double(rs[1]), double(rs[2]), double(rs[3]));
        }
    }
    const LampSpot& sp = g_lamp_spot;
    std::string boss;
    deathrules::HostDeathView v;
    v.party_on = true;
    v.members = members;
    v.down_reported = dead;
    v.boss_fight = BossFightActive(&boss);
    v.revive_opt_in = HostReviveOn();
    // The stage id (PlayerIns +0x3F8) and the map id share the area / block bytes.
    v.lamp_here = sp.valid && sp.lamp == lamp && (sp.map >> 16) == (stage >> 16);
    v.alone_opt = !(std::getenv("BB_PARTY_HOST_DEATH_ALONE") && std::getenv("BB_PARTY_HOST_DEATH_ALONE")[0] == '0');
    v.solo_fixture = SoloReviveTest();
    const deathrules::HostDeathPlan plan = deathrules::PlanHostDeath(v);
    switch (plan) {
    case deathrules::HostDeathPlan::Stock:
        return false;
    case deathrules::HostDeathPlan::Wipe:
        Log("host death: every member is down too (%d of %d): a wipe; the party respawns together (one load)", dead,
            members);
        return false;
    case deathrules::HostDeathPlan::PartyLoad:
        Log("host death: the respawn load for the whole party at lamp %u (vanilla loading screen, the world resets); "
            "%d member(s), %d down%s",
            lamp, members, dead, HostReviveOn() ? "" : " (BB_PARTY_B2_HOST_REVIVE=1: revive in place instead)");
        return false;
    case deathrules::HostDeathPlan::AloneLoad:
        // Design decision 1 (revive mode): the host respawns at its own last lamp in another map
        // ALONE; the guests stay where they are (the captured TravelIntent carries `alone`).
        HostTravelAloneNext();
        Log("host death: last lamp %u is not in the loaded map (stage %#x): the host loads it alone (the guests stay)",
            lamp, stage);
        return false;
    case deathrules::HostDeathPlan::BossLimbo:
        g_host_down = true;
        g_host_down_at = Now();
        g_host_limbo = true;
        Log("host death in a boss fight (%s): limbo until the boss is gone (then the %s), a wipe or a rest; "
            "%d member(s), %d down",
            boss.c_str(), HostReviveOn() ? "respawn" : "party's respawn load", members, dead);
        SpectateBegin("host");
        return true;
    case deathrules::HostDeathPlan::ReviveInPlace:
        break;
    }
    g_host_down = true;
    g_host_down_at = Now();
    Log("host death taken: no stage request, no party move; revive at lamp %u (%.1f, %.1f, %.1f) in %s; %d member(s), "
        "%d down; echoes %d go to the bloodstain",
        lamp, double(sp.pos[0]), double(sp.pos[1]), double(sp.pos[2]), MapName(sp.map).c_str(), members, dead,
        Field<int>(Field<u64>(Slot(kSlotGameData), 8), 0x94, -1));
    return true;
}

/// Main thread: after a death kept without a load (a revive in place), the game's death script is
/// armed again. Its handler 0x13813D0 returns at once while event flag 4000 is set or the death
/// request [[SprjEventMan 0x553B108] +0x10] +8 is above 0, and sets both (4000 on, request 4) when
/// it runs; only a load clears them. Left set, the next death of a revived player never reached
/// PartyGhostDeath (the guest waited for the 11 s dead watchdog). Lead: deadinside's notes on
/// CLS_AMG's BBSC (docs/upstream/deadinside_0.6-pre1.md); facts only.
void RearmDeathScript(const char* why) {
    const u64 em = Slot(0x553b108);
    const u64 req_obj = Field<u64>(em, 0x10);
    const int req = Field<int>(req_obj, 8, -1);
    bool flag = false;
    const bool flag_ok = ReadEventFlag(4000, &flag);
    if (req_obj && req > 0) {
        const int zero = 0;
        SafeWrite(req_obj + 8, &zero, 4);
    }
    if (flag_ok && flag) {
        WriteEventFlag(4000, false);
    }
    bool after = false;
    ReadEventFlag(4000, &after);
    Log("%s: the game's death script re-armed (death request %d -> %d, flag 4000 %d -> %d)", why, req,
        Field<int>(req_obj, 8, -1), flag ? 1 : 0, after ? 1 : 0);
}

/// Main thread: the taken host death's revive.
void ReviveLocal(const GameSnapshot& s, const LampSpot& sp, bool host);
void ReviveHost(const GameSnapshot& s) {
    ReviveLocal(s, g_lamp_spot, true);
}

/// Main thread: the local player's chr revived where it lies and put on `sp` (its lamp's spawn
/// spot in the loaded map). `host`: the party host (peers re-create its chr on "host_revived");
/// otherwise a guest (peers re-create the member chr nearest to where it died, "member_revived").
void ReviveLocal(const GameSnapshot& s, const LampSpot& sp, bool host) {
    const u64 p = s.player;
    float died_at[4] = {}, died_rot[4] = {};
    ChrTransform(p, died_at, died_rot);
    DeathFields before;
    ReadDeathFields(p, &before);
    Log("host revive: before: hp %d, %s; alive: %s; echoes %d", ChrHp(p), DescribeDeathFields(before).c_str(),
        g_alive_fields_ok ? DescribeDeathFields(g_alive_fields).c_str() : "-", s.echoes);
    static const bool diff = std::getenv("BB_PARTY_B2_REVIVE_DIFF") && std::getenv("BB_PARTY_B2_REVIVE_DIFF")[0] == '1';
    if (diff && !g_alive_raw.empty()) {
        LogRawDiff(p);
    }
    if (RecipeHas("restore") && !g_alive_raw.empty() && Field<u64>(p, 0x58) == [&] {
            for (const RawPart& r : g_alive_raw) {
                if (std::strcmp(r.name, "chr") == 0) {
                    u64 v = 0;
                    std::memcpy(&v, &r.bytes[0x58], 8);
                    return v;
                }
            }
            return u64(0);
        }()) {
        Log("host revive: %d death field(s) put back from the alive copy", RestoreAliveFields(p));
    }
    const u64 ctrl = Field<u64>(p, 0x58), m8 = Field<u64>(Field<u64>(p, 0x3b0), 8),
              m20 = Field<u64>(Field<u64>(p, 0x3b0), 0x20);
    if (RecipeHas("recover") && host) {  // a guest keeps its phantom type (0x1900710 writes +0x78 = 0)
        // RevivePlayer / OnSpEffectRevive's 0x1900710: body form, SpEffects reset, HP to max.
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(Guest(0x1900710))(p);
    }
    if (RecipeHas("flags")) {
        u64 f = 0;
        SafeRead(p + 0x1e1, &f, 5);
        f &= ~u64(0x10000 | 0x20000 | 0x2000 | 0x40000);
        SafeWrite(p + 0x1e1, &f, 5);
    }
    if (RecipeHas("ctrl") && ctrl) {
        // ChrIns_OnDeath sets ChrCtrl +0x114 bit 0 (and +0x116 bit 0: the live diff read 0 -> 0x10001).
        const u32 c = Field<u32>(ctrl, 0x114) & ~0x10001u;
        SafeWrite(ctrl + 0x114, &c, 4);
        g_ctrl_cleared_for = p;
        if (const u64 c38 = Field<u64>(ctrl, 0x38)) {
            const u8 b = u8(Field<u8>(c38, 0x48) & ~1u);
            SafeWrite(c38 + 0x48, &b, 1);
        }
    }
    if (RecipeHas("module") && m8 && g_alive_fields_ok) {
        SafeWrite(m8 + 0x1b4, &g_alive_fields.m8_1b4, 4);
        SafeWrite(m8 + 0x1b8, &g_alive_fields.m8_1b8, 4);
    }
    if (RecipeHas("v158")) {
        // ChrIns_OnDeath calls vfunc +0x158(chr, 1).
        if (const u64 fn = Field<u64>(Field<u64>(p, 0), 0x158)) {
            reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(fn)(p, 0);
        }
    }
    if (m20) {
        const int mx = Field<int>(m20, 0xfc, -1);
        if (mx > 0) {
            SafeWrite(m20 + 0xf8, &mx, 4);  // HP to max
        }
    }
    if (RecipeHas("fade")) {
        // The death dissolve (+0x20C alpha -> 0, +0x1E5 bit 2): faded in again from 0 over ~0.7 s,
        // as ChrFadeIn does (rate = (1 - alpha) / time).
        const float a0 = 0.0f, rate = 1.5f;
        SafeWrite(p + 0x20c, &a0, 4);
        SafeWrite(p + 0x210, &rate, 4);
        const u8 b = u8(Field<u8>(p, 0x1e5) & ~4u);
        SafeWrite(p + 0x1e5, &b, 1);
    }
    if (RecipeHas("gsm")) {
        const u64 gsm = Slot(kSlotGsm);
        const u8 zero = 0;
        SafeWrite(gsm + 0x1522, &zero, 1);  // "the local player died" (ChrIns_OnDeath)
        SafeWrite(gsm + 0x1523, &zero, 1);
    }
    if (RecipeHas("reset")) {
        // Lua ChrResetRequest 0x13370B0: 0x18BCF30(chr, 1) (transform re-applied, ChrCtrl vfunc +0x38,
        // the behavior graph reset) and +0x1E4 |= 2.
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(Guest(0x18bcf30))(p, 1);
        const u8 b = u8(Field<u8>(p, 0x1e4) | 2);
        SafeWrite(p + 0x1e4, &b, 1);
    }
    if (RecipeHas("idle")) {
        // Lua ChrResetAnimation 0x13370F0: the behavior event L"Idle_wild" (0x4992C40) through
        // 0x1A19F60(behavior [+0x3B0]+0x30, name) when its +0x384 is set: out of BeDead to idle.
        const u64 m30 = Field<u64>(Field<u64>(p, 0x3b0), 0x30);
        if (m30 && Field<u8>(m30, 0x384)) {
            reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(Guest(0x1a19f60))(m30, Guest(0x4992c40));
        } else {
            Log("host revive: no behavior to reset (gate %u)", unsigned(Field<u8>(m30, 0x384)));
        }
    }
    if (RecipeHas("anim")) {
        // Lua ForcePlayAnimation (0x132E290 -> 0x13CBFD0(entity, anim, 0, 0)); 10000 = local player.
        // BB_PARTY_B2_REVIVE_ANIM: '|' separates the animations of successive revivals.
        int anim = 0;
        if (const char* v = std::getenv("BB_PARTY_B2_REVIVE_ANIM"); v && v[0]) {
            const char* p = v;
            for (int k = 0; k < g_revivals.load() && std::strchr(p, '|'); ++k) {
                p = std::strchr(p, '|') + 1;
            }
            anim = std::atoi(p);
        }
        const u64 m30 = Field<u64>(Field<u64>(p, 0x3b0), 0x30);
        Log("host revive: ForcePlayAnimation(10000, %d); anim gate [+0x3B0]+0x30 +0x384 = %u", anim,
            unsigned(Field<u8>(m30, 0x384)));
        reinterpret_cast<void(BB_COOP_SYSV*)(u32, u32, u32, u32)>(Guest(0x13cbfd0))(10000, u32(anim), 0, 0);
    }
    if (RecipeHas("warp") && sp.valid) {
        alignas(16) float pos[4], rot[4];
        std::memcpy(pos, sp.pos, sizeof pos);
        std::memcpy(rot, sp.rot, sizeof rot);
        pos[3] = 1.0f;
        // Another party member standing on the lamp spot (within 1 m): side by side instead of inside
        // it (BesideHost: 0.5 m by party slot).
        for (const TaskInfo& t : Tasks(false)) {
            const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
            float q[3];
            if (c && ChrPos(c, q) && (q[0] - pos[0]) * (q[0] - pos[0]) + (q[2] - pos[2]) * (q[2] - pos[2]) < 1.0f &&
                std::fabs(q[1] - pos[1]) < 2.0f) {
                BesideHost(pos, rot, host ? 2 : (g_local_slot.load() > 0 ? g_local_slot.load() : 1));
                Log("revive: a member stands on the lamp spot; placed beside it (%.2f, %.2f, %.2f)", double(pos[0]),
                    double(pos[1]), double(pos[2]));
                break;
            }
        }
        LocalChrWarpImpl(p, pos, rot, host ? "host revive at the lamp" : "guest revive at the lamp");
    }
    if (RecipeHas("rearm")) {
        RearmDeathScript(host ? "host revive" : "guest revive");
    }
    DeathFields after;
    ReadDeathFields(p, &after);
    g_revivals.fetch_add(1);
    g_seen_dying = false;
    Log("%s revive #%d: after: hp %d, %s; at lamp %u (%s)", host ? "host" : "guest", g_revivals.load(), ChrHp(p),
        DescribeDeathFields(after).c_str(), sp.lamp, DescribeChr(p).c_str());
    if (host) {
        Send(-1, "{\"op\":\"host_revived\"}");
    } else {
        char b[128];
        std::snprintf(b, sizeof b, "{\"op\":\"member_revived\",\"pos\":[%.3f,%.3f,%.3f]}", died_at[0], died_at[1],
                      died_at[2]);
        Send(-1, b);
    }
}

/// Guest, main thread: its held death respawns at `lamp` in the loaded map with no load (the
/// Bloodborne rule: a load only for a real map change). False: the lamp is not in this map or has
/// no known spawn spot (the caller loads as before).
bool GuestReviveInPlace(const GameSnapshot& s, u32 lamp, const char* why) {
    static const bool on = !(std::getenv("BB_PARTY_GUEST_REVIVE") && std::getenv("BB_PARTY_GUEST_REVIVE")[0] == '0');
    const u32 stage = Field<u32>(s.player, 0x3f8);
    float rs[4];
    if (!on || !s.player || lamp == 0 || lamp == kTravelNone || (ids::EntityMap(lamp) >> 16) != (stage >> 16) ||
        !ids::ReturnSpot(lamp, rs)) {
        return false;
    }
    LampSpot sp;
    sp.valid = true;
    sp.lamp = lamp;
    sp.map = ids::EntityMap(lamp);
    sp.pos[0] = rs[0], sp.pos[1] = rs[1], sp.pos[2] = rs[2], sp.pos[3] = 1.0f;
    sp.rot[1] = rs[3] * 3.14159265f / 180.0f;
    Log("guest: died alone: revived at %s %u in the loaded map (%.1f, %.1f, %.1f), no load", why, lamp, double(rs[0]),
        double(rs[1]), double(rs[2]));
    ReviveLocal(s, sp, false);
    return true;
}

bool GuestDeathAloneToOwnLamp(const GameSnapshot& s, u32 own) {
    // On by default since 2026-10-15 (live: Cathedral Ward re-join, Hemwick alone load); "0" off.
    static const bool on = !(std::getenv("BB_PARTY_GUEST_DEATH_ALONE") && std::getenv("BB_PARTY_GUEST_DEATH_ALONE")[0] == '0');
    TravelIntent lt;
    if (!on || own == 0 || own == kTravelNone || !LampIntent(own, &lt)) {
        return false;
    }
    LampSpot none;  // not valid: revived where it lies, no warp
    ReviveLocal(s, none, false);
    if (!TravelAlone(lt, AloneWhy::Lamp)) {
        Log("guest: died alone: revived, but the alone travel to %u was refused", own);
        return false;
    }
    Log("guest: died alone: revived, then travels alone to its own last lamp %u in another map", own);
    return true;
}

/// Any role: a stage request armed after a revive (the ChrCtrl death bits were cleared): set them
/// again so the load goes ahead (see g_ctrl_cleared_for).
void ReviveRearmTick(const GameSnapshot& s) {
    if (!s.player || g_ctrl_cleared_for != s.player || Field<u8>(Slot(kSlotGsm), 8) == 0) {
        return;
    }
    g_ctrl_cleared_for = 0;
    const u64 ctrl = Field<u64>(s.player, 0x58);
    const u32 c = Field<u32>(ctrl, 0x114) | 0x10001u;
    SafeWrite(ctrl + 0x114, &c, 4);
    if (const u64 c38 = Field<u64>(ctrl, 0x38)) {
        const u8 b = u8(Field<u8>(c38, 0x48) | 1u);
        SafeWrite(c38 + 0x48, &b, 1);
    }
    Log("revive: a stage request after the revive; ChrCtrl death bits set again for the load");
}

std::mutex g_member_revived_mu;
std::vector<std::array<float, 3>> g_member_revived;  // positions where revived members died (under mu)
std::vector<std::array<float, 3>> g_spectators;      // positions where members entered a limbo (under mu)

/// Peers: while a member spectates (boss-fight limbo), its dead copy here is disabled (Lua
/// SetDisable 0x1332060 -> 0x18C6390(chr, 1): not drawn, no name plate) instead of removed - a
/// removed copy was built again by its task and its plate followed the spectator around (spec3).
/// Its revive / respawn replaces the copy with a fresh one as usual.
std::atomic<double> g_spectator_until{0};
void SpectatorRemoveTick(const GameSnapshot& s) {
    {
        std::lock_guard<std::mutex> lk(g_member_revived_mu);
        if (!g_spectators.empty()) {
            g_spectators.clear();
            g_spectator_until = Now() + 300.0;
        }
    }
    if (!s.world_up || s.loading || Now() > g_spectator_until.load()) {
        return;
    }
    static std::map<u64, bool> done;
    for (const TaskInfo& t : Tasks(false)) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        if (!c || ChrHp(c) != 0 || done[c]) {
            continue;
        }
        done[c] = true;
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(Guest(0x18c6390))(c, 1);
        Log("spectator: its dead copy %#llx here disabled (not drawn, no plate) until it respawns", ull(c));
    }
}

/// Removes the member chr here nearest to `at` (within 6 m) and rewinds its task so it is built
/// again from the member's GameData row (its record); none near: the chr-less tasks are rewound.
void RecreateMemberNear(const std::array<float, 3>& at, const char* why) {
    u64 victim = 0;
    float best = 36.0f;
    for (const TaskInfo& t : Tasks(false)) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        float p[3];
        if (c && ChrPos(c, p)) {
            const float d = (p[0] - at[0]) * (p[0] - at[0]) + (p[1] - at[1]) * (p[1] - at[1]) +
                            (p[2] - at[2]) * (p[2] - at[2]);
            if (d < best) {
                best = d;
                victim = c;
            }
        }
    }
    if (!victim) {
        // Already removed (its limbo: "spectating") or never here: built again by its task.
        const int n = RewindRemoteTasks(why);
        Log("%s: no member chr within 6 m of (%.1f, %.1f, %.1f) here; %d task(s) rewound", why, double(at[0]),
            double(at[1]), double(at[2]), n);
        return;
    }
    using Release = u64(BB_COOP_SYSV*)(u64, u32);
    using Remove = void(BB_COOP_SYSV*)(u64, u64);
    reinterpret_cast<Release>(Guest(0x15bc080))(Field<u64>(Slot(kSlotGsm), 0x16f8), Field<u32>(victim, 8));
    reinterpret_cast<Remove>(Guest(0x19186e0))(Slot(kSlotWorldChrMan), victim);
    Log("%s: its chr %#llx here removed (%.1f m from (%.1f, %.1f, %.1f)); re-created", why, ull(victim),
        double(std::sqrt(best)), double(at[0]), double(at[1]), double(at[2]));
    RewindRemoteTasks(why);
}

/// Host and guests: a member revived in place: its chr here (dead where it fell) is removed and
/// built again by its task at the member's new spot.
void MemberRevivedTick(const GameSnapshot& s) {
    if (!s.world_up || s.loading) {
        return;
    }
    std::vector<std::array<float, 3>> todo;
    {
        std::lock_guard<std::mutex> lk(g_member_revived_mu);
        todo.swap(g_member_revived);
    }
    for (const auto& at : todo) RecreateMemberNear(at, "member revived");
}

// H5 (0.6.2): a member changed its appearance (the Dream mirror's editor). Its chr on the other
// games was built from its player record at its join or last load; the member re-broadcasts the
// record (0x1781AC0, as InGameStart does after every load) and asks the peers to rebuild its chr
// from it (1.5 s later, once the record has arrived): the new look shows at once, no load.
std::atomic<bool> g_own_appearance_changed{false};
std::vector<std::pair<double, std::array<float, 3>>> g_appearance_todo;  // (due, where), under g_member_revived_mu

void AppearanceTick(const GameSnapshot& s) {
    const bool session = (Role() == RoleHost && s.cooperators > 0) || Role() == RoleClient;
    if (g_own_appearance_changed.load() && s.world_up && !s.loading) {
        g_own_appearance_changed = false;
        float p[3] = {0, 0, 0};
        if (session && ChrPos(Field<u64>(Slot(kSlotWorldChrMan), 0x60), p)) {
            RebroadcastOwnRecord("appearance changed");
            char b[128];
            std::snprintf(b, sizeof b, "{\"op\":\"appearance\",\"pos\":[%.3f,%.3f,%.3f]}", p[0], p[1], p[2]);
            Send(-1, b);
            Log("appearance: changed at the mirror: own record re-broadcast, the peers rebuild this chr");
        } else {
            Log("appearance: changed at the mirror (no party session here: nothing to send)");
        }
    }
    if (!s.world_up || s.loading) return;
    std::vector<std::array<float, 3>> due;
    {
        std::lock_guard<std::mutex> lk(g_member_revived_mu);
        const double now = Now();
        for (auto it = g_appearance_todo.begin(); it != g_appearance_todo.end();) {
            if (now >= it->first) {
                due.push_back(it->second);
                it = g_appearance_todo.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& at : due) RecreateMemberNear(at, "appearance changed");
}

/// Main thread, host: the alive snapshot, the taken death's revive (or a wipe's vanilla funnel)
/// and a 10 s watch of the revived chr.
void HostDownTick(const GameSnapshot& s, double now) {
    static double watch_until = 0, next_watch = 0;
    if (Role() != RoleHost && !SoloReviveTest()) {
        if (g_host_down.exchange(false)) {
            Log("host death: the session ended while the host was down; the stock funnel runs");
            HostDeathFunnelNow();
        }
        std::lock_guard<std::mutex> lk(g_dead_mu);
        g_dead_members.clear();
        return;
    }
    if (s.loading && g_host_down.load()) {
        g_host_down = false;  // a travel (a rest, a guest's lamp) took over: the load revives everyone
        g_host_limbo = false;
        Log("host death: a load started while the host was down; nothing left to revive");
    }
    if (!s.world_up || s.loading || !s.player) {
        g_seen_dying = false;  // a load re-creates the chr
        return;
    }
    {
        // Diagnostics: the local chr's death state as it changes (hp, +0x1E1 0x10000 "dying").
        static int last_hp = -2;
        static u64 last_f = ~0ull;
        u64 f = 0;
        SafeRead(s.player + 0x1e1, &f, 5);
        const int hp = ChrHp(s.player);
        if (f & 0x10000) {
            g_seen_dying = true;  // dead until the revive (the flag itself clears before the funnel)
        }
        if ((hp <= 0) != (last_hp <= 0) || ((f ^ last_f) & 0x10000)) {
            Log("host chr: hp %d, +0x1e1 %010llx (%s)", hp, ull(f), (f & 0x10000) ? "dying" : "alive");
        }
        last_hp = hp;
        last_f = f;
    }
    // (stage-request re-arm: ReviveRearmTick, any role)
    if (!g_host_down.load()) {
        if (ChrHp(s.player) > 0 && !(Field<u64>(s.player, 0x1e1) & 0x10000) && !g_seen_dying) {
            g_alive_fields_ok = ReadDeathFields(s.player, &g_alive_fields);
            static double next_raw = 0;
            if (now >= next_raw && watch_until == 0) {
                next_raw = now + 1.0;
                g_alive_raw = SnapshotRaw(s.player);
            }
        }
        if (watch_until > 0 && now >= next_watch) {
            next_watch = now + 0.5;
            DeathFields d;
            ReadDeathFields(s.player, &d);
            Log("host revive watch: %s, %s", DescribeChr(s.player).c_str(), DescribeDeathFields(d).c_str());
            if (now >= watch_until) {
                watch_until = 0;
            }
        }
        return;
    }
    const int members = MemberTaskCount(), dead = DeadMemberCount();
    static const double delay = [] {
        const char* v = std::getenv("BB_PARTY_B2_HOST_REVIVE_DELAY");
        return v && v[0] ? std::atof(v) : 1.0;
    }();
    deathrules::DownView dv;
    dv.members = members;
    dv.down_reported = dead;
    dv.limbo = g_host_limbo.load();
    dv.boss_active = dv.limbo && BossFightActive();
    dv.revive_opt_in = HostReviveOn();
    dv.down_for = now - g_host_down_at.load();
    dv.delay = delay;
    {
        const u32 lamp = Field<u32>(Slot(kSlotGsm), 0x1528);
        const LampSpot& sp = g_lamp_spot;
        dv.lamp_here = sp.valid && sp.lamp == lamp && (sp.map >> 16) == (Field<u32>(s.player, 0x3f8) >> 16);
    }
    const deathrules::DownTick plan = deathrules::PlanDownTick(dv);
    if (plan == deathrules::DownTick::Wait) {
        return;
    }
    const bool was_limbo = g_host_limbo.exchange(false);
    g_host_down = false;
    if (plan == deathrules::DownTick::Wipe) {
        Log("host death: every member went down while the host was (%d of %d, recounted %.1f s after the death): a "
            "wipe; the stock funnel runs",
            dead, members, dv.down_for);
        HostDeathFunnelNow();
        return;
    }
    if (was_limbo) {
        Log("host death: the boss fight is over (%.1f s after the death); %s", dv.down_for,
            plan == deathrules::DownTick::Revive      ? "respawn at the lamp here"
            : plan == deathrules::DownTick::AloneLoad ? "the lamp is in another map: the host loads it alone"
                                                      : "the party's respawn load");
    }
    if (plan == deathrules::DownTick::PartyLoad) {
        Log("host death: the respawn load for the whole party (%d member(s), %d down)", members, dead);
        HostDeathFunnelNow();
        return;
    }
    if (plan == deathrules::DownTick::AloneLoad) {
        HostTravelAloneNext();
        HostDeathFunnelNow();
        return;
    }
    ReviveHost(s);
    watch_until = now + 10.0;
    next_watch = now + 0.5;
}

/// Guest: the host was revived in place; its chr here died with it: removed and built again by
/// its task (as the host does for a guest's lone respawn, 6.9).
/// Diagnostics (always on in a session, 1 line / s while anything changes): every remote player
/// chr's render state - position, distance, +0x208 (draw factor, 0 after a death dissolve),
/// +0x20C/+0x210 (alpha / rate), +0x1E1 flags, HP - to tell a ghost (plate, no body) apart.
void RemoteChrRenderWatch(const GameSnapshot& s, double now) {
    static double next = 0;
    static std::string last;
    if (!g_on.load() || !SessionUp() || !s.world_up || s.loading || now < next) {
        return;
    }
    next = now + 1.0;
    float me[3] = {};
    ChrPos(s.player, me);
    std::string line;
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode != 1 && t.mode != 2) {
            continue;
        }
        const u64 c = PlayerForPeer(t.task + kTaskPeer);
        char b[240];
        float p[3] = {};
        if (!c || !ChrPos(c, p)) {
            std::snprintf(b, sizeof b, " [mode %d no chr]", t.mode);
        } else {
            u64 f = 0;
            SafeRead(c + 0x1e1, &f, 5);
            const float d = std::sqrt((p[0] - me[0]) * (p[0] - me[0]) + (p[2] - me[2]) * (p[2] - me[2]));
            std::snprintf(b, sizeof b, " [mode %d %#llx (%.1f, %.1f, %.1f) %.1f m draw %.2f alpha %.2f/%+.2f f %010llx hp %d]",
                          t.mode, ull(c), double(p[0]), double(p[1]), double(p[2]), double(d),
                          double(Field<float>(c, 0x208)), double(Field<float>(c, 0x20c)), double(Field<float>(c, 0x210)),
                          ull(f), ChrHp(c));
        }
        line += b;
    }
    // Rounded so a standing chr does not log every second.
    std::string key;
    for (char ch : line) {
        key += (ch >= '0' && ch <= '9') ? '#' : ch;
    }
    if (line != last) {
        last = line;
        Log("remote chrs:%s", line.c_str());
    }
}

/// Diagnostics (BB_PARTY_POS_SCAN=1): where the local player's position is kept besides the chr -
/// GSM (*0x5556678, 0x2000 bytes), GameDataMan (*0x553B130, 0x400) and PlayerGameData (GameDataMan
/// +8, 0x2000): every float triple within 1 m of the player's position, every 10 s while it changes.
void PosScanTick(const GameSnapshot& s, double now) {
    static const bool on = std::getenv("BB_PARTY_POS_SCAN") && std::getenv("BB_PARTY_POS_SCAN")[0] == '1';
    static double next = 0;
    static std::string last;
    if (!on || !s.world_up || s.loading || !s.player || now < next) {
        return;
    }
    next = now + 5.0;
    float me[4];
    float rot[4];
    if (!ChrTransform(s.player, me, rot)) {
        return;
    }
    struct Area {
        const char* name;
        u64 base;
        std::size_t n;
    } areas[] = {{"gsm", Slot(kSlotGsm), 0x2000}, {"gdm", Slot(kSlotGameData), 0x400},
                 {"pgd", Field<u64>(Slot(kSlotGameData), 8), 0x2000}};
    std::string out;
    for (const Area& a : areas) {
        std::vector<u8> buf(a.n);
        if (!a.base || !SafeRead(a.base, buf.data(), a.n)) {
            continue;
        }
        const u32 stage = Field<u32>(s.player, 0x3f8) & 0xffff0000u;
        for (std::size_t o = 0; o + 12 <= a.n; o += 4) {
            u32 w;
            std::memcpy(&w, &buf[o], 4);
            if (w == stage) {
                char b[48];
                std::snprintf(b, sizeof b, " %s+%#zx=map", a.name, o);
                out += b;
            }
            float v[3];
            std::memcpy(v, &buf[o], 12);
            if (std::fabs(v[0] - me[0]) < 1.0f && std::fabs(v[1] - me[1]) < 1.0f && std::fabs(v[2] - me[2]) < 1.0f) {
                char b[96];
                std::snprintf(b, sizeof b, " %s+%#zx(%.1f,%.1f,%.1f)", a.name, o, double(v[0]), double(v[1]), double(v[2]));
                out += b;
            }
        }
    }
    const u64 gsm = Slot(kSlotGsm);
    char h[256];
    std::snprintf(h, sizeof h, "pos scan: me (%.1f, %.1f, %.1f) stage %#x own-world %u; gsm+0x14c4 %#x +0x1644 %#x +0x14f0 %#x:",
                  double(me[0]), double(me[1]), double(me[2]), Field<u32>(s.player, 0x3f8), unsigned(Field<u8>(gsm, 0x1592)),
                  Field<u32>(gsm, 0x14c4), Field<u32>(gsm, 0x1644), Field<u32>(gsm, 0x14f0));
    if (out != last) {
        last = out;
        Log("%s%s", h, out.empty() ? " none" : out.c_str());
        std::string d;
        for (u64 o = 0x1490; o < 0x1660; o += 4) {
            const u32 v = Field<u32>(gsm, o);
            char b[40];
            if (v == 0) {
                continue;
            }
            float f;
            std::memcpy(&f, &v, 4);
            if (std::fabs(f) > 0.01f && std::fabs(f) < 100000.0f) {
                std::snprintf(b, sizeof b, " %llx:%.2f", ull(o), double(f));
            } else {
                std::snprintf(b, sizeof b, " %llx:%08x", ull(o), v);
            }
            d += b;
        }
        Log("pos scan: gsm 0x1490..0x1660:%s", d.c_str());
    }
}

/// Guest: its own save took the host-world position (Continue started it beside the host's spot in
/// the host's map, sometimes in a wall; user, hd31/hd32). GSM +0x15B0 (pos) / +0x15C0 (rot) /
/// +0x15E0 (map) and +0x15F0 / +0x1600 follow the chr also in the host's world (BB_PARTY_POS_SCAN);
/// +0x14A0 / +0x14B0 / +0x14C0 hold the pre-summon spot after the join, but a B2 host-world load
/// rewrites it too. So the pre-summon spot is copied once per session (the first steady world as a
/// client after the bell join) and, while the guest is in the host's world and no B2 load runs,
/// written back into all three records and the forced placement. BB_PARTY_GUEST_PIN_SAVE_POS=1: on
/// (default off: hd pin1-pin4, the Continue still started at the B2 arrival spot - the save is
/// taken at the arrival load, before this runs).
void PinOwnSpotTick(const GameSnapshot& s) {
    static const bool on = std::getenv("BB_PARTY_GUEST_PIN_SAVE_POS") && std::getenv("BB_PARTY_GUEST_PIN_SAVE_POS")[0] == '1';
    struct Spot {
        float pos[4], rot[4];
        u32 map;
    };
    static Spot own;
    static bool have = false;
    static int logged = 0;
    if (!on || !g_on.load() || Role() != RoleClient) {
        have = false;
        return;
    }
    if (!s.world_up || s.loading || g_rejoin.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(S().mu);
        if (S().gphase != GuestPhase::Idle) {
            return;
        }
    }
    const u64 gsm = Slot(kSlotGsm);
    if (!gsm || Field<u8>(gsm, 0x1592) != 0 || Field<int>(Slot(kSlotFlow), 0x84, -1) != 4) {
        return;
    }
    if (!have) {
        if (!SafeRead(gsm + 0x14a0, own.pos, 16) || !SafeRead(gsm + 0x14b0, own.rot, 16) ||
            !SafeGet(gsm + 0x14c0, &own.map) || own.map == 0 || own.map == 0xffffffffu) {
            return;
        }
        have = true;
        logged = 0;
        Log("guest: own save spot pinned for this session: map %#x (%.1f, %.1f, %.1f)", own.map, double(own.pos[0]),
            double(own.pos[1]), double(own.pos[2]));
        return;
    }
    const u32 m15e0 = Field<u32>(gsm, 0x15e0);
    float p15b0[4];
    SafeRead(gsm + 0x15b0, p15b0, 16);
    const bool moved = m15e0 != own.map || std::fabs(p15b0[0] - own.pos[0]) > 0.01f || std::fabs(p15b0[2] - own.pos[2]) > 0.01f;
    SafeWrite(gsm + 0x14a0, own.pos, 16);
    SafeWrite(gsm + 0x14b0, own.rot, 16);
    SafeWrite(gsm + 0x14c0, &own.map, 4);
    SafeWrite(gsm + 0x15b0, own.pos, 16);
    SafeWrite(gsm + 0x15c0, own.rot, 16);
    SafeWrite(gsm + 0x15e0, &own.map, 4);
    SafeWrite(gsm + 0x15f0, own.pos, 16);
    SafeWrite(gsm + 0x1600, own.rot, 16);
    // The forced placement (+0x14F0 map, +0x1500 position, +0x1510 rotation; what the B2 loads'
    // exact transform wrote): the Continue spawned exactly there (pin3b: (-193.05, -28.65, 68.15),
    // the spot beside the host). Vanilla's send-home writes the pre-summon spot there.
    SafeWrite(gsm + 0x14f0, &own.map, 4);
    SafeWrite(gsm + 0x1500, own.pos, 16);
    SafeWrite(gsm + 0x1510, own.rot, 16);
    if (moved && logged++ < 3) {
        Log("guest: own save spot kept at map %#x (%.1f, %.1f, %.1f) (the record read map %#x (%.1f, %.1f, %.1f))",
            own.map, double(own.pos[0]), double(own.pos[1]), double(own.pos[2]), m15e0, double(p15b0[0]),
            double(p15b0[1]), double(p15b0[2]));
    }
}

/// Once per own-world load (not summoned): the own bloodstain record (GameDataMan +0x28 flag, the
/// +0x30 record: position +0x18, echoes +0x38, map +0x3C) - where a guest's party-death stain lives.
void OwnStainLogTick(const GameSnapshot& s) {
    static u64 logged_for = 0;
    const u64 gsm = Slot(kSlotGsm);
    if (!g_on.load() || !s.world_up || s.loading || !s.player || logged_for == s.player || Field<u8>(gsm, 0x1592) != 1) {
        return;
    }
    logged_for = s.player;
    const u64 gdm = Slot(kSlotGameData);
    const u64 rec = Field<u64>(gdm, 0x30);
    float p[3] = {}, me[3] = {};
    SafeRead(rec + 0x18, p, 12);
    ChrPos(s.player, me);
    Log("own world: bloodstain flag %u, record %d echoes at (%.1f, %.1f, %.1f) map %#x; me (%.1f, %.1f, %.1f) map %#x, "
        "echoes %d", unsigned(Field<u8>(gdm, 0x28)), Field<int>(rec, 0x38, -1), double(p[0]), double(p[1]), double(p[2]),
        Field<u32>(rec, 0x3c), double(me[0]), double(me[1]), double(me[2]), s.map_id, s.echoes);
}

/// Any role, every frame (playtest 1 bug 13: two host stains were never picked up and nothing
/// told why): the own bloodstain record as it changes - recorded at a death, taken (the echoes
/// came back), or replaced by a later death before it was taken (those echoes are lost).
void OwnStainWatchTick(const GameSnapshot& s) {
    static int flag_was = -1, echoes_was = -1;
    static float pos_was[3] = {};
    if (!g_on.load() || !s.world_up || s.loading || !s.player) {
        return;
    }
    const u64 gdm = Slot(kSlotGameData);
    const u64 rec = Field<u64>(gdm, 0x30);
    if (!gdm || !rec) {
        return;
    }
    const int flag = Field<u8>(gdm, 0x28), echoes = Field<int>(rec, 0x38, -1);
    float p[3] = {};
    SafeRead(rec + 0x18, p, 12);
    if (flag_was < 0) {
        flag_was = flag;
        echoes_was = echoes;
        std::memcpy(pos_was, p, sizeof pos_was);
        return;
    }
    const deathrules::StainChange c = deathrules::PlanStainChange(flag_was, echoes_was, flag, echoes);
    if (c != deathrules::StainChange::None) {
        float me[3] = {};
        ChrPos(s.player, me);
        Log("own bloodstain %s: flag %d -> %d, echoes %d -> %d at (%.1f, %.1f, %.1f) map %#x (was at (%.1f, %.1f, %.1f)); "
            "me (%.1f, %.1f, %.1f) %s, own echoes %d%s",
            deathrules::StainChangeName(c), flag_was, flag, echoes_was, echoes, double(p[0]), double(p[1]), double(p[2]),
            Field<u32>(rec, 0x3c), double(pos_was[0]), double(pos_was[1]), double(pos_was[2]), double(me[0]),
            double(me[1]), double(me[2]), MapName(s.map_id).c_str(), s.echoes,
            c == deathrules::StainChange::Replaced ? ": the old stain's echoes are lost" : "");
    }
    flag_was = flag;
    echoes_was = echoes;
    std::memcpy(pos_was, p, sizeof pos_was);
}

/// Finding 1: the party stain (see PartyStain), every frame.
void GuestStainTick(const GameSnapshot& s) {
    static double test_at = -1;
    static int own_flag_was = -1, own_echoes_was = -1;
    static double own_seen_at = 0;
    if (!g_on.load() || !PartyStainOn() || !s.world_up || s.loading || !s.player) {
        party::ping::SetStain({});
        own_flag_was = -1;
        return;
    }
    const int slot = party::flow::CurrentSaveSlot();
    if (slot < 0) {
        return;
    }
    const u64 gsm = Slot(kSlotGsm), gdm = Slot(kSlotGameData);
    const u64 rec = Field<u64>(gdm, 0x30);
    const bool client = s.session_role == RoleClient;
    const bool own_world = !client && Field<u8>(gsm, 0x1592) == 1;
    const int own_flag = Field<u8>(gdm, 0x28), own_echoes = Field<int>(rec, 0x38, -1);
    std::lock_guard<std::mutex> lk(g_pstain_mu);
    if (g_pstain_slot != slot) {
        LoadStainLocked(slot);
        g_pstain_slot = slot;
    }
    PartyStain& p = g_pstain;
    if (client) {
        // The own stain from before the session counts too (and is kept if a load drops its flag).
        if (!p.s.live && own_flag == 1 && own_echoes > 0) {
            StainFromRecordLocked(slot, "the own stain, now in a party");
        }
        own_flag_was = -1;
    }
    if (own_world) {
        party::ping::SetStain({});
        // The game's own stain shows it here. Taken by the player (flag 1 -> 0, the echoes came
        // back): the party stain goes too. A new own death replaces it (the record changed).
        static double drop_at = -1;
        static int echoes_at_drop = 0;
        if (own_flag_was == 1 && own_flag == 0) {
            drop_at = Now();
            echoes_at_drop = own_echoes_was;
        }
        const bool came_back = drop_at > 0 && s.echoes - echoes_at_drop >= p.s.echoes / 2;
        if (p.s.live && own_flag == 0 && came_back) {
            Log("party stain: taken in the own world (echoes %d -> %d)", echoes_at_drop, s.echoes);
            p = PartyStain{};
            WriteStainLocked(slot);
            drop_at = -1;
        } else if (own_flag == 1 && own_echoes > 0 && p.s.live) {
            StainFromRecordLocked(slot, "own world record");
        } else if (own_flag == 0 && p.s.live && p.have_raw && own_echoes == p.s.echoes && Now() - own_seen_at > 3.0 &&
                   own_flag_was == 0 &&
                   (drop_at < 0 || Now() - drop_at > 5.0)) {
            // A load dropped the own record's flag while the stain was not taken: put it back
            // (the game builds the stain from the record: 0x1868A50(SprjEventMan +0x80 [+8], 1)).
            if (rec && SafeWrite(rec, p.raw, sizeof p.raw)) {
                const u8 one = 1;
                SafeWrite(gdm + 0x28, &one, 1);
                const u64 events = Field<u64>(Slot(0x553b108), 0x80);
                const u64 stains = Field<u64>(events, 8);
                if (stains) {
                    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(Guest(0x1868a50))(stains, 1);
                }
                Log("party stain: the own world's bloodstain flag was dropped by a load; put back (%d Blood Echoes at "
                    "(%.1f, %.1f, %.1f), live stain %s)",
                    p.s.echoes, double(p.s.pos[0]), double(p.s.pos[1]), double(p.s.pos[2]), stains ? "built" : "next load");
            }
        }
        if (own_flag_was < 0) {
            own_seen_at = Now();
        }
        own_flag_was = own_flag;
        own_echoes_was = s.echoes;
        return;
    }
    if (!client || Field<u8>(gsm, 0x1592) != 0 || !gueststain::InMap(p.s, s.map_id)) {
        party::ping::SetStain({});
        return;
    }
    party::ping::StainMark m;
    m.live = true;
    m.map = p.s.map;
    std::memcpy(m.pos, p.s.pos, sizeof m.pos);
    m.echoes = p.s.echoes;
    party::ping::SetStain(m);
    float me[3] = {};
    if (ChrHp(s.player) <= 0 || !ChrPos(s.player, me)) {
        return;
    }
    static double shown_at = -1;
    if (shown_at < 0 || p.made_at > shown_at) {
        shown_at = Now();
        Log("party stain: shown in the host's world, %d Blood Echoes at (%.1f, %.1f, %.1f), %.1f m away", p.s.echoes,
            double(p.s.pos[0]), double(p.s.pos[1]), double(p.s.pos[2]), double(gueststain::Horizontal(me, p.s.pos)));
    }
    // Picked up by walking back onto it: armed once the living guest stood kArmDistance away (a
    // revive at a lamp right beside it does not pay at once; live ngB).
    if (!p.armed && gueststain::Arms(me, p.s.pos)) {
        p.armed = true;
        test_at = Now();
        Log("party stain: armed (%.1f m away): walking onto it recovers the echoes", double(gueststain::Horizontal(me, p.s.pos)));
    }
    // Test aid: BB_PARTY_STAIN_TEST=S: 6 s after the stain is shown the guest's chr is put 4 m from
    // it (towards where it stands; arms it), S s later onto it.
    static const double test_s = std::getenv("BB_PARTY_STAIN_TEST") ? std::atof(std::getenv("BB_PARTY_STAIN_TEST")) : -1.0;
    static double test_away_for = -1;
    if (test_s >= 0 && !p.armed && test_away_for != p.made_at && Now() - shown_at >= 6.0) {
        test_away_for = p.made_at;
        float dx = me[0] - p.s.pos[0], dz = me[2] - p.s.pos[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        dx = d > 0.1f ? dx / d : 0.0f;
        dz = d > 0.1f ? dz / d : 1.0f;
        const float to[3] = {p.s.pos[0] + 4.0f * dx, me[1] + 0.3f, p.s.pos[2] + 4.0f * dz};
        WarpLocal(to, 0.0f, "stain test: 4 m from the party stain");
        return;
    }
    if (p.armed && test_s >= 0 && test_at > 0 && Now() - test_at >= test_s) {
        test_at = -1;
        const float to[3] = {p.s.pos[0], p.s.pos[1] + 0.3f, p.s.pos[2]};
        WarpLocal(to, 0.0f, "stain test: onto the party stain");
        return;
    }
    if (!p.armed || !gueststain::Touching(me, p.s.pos)) {
        return;
    }
    const u64 pgd = Field<u64>(gdm, 8);
    const int before = Field<int>(pgd, 0x94, -1);
    const u64 add = Field<u64>(Field<u64>(s.player, 0), 0x3d0);
    if (!add || before < 0) {
        return;
    }
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, int, int, int)>(add)(s.player, p.s.echoes, 0, 1);
    const int after = Field<int>(pgd, 0x94, -1);
    // The own record is this stain: taken (no second payment at home).
    bool own_cleared = false;
    if (own_echoes == p.s.echoes) {
        // The record's echoes too: a later load re-flags the old record (0x13D2910 with record 0
        // builds the stain again; live ngB paid it twice before this), and a 0-echo record is
        // dropped (EmptyStainTick).
        const u8 zero = 0;
        const int none = 0;
        SafeWrite(gdm + 0x28, &zero, 1);
        SafeWrite(rec + 0x38, &none, 4);
        const u64 stains = Field<u64>(Field<u64>(Slot(0x553b108), 0x80), 8);
        if (stains) {
            reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(Guest(0x1868a50))(stains, 0);
        }
        own_cleared = true;
    }
    Log("party stain: recovered %d Blood Echoes in the host's world at (%.1f, %.1f, %.1f): echoes %d -> %d%s", p.s.echoes,
        double(me[0]), double(me[1]), double(me[2]), before, after, own_cleared ? "; the own record taken too" : "");
    char toast[96];
    std::snprintf(toast, sizeof toast, "Blood Echoes recovered: %d", p.s.echoes);
    party::status::SetLastEvent(toast);
    p = PartyStain{};
    WriteStainLocked(slot);
    party::ping::SetStain({});
    RequestGameSave("guest bloodstain recovered");
}

// ---- Finding 3 (statsaudit): the guest's own NG cycle ----
// While summoned, GameDataMan +0x68 holds the host's cycle (packet 0x19 -> GSM +0x1594, the
// summoned load's step 0x1937570). That step first saves the current +0x68 into GSM +0x159C with
// +0x1598 = 1, and the next own-world load (descriptor 0x1943F10) restores +0x68 from +0x159C
// while +0x1598 is set. Vanilla has one summoned load per session; B2 makes many, and each one
// re-saved "the current +0x68" - already the host's after the first - so the own cycle was lost
// (mixed NG cycles: the guest's own world went to the host's NG). The guard learns the own cycle
// (at home, or from the first summoned load's saved value for a title join), keeps +0x159C = own
// and +0x1598 = 1 while a client (an alone load's own setup excepted), and at the first own world
// after a session puts the own cycle back if the host's is there. BB_PARTY_NG_GUARD=0: off.
std::atomic<bool> g_ng_alone_pending{false};
std::atomic<double> g_ng_alone_at{0};
// Guest (ngfix): the host's NG cycle from its "hostng" event (-1 unknown). The game's own copy (the
// packet-0x19 state GSM +0x1594 with +0x1593) is not there yet at a guest's first load into the
// host's world (live ngfix run1: +0x1593 0 at the Dream arrival), so the host sends it on the link.
std::atomic<int> g_link_host_ng{-1};

void NgGuardTick(const GameSnapshot& s) {
    static const bool on = !EnvOff("BB_PARTY_NG_GUARD");
    static int own = -1, host = -1, steady = 0, logged_host = -1;
    static bool was_client = false, saw_load = false;
    static double learned_at = 0;
    if (!g_on.load()) {
        return;
    }
    const u64 gsm = Slot(kSlotGsm), gdm = Slot(kSlotGameData);
    if (Role() == RoleHost) {
        // The host's cycle to the guests (every 5 s and at a change): they run its world at it.
        static double sent_at = 0;
        static int sent = -1;
        const int live = gdm ? int(Field<u32>(gdm, 0x68, 0xffffffffu)) : -1;
        if (live >= 0 && live <= 7 && (live != sent || Now() - sent_at > 5.0)) {
            if (live != sent) {
                Log("NG guard: the host's NG cycle %d sent to the party", live);
            }
            sent = live;
            sent_at = Now();
            Send(-1, "{\"op\":\"hostng\",\"ng\":" + std::to_string(live) + "}");
        }
    }
    // In the host's world (also during a load, once the MoveMapStep ctor switched to host-world
    // mode and before the map's characters are made) the live cycle is the host's (ngfix: a B2
    // load's descriptor took the own one from +0x159C, so the guest ran the host's world at its own
    // NG - other enemy HP, damage and echoes than on the host's game).
    // A guest in the host's world: host-world mode (GSM +0x1592 == 0, set by the MoveMapStep ctor)
    // with the host's 0x19 state, a client session, or the flow loading into the host's world (3).
    const int role_now = Role();
    const bool host_world = gsm && gdm && role_now != RoleHost && Field<u8>(gsm, 0x1592, 1) == 0 &&
                            (Field<u8>(gsm, 0x1593) == 1 || role_now == RoleClient ||
                             Field<int>(Slot(kSlotFlow), 0x84, -1) == 3);
    if (host_world) {
        const u32 h = Field<u8>(gsm, 0x1593) == 1 ? Field<u32>(gsm, 0x1594, 0xffffffffu) : 0xffffffffu;
        const int link = g_link_host_ng.load();
        if (link >= 0 && link <= 7) {
            host = link;
        } else if (h <= 7) {
            host = int(h);
        }
    }
    if (on && own < 0 && !host_world && role_now != RoleClient && s.world_up && !s.loading && gsm && gdm &&
        Field<u8>(gsm, 0x1592, 0) == 1) {
        // The own world is up: its cycle at once (a fast join may come before ~2 s of steady world).
        const u32 v = Field<u32>(gdm, 0x68, 0xffffffffu);
        if (v <= 7) {
            own = int(v);
            learned_at = Now();
            Log("NG guard: own NG cycle %d (own world)", own);
        }
    }
    if (on && host_world && own < 0 && !g_ng_alone_pending.load() && (s.loading || s.world_up)) {
        // The own cycle before anything put the host's in: the restore value a summoned load
        // saved (+0x1598 set), else the live one (no host-world load has changed it yet).
        const bool saved = Field<u8>(gsm, 0x1598) == 1;
        const u32 v = saved ? Field<u32>(gsm, 0x159c, 0xffffffffu) : Field<u32>(gdm, 0x68, 0xffffffffu);
        if (v <= 7) {
            own = int(v);
            learned_at = Now();
            Log("NG guard: own NG cycle %d (%s, %s)", own, saved ? "the summoned load's saved value GSM +0x159C" : "live, before the host's",
                s.loading ? "during the load" : "in the host's world");
        }
    }
    if (on && host_world) {
        ngguard::View w;
        w.host_world = true;
        w.alone_pending = g_ng_alone_pending.load();
        w.own = own;
        w.host = host;
        w.live = int(Field<u32>(gdm, 0x68, 0xffffffffu));
        w.restore_flag = Field<u8>(gsm, 0x1598);
        w.restore_value = int(Field<u32>(gsm, 0x159c, 0xffffffffu));
        const u8 one = 1;
        const u32 own_u = u32(own);
        if (ngguard::RestoreWrong(w)) {
            SafeWrite(gsm + 0x159c, &own_u, 4);
            SafeWrite(gsm + 0x1598, &one, 1);
            Log("NG guard: the own NG cycle kept for the own save (%s): GSM +0x1598 %d -> 1, +0x159C %d -> %d",
                s.loading ? "during a load" : "in the host's world", w.restore_flag, w.restore_value, own);
        }
        const int target = ngguard::LiveNg(w);
        if (target >= 0) {
            const u32 t = u32(target);
            SafeWrite(gdm + 0x68, &t, 4);
            Log("NG guard: the host's NG cycle %d is live in the host's world (was %d, the own %d stays in the save; %s)",
                target, w.live, own, s.loading ? "during the load" : "steady world");
        }
    }
    if (s.loading || !s.world_up || !gsm || !gdm) {
        steady = 0;
        if (g_ng_alone_pending.load() && s.loading) {
            saw_load = true;
        }
        return;
    }
    if (g_ng_alone_pending.load() && (saw_load || Now() - g_ng_alone_at.load() > 40.0)) {
        g_ng_alone_pending = false;
        saw_load = false;
    }
    if (++steady < 60) {  // ~2 s of steady world
        return;
    }
    const bool client = s.session_role == RoleClient;
    {
        // Test aid (fixtures only): BB_PARTY_TEST_NG=N writes the own world's NG cycle once and
        // saves (a mixed-NG test-save pair without playing an ending).
        static bool test_done = false;
        static const char* test_ng = std::getenv("BB_PARTY_TEST_NG");
        if (test_ng && *test_ng && !test_done && !client && Field<u8>(gsm, 0x1592) == 1) {
            test_done = true;
            const u32 n = u32(std::atoi(test_ng)) > 7 ? 7 : u32(std::atoi(test_ng));
            const u32 was = Field<u32>(gdm, 0x68);
            SafeWrite(gdm + 0x68, &n, 4);
            Log("test: own NG cycle %u -> %u (BB_PARTY_TEST_NG), save requested", was, n);
            RequestGameSave("test NG cycle");
        }
    }
    if (!on) {
        return;
    }
    ngguard::View v;
    v.steady = true;
    v.client = client;
    v.own_world = !client && Field<u8>(gsm, 0x1592) == 1;
    v.live = int(Field<u32>(gdm, 0x68, 0xffffffffu));
    v.restore_flag = Field<u8>(gsm, 0x1598);
    v.restore_value = int(Field<u32>(gsm, 0x159c, 0xffffffffu));
    v.own = own;
    v.host = host;
    v.after_session = was_client;
    v.ending_since = StoryLastEndingAt() > learned_at;
    v.alone_pending = g_ng_alone_pending.load();
    if (client && !host_world && v.live >= 0 && v.live <= 7) {
        // Not in host-world mode (an alone load, a join starting from the own world): the host's
        // cycle from its link event, else the live value.
        const int link = g_link_host_ng.load();
        host = link >= 0 && link <= 7 ? link : v.live;
    }
    if (client && host >= 0) {
        if (host != logged_host && own >= 0) {
            logged_host = host;
            Log("NG guard: summoned into NG cycle %d (own %d%s)", host, own,
                host != own ? ": mixed NG cycles, the own save keeps its own" : "");
        }
    }
    const ngguard::Act act = ngguard::Plan(v);
    const u8 one = 1;
    const u32 own_u = u32(own);
    switch (act) {
    case ngguard::Act::None:
        break;
    case ngguard::Act::Learn:
        own = client ? v.restore_value : v.live;
        learned_at = Now();
        Log("NG guard: own NG cycle %d (%s)", own,
            client ? "the first summoned load's saved value GSM +0x159C" : "own world");
        break;
    case ngguard::Act::KeepRestore:
        SafeWrite(gsm + 0x159c, &own_u, 4);
        SafeWrite(gsm + 0x1598, &one, 1);
        Log("NG guard: kept the own NG cycle for the own world: GSM +0x1598 %d -> 1, +0x159C %d -> %d (live %d = the "
            "host's)",
            v.restore_flag, v.restore_value, own, v.live);
        break;
    case ngguard::Act::RestoreHome:
        SafeWrite(gdm + 0x68, &own_u, 4);
        Log("NG guard: own world after the session had the host's NG cycle %d: put back to the own %d", v.live, own);
        RequestGameSave("own NG cycle put back");
        break;
    case ngguard::Act::AcceptChange:
        Log("NG guard: own NG cycle %d -> %d (%s)", own, v.live,
            v.ending_since ? "an ending" : (was_client ? "not the host's value" : "own world"));
        own = v.live;
        learned_at = Now();
        break;
    }
    if (client) {
        was_client = true;
    } else if (v.own_world) {
        was_client = false;
    }
}

// Host: a guest's rest (playtest 1 bug 3): the slot + 1 that asked (0 none), its lamp, and when.
std::atomic<int> g_rest_req_slot{0};
std::atomic<u32> g_rest_req_lamp{0};
std::atomic<double> g_rest_req_at{0};
// Host (pt1polish): the queued rest came with the guest's re-join into this area ("rejoin" + "rest"):
// that re-join waits for the rest load (deathrules::RestCarryHeld); 0 = none / the load started.
std::atomic<bool> g_rest_req_carry{false};
std::atomic<double> g_rest_carry_at{0};
std::atomic<double> g_last_down_at{0};  // host: the last member_down (test aid BB_PARTY_TEST_DIE_ON_DOWN)
double g_rest_load_last = -1e9;  // main thread

/// Host, main thread: a guest's rest request runs as a rest load where the host stands
/// (HostTravelTransformNow: the vanilla loading screen, the world resets, the guests in this world
/// follow). Waits up to 30 s for the host to be ready; dropped with a log line otherwise.
void HostRestLoadTick(const GameSnapshot& s, double now) {
    const int req = g_rest_req_slot.load();
    if (req <= 0) {
        return;
    }
    if (!g_on.load() || Role() != RoleHost) {
        g_rest_req_slot = 0;
        g_rest_carry_at = 0;
        return;
    }
    deathrules::RestView v;
    v.world_up = s.world_up;
    v.loading = s.loading;
    v.alive = s.player && ChrHp(s.player) > 0;
    v.boss_fight = BossFightActive();
    v.in_dream = (s.map_id >> 24) == 21;
    v.travel_pending = g_host_down.load() || Field<u8>(Slot(kSlotGsm), 8) != 0 || S().hphase != HostPhase::Idle;
    v.since_last = now - g_rest_load_last;
    const deathrules::RestLoad r = deathrules::PlanRestLoad(v);
    const double waited = now - g_rest_req_at.load();
    if (r == deathrules::RestLoad::NotReady || r == deathrules::RestLoad::Busy) {
        if (waited > 30.0) {
            g_rest_req_slot = 0;
            g_rest_carry_at = 0;
            Log("host: slot %d's rest at lamp %u: no rest load (%s for 30 s)", req - 1, g_rest_req_lamp.load(),
                deathrules::RestLoadName(r));
        }
        return;
    }
    g_rest_req_slot = 0;
    const bool carried = g_rest_req_carry.exchange(false);
    // C7 (ERSC: resting removes the others from the boss room): during a boss fight the rest load
    // goes to the host's last lamp, so the fight resets and everyone in this world is taken out of
    // the boss room (BB_PARTY_REST_BOSS_KICK=0: the old "no rest load in a boss fight").
    static const bool boss_kick = !(std::getenv("BB_PARTY_REST_BOSS_KICK") && std::getenv("BB_PARTY_REST_BOSS_KICK")[0] == '0');
    const bool out_of_boss = r == deathrules::RestLoad::BossFight && boss_kick;
    if (r != deathrules::RestLoad::Run && !out_of_boss) {
        g_rest_carry_at = 0;  // its re-join is served now
        Log("host: slot %d rested at lamp %u: no rest load here (%s)", req - 1, g_rest_req_lamp.load(),
            deathrules::RestLoadName(r));
        return;
    }
    float pos[4] = {}, rot[4] = {};
    if (!ChrTransform(s.player, pos, rot)) {
        Log("host: slot %d rested: the host's position cannot be read; no rest load", req - 1);
        g_rest_carry_at = 0;
        return;
    }
    u32 dest_map = s.map_id;
    if (out_of_boss) {
        const u32 lamp = Field<u32>(Slot(kSlotGsm), 0x1528);
        float rs[4];
        if (lamp == 0 || lamp == kTravelNone || !ids::ReturnSpot(lamp, rs)) {
            g_rest_carry_at = 0;
            Log("host: slot %d rested at lamp %u during a boss fight: the host's last lamp %u has no known spot; no "
                "rest load", req - 1, g_rest_req_lamp.load(), lamp);
            return;
        }
        if ((ids::EntityMap(lamp) >> 16) != (s.map_id >> 16)) {
            dest_map = ids::EntityMap(lamp);  // its last lamp is in another map: the party goes there
        }
        std::string boss;
        BossFightActive(&boss);
        pos[0] = rs[0], pos[1] = rs[1], pos[2] = rs[2], pos[3] = 1.0f;
        rot[0] = 0, rot[1] = rs[3] * 3.14159265f / 180.0f, rot[2] = 0, rot[3] = 0;
        Log("host: slot %d rested at lamp %u during a boss fight (%s): the party is taken out of the boss room - a "
            "rest load to the host's last lamp %u in %s (%.1f, %.1f, %.1f); the fight resets",
            req - 1, g_rest_req_lamp.load(), boss.c_str(), lamp, MapName(dest_map).c_str(), double(pos[0]),
            double(pos[1]), double(pos[2]));
    }
    g_rest_load_last = now;
    // Who follows this load: the resting guest went alone (batch 6: the host waited 60 s for its
    // arrival); members in another area stay. Nobody here: an alone load (no re-sync, no wait).
    std::vector<deathrules::RestMember> ms;
    for (const Where& w : g_where) {
        if (w.slot == g_local_slot.load()) {
            continue;
        }
        deathrules::RestMember m;
        m.slot = w.slot;
        m.connected = w.connected;
        m.area = w.age_ms < 5000 ? AreaOf(w.map) : 0u;
        ms.push_back(m);
    }
    TravelIntent rest_t;
    const u32 rest_area = LampIntent(g_rest_req_lamp.load(), &rest_t) ? AreaOf(DestMapOf(rest_t)) : 0u;
    const int expected = deathrules::RestLoadExpected(ms.data(), static_cast<int>(ms.size()), req - 1,
                                                      AreaOf(s.map_id), rest_area, carried);
    Log("host: slot %d rested at lamp %u and travels alone: a rest load for this world %s "
        "(%.1f, %.1f, %.1f) in %s (loading screen; the enemies come back); %d member(s) here follow%s",
        req - 1, g_rest_req_lamp.load(), out_of_boss ? "to the host's last lamp" : "where the host stands",
        double(pos[0]), double(pos[1]), double(pos[2]), MapName(dest_map).c_str(),
        expected, expected == 0 ? " (an alone load: no arrival to wait for)" : "");
    if (carried) {
        Log("host: slot %d comes back into this area by its own re-join: served after this load (one loading screen "
            "for it, into the reset world)", req - 1);
    }
    if (expected == 0) {
        g_host_alone_next = true;
        g_host_alone_at = now;
    }
    if (!HostTravelTransformNow(dest_map, pos, rot, out_of_boss ? "a guest's rest in a boss fight" : "a guest's rest")) {
        g_host_alone_next = false;
        g_rest_carry_at = 0;
        Log("host: the rest load could not start (travel off or a stage request pending)");
    }
}

// ---- Party banners and an empty bloodstain (pt1polish, playtest 1 verification replay) ----
// The SummonMessage banner (ShowSummonMessage 0x1BFFE80(EventText id, 0), docs/party/join_timing.md):
// after a wipe load the host saw "The bell's resonance has beckoned Hunter1" (102001) again, and a
// guest whose death B2 holds saw "You died. Returning to your world." (103100..103500) while it
// stays. deathrules::HidePartyBanner decides; BB_PARTY_BANNERS=0: every banner as the game shows it.
constexpr u64 kShowSummonMessage = 0x1bffe80;
void InstallCall(u64 off, u64 target, const void* handler, const char* name);
using ShowMsgFn = void(BB_COOP_SYSV*)(u32, u32);
void* g_show_msg_orig = nullptr;
bool BannersOn() {
    static const bool on = !(std::getenv("BB_PARTY_BANNERS") && std::getenv("BB_PARTY_BANNERS")[0] == '0');
    return on;
}
bool HideBanner(u32 id) {
    if (!BannersOn()) {
        return false;
    }
    const double at = g_rejoin_banner_at.load();
    const double since = at > 0 ? Now() - at : -1.0;
    if (!deathrules::HidePartyBanner(int(id), g_on.load(), Role() == RoleClient, since)) {
        return false;
    }
    Log("banner %u hidden (%s)", id,
        deathrules::IsReturnHomeBanner(int(id)) ? "the guest stays with the party" : "a re-join, not a new summon");
    return true;
}
BB_COOP_SYSV void ShowMsgEntry(u32 id, u32 arg) {
    if (HideBanner(id)) {
        return;
    }
    reinterpret_cast<ShowMsgFn>(g_show_msg_orig)(id, arg);
}
BB_COOP_SYSV void ShowMsgCall(u32 id, u32 arg) {
    if (HideBanner(id)) {
        return;
    }
    reinterpret_cast<ShowMsgFn>(Guest(kShowSummonMessage))(id, arg);
}
/// The join banner ("The bell's resonance has beckoned <?joinName?>." 102001, "Entered the world
/// of ..." 102100 / 102200) does not go through ShowSummonMessage: the join handler (fragment
/// 0x196BAE9) builds the text itself (id from the per-type table 0x553D784 + type * 0x80) and sets
/// the pending-banner byte 0x55813D8 = 1, then calls 0x196F360(member). pt1last live: the host's
/// "beckoned" after a wipe re-join was shown with no hook hit. That call is replaced: after it,
/// a hidden id clears the pending byte (the banner consumer 0x1BFF910 shows it only while it is 1).
constexpr u64 kJoinBannerTail = 0x196be0d;
BB_COOP_SYSV void JoinBannerTail(u64 member) {
    reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(Guest(0x196f360))(member);
    const int type = member ? int(Field<signed char>(member, 0xfc, -1)) : -1;
    const u32 id = type >= 0 && type < 0x22 ? Field<u32>(Guest(0x553d784) + u64(type) * 0x80, 0) : 0;
    if (id && HideBanner(id)) {
        const u8 zero = 0;
        SafeWrite(Guest(0x55813d8), &zero, 1);
    } else if (g_on.load()) {
        Log("join banner %u shown (member type %d)", id, type);
    }
}
void InstallBannerFilter() {
    if (!BannersOn()) {
        Log("party banners: as the game shows them (BB_PARTY_BANNERS=0)");
        return;
    }
    InstallCall(kJoinBannerTail, 0x196f360, reinterpret_cast<const void*>(&JoinBannerTail), "party join banner filter");
    if (ReplacePrologue(kShowSummonMessage, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                        reinterpret_cast<const void*>(&ShowMsgEntry), &g_show_msg_orig,
                        "party banner filter (0x1BFFE80)")) {
        return;
    }
    g_show_msg_orig = nullptr;
    // Its prologue differs: the callers that show join / death banners (calls.tsv).
    static const u64 sites[] = {0x1386a68, 0x1386c60, 0x138a20b, 0x145a63d, 0x196b4c9, 0x196b9f4, 0x138ccfb,
                                0x138ce30, 0x138d060, 0x138d1d0, 0x138d340, 0x138d3d7, 0x138b296};
    for (u64 site : sites) {
        InstallCall(site, kShowSummonMessage, reinterpret_cast<const void*>(&ShowMsgCall), "party banner filter (call)");
    }
}
/// Every frame: when a re-join runs in a session that started more than 20 s ago (not the first
/// summon's own load), the join banners of the next 15 s are a re-join's.
void BannerRejoinTick() {
    static double session_at = -1;
    const int role = Role();
    const bool in = g_on.load() && (role == RoleHost || role == RoleClient);
    const double now = Now();
    if (!in) {
        session_at = -1;
        return;
    }
    if (session_at < 0) {
        session_at = now;
    }
    if (g_rejoin.load() && now - session_at > 20.0) {
        g_rejoin_banner_at = now;
    }
}
/// The death's stain record 0x13D2910(SprjEventMan +0x80, chr, 1) (SoloPlayDeath_2 0x1381F5E and
/// the other death paths) moves PlayerGameData +0x94 (+ SprjEventMan[+8] +0x28) into the record,
/// sets GameDataMan +0x28 and builds the live stain at once through 0x1868A50(events[+8], echoes
/// >= 0): a death with 0 echoes left an "Acquire Blood Echoes" stain for nothing (live pt1polish
/// r2: clearing the flag afterwards was too late, the stain stood after the respawn load). Now,
/// in a party, after such a record (the rest of the death runs as the game does) the flag is
/// cleared and 0x1868A50(.., 0) removes the live stain (it destroys the object first, builds one
/// only for flag && show); the old stain is gone as in vanilla (deathrules::SkipEmptyStainRecord).
/// pt1last (live): the same function called with record 0 (a respawn load) skips the record but
/// sets the flag and builds the stain from the old 0-echo record again, so every call is checked.
/// BB_PARTY_EMPTY_STAIN=1: as the game does.
bool EmptyStainKept() {
    static const bool keep = deathrules::OptIn(std::getenv("BB_PARTY_EMPTY_STAIN"));
    return keep;
}
using StainRecordFn = void(BB_COOP_SYSV*)(u64, u64, u32);
BB_COOP_SYSV void StainRecordHook(u64 events, u64 chr, u32 on) {
    reinterpret_cast<StainRecordFn>(Guest(0x13d2910))(events, chr, on);
    const u64 gdm = Slot(kSlotGameData);
    const u64 rec = gdm ? Field<u64>(gdm, 0x30) : 0;
    const int flag = gdm ? int(Field<u8>(gdm, 0x28)) : 0;
    const int echoes = rec ? Field<int>(rec, 0x38, -1) : -1;
    if (EmptyStainKept() ||
        !deathrules::SkipEmptyStainRecord(g_on.load() && (Role() == RoleHost || Role() == RoleClient), flag, echoes)) {
        if (g_on.load()) {
            Log("bloodstain record (record %u): flag %d, %d echoes: as the game does", on & 0xff, flag, echoes);
        }
        return;
    }
    const u8 zero = 0;
    const u64 stains = Field<u64>(events, 8);
    SafeWrite(gdm + 0x28, &zero, 1);
    if (stains) {
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(Guest(0x1868a50))(stains, 0);
    }
    Log("bloodstain record (record %u) with 0 Blood Echoes: its bloodstain removed (flag 0%s; no \"Acquire Blood "
        "Echoes\" for nothing)",
        on & 0xff, stains ? ", live stain destroyed" : "");
}
void InstallEmptyStainSkip() {
    if (EmptyStainKept()) {
        return;
    }
    for (u64 site : {0x1381f5eull, 0x13825deull, 0x133090cull, 0x1334250ull}) {
        InstallCall(site, 0x13d2910, reinterpret_cast<const void*>(&StainRecordHook), "party empty bloodstain skip");
    }
}
/// Any role, every frame: a 0-echo stain record left over (a path not hooked above) is not placed
/// again by the next load (deathrules::ClearEmptyStain).
void EmptyStainTick() {
    const bool keep = EmptyStainKept();
    const u64 gdm = Slot(kSlotGameData);
    const u64 rec = gdm ? Field<u64>(gdm, 0x30) : 0;
    if (keep || !gdm || !rec) {
        return;
    }
    const int flag = Field<u8>(gdm, 0x28), echoes = Field<int>(rec, 0x38, -1);
    if (!deathrules::ClearEmptyStain(g_on.load() && (Role() == RoleHost || Role() == RoleClient), flag, echoes)) {
        return;
    }
    const u8 zero = 0;
    SafeWrite(gdm + 0x28, &zero, 1);
    Log("own bloodstain with 0 echoes: not placed (flag %d -> 0; no \"Acquire Blood Echoes\" for nothing)", flag);
}

/// Guest in the host's world: its own chr at HP 0 tells the host at once ("member_down"), so a
/// host death a moment later counts it (playtest 1 bug 18: the game's death state reports the
/// death seconds later, after the host had decided "0 down" and revived alone).
/// A guest's own body back to a white phantom (PlayerIns +0x78 = 1) after a B2 host-world load or
/// a title join left it an own-world body (0): its death then takes PartyGhostDeath (the B2 funnel:
/// held death, the party's respawn load) instead of SoloPlayDeath (leave the session). Verification
/// replay of playtest 1 (party_death_rules.h GuestChrTypeFixDue). BB_PARTY_GUEST_CHR_TYPE=0: off.
void GuestChrTypeTick(const GameSnapshot& s) {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_GUEST_CHR_TYPE");
        return !(v && v[0] == '0');
    }();
    static double since = -1;
    const bool client = on && g_on.load() && Role() == RoleClient && s.world_up && !s.loading && s.player &&
                        !g_rejoin.load();
    const int type = client ? Field<int>(s.player, 0x78, -1) : -1;
    if (!client || type != 0) {
        since = -1;
        return;
    }
    const double now = Now();
    if (since < 0) {
        since = now;
    }
    const int hp = ChrHp(s.player);
    if (!deathrules::GuestChrTypeFixDue(true, type, hp, Field<int>(Slot(kSlotFlow), 0x84, -1), now - since)) {
        return;
    }
    const int one = 1;
    SafeWrite(s.player + 0x78, &one, sizeof one);
    Log("guest: own chr type 0 -> %d%s, %.2f s in the world (a white phantom again after a B2 load / title join: "
        "its death takes the party path, not a session leave)", Field<int>(s.player, 0x78, -1),
        hp == 0 ? " at its death (HP 0)" : "", now - since);
    since = -1;
}

void GuestDownNoticeTick(const GameSnapshot& s) {
    static bool sent = false;
    if (!g_on.load() || Role() != RoleClient || !s.world_up || s.loading || !s.player) {
        sent = false;
        return;
    }
    const int hp = ChrHp(s.player);
    if (hp > 0 || hp < 0) {
        sent = false;
        return;
    }
    if (!sent && Field<int>(Slot(kSlotFlow), 0x84, -1) == 4) {
        sent = true;
        Send(0, "{\"op\":\"member_down\"}");
        Log("guest: own chr at HP 0: told the host at once (member_down)");
    }
}

extern "C" std::uint32_t runtime_pad_cross_presses(void); // src/runtime_pad.c

// ---- C5: spectate in a boss-fight limbo (BB_PARTY_SPECTATE, default on) ----
// The dead player's camera follows its own chr: that chr (dissolved, draw 0) is carried along
// 3 m behind a living party member with the chr warp, so the camera shows the fight. Peers remove
// the dead copy meanwhile ("spectating"); the respawn (revive, a wipe load) builds it again.
std::atomic<bool> g_spectating{false};
bool SpectateOn() {
    static const bool on = !(std::getenv("BB_PARTY_SPECTATE") && std::getenv("BB_PARTY_SPECTATE")[0] == '0');
    return on;
}
void SpectateBegin(const char* who) {
    if (!SpectateOn() || g_spectating.exchange(true)) {
        return;
    }
    float p[3] = {0, 0, 0};
    ChrPos(Field<u64>(Slot(kSlotWorldChrMan), 0x60), p);
    char b[128];
    std::snprintf(b, sizeof b, "{\"op\":\"spectating\",\"pos\":[%.3f,%.3f,%.3f]}", p[0], p[1], p[2]);
    Send(-1, b);
    Log("%s: spectating a living party member until the respawn", who);
}
void SpectateEnd() {
    if (g_spectating.exchange(false)) {
        Log("spectating ended");
    }
}
void SpectateTick(const GameSnapshot& s, double now) {
    static double next = 0;
    static int target = -1;            // the followed member's task slot
    static u32 cross_seen = 0;
    if (!g_spectating.load()) {
        target = -1;
        return;
    }
    if (!SessionUp() || s.loading || !s.world_up || !s.player || ChrHp(s.player) > 0) {
        SpectateEnd();  // a load or a revive took over
        target = -1;
        return;
    }
    // Cross (does nothing while dead) switches to the next living member (C5 member switch).
    const u32 cross = runtime_pad_cross_presses();
    const bool advance = target >= 0 && cross != cross_seen;
    cross_seen = cross;
    if (now < next && !advance) {
        return;
    }
    next = now + 0.2;
    int living[8];
    u64 chrs[8];
    int n = 0;
    for (const TaskInfo& t : Tasks(false)) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        if (c && ChrHp(c) > 0 && n < 8) {
            living[n] = t.slot;
            chrs[n++] = c;
        }
    }
    const int pick = SpectateTarget(living, n, target, advance);
    if (pick != target || advance) {
        // C5: whose view this is, on screen (the party notice line): the member's name from the
        // HUD's chr -> member match, and the Cross hint when there is another one to switch to.
        std::string name;
        for (int i = 0; i < n; ++i) {
            if (living[i] == pick) {
                name = PartyHudChrName(chrs[i]);
            }
        }
        const std::string notice = deathrules::SpectateNotice(name, n);
        if (!notice.empty()) {
            party::status::SetLastEvent(notice);
        }
        Log("spectating: following slot %d (%d living member%s%s)%s%s", pick, n, n == 1 ? "" : "s",
            advance ? ", Cross: next member" : "", notice.empty() ? "" : "; notice: ", notice.c_str());
    }
    const bool new_target = pick != target || advance;
    target = pick;
    for (int i = 0; i < n; ++i) {
        alignas(16) float pos[4], rot[4];
        if (living[i] != pick || !ChrTransform(chrs[i], pos, rot)) {
            continue;
        }
        const float yaw = rot[1];
        pos[0] -= std::sin(yaw) * 3.0f;
        pos[2] -= std::cos(yaw) * 3.0f;
        pos[1] += 0.5f;
        pos[3] = 1.0f;
        if (const u64 fn = Field<u64>(Field<u64>(s.player, 0), 0x5f8)) {
            reinterpret_cast<void(BB_COOP_SYSV*)(u64, const float*, const float*, u32)>(fn)(s.player, pos, rot, 1);
        }
        // The camera keeps its own direction when the dead chr is carried (parx2 smoke2: the host's
        // view showed a wall for 106 s, the two living members 3-5 m away off screen): the Lua
        // CamReset (as WarpLocal) puts it behind the carried chr, which faces the way the followed
        // member faces - at a new target and when the member has turned by more than ~35 degrees.
        static float reset_yaw = 0.0f;
        float turn = std::fabs(yaw - reset_yaw);
        if (turn > 3.14159265f) turn = 6.2831853f - std::fmod(turn, 6.2831853f);
        if (new_target || turn > 0.6f) {
            reset_yaw = yaw;
            using CamReset = u64(BB_COOP_SYSV*)(u64, u32, u32);
            reinterpret_cast<CamReset>(Guest(0x132f5a0))(0, 10000, 1);
            if (new_target) Log("spectating: camera put behind the followed member (yaw %.2f)", double(yaw));
        }
        return;
    }
}

/// Guest, after a bell join's arrival in the host's world: once the host's chr stands in this
/// world, a guest more than 4 m away (the summon placed it at its sign spot: an own-world gather
/// that did not reach the host's lamp put it ~150 m off, join4) is put beside the host with the
/// chr warp, as a B2 arrival is. BB_PARTY_JOIN_BESIDE=0: off.
// Guest: the host's last A11 hold (dsummon::Busy as int; DirectSummonEvent).
std::atomic<int> g_host_busy{0};

void JoinBesideTick(const GameSnapshot& s, double now) {
    static const bool on = !(std::getenv("BB_PARTY_JOIN_BESIDE") && std::getenv("BB_PARTY_JOIN_BESIDE")[0] == '0');
    static int prev_role = -1;
    static double pending_since = 0;
    const int role = Role();
    if (role == RoleClient && prev_role != RoleClient) {
        pending_since = now;
    }
    prev_role = role;
    if (!on || pending_since <= 0 || role != RoleClient) {
        return;
    }
    if (now - pending_since > 60.0) {
        pending_since = 0;
        return;
    }
    if (!s.world_up || s.loading || !s.player || g_rejoin.load() || Field<int>(Slot(kSlotFlow), 0x84, -1) != 4) {
        return;
    }
    for (const TaskInfo& t : Tasks(false)) {
        const u64 host = t.mode == 2 ? PlayerForPeer(t.task + kTaskPeer) : 0;
        alignas(16) float hp[4], hr[4], me[4], mr[4];
        if (!host || !ChrTransform(host, hp, hr) || !ChrTransform(s.player, me, mr)) {
            continue;
        }
        if (dsummon::JoinBesideWaits(static_cast<dsummon::Busy>(g_host_busy.load()), hp)) {
            // A11 (pard S4): during the host's cutscene its chr reads at the map origin; the warp
            // waits (and its 60 s window restarts) until the scene is over.
            static double logged = 0;
            if (now - logged > 10.0) {
                logged = now;
                Log("join: arrived; the host is busy (%s) or its chr reads (%.1f, %.1f, %.1f): beside-the-host warp "
                    "waits", dsummon::BusyWord(static_cast<dsummon::Busy>(g_host_busy.load())), double(hp[0]),
                    double(hp[1]), double(hp[2]));
            }
            pending_since = now;
            return;
        }
        {
            // A11 (live a11c): the host's chr not placed yet (its first sync waits behind a
            // cutscene): it stands at the origin here; a warp beside it fell into the void.
            const Where* w = WhereOf(0);
            const bool vit = w && w->has_pos && w->age_ms < 2000;
            // Not in the host's map (live a11e: a summon load that built the guest's own Dream as
            // the host's world): the host's coordinates mean nothing here; the A5 arrival load
            // brings it over (CarryArrivalTick).
            const bool other_map = vit && w->map != 0 && s.map_id != 0xffffffffu && (w->map >> 16) != (s.map_id >> 16);
            if (other_map || !deathrules::HostChrPlaced(hp, vit, vit ? w->pos : hp)) {
                static double said = -100;
                if (now - said > 10.0) {
                    said = now;
                    Log("join: %s (%.1f, %.1f, %.1f%s); no move beside it yet",
                        other_map ? "this guest is not in the host's map" : "the host's chr is not placed yet here",
                        double(hp[0]), double(hp[1]), double(hp[2]), vit ? ", not where the host reports itself" : "");
                }
                return;
            }
        }
        pending_since = 0;
        const float dx = hp[0] - me[0], dz = hp[2] - me[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        static const float min_d = [] {
            const char* v = std::getenv("BB_PARTY_JOIN_BESIDE_MIN");  // test aid: the distance kept (m)
            return v && v[0] ? float(std::atof(v)) : 4.0f;
        }();
        if (d <= min_d && std::fabs(hp[1] - me[1]) < 3.0f) {
            Log("join: arrived %.1f m from the host; left there", double(d));
            return;
        }
        float pos[4];
        std::memcpy(pos, hp, sizeof pos);
        BesideHost(pos, hr, g_local_slot.load());
        pos[3] = 1.0f;
        WarpLocal(pos, hr[1], "join arrival: beside the host");
        Log("join: arrived %.1f m from the host (%.1f, %.1f, %.1f); put beside it (%.1f, %.1f, %.1f)", double(d),
            double(me[0]), double(me[1]), double(me[2]), double(pos[0]), double(pos[1]), double(pos[2]));
        return;
    }
}

/// A5 (one-load cross-map join, BB_PARTY_GATHER_DSUM, default on): the direct summon of a guest
/// in another map than the host's joins the host's room without loading the host's map (live a5,
/// 2026-10-13: a client still in its own m21_00). Set when that join is reported; the arrival
/// tick below then runs the B2 re-join into the host's map (TravelAlone, AloneWhy::Join): the one
/// loading screen of the join, instead of the gather's lamp load plus the summon's load.
std::atomic<double> g_carry_join_at{-1};
std::atomic<double> g_carry_gd_at{-1};  // A5: the arrival started; its GameData follows in the host's map

/// A5 (live a5c / a11h: the host counted 0 cooperators and its watchdog ended the session 60 s
/// later): a guest that joined from its own world never got the summoned placement, so its own
/// GameData kept the own-world values (+0x5D8 0: chr type 1/0, not a cooperator). Once it stands
/// in the host's map after the arrival: the summoned guest's values, as the title join writes them,
/// and its player record again so the host counts it.
void CarryGameDataTick(const GameSnapshot& s, double now) {
    const double at = g_carry_gd_at.load();
    if (at < 0) {
        return;
    }
    if (now - at > 60.0 || Role() != RoleClient) {
        g_carry_gd_at = -1;
        return;
    }
    const u32 host = HostMap();
    if (!s.world_up || s.loading || !s.player || Field<int>(Slot(kSlotFlow), 0x84, -1) != 4 || host == 0 ||
        s.map_id == 0xffffffffu || (host >> 16) != (s.map_id >> 16)) {
        return;
    }
    g_carry_gd_at = -1;
    if (const u64 gd = Field<u64>(Slot(kSlotGameData), 8)) {
        const int was = Field<int>(gd, 0x5d8, -1);
        const int two = 2, one = 1, seven = 7;
        SafeWrite(gd + 0x10, &two, 4);
        SafeWrite(gd + 0xa4, &one, 4);
        SafeWrite(gd + 0x5d8, &seven, 4);
        RebroadcastOwnRecord("A5 arrival: a summoned guest's session type");
        Log("A5: arrived in the host's map: own GameData as a summoned guest's (+0x10 2, +0xA4 1, +0x5D8 %d -> 7)", was);
    }
}

/// The lamp of the host's map nearest to where the host stands (its vitals position); without a
/// position the host's last lamp when it lies in that map (GatherLampFor). 0: none.
u32 LampNearHost(u32 host_map) {
    u32 lamp = GatherLampFor(GuestDeathRedirectLamp(), host_map);
    const Where* w = WhereOf(0);
    if (w && w->has_pos) {
        float best = 1e18f;
        for (const std::uint32_t id : ids::kPartyReturnPoints) {
            float sp[4];
            if (ids::EntityMap(id) >> 16 != host_map >> 16 || id / 100000 == 29 || !ids::ReturnSpot(id, sp)) {
                continue;
            }
            const float dx = sp[0] - w->pos[0], dy = sp[1] - w->pos[1], dz = sp[2] - w->pos[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best) {
                best = d;
                lamp = id;
            }
        }
    }
    return lamp == kTravelNone ? 0u : lamp;
}

void CarryArrivalTick(const GameSnapshot& s, double now) {
    const double at = g_carry_join_at.load();
    if (at < 0) {
        return;
    }
    if (now - at > 60.0) {
        g_carry_join_at = -1;
        g_carry_hold = false;
        Log("A5: cross-map join: no arrival load within 60 s (role %d, %s); the gather path stays", Role(),
            MapName(s.map_id).c_str());
        return;
    }
    if (Role() != RoleClient || !s.world_up || s.loading || !s.player || g_rejoin.load()) {
        return;
    }
    const u32 host = HostMap();
    if (host == 0 || host == 0xffffffffu || s.map_id == 0 || s.map_id == 0xffffffffu) {
        return;
    }
    if ((host >> 16) == (s.map_id >> 16)) {
        g_carry_join_at = -1;
        g_carry_hold = false;
        Log("A5: cross-map join: this guest is in the host's map %s (%.1f s after the join)", MapName(host).c_str(),
            now - at);
        return;
    }
    // 1 s after the join (live): the game first takes the join's own snapshot (started at once,
    // a5d / a11f / a11g, the load built the guest's own Dream); its own summon load meanwhile
    // (a11e: 0.8 s after the client role, also into the Dream) is held by K6 (g_carry_hold).
    static double tried = -100;
    if (now - at < 1.0 || now - tried < 5.0) {
        return;
    }
    tried = now;
    const u32 lamp = LampNearHost(host);
    TravelIntent t;
    if (lamp == 0 || !LampIntent(lamp, &t)) {
        Log("A5: cross-map join: no lamp of the host's map %s known; waiting", MapName(host).c_str());
        return;
    }
    const bool started = TravelAlone(t, AloneWhy::Join);
    g_carry_hold = false;
    if (started) {
        g_carry_gd_at = now;
        g_carry_join_at = -1;
        Log("A5: cross-map join: client in %s, the host in %s: one load into the host's map (lamp %u, then beside "
            "the host) %.1f s after the join",
            MapName(s.map_id).c_str(), MapName(host).c_str(), lamp, now - at);
    }
}

void HostRevivedTick(const GameSnapshot& s) {
    static double watch_until = 0, next_watch = 0;
    const double now = Now();
    if (watch_until > 0 && now >= next_watch && s.world_up && !s.loading) {
        next_watch = now + 1.0;
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode == 2) {
                const u64 chr = PlayerForPeer(t.task + kTaskPeer);
                DeathFields d;
                ReadDeathFields(chr, &d);
                Log("guest: revived host chr watch: %s, %s", chr ? DescribeChr(chr).c_str() : "none",
                    DescribeDeathFields(d).c_str());
            }
        }
        if (now >= watch_until) {
            watch_until = 0;
        }
    }
    if (!g_host_revived_note.load() || !s.world_up || s.loading) {
        return;
    }
    watch_until = now + 6.0;
    next_watch = now + 1.0;
    g_host_revived_note = false;
    if (Role() != RoleClient) {
        return;
    }
    static const bool recreate = !(std::getenv("BB_PARTY_B2_HOST_REVIVE_RECREATE") &&
                                   std::getenv("BB_PARTY_B2_HOST_REVIVE_RECREATE")[0] == '0');
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode != 2) {
            continue;
        }
        const u64 chr = PlayerForPeer(t.task + kTaskPeer);
        DeathFields d;
        ReadDeathFields(chr, &d);
        Log("guest: the host revived in place; its chr here: %s, %s%s", chr ? DescribeChr(chr).c_str() : "none",
            DescribeDeathFields(d).c_str(), recreate && chr ? "; re-created" : "");
        if (!chr || !recreate) {
            continue;
        }
        using Release = u64(BB_COOP_SYSV*)(u64, u32);
        using Remove = void(BB_COOP_SYSV*)(u64, u64);
        reinterpret_cast<Release>(Guest(0x15bc080))(Field<u64>(Slot(kSlotGsm), 0x16f8), Field<u32>(chr, 8));
        reinterpret_cast<Remove>(Guest(0x19186e0))(Slot(kSlotWorldChrMan), chr);
    }
    if (recreate) {
        RewindRemoteTasks("guest (host revived)");
    }
}

/// BB_PARTY_B2_ARRIVAL_WARP (default on): a guest's B2 load is the host's own travel (no forced
/// transform); it is put beside the host with a chr warp once it has arrived.
bool ArrivalWarpMode() {
    static const bool on = !(std::getenv("BB_PARTY_B2_ARRIVAL_WARP") && std::getenv("BB_PARTY_B2_ARRIVAL_WARP")[0] == '0');
    return on;
}
bool g_arrival_warp_pending = false;
alignas(16) float g_arrival_pos[4] = {}, g_arrival_rot[4] = {};

void GuestTick(const GameSnapshot& s, double now) {
    State& st = S();
    const u64 flow = Slot(kSlotFlow);
    const int fstate = Field<int>(flow, 0x84, -1);
    GuestPhase phase;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        phase = st.gphase;
    }
    switch (phase) {
    case GuestPhase::Idle: {
        u32 lamp = 0;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            std::swap(lamp, st.warp_lamp);
        }
        if (lamp && s.world_up && !s.loading) {
            if (WarpToLampInWorld(lamp, true, "the host's party move")) {
                return;
            }
            // Not in this block (apart): the party move brings it along - one load, its own.
            TravelIntent t;
            if (LampIntent(lamp, &t) && TravelAlone(t, AloneWhy::Lamp)) {
                return;
            }
            Log("guest: could not follow the host's in-world move to lamp %u", lamp);
        }
        return;
    }
    case GuestPhase::InWorldFollow: {
        // The host loads a map this guest already stands in: no load here. Once the host is in
        // (its "dest"), stand beside it, drop the snapshot packets it sent, say "arrived" (the host
        // re-creates this guest's chr; the host's own chr here follows its new PlayerIns).
        bool have_dest;
        float pos[4], rot[4];
        {
            std::lock_guard<std::mutex> lk(st.mu);
            have_dest = st.have_dest;
            std::memcpy(pos, st.dest_pos, sizeof pos);
            std::memcpy(rot, st.dest_rot, sizeof rot);
        }
        if (have_dest && s.world_up && !s.loading) {
            DrainSnapshotQueue("in-world follow");
            BesideHost(pos, rot, g_local_slot.load());
            if (ChrHp(s.player) <= 0) {
                // A dead guest following the host's death load in this map (a wipe): revived beside
                // the host instead of only moved (it would stay dead: hostdeath wipe4a).
                LampSpot sp;
                sp.valid = true;
                std::memcpy(sp.pos, pos, sizeof sp.pos);
                std::memcpy(sp.rot, rot, sizeof sp.rot);
                sp.map = s.map_id;
                ReviveLocal(s, sp, false);
                g_dead_follow = false;
                g_solo_respawn = false;
                Log("guest: dead at the host's in-world respawn (a wipe): revived beside the host, no load");
            } else {
                WarpLocal(pos, rot[1], "follow the host's load into this map");
            }
            SendDeadEnemies(s, "in-world follow");
            const int n = RewindRemoteTasks("guest in-world follow");
            // The host loaded and holds this guest's record from before: send it fresh, and the
            // "arrived" (the host's re-creation) a moment later (see the meet in SplitTick).
            RebroadcastOwnRecord("in-world follow");
            g_arrived_at = now + 1.5;
            Log("guest: followed the host inside the loaded map (%s): no load; %d task(s) rewound",
                MapName(s.map_id).c_str(), n);
            g_rejoin = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        } else if (now - st.since > 60.0) {
            g_rejoin = false;
            Log("guest: in-world follow: no host destination within 60 s; staying");
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
    }
    case GuestPhase::AwayLoading: {
        // Split travel: this guest's alone load (flow 3 -> the game's 4 at the end of the load).
        const bool loading = s.loading || !s.world_up;
        st.load_seen = st.load_seen || loading;
        st.stable = (!loading && st.load_seen) ? st.stable + 1 : 0;
        if (st.stable >= 30 && fstate == 4) {
            if (st.saved_1521 >= 0) {
                const u8 v = u8(st.saved_1521);
                SafeWrite(Slot(kSlotGsm) + 0x1521, &v, 1);
                st.saved_1521 = -1;
            }
            g_rejoin = false;
            g_dead_follow = false;
            if (g_alone_spot_valid) {
                g_alone_spot_valid = false;
                float p[3] = {};
                ChrPos(s.player, p);
                const float dx = p[0] - g_alone_spot[0], dy = p[1] - g_alone_spot[1], dz = p[2] - g_alone_spot[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > 2.0f) {
                    WarpLocal(g_alone_spot, g_alone_spot_yaw, "alone arrival onto the lamp's spot");
                }
                Log("guest: alone arrival %.1f m from the lamp's spot; forced placement (own save spot) map %#x (%.1f, %.1f, %.1f)",
                    double(d), Field<u32>(Slot(kSlotGsm), 0x14f0), double(Field<float>(Slot(kSlotGsm), 0x1500)),
                    double(Field<float>(Slot(kSlotGsm), 0x1504)), double(Field<float>(Slot(kSlotGsm), 0x1508)));
            }
            Log("guest: arrived alone in %s (host in %s), still a client (role %d); %s; tasks:%s",
                MapName(s.map_id).c_str(), MapName(HostMap()).c_str(), Role(), DescribeChr(s.player).c_str(),
                DescribeTasks().c_str());
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        } else if (now - st.since > 90.0) {
            if (fstate == 3) {
                const int four = 4;
                SafeWrite(flow + 0x84, &four, 4);
            }
            g_rejoin = false;
            Log("guest: the alone load did not finish within 90 s (flow %d, %s)", fstate, loading ? "loading" : "in world");
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
    }
    case GuestPhase::AwaitSnapshot:
        if (st.enter_state1) {
            // A follow taken on the link thread: flow state 1 from the main thread, after the
            // stale snapshot packets are gone (split travel; DrainSnapshotQueue).
            st.enter_state1 = false;
            DrainSnapshotQueue("the host travels");
            const int one = 1;
            const float zero = 0.0f;
            SafeWrite(flow + 0x84, &one, 4);
            SafeWrite(flow + 0x108, &zero, 4);
            return;
        }
        if (!st.screen_done) {
            st.screen_done = true;
            SaveOverlay();
            st.hp_before = Field<int>(Field<u64>(Field<u64>(s.player, 0x3b0), 0x20), 0xf8, -1);
            if (st.hp_before <= 0) {
                // Dead at the host's travel (a held death): back with full HP, as the host respawns.
                st.hp_before = Field<int>(Field<u64>(Field<u64>(s.player, 0x3b0), 0x20), 0xfc, -1);
                Log("guest: dead at the host's travel; full HP (%d) after the load", st.hp_before);
            }
            ApplyTestSpEffects(s.player);
            // A dead guest's buffs end with the death, as in vanilla (none restored after the load).
            st.buffs_before = g_dead_follow.load() ? std::vector<std::pair<int, float>>{} : TimedSpEffects(s.player);
            LogGuestState(s, "at the host's travel");
            static const bool early = !(std::getenv("BB_PARTY_B2_EARLY_SCREEN") &&
                                        std::getenv("BB_PARTY_B2_EARLY_SCREEN")[0] == '0');
            if (early) {
                RaiseLoadingScreen();
            }
        }
        bool have_dest;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            have_dest = st.have_dest;
        }
        if (fstate == 2 && !st.blobs_saved) {
            st.blobs_saved = true;
            SaveHostBlobs();
        }
        if (fstate == 2 && have_dest) {
            // The game applied the host's 0x11 world snapshot and 0x10 flags (+0x84 = 2; K6 ate
            // the Lua join start). Load the host's destination in host-world mode.
            TravelIntent t;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                t = st.intent;
                st.gphase = GuestPhase::Loading;
                st.since = now;
                st.load_seen = false;
                st.stable = 0;
            }
            const int three = 3;
            SafeWrite(flow + 0x84, &three, 4);  // the setter 0x178D9A0's write
            bool solo;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                solo = st.solo;
            }
            if (solo) {
                // Split travel: the guest's own destination (its lamp), not the host's spot: the
                // summoned placement (GSM +0x1521 set by the join) would put it beside the host.
                const u64 gsm = Slot(kSlotGsm);
                const u8 was = Field<u8>(gsm, 0x1521), zero = 0;
                if (was) {
                    st.saved_1521 = was;
                    SafeWrite(gsm + 0x1521, &zero, 1);
                }
                {
                    // The host-world load takes its stage from the summoned placement +0x14C4 (live
                    // split10: still the Dream's after an alone load, so the return to m24_01
                    // landed in m21): the lamp's own spot goes there.
                    const std::uint32_t lamp = t.lamp_id != kTravelNone && t.lamp_id ? t.lamp_id
                                                                                     : static_cast<std::uint32_t>(t.last_lamp);
                    float spot[4];
                    if (lamp / 100000 != 29 && ids::ReturnSpot(lamp, spot)) {
                        const u32 stage = ids::EntityMap(lamp);
                        alignas(16) float p16[4] = {spot[0], spot[1], spot[2], 1.0f};
                        alignas(16) float r16[4] = {0.0f, spot[3], 0.0f, 0.0f};
                        // The stage of the host-world load (split10); written whatever +0x1521 says.
                        SafeWrite(gsm + 0x14c4, &stage, 4);
                        SafeWrite(gsm + 0x14d0, p16, 16);
                        SafeWrite(gsm + 0x14e0, r16, 16);
                        if (ArrivalWarpMode()) {
                            // No forced transform (it would become the own save's spawn spot): the
                            // lamp travel itself, then a chr warp onto the lamp's spot on arrival.
                            g_arrival_warp_pending = true;
                            std::memcpy(g_arrival_pos, p16, sizeof g_arrival_pos);
                            std::memcpy(g_arrival_rot, r16, sizeof g_arrival_rot);
                        } else {
                            t.kind = TravelKind::Transform;
                            t.has_pos = true;
                            t.pos_map = stage;
                            std::memcpy(t.pos, p16, sizeof t.pos);
                            std::memcpy(t.rot, r16, sizeof t.rot);
                        }
                    }
                }
                RestoreZeroedOverlay();
                coop::progress::OnVanillaSnapshotApplied();
                FunnelKeepRoom keep;
                Log("guest: host snapshot applied; travelling alone to %s in host-world mode (K7 %s, GSM +0x1521 %u)",
                    DescribeTravel(t).c_str(), keep.armed ? "armed" : "NOT armed: the room will close", unsigned(was));
                if (!ReplayTravelNow(t)) {
                    FallbackToB1("no usable destination");
                }
                return;
            }
            {
                // A guest's loads use the summoned placement (GSM +0x1521 set by the join):
                // +0x14C4 stage, +0x14D0 position, +0x14E0 rotation - the host's, 1.5 m aside.
                std::lock_guard<std::mutex> lk(st.mu);
                const u64 gsm = Slot(kSlotGsm);
                float pos[4];
                std::memcpy(pos, st.dest_pos, sizeof pos);
                BesideHost(pos, st.dest_rot, g_local_slot.load());
                if (ArrivalWarpMode()) {
                    // The host-world load takes its stage from +0x14C4: the host's spot itself (the
                    // guest is put beside it with a chr warp after the load, ArrivalWarpTick).
                    SafeWrite(gsm + 0x14c4, &st.dest_stage, 4);
                    SafeWrite(gsm + 0x14d0, st.dest_pos, 16);
                    SafeWrite(gsm + 0x14e0, st.dest_rot, 16);
                } else {
                    WriteSummonedPlacement(gsm, &st.dest_stage, pos, st.dest_rot);
                }
                Log("guest: summoned placement = stage %#x pos (%.1f, %.1f, %.1f), GSM +0x1521 = %u", st.dest_stage,
                    pos[0], pos[1], pos[2], unsigned(Field<u8>(gsm, 0x1521)));
            }
            {
                // Written once, before the load: an exact transform (forced placement +0x14F0 map,
                // +0x1500 position, +0x1510 rotation, +0x1520) beside the host's arrival spot.
                std::lock_guard<std::mutex> lk(st.mu);
                if (ArrivalWarpMode() && g_dead_follow.load()) {
                    // A dead follow (a wipe, a lone respawn): its death load goes to the respawn
                    // records whatever a forced transform says, and the forced transform would
                    // become the own save's spawn spot; none. (Placed after arrival as before.)
                } else if (ArrivalWarpMode()) {
                    // No forced transform: the game's own placement of the travel (a lamp warp's
                    // spawn spot), then a chr warp beside the host after the load. The forced
                    // placement keeps the pre-summon spot the join wrote, which is what the guest's
                    // own save records (a Continue after a B2 load started at the forced spot beside
                    // the host, hd pin3/pin4).
                    g_arrival_warp_pending = true;
                    std::memcpy(g_arrival_pos, st.dest_pos, sizeof g_arrival_pos);
                    std::memcpy(g_arrival_rot, st.dest_rot, sizeof g_arrival_rot);
                    BesideHost(g_arrival_pos, g_arrival_rot, g_local_slot.load());
                } else if (SummonOffset() > 0.0f) {
                    t.kind = TravelKind::Transform;
                    t.has_pos = true;
                    t.pos_map = st.dest_stage;
                    std::memcpy(t.pos, st.dest_pos, sizeof t.pos);
                    BesideHost(t.pos, st.dest_rot, g_local_slot.load());
                    std::memcpy(t.rot, st.dest_rot, sizeof t.rot);
                }
            }
            if (g_dead_follow.load()) {
                const u64 gsm = Slot(kSlotGsm);
                OwnRespawn& o = g_own_respawn;
                o.saved = gsm && SafeGet(gsm + 0x1528, &o.last_lamp) && SafeGet(gsm + 0x1538, &o.mode) &&
                          SafeGet(gsm + 0x153c, &o.respawn);
                if (o.saved) {
                    const u64 lamp = t.last_lamp != ~0ull ? t.last_lamp : o.last_lamp;
                    const u64 resp = t.respawn_record != ~0ull ? t.respawn_record : lamp;
                    const u32 mode = t.respawn_mode ? t.respawn_mode : 1;
                    SafeWrite(gsm + 0x1528, &lamp, 8);
                    SafeWrite(gsm + 0x1538, &mode, 4);
                    SafeWrite(gsm + 0x153c, &resp, 8);
                    Log("guest: dead, follows the host: respawn records = the host's (last lamp %#llx -> %#llx, "
                        "mode %u -> %u, respawn %#llx -> %#llx)",
                        ull(o.last_lamp), ull(lamp), o.mode, mode, ull(o.respawn), ull(resp));
                }
            }
            RestoreZeroedOverlay();
            coop::progress::OnVanillaSnapshotApplied();  // the host's image wins over the saved copy
            LogIntroFlags("before the host-world load");
            FunnelKeepRoom keep;
            Log("guest: host snapshot applied; loading %s in host-world mode (K7 %s)", DescribeTravel(t).c_str(),
                keep.armed ? "armed" : "NOT armed: the room will close");
            if (!ReplayTravelNow(t)) {
                FallbackToB1("no usable destination");
            }
            return;
        }
        if (now - st.since > 45.0) {  // the flow leaves by itself after 60 s in states 1-3
            FallbackToB1(fstate == 2 ? "no host destination within 45 s" : "no host snapshot within 45 s");
        }
        return;
    case GuestPhase::Loading: {
        const bool loading = s.loading || !s.world_up;
        st.load_seen = st.load_seen || loading;
        st.stable = (!loading && st.load_seen) ? st.stable + 1 : 0;
        if (st.stable >= 30 && fstate == 4) {
            g_early_screen = false;  // the load's own InGameStart cleared it
            {
                // The guest's state is not the load's business (no rest): its HP as before the
                // host's travel (a load may refill it). Data module: chr +0x3B0 -> +0x20: HP +0xf8,
                // max +0xFC.
                const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
                const int hp = Field<int>(dm, 0xf8, -1), mx = Field<int>(dm, 0xfc, -1);
                if (RestedFromTravel(st.intent)) {
                    Log("guest: HP %d after the load (a rest travel: the refill heals, max %d)", hp, mx);
                } else if (dm && st.hp_before > 0 && hp >= 0 && hp != st.hp_before) {
                    const int keep = st.hp_before < mx ? st.hp_before : mx;
                    SafeWrite(dm + 0xf8, &keep, 4);
                    Log("guest: HP %d after the load -> %d as before the travel (max %d)", hp, keep, mx);
                } else {
                    Log("guest: HP %d after the load (before %d, max %d)", hp, st.hp_before, mx);
                }
            }
            LogGuestState(s, "after the B2 load");
            RestoreBuffs(s.player, st.buffs_before);
            st.buffs_before.clear();
            if (!TestSpEffects().empty()) {
                LogGuestState(s, "after the buff restore");
            }
            if (st.saved_1521 >= 0) {
                const u8 v = u8(st.saved_1521);
                SafeWrite(Slot(kSlotGsm) + 0x1521, &v, 1);
                st.saved_1521 = -1;
            }
            {
                std::lock_guard<std::mutex> lk(st.mu);
                if (st.solo) {
                    Log("guest: arrived alone at %s in the host's area (%s)", DescribeTravel(st.intent).c_str(),
                        DescribeChr(s.player).c_str());
                }
                st.solo = false;
            }
            const int n = RewindRemoteTasks("guest");
            Log("guest: arrived in the host world (flow 4); %d remote task(s) rewound", n);
            if (g_arrival_warp_pending && s.player) {
                g_arrival_warp_pending = false;
                alignas(16) float p16[4] = {g_arrival_pos[0], g_arrival_pos[1], g_arrival_pos[2], 1.0f};
                alignas(16) float r16[4] = {g_arrival_rot[0], g_arrival_rot[1], g_arrival_rot[2], g_arrival_rot[3]};
                LocalChrWarpImpl(s.player, p16, r16, "arrival beside the host");
                const u64 gsm = Slot(kSlotGsm);
                Log("guest: arrived by the host's own travel; placed beside the host (%.2f, %.2f, %.2f); forced placement "
                    "(own save spot) map %#x (%.1f, %.1f, %.1f)",
                    double(p16[0]), double(p16[1]), double(p16[2]), Field<u32>(gsm, 0x14f0), double(Field<float>(gsm, 0x1500)),
                    double(Field<float>(gsm, 0x1504)), double(Field<float>(gsm, 0x1508)));
            }
            if (g_dead_follow.exchange(false) && g_own_respawn.saved) {
                const u64 gsm = Slot(kSlotGsm);
                OwnRespawn& o = g_own_respawn;
                SafeWrite(gsm + 0x1528, &o.last_lamp, 8);
                SafeWrite(gsm + 0x1538, &o.mode, 4);
                SafeWrite(gsm + 0x153c, &o.respawn, 8);
                o.saved = false;
                Log("guest: own respawn records put back after the death follow (map now %s)", MapName(s.map_id).c_str());
                // A death load lands on the lamp spot itself (the forced placement is ignored):
                // the guest stood inside the host (live r13: 0.00 m, the host pushed). Beside it.
                float pos[4], rot[4];
                {
                    std::lock_guard<std::mutex> lk(st.mu);
                    std::memcpy(pos, st.dest_pos, sizeof pos);
                    std::memcpy(rot, st.dest_rot, sizeof rot);
                }
                if (g_solo_respawn.exchange(false)) {
                    Log("guest: respawned at the host's lamp in the host's world (%s), still a client",
                        DescribeChr(s.player).c_str());
                } else if (SummonOffset() > 0.0f && s.player) {
                    BesideHost(pos, rot, g_local_slot.load());
                    alignas(16) float p16[4] = {pos[0], pos[1], pos[2], 1.0f}, r16[4] = {rot[0], rot[1], rot[2], rot[3]};
                    if (LocalChrWarpImpl(s.player, p16, r16, "beside the host after the death follow")) {
                        Log("guest: placed beside the host after the death follow (%.2f, %.2f, %.2f)", double(pos[0]),
                            double(pos[1]), double(pos[2]));
                    }
                }
            }
            Send(0, "{\"op\":\"arrived\"}");  // guest: slot 0 = the host
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Rewinding;
            st.since = now;
        } else if (now - st.since > 90.0) {
            FallbackToB1("host-world load not finished within 90 s");
        }
        return;
    }
    case GuestPhase::Rewinding: {
        bool all = true;
        for (const TaskInfo& t : Tasks(false)) {
            if ((t.mode == 1 || t.mode == 2) && !PlayerForPeer(t.task + kTaskPeer)) {
                all = false;
            }
        }
        if (all || now - st.since > 60.0) {
            const u64 remo = Field<u64>(Slot(0x5540058), 8);
            Log("guest: enterleave at the re-join's end:%s", DescribeEnterLeave().c_str());
            Log("guest: re-join %s after %.1f s (SprjRemo %d); tasks:%s", all ? "done" : "INCOMPLETE",
                now - st.since, remo ? int(Field<u8>(remo, 0x168) & 1) : -1, DescribeTasks().c_str());
            if (!all) {
                FallbackToB1("host chr not re-created within 60 s");
                return;
            }
            g_rejoin = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
    }
    }
}

/// Host: where it stands, to the guests (their summoned placement / arrival spot).
void SendDest(const GameSnapshot& s) {
    const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
    float pos[4] = {}, rot[4] = {};
    if (a && SafeRead(a + 0x1e0, pos, 16) && SafeRead(a + 0x1d0, rot, 16)) {
        char b[320];
        std::snprintf(b, sizeof b,
                      "{\"op\":\"dest\",\"stage\":%u,\"pos\":[%.4f,%.4f,%.4f,%.4f],\"rot\":[%.6f,%.6f,%.6f,%.6f]}",
                      Field<u32>(s.player, 0x3f8), pos[0], pos[1], pos[2], pos[3], rot[0], rot[1], rot[2], rot[3]);
        Send(-1, b);
        Log("host: destination sent: %s", b);
    }
}

void HostTick(const GameSnapshot& s, double now) {
    State& st = S();
    const bool loading = s.loading || !s.world_up;
    if (Role() == RoleHost) {
        ResyncRetryTick(s, now);
    } else {
        g_resync_retry.clear();
    }
    if (Role() != RoleHost) {
        if (st.hphase != HostPhase::Idle && Role() != RoleClient) {
            // The room ended during a re-join (a member's respawn, a travel): nothing of it
            // carries over into the next room (r23: g_rejoin stayed set across two later rooms).
            g_rejoin = false;
            Log("host: the session ended during a re-join; re-join state cleared");
        }
        st.hphase = HostPhase::Idle;
        return;
    }
    bool respawn = false, away = false, away_have = false, live = false;
    float apos[3] = {};
    if (loading && g_rest_carry_at.load() > 0) {
        g_rest_carry_at = 0;  // the carried rest load started: the re-join waits for its end anyway
    }
    const bool carry_held = deathrules::RestCarryHeld(now, g_rest_carry_at.load(), false);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        std::swap(away, st.away_request);
        away_have = st.away_have_pos;
        std::memcpy(apos, st.away_pos, sizeof apos);
        // A guest travelling alone into this world during the host's own load: the load's own
        // re-sync covers it (pt1polish; a dead member's respawn keeps the old preemption).
        const bool settling = st.hphase == HostPhase::Loading && st.respawn_live;
        if (deathrules::HostServesRejoin(loading, settling, carry_held)) {
            std::swap(respawn, st.respawn_request);
            live = st.respawn_live;
        }
    }
    {
        static bool held_logged = false;
        if (carry_held && !held_logged) {
            Log("host: the guest's re-join waits for the rest load it carries");
        }
        held_logged = carry_held;
    }
    if (away) {
        // A guest travels alone into another area (split travel): its chr here goes now (it would
        // stand frozen, then follow coordinates of another map); the task is kept.
        const u64 c = away_have ? ChrNear(apos) : 0;
        if (c) {
            RemoveRemoteChr(c, "the guest travels alone");
        }
        Log("host: a guest travels alone (%s); tasks:%s", c ? "its chr removed" : "no chr of it here", DescribeTasks().c_str());
    }
    if (respawn && live) {
        Log("host: a guest travels alone into this area (split travel): the snapshot again, as for a lone respawn");
    }
    if (respawn && !loading) {
        // A guest died alone and reloads this world (SoloRespawn): the join snapshot again and
        // where the host stands; its chr is re-created on "arrived" as after a host travel.
        Log("host: a guest respawns in this world; tasks:%s", DescribeTasks().c_str());
        // Its dead chr stays here otherwise (live r16: hp 1 at the death spot after the guest stood
        // at the lamp, 0 tasks rewound): removed as the task's own STEP_DeleteWait 0x1E50A70 does
        // (slot release 0x15BC080, WorldChrMan remove 0x19186E0); the task is kept and rewound to
        // STEP_Create on "arrived", which builds the chr again from the member's GameData row.
        bool have_pos;
        float dpos[3];
        {
            std::lock_guard<std::mutex> lk(st.mu);
            have_pos = st.respawn_have_pos;
            std::memcpy(dpos, st.respawn_pos, sizeof dpos);
        }
        // The member chr nearest to where the guest died (within 6 m); without a position, a lone
        // member's chr.
        u64 victim = 0;
        float best = 36.0f;
        int members = 0;
        for (const TaskInfo& t : Tasks(false)) {
            const u64 c = t.mode == 1 ? PlayerForPeer(t.task + kTaskPeer) : 0;
            if (!c) {
                continue;
            }
            ++members;
            float p[3];
            if (have_pos && ChrPos(c, p)) {
                const float d = (p[0] - dpos[0]) * (p[0] - dpos[0]) + (p[1] - dpos[1]) * (p[1] - dpos[1]) +
                                (p[2] - dpos[2]) * (p[2] - dpos[2]);
                if (d < best) {
                    best = d;
                    victim = c;
                }
            } else if (!have_pos) {
                victim = c;
            }
        }
        if (!have_pos && members != 1) {
            victim = 0;
        }
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode != 1) {
                continue;
            }
            const u64 chr = PlayerForPeer(t.task + kTaskPeer);
            const int hp = Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0xf8, -1);
            if (!chr || chr != victim) {
                continue;
            }
            RemoveRemoteChr(chr, "a dead member respawns");  // with the presentation leave
            Log("host: dead member chr %#llx (hp %d) removed; its task is kept for the re-creation", ull(chr), hp);
        }
        static const bool fail = std::getenv("BB_PARTY_B2_TEST_FAIL") &&
                                 std::strcmp(std::getenv("BB_PARTY_B2_TEST_FAIL"), "resync") == 0;
        for (u64 id : Members()) {
            if (fail) {
                Log("test fail: join snapshot NOT re-sent (respawn)");
                continue;
            }
            ResyncMember(id);
        }
        SendDest(s);
        st.hphase = HostPhase::Resynced;
        st.hsince = now;
        g_rejoin = true;
        std::lock_guard<std::mutex> lk(st.mu);
        st.arrived = false;
        return;
    }
    switch (st.hphase) {
    case HostPhase::Idle:
        if (loading) {
            st.hphase = HostPhase::Loading;
            st.hsince = now;
            st.stable = 0;
            g_rejoin = true;
            st.alone_load = (g_host_alone_next.exchange(false) && now - g_host_alone_at.load() < 10.0) ||
                            LastAloneCaptureAgo() < 10.0;
            Log("host: own load with a session; members keep the room%s",
                st.alone_load ? " (an alone travel: the guests stay where they are)" : "");
        }
        return;
    case HostPhase::Loading:
        st.stable = loading ? 0 : st.stable + 1;
        if (st.stable >= 30 && st.alone_load) {
            // Split travel: nobody follows; no snapshot (it would wait in every guest's receive
            // queue). Members already in this area are met without a load (SplitTick).
            st.alone_load = false;
            st.hphase = HostPhase::Idle;
            g_rejoin = false;
            Log("host: arrived alone in %s; tasks:%s", MapName(s.map_id).c_str(), DescribeTasks().c_str());
            return;
        }
        if (st.stable >= 30) {
            // BB_PARTY_B2_TEST_FAIL=resync: the host sends nothing (the guests' B1 fallback test).
            static const bool fail = std::getenv("BB_PARTY_B2_TEST_FAIL") &&
                                     std::strcmp(std::getenv("BB_PARTY_B2_TEST_FAIL"), "resync") == 0;
            for (u64 id : Members()) {
                if (fail) {
                    Log("test fail: join snapshot NOT re-sent");
                    continue;
                }
                ResyncMember(id);
            }
            // Where the host stands now: the guests' summoned placement (GSM +0x14C4/+0x14D0/
            // +0x14E0, what the descriptor builder 0x1943F10 uses while GSM +0x1521 is set).
            {
                const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
                float pos[4] = {}, rot[4] = {};
                if (a && SafeRead(a + 0x1e0, pos, 16) && SafeRead(a + 0x1d0, rot, 16)) {
                    char b[320];
                    std::snprintf(b, sizeof b,
                                  "{\"op\":\"dest\",\"stage\":%u,\"pos\":[%.4f,%.4f,%.4f,%.4f],\"rot\":[%.6f,%.6f,%.6f,%.6f]}",
                                  Field<u32>(s.player, 0x3f8), pos[0], pos[1], pos[2], pos[3], rot[0], rot[1], rot[2], rot[3]);
                    Send(-1, b);
                    Log("host: destination sent: %s", b);
                }
            }
            st.hphase = HostPhase::Resynced;
            st.hsince = now;
            st.rejoined_logged = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.arrived = false;
            if (st.respawn_request && st.respawn_live) {
                st.respawn_request = false;  // this re-sync is its snapshot too
                Log("host: the re-sync after this load also serves the guest travelling into this area");
            }
        }
        return;
    case HostPhase::Resynced: {
        if (loading) {
            // Another own load (a death right after a warp, live r8) while the last re-join still
            // waits: re-sync after this one, or the guests wait for a snapshot that never comes.
            st.hphase = HostPhase::Loading;
            st.hsince = now;
            st.stable = 0;
            g_rejoin = true;
            st.alone_load = (g_host_alone_next.exchange(false) && now - g_host_alone_at.load() < 10.0) ||
                            LastAloneCaptureAgo() < 10.0;
            Log("host: another own load before the last re-join settled; members keep the room, re-sync after it%s",
                st.alone_load ? " (alone: no re-sync)" : "");
            return;
        }
        bool arrived;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            arrived = st.arrived;
            st.arrived = false;
        }
        const bool timeout = now - st.hsince > 60.0;
        if (arrived || timeout) {
            const int n = RewindRemoteTasks("host");
            Log("host: %s; %d member task(s) rewound", arrived ? "a guest arrived" : "60 s without arrival", n);
            if (timeout) {
                st.hphase = HostPhase::Idle;
                g_rejoin = false;
                return;
            }
        }
        // Done once every member task has its chr again (each guest's "arrived" rewinds it). A
        // member apart in another area (B5: it stayed there at the host's death) has none by design.
        int missing = 0;
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode == 1 && !PlayerForPeer(t.task + kTaskPeer)) {
                ++missing;
            }
        }
        const bool all = missing <= g_apart.load();
        const bool only_apart = missing > 0 && g_apart.load() > 0 && now - st.hsince > 3.0;  // nobody to wait for
        if (all && (arrived || st.rejoined_logged || only_apart)) {
            st.hphase = HostPhase::Idle;
            g_rejoin = false;
            Log("host: re-join done after %.1f s; tasks:%s; enterleave%s", now - st.hsince, DescribeTasks().c_str(),
                DescribeEnterLeave().c_str());
        }
        st.rejoined_logged = st.rejoined_logged || arrived;
        return;
    }
    }
}


// ---- C6 RE: the game's transport objects (NexusRevolution) ----
constexpr u64 kNrCreateCall = 0xcdac45;  // FUN_00CDABE0: call FUN_00CDB250(obj, mgr[1], mgr[2], cfg)
constexpr u64 kNrCreate = 0xcdb250;
std::mutex g_nr_mu;
std::deque<NrTransport> g_nr;

BB_COOP_SYSV u64 NrCreateHook(u64 obj, u64 a1, u64 a2, u64 cfg) {
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64);
    const u64 r = reinterpret_cast<Fn>(Guest(kNrCreate))(obj, a1, a2, cfg);
    NrTransport t;
    t.owner = obj;
    t.factory = a2;
    t.transport = Field<u64>(obj, 8);
    // Kept for the C6 crash drop (NrDropSilentPeers); the rest is an RE aid (BB_PARTY_NR_FAST_EXPIRE=1).
    static const bool on = EnvOn("BB_PARTY_NR_FAST_EXPIRE");
    if (!on) {
        std::lock_guard<std::mutex> lk(g_nr_mu);
        g_nr.push_front(t);
        if (g_nr.size() > 16) g_nr.pop_back();
        return r;
    }
    static const std::uint32_t pat[7] = {0x4d0, 3000, 5000, 6000, 25000, 20000, 200};
    long found_at = -1;
    if (t.transport) {
        std::vector<std::uint32_t> buf(0x4000 / 4);
        for (std::size_t chunk = 0; chunk < buf.size(); chunk += 256) {
            if (!SafeRead(t.transport + chunk * 4, buf.data() + chunk, 1024)) break;
        }
        for (std::size_t i = 0; i + 7 <= buf.size(); ++i) {
            if (buf[i] == 0x4d0 && std::memcmp(&buf[i], pat, sizeof pat) == 0) {
                found_at = static_cast<long>(i * 4);
                t.config = reinterpret_cast<std::uint32_t*>(t.transport + i * 4);
                break;
            }
        }
    }
    if (t.transport) {
        std::uint32_t w[32] = {};
        SafeRead(t.transport + 0x130, w, sizeof w);
        std::string hex;
        for (int i = 0; i < 32; ++i) {
            char x[16];
            std::snprintf(x, sizeof x, " %x", w[i]);
            hex += x;
        }
        Log("NR transport +0x130..:%s", hex.c_str());
    }
    Log("NR transport created: owner %#llx transport %#llx (ok %d); config copy %s%#lx", ull(obj), ull(t.transport),
        static_cast<int>(r & 0xff), found_at >= 0 ? "at +" : "not found in 16 KiB ", found_at >= 0 ? found_at : 0L);
    std::lock_guard<std::mutex> lk(g_nr_mu);
    g_nr.push_front(t);
    if (g_nr.size() > 16) g_nr.pop_back();
    return r;
}

void InstallCall(u64 off, u64 target, const void* handler, const char* name) {
    if (!HookCallSite(off, target, handler, name)) {
        Log("%s: NOT installed (bytes differ)", name);
    }
}

// ---- Title join (BB_PARTY_TITLE_JOIN): Continue straight into the host's world ----
// The guest joins the host's session from the title screen (no world, no sign): the host
// serializes its session descriptor as its summon message does (0x1872360: [u32 -> GSM +0x16f0]
// [u32, low byte -> GSM +0x16f4] [descriptor 0xc90b00]), the guest deserializes it (0xc90dd0) and
// joins (0x1789010 -> SprjSessionManager 0x1ED0670: JoinRoom). The flow update 0x1789B60 (session
// update 0x1ED0B20 + packet handler) only runs in the world / load steps: the guest runs it at the
// title until the host's join snapshot is applied (flow +0x84 = 2), then sets +0x84 = 3 and the
// summoned placement (GSM +0x14C4 stage, +0x14D0 pos, +0x14E0 rot) and lets Continue run: the
// MoveMapStep ctor 0x1937570 makes that first load a host-world load. docs/party/join_timing.md.
constexpr u64 kRoomCtor = 0xc8fbf0;     // void (room*)
constexpr u64 kRoomDtor = 0xc90520;     // void (room*)
constexpr u64 kRoomRead = 0xc90dd0;     // bool (room*, reader*)
constexpr u64 kRoomWrite = 0xc90b00;    // bool (room*, writer*)
constexpr u64 kRoomFetch = 0xc8f500;    // bool (SessionMan+0x18, room* = SessionMan+0x38, bool)
constexpr u64 kRoomCopy = 0xc90270;     // void (room* dst, const room* src)
constexpr u64 kRoomValid = 0xc90d80;    // bool (room*)
constexpr u64 kStreamWrite = 0x2078780; // u64 (writer*, const void*, u64)
constexpr u64 kJoinSession = 0x1789010; // bool (flow, room*)
constexpr u64 kFlowUpdate = 0x1789b60;  // void (float dt, flow)
constexpr u64 kWriterVtable = 0x53b2c80, kReaderVtable = 0x53b2be0;

enum class TitlePhase { Off, Idle, Requested, Joining, Snapshot, Ready, Loading, Done, Failed };
const char* TitlePhaseName(TitlePhase p) {
    switch (p) {
    case TitlePhase::Off: return "off";
    case TitlePhase::Idle: return "idle";
    case TitlePhase::Requested: return "requested";
    case TitlePhase::Joining: return "joining";
    case TitlePhase::Snapshot: return "snapshot";
    case TitlePhase::Ready: return "ready";
    case TitlePhase::Loading: return "loading";
    case TitlePhase::Done: return "done";
    case TitlePhase::Failed: return "failed";
    }
    return "?";
}

struct TitleJoin {
    std::mutex mu;
    TitlePhase phase = TitlePhase::Idle;
    double since = 0, last_update = 0, done_load = 0;
    float last_timer = -1.0f;
    int updates = 0;
    bool have_reply = false, acked = false;
    int host_state = -1;    // the host's last title_wait: 0 title, 1 loading / in its world
    bool own_have = false;  // the guest's own save spot (the forced placement its save loaded)
    u32 own_map = 0;
    float own_pos[4] = {}, own_rot[4] = {};
    std::vector<u8> room;  // the host's reply: head + descriptor
    u32 stage = 0;
    float pos[4] = {}, rot[4] = {};
    int last_role = -2, last_state = -2;
    std::vector<int> requests;  // host: slots that asked
};
TitleJoin& TJ() {
    static TitleJoin t;
    return t;
}

bool TitleJoinOn() {
    // Default on (Continue party game loads the host's world directly); BB_PARTY_TITLE_JOIN=0 off.
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_TITLE_JOIN");
        return !(v && v[0]) || EnvOn("BB_PARTY_TITLE_JOIN");
    }();
    return on;
}

std::string Hex(const std::vector<u8>& b, std::size_t max) {
    std::string s;
    char h[3];
    for (std::size_t i = 0; i < b.size() && i < max; ++i) {
        std::snprintf(h, sizeof h, "%02x", b[i]);
        s += h;
    }
    return s;
}

bool Unhex(const char* p, std::vector<u8>* out) {
    out->clear();
    auto nib = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    while (p[0] && p[0] != '"') {
        const int a = nib(p[0]), b = p[1] ? nib(p[1]) : -1;
        if (a < 0 || b < 0 || out->size() >= 0x400) {
            return false;
        }
        out->push_back(u8(a << 4 | b));
        p += 2;
    }
    return !out->empty();
}

/// Host: the session descriptor as the summon message carries it, behind the 8-byte head.
bool SerializeRoom(u32 head0, u32 head1, std::vector<u8>* out) {
    const u64 ssm = Slot(kSlotSession);
    if (!ssm || Role() != RoleHost) {
        return false;
    }
    using Fetch = u8(BB_COOP_SYSV*)(u64, u64, u32);
    using Copy = void(BB_COOP_SYSV*)(u64, u64);
    using One = u8(BB_COOP_SYSV*)(u64);
    using Two = u8(BB_COOP_SYSV*)(u64, u64);
    using Dtor = void(BB_COOP_SYSV*)(u64);
    using Put = u64(BB_COOP_SYSV*)(u64, const void*, u64);
    reinterpret_cast<Fetch>(Guest(kRoomFetch))(ssm + 0x18, ssm + 0x38, 1);
    alignas(16) u8 room[0x200] = {};
    reinterpret_cast<Copy>(Guest(kRoomCopy))(u64(room), ssm + 0x38);
    bool ok = false;
    if (reinterpret_cast<One>(Guest(kRoomValid))(u64(room)) & 1) {
        // DLMemoryWriter as 0x1872360 builds it: +8 allocator (none: never grows), +0x10 buffer,
        // +0x18 capacity, +0x20 growable, +0x30 end, +0x38 grow step.
        alignas(16) u8 buf[0x400] = {};
        alignas(16) u64 w[11] = {};
        w[0] = u64(Guest(kWriterVtable));
        w[2] = u64(buf);
        w[3] = sizeof buf;
        w[7] = sizeof buf;
        reinterpret_cast<Put>(Guest(kStreamWrite))(u64(w), &head0, 4);
        reinterpret_cast<Put>(Guest(kStreamWrite))(u64(w), &head1, 4);
        ok = reinterpret_cast<Two>(Guest(kRoomWrite))(u64(room), u64(w)) & 1;
        const u64 n = w[6];
        if (ok && n > 8 && n <= sizeof buf) {
            out->assign(buf, buf + n);
        } else {
            ok = false;
        }
    }
    reinterpret_cast<Dtor>(Guest(kRoomDtor))(u64(room));
    return ok;
}

/// The head the host's summon message puts before the descriptor for presentation slot `slot`
/// (0x1872360: the session-type table 0x553D750 + slot * 0x80, its +0xC, then the slot).
void RoomHead(int type, u32* head0, u32* head1) {
    *head1 = u32(type);
    *head0 = 0;
    SafeGet(u64(Guest(kSessionTypeDesc + 0xc + u64(type) * 0x80)), head0);
}

void TitleJoinHostTick(const GameSnapshot& s) {
    std::vector<int> req;
    {
        std::lock_guard<std::mutex> lk(TJ().mu);
        req.swap(TJ().requests);
    }
    static double first_wait = 0;
    // C6: a crashed member's restarted game asks at its title while our game still holds the dead
    // game's transport connection (FromApi sign hold, ~26 s from the drop): a room sent now is
    // joined over that connection and fails after ~20 s (live: crash rejoin 41 s). Wait it out.
    if (!req.empty()) {
        const std::int64_t left = bbnet::party::FromApi::instance().hold_left_ms();
        if (left > 0) {
            static double last = 0;
            const double now = Now();
            if (now - last >= 2.0) {
                last = now;
                for (int slot : req) Send(slot, "{\"op\":\"title_wait\",\"host\":1}");
                Log("title join: slot %d waits %.1f s more (a crashed member's old connection is still held)", req[0],
                    double(left) / 1000.0);
            }
            std::lock_guard<std::mutex> lk(TJ().mu);
            TJ().requests.insert(TJ().requests.end(), req.begin(), req.end());
            return;
        }
    }
    // A11 (ERSC_PARITY.md, user decision): no title join into a boss fight / cutscene (live: the
    // guest's Continue put it beside the host in the Cleric Beast's arena mid-fight). Refused with
    // the reason: the guest continues into its own world and waits; the host's bell after the end
    // summons it (party_director A11). Playtest 1 #1: none into the host's prologue either (the
    // room below was made on demand while the host was not start-ready); the host's bell rings for
    // the guest once it is ready.
    if (!req.empty() && s.world_up && !s.loading) {
        const bool ready = coop::LocalStartReady();
        if (!ready && !coop::LocalStartInPrologue()) {
            // Not ready for no known reason yet (a load just ended, the start flags are read next
            // tick): asked again next frame, neither refused nor served.
            std::lock_guard<std::mutex> lk(TJ().mu);
            TJ().requests.insert(TJ().requests.end(), req.begin(), req.end());
            return;
        }
        const dsummon::Busy busy = dsummon::TitleJoinBusy(Role() == RoleHost && BossBarShown(),
                                                          Role() == RoleHost && CutscenePlaying(), ready,
                                                          !coop::OthersInPrologue().empty());
        if (busy != dsummon::Busy::None) {
            for (int slot : req) {
                Send(slot, std::string("{\"op\":\"title_room\",\"room\":\"\",\"wait\":\"") + dsummon::BusyWord(busy) + "\"}");
                Log("title join: slot %d refused: %s (it waits in its own world; the bell summons it once that is over)",
                    slot, dsummon::BusyText(busy, false).c_str());
            }
            return;
        }
    }
    // A stage request already armed (GSM +8: the host's lamp warp / death load starts in a frame or
    // two): the room would carry the map the host is leaving (parx2 a6s, live: a title join asked
    // 0.16 s after the host's lamp warp loaded the guest into the host's old map, the Dream, alone,
    // apart for 225 s). Served once the host stands in its new world.
    const bool stage_pending = Field<u8>(Slot(kSlotGsm), 8) != 0;
    if (!req.empty() && stage_pending && Role() == RoleHost) {
        static double logged = 0;
        if (Now() - logged > 5.0) {
            logged = Now();
            Log("title join: slot %d waits: this host's load is about to start (a stage request is armed)", req[0]);
        }
        std::lock_guard<std::mutex> lk(TJ().mu);
        TJ().requests.insert(TJ().requests.end(), req.begin(), req.end());
        return;
    }
    if (!req.empty() && (Role() != RoleHost || !s.world_up || s.loading || stage_pending)) {
        // No room yet (the host rings its bell once it is in the world): asked again next frame.
        const double now = Now();
        if (first_wait == 0) {
            first_wait = now;
            Log("title join: slot %d asked for the room; waiting for a room here (role %d, world %d)", req[0], Role(),
                int(s.world_up));
        }
        // The host's room comes with a summon only (the bell alone opens none): create it as the
        // flow's event 0x1E does (0x178E400: 0x1ECFCD0(float [0x49271C8], SessionMan, flow +0xC)).
        static double last_create = -100.0;
        if (s.world_up && !s.loading && !stage_pending && Role() == 0 && now - last_create > 15.0) {
            last_create = now;
            float arg = 0;
            SafeGet(u64(Guest(0x49271c8)), &arg);
            const u64 flow = Slot(kSlotFlow);
            const int members = Field<int>(flow, 0xc, 4);
            using Create = u8(BB_COOP_SYSV*)(float, u64, int);
            const bool ok = reinterpret_cast<Create>(Guest(0x1ecfcd0))(arg, Slot(kSlotSession), members) & 1;
            Log("title join: creating this host's room for the waiting guest (%.1f, %d members) -> %d, role %d", double(arg),
                members, int(ok), Role());
        }
        static double last_wait = 0;
        if (now - last_wait >= 2.0) {
            last_wait = now;
            // "At its title" only while nothing there continues into the world (AutoContinue).
            const bool at_title = !s.world_up && !s.loading && !g_title_continuing.load();
            for (int slot : req) {
                Send(slot, at_title ? "{\"op\":\"title_wait\",\"host\":0}" : "{\"op\":\"title_wait\",\"host\":1}");
            }
        }
        if (now - first_wait < 60.0) {
            std::lock_guard<std::mutex> lk(TJ().mu);
            TJ().requests.insert(TJ().requests.end(), req.begin(), req.end());
            return;
        }
        for (int slot : req) {
            Send(slot, "{\"op\":\"title_room\",\"room\":\"\"}");
        }
        first_wait = 0;
        Log("title join: no room here within 60 s; refused");
        return;
    }
    first_wait = 0;
    for (int slot : req) {
        u32 h0 = 0, h1 = 0;
        RoomHead(7, &h0, &h1);  // 7: a co-op summon (the bell sign's type; live head ffffffff 07000000)
        std::vector<u8> room;
        const bool ok = SerializeRoom(h0, h1, &room);
        const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
        float pos[4] = {}, rot[4] = {};
        SafeRead(a + 0x1e0, pos, 16);
        SafeRead(a + 0x1d0, rot, 16);
        std::string b = "{\"op\":\"title_room\",\"room\":\"" + (ok ? Hex(room, room.size()) : std::string()) + "\"";
        char tail[256];
        std::snprintf(tail, sizeof tail, ",\"stage\":%u,\"pos\":[%.4f,%.4f,%.4f,%.4f],\"rot\":[%.6f,%.6f,%.6f,%.6f]}",
                      Field<u32>(s.player, 0x3f8), pos[0], pos[1], pos[2], pos[3], rot[0], rot[1], rot[2], rot[3]);
        b += tail;
        Send(slot, b);
        Log("title join: slot %d asked for the room: %s (%zu bytes, head %s), stage %#x", slot, ok ? "sent" : "NOT serialized",
            room.size(), Hex(room, 24).c_str(), Field<u32>(s.player, 0x3f8));        if (ok) g_title_served_at = Now();  // the host's lamp travel waits for this joiner (HostTravelHeldForJoin)
    }
    // The guest that came in from its title has session type 0 here (its Continue load carries
    // none): its chr is no cooperator (0x15BDC20 counts net types 1/5/7/19) until its row says 7.
    static double served_at = -100.0;
    if (!req.empty()) {
        served_at = Now();
    }
    if (Now() - served_at < 120.0 && s.world_up && !s.loading && Role() == RoleHost) {
        using Get = u64(BB_COOP_SYSV*)(u64);
        for (const TaskInfo& t : Tasks(false)) {
            const u64 chr = t.mode == 1 ? PlayerForPeer(t.task + kTaskPeer) : 0;
            const u64 f1c8 = chr ? Field<u64>(Field<u64>(chr, 0), 0x1c8) : 0;
            const u64 gd = f1c8 ? reinterpret_cast<Get>(f1c8)(chr) : 0;
            if (gd && Field<int>(gd, 0x5d8, -1) == 0) {
                const int seven = 7;
                SafeWrite(gd + 0x5d8, &seven, 4);
                Log("title join: the guest's chr here: session type 0 -> 7 (co-op)");
            }
        }
    }
    // The guest's first sync: a vanilla joiner gets the host's packet 0xA (data request) when the
    // host loads or the guest's summon load ends at the host (0x178D9C0 over the pending members);
    // a title-joined guest's load ended before it was the host's pending member, so its task here
    // waits at FirstSyncCompleteWait (step 3) for the 0xC it never sends. Ask for its data again.
    static std::map<u64, double> asked;
    // Every member already in the world, host or guest: a title joiner announces itself to nobody
    // (its first sync went to the host only; four_players.md: each guest missed the later joiners),
    // so each peer asks the newcomer for its data itself.
    static std::map<u64, int> asked_n;
    if (s.world_up && !s.loading && SessionUp() && Field<int>(Slot(kSlotFlow), 0x84, -1) == 4) {
        using SendFn = int(BB_COOP_SYSV*)(u64, u64, u32, const void*, u32);
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode != 1 || t.step != 3 || PlayerForPeer(t.task + kTaskPeer)) {
                continue;
            }
            double& at = asked[t.task];
            if (at == 0) {
                at = Now() - 1.0;  // first ask 1 s after the task waits
                continue;
            }
            if (Now() - at >= 2.0) {
                at = Now();
                const u8 zero = 0;
                const int r = reinterpret_cast<SendFn>(Guest(kSendPacket))(Slot(kSlotFlow), t.task + kTaskPeer, 0xa, &zero, 1);
                const int n = ++asked_n[t.task];
                if (n <= 3 || n % 10 == 0) {
                    Log("first sync: a member's task waits at FirstSyncCompleteWait: data request (0xA) #%d sent -> %d (role %d)", n,
                        r, Role());
                }
            }
        }
    }
    (void)served_at;
    // Validation in an ordinary bell join: the descriptor as this host would send it, next to the
    // guest's `summon message ... (head ...)` line.
    static int last_coop = 0;
    if (s.world_up && Role() == RoleHost && s.cooperators > last_coop && EnvOn("BB_PARTY_TITLE_JOIN_PROBE")) {
        u32 h0 = 0, h1 = 0;
        RoomHead(7, &h0, &h1);
        std::vector<u8> room;
        const bool ok = SerializeRoom(h0, h1, &room);
        Log("title join probe: host descriptor %s, %zu bytes, head %s", ok ? "ok" : "FAILED", room.size(), Hex(room, 24).c_str());
    }
    if (s.cooperators >= 0) {
        last_coop = s.cooperators;
    }
}


// ---- Direct summon delivery (BB_PARTY_DIRECT_SUMMON, party_dsummon.h; from droogie/bbhost) ----
// The guest asks when it rings; the host answers with its room (made at once if it has none, as
// the title join does) and holds the guest's signs so its game does not summon the same guest by
// sign meanwhile; the guest hands the room to its game's SummonStepManager as a type-1 summon
// message for the bell's own waiting entry, and the game joins as if the host's game had sent it.
constexpr u64 kNetMan = 0x553b120;          // FrpgNetMan*: +0xc50 SummonStepManager, +0xa7c area, +0xaa8 level, +0xaac pos
constexpr u64 kWrapperCtor = 0x1486a10;     // bool (wrapper*, u8 type, u16 len, const void* data)
constexpr u64 kWrapperVtable = 0x5322570;   // the wrapper's vtable (address point; 23 sites lea 0x5322560 + 0x10)
constexpr u64 kHandleItem = 0x14ba340;      // SummonStepManager vtable +0x10: handleIncomingItem(ssm, sender, wrapper)

struct DirectSummon {
    std::mutex mu;
    std::vector<int> requests;          // host: slots that asked
    std::map<int, std::string> names;   // slot -> online id (from the runtime)
    dsummon::Guest g;                   // guest
    dsummon::Room room;
    double asked_at = 0;
    // Host: the last request / served room / outcome (the bell fallback, HostBellHeld).
    double req_at = -1e9, served_at = -1e9, done_at = -1e9;
    bool done_ok = false, refused_wait = false;
    double refused_at = -1e9;  // host: the last refusal (HostBellHeld: an unanswered request holds the bell)
};
DirectSummon& DS() {
    static DirectSummon d;
    return d;
}

bool DirectSummonOn() {
    static const bool on = dsummon::SwitchOn(std::getenv("BB_PARTY_DIRECT_SUMMON"), true);  // on by default; =0 keeps the bell signs only
    return on;
}

u64 SummonStepManager() {
    const u64 net = Field<u64>(Guest(kNetMan), 0, 0);
    return net ? Field<u64>(net, 0xc50, 0) : 0;
}

/// The bell's passive guest entry still waiting for a summon message (SSM +0x158 list; node
/// {next, prev, entry}; entry +0x10 its 8-byte id, +0xc8 state, 0 = waiting).
u64 WaitingGuestEntry() {
    const u64 ssm = SummonStepManager();
    const u64 head = ssm ? Field<u64>(ssm, 0x158, 0) : 0;
    if (!head) return 0;
    u64 node = Field<u64>(head, 0, 0);
    for (int n = 0; node && node != head && n < 32; ++n) {
        const u64 e = Field<u64>(node, 0x10, 0);
        if (e && Field<u32>(e, 0xc8, 1) == 0) return e;
        node = Field<u64>(node, 0, 0);
    }
    return 0;
}

bool DeliverSummon(const dsummon::Room& room, const std::string& host_name) {
    const u64 ssm = SummonStepManager();
    const u64 entry = WaitingGuestEntry();
    if (!ssm || !entry) return false;
    const u64 fn = Field<u64>(Field<u64>(ssm, 0, 0), 0x10, 0);
    if (fn != u64(Guest(kHandleItem))) {
        Log("direct summon: the manager's handler is %#llx, not the 1.09 one: not delivered", (unsigned long long)fn);
        return false;
    }
    u8 id[8] = {};
    SafeRead(entry + 0x10, id, 8);
    const std::vector<u8> payload = dsummon::BuildPayload(id, room);
    alignas(16) u64 wrapper[2] = {u64(Guest(kWrapperVtable)), 0};
    using Make = u8(BB_COOP_SYSV*)(u64, u64, u64, const void*);
    const bool made = reinterpret_cast<Make>(Guest(kWrapperCtor))(u64(wrapper), 1, u64(payload.size()), payload.data()) & 1;
    if (!made || !wrapper[1]) {
        Log("direct summon: the game's wrapper constructor refused");
        return false;
    }
    // The sender as the handler sees it: +0x20 -> the sender's 16-byte online id.
    char ident[17] = {};
    std::snprintf(ident, sizeof ident, "%s", host_name.c_str());
    alignas(16) u8 sender[0x40] = {};
    const u64 idp = u64(ident);
    std::memcpy(sender + 0x20, &idp, 8);
    using Handle = u64(BB_COOP_SYSV*)(u64, u64, u64);
    reinterpret_cast<Handle>(fn)(ssm, u64(sender), u64(wrapper));
    const u32 state = Field<u32>(entry, 0xc8, 0);
    Log("direct summon: the host's room (%zu bytes) handed to the game as a type-1 summon message: entry state %u",
        room.data.size(), state);
    return state != 0;
}

void DirectSummonTick(const GameSnapshot& s, double now) {
    if (!DirectSummonOn()) return;
    // Host: serve the requests.
    std::vector<int> req;
    std::map<int, std::string> names;
    {
        std::lock_guard<std::mutex> lk(DS().mu);
        req.swap(DS().requests);
        names = DS().names;
    }
    static double first = 0;
    static const bool gather_dsum = !EnvOff("BB_PARTY_GATHER_DSUM");  // A5: carry across maps (default on)
    std::vector<int> keep, defer;
    for (int slot : req) {
        dsummon::HostView v;
        v.world_up = s.world_up;
        v.loading = s.loading;
        // Batch 6 (live): a room served while the host's own warp was armed took the guest into the
        // map the host was leaving (it arrived in an empty Dream). The sign path follows the host.
        v.travel_pending = Field<u8>(Slot(kSlotGsm), 8) != 0;
        // Playtest 1 #1 / #9: no summon into the host's prologue, none of a guest in its own prologue,
        // and none across maps (the guest gathers with a loading screen first).
        v.host_ready = coop::LocalStartReady();
        int guest_state = -1;
        coop::MemberRoster(slot, &guest_state, &v.guest_map);
        v.guest_in_prologue = guest_state == int(party::MemberState::Prologue);
        v.member_in_prologue = !coop::OthersInPrologue().empty();  // #19: everybody ready first
        v.host_map = s.map_id;
        v.carry = dsummon::CarriesAcrossMaps(s.map_id, gather_dsum);
        v.cooperators = s.cooperators < 0 ? 0 : s.cooperators;
        v.ms_since_sign_summon = bbnet::party::FromApi::instance().ms_since_request();
        v.other_hold_ms = bbnet::party::FromApi::instance().hold_left_ms();
        v.busy = dsummon::HostBusy(BossBarShown(), CutscenePlaying());  // A11: joiners wait outside
        // F5 (live): a member's game left our room in a network outage; ~5 s after its leave reached
        // us our game ended that session itself, and a room served in between was gone before the
        // guest got in. Until our game is out of it (a fresh room is created then), the guest waits.
        v.settling = Role() == RoleHost && s.cooperators <= 0 && bbnet::np::ms_since_member_left() < 8000;
        const std::string name = names.count(slot) ? names[slot] : std::string();
        // F5: a member back from a network outage: its sign hold ends once the scan for its old
        // game connection ran (NrDropAfterOutage, ~1.5 s after it was back); the answer waits for
        // that (the guest waits 4 s for it) instead of refusing the first direct summon.
        static std::map<int, double> deferred_at;
        if (v.other_hold_ms > 0 && NrDropPending()) {
            auto [it, fresh] = deferred_at.emplace(slot, now);
            if (fresh) Log("direct summon: slot %d (%s) waits for the old connection check (back after an outage)", slot, name.c_str());
            if (now - it->second < 3.0) {
                defer.push_back(slot);
                continue;
            }
        }
        deferred_at.erase(slot);
        if (const char* why = dsummon::HostRefusal(v)) {
            const dsummon::Busy wait = dsummon::RefusalWait(v);
            Send(slot, dsummon::EncodeRefuse(wait));
            {
                std::lock_guard<std::mutex> lk(DS().mu);
                DS().refused_wait = wait != dsummon::Busy::None;
                DS().refused_at = Now();
            }
            Log("direct summon: slot %d (%s) refused (%s; host %s, guest %s, wait %s)%s", slot, name.c_str(), why,
                MapName(v.host_map).c_str(), MapName(v.guest_map).c_str(), dsummon::BusyWord(wait),
                wait == dsummon::Busy::None ? ": the bell sign path goes on" : "");
            continue;
        }
        if (Role() != RoleHost) {
            static double last_create = -100.0;
            if (first == 0) first = now;
            if (now - last_create > 5.0 && Role() != RoleClient) {
                last_create = now;
                float arg = 0;
                SafeGet(u64(Guest(0x49271c8)), &arg);
                const int members = Field<int>(Slot(kSlotFlow), 0xc, 4);
                using Create = u8(BB_COOP_SYSV*)(float, u64, int);
                const bool ok = reinterpret_cast<Create>(Guest(0x1ecfcd0))(arg, Slot(kSlotSession), members) & 1;
                Log("direct summon: creating this host's room for slot %d -> %d", slot, int(ok));
            }
            if (now - first < 4.0) {
                keep.push_back(slot);
            } else {
                Send(slot, dsummon::EncodeRefuse());
                Log("direct summon: slot %d refused (no room here within 4 s)", slot);
                {
                    std::lock_guard<std::mutex> lk(DS().mu);
                    DS().refused_at = Now();
                    DS().refused_wait = false;
                }
            }
            continue;
        }
        dsummon::Room room;
        u32 h0 = 0, h1 = 0;
        RoomHead(7, &h0, &h1);
        if (!SerializeRoom(h0, h1, &room.data)) {
            Send(slot, dsummon::EncodeRefuse());
            Log("direct summon: slot %d refused (the room did not serialize)", slot);
            {
                std::lock_guard<std::mutex> lk(DS().mu);
                DS().refused_at = Now();
                DS().refused_wait = false;
            }
            continue;
        }
        const u64 net = Field<u64>(Guest(kNetMan), 0, 0);
        room.area = Field<u32>(net, 0xa7c, 0);
        room.level = 0;  // bbhost's FrpgNetMan +0xaa8 holds a float on our 1.09 (live: 0x3f49...): not sent
        SafeRead(net + 0xaac, room.pos, 12);
        // A4 (parab3 a4t2, live): a host left alone counts its room down (flow +0xB8, ~10 s after the
        // last member left; 0x1789B60 raises OnRoomTimeOut and the game leaves the room at 0). A
        // room served 27 ms before that ended was gone when the guest's JoinRoom came ("Room not
        // found"): an 8 s failure and the bell's 30 s fallback. A served room gets 10 s more.
        if (Role() == RoleHost && s.cooperators <= 0) {
            const u64 flow = Slot(kSlotFlow);
            const float left = Field<float>(flow, 0xb8, -1.0f);
            if (left >= 0.0f && left < 10.0f) {
                const float more = 10.0f;
                SafeWrite(flow + 0xb8, &more, 4);
                Log("direct summon: this host's empty room had %.1f s left before the game closes it: 10 s for the joiner",
                    double(left));
            }
        }
        if (!name.empty()) bbnet::party::FromApi::instance().hold_signs(name, 20000);
        Send(slot, dsummon::EncodeRoom(room));
        {
            std::lock_guard<std::mutex> lk(DS().mu);
            DS().served_at = now;
            DS().refused_wait = false;
        }
        Log("direct summon: slot %d (%s) gets this host's room (%zu bytes, area %#x, level %u); its signs are held", slot,
            name.c_str(), room.data.size(), room.area, room.level);
    }
    if (keep.empty()) first = 0;
    {
        std::lock_guard<std::mutex> lk(DS().mu);
        DS().requests.insert(DS().requests.end(), keep.begin(), keep.end());
        DS().requests.insert(DS().requests.end(), defer.begin(), defer.end());
    }
    // Guest: deliver and report.
    dsummon::Action a;
    dsummon::Room room;
    std::string host_name;
    double asked_at = 0;
    const char* fail_why = "";
    bool gather = false;
    {
        std::lock_guard<std::mutex> lk(DS().mu);
        const dsummon::Phase before = DS().g.phase;
        // Playtest 1 #9: a room of another map is never handed to the game (it would join without
        // loading the host's map and sit in its own world as a client): fail, gather, ring again.
        if (dsummon::RoomElsewhere(DS().g, now, DS().room.area, s.map_id,
                                   dsummon::CarriesAcrossMaps(DS().room.area, gather_dsum))) {
            a = dsummon::Action::ReportFail;
            Log("direct summon: the host's room is in %s, this guest in %s: not delivered",
                MapName(DS().room.area).c_str(), MapName(s.map_id).c_str());
        } else {
            const bool entry = s.world_up && !s.loading && WaitingGuestEntry() != 0;
            // Playtest 1 G4: say once per request what keeps a room from being handed over.
            static double blocked_logged = -1;
            if (DS().g.phase == dsummon::Phase::HaveRoom && !entry && Role() != RoleClient &&
                now - DS().g.since > dsummon::kRearmAfter - 0.5 && blocked_logged != DS().asked_at) {
                blocked_logged = DS().asked_at;
                Log("direct summon: the room waits: %s (world %s, loading %s, summon manager %s, bell 9005 %s)",
                    !s.world_up ? "no world" : s.loading ? "a load runs" : "the bell has no waiting summon entry",
                    s.world_up ? "up" : "down", s.loading ? "yes" : "no", SummonStepManager() ? "ok" : "none",
                    GuestBellEffectOn() ? "on" : "off");
            }
            a = dsummon::Step(DS().g, now, entry, Role() == RoleClient);
        }
        if (before == dsummon::Phase::Asked && DS().g.phase == dsummon::Phase::Failed) {
            Log("direct summon: no answer from the host within %.0f s: the bell sign path", dsummon::kAnswerWait);
        }
        fail_why = DS().g.fail_why;
        gather = DS().g.gather;
        room = DS().room;
        host_name = DS().names.count(0) ? DS().names[0] : std::string();
        asked_at = DS().asked_at;
    }
    // Rearm: the bell's effect down now, up again a moment later (a new ring makes a new entry).
    static double rearm_at = -1;
    if (a == dsummon::Action::Rearm) {
        const bool was_on = GuestBellEffectOn();
        const bool dropped = DropGuestBellEffect();
        rearm_at = now + 0.5;
        Log("direct summon: no waiting summon entry %.1f s after the room came: re-ringing the bell (SpEffect 9005 "
            "%s, %s)", dsummon::kRearmAfter, was_on ? "was on" : "was off", dropped ? "dropped" : "not on the list");
    } else if (rearm_at > 0 && now >= rearm_at) {
        rearm_at = -1;
        const bool ok = s.world_up && !s.loading && Role() != RoleClient && ApplyGuestBellEffect();
        Log("direct summon: bell re-rung: SpEffect 9005 %s", ok ? (GuestBellEffectOn() ? "on" : "applied, not listed")
                                                               : "NOT applied (no world / joined)");
    }
    if (a == dsummon::Action::Reask) {
        Send(0, dsummon::EncodeReq());
        Log("direct summon: asked the host again (its game was ending the session we left)");
    } else if (a == dsummon::Action::Deliver) {
        const bool ok = DeliverSummon(room, host_name);
        std::lock_guard<std::mutex> lk(DS().mu);
        dsummon::OnDelivered(DS().g, now, ok);
    } else if (a == dsummon::Action::ReportOk || a == dsummon::Action::ReportFail) {
        const bool ok = a == dsummon::Action::ReportOk;
        Send(0, dsummon::EncodeDone(ok));
        if (ok) {
            Log("direct summon: joined the host's room (%.1f s after the request)", now - asked_at);
            if (!dsummon::SameMap(room.area, s.map_id) && ((room.area >> 24) & 0xff) != 29) {
                g_carry_join_at = Now();  // A5: the arrival load into the host's map (CarryArrivalTick)
                Log("A5: joined from another map (own %s, the host's room in %s): the arrival load follows",
                    MapName(s.map_id).c_str(), MapName(room.area).c_str());
                g_carry_hold = true;  // K6 holds the game's own summon load until the arrival starts
            }
        } else {
            Log("direct summon: FAILED (%s; %.1f s after the request): %s", fail_why, now - asked_at,
                gather ? "the bell goes down; gather to the host's area first (loading screen), then ring again"
                       : "the bell sign path takes over");
        }
    }
}

void DirectSummonEvent(int from_slot, const std::string& body) {
    switch (dsummon::KindOf(body)) {
    case dsummon::Kind::Busy: {  // guest: A11, the host's boss fight / cutscene started or ended
        if (Role() == RoleHost) return;
        const dsummon::Busy b = dsummon::WaitOf(body);
        g_host_busy.store(static_cast<int>(b));
        party::bridge::OnHostBusy(dsummon::BusyText(b, false));
        Log("A11: the host's hold: %s%s", dsummon::BusyWord(b), b == dsummon::Busy::None ? " (its bell rings again)" : "");
        return;
    }
    case dsummon::Kind::Req: {
        if (!DirectSummonOn()) {
            Send(from_slot, dsummon::EncodeRefuse());
            Log("direct summon: slot %d refused (direct summon is off on this host, BB_PARTY_DIRECT_SUMMON=0): "
                "the bell sign path goes on", from_slot);
            return;
        }
        std::lock_guard<std::mutex> lk(DS().mu);
        DS().req_at = Now();
        if (std::find(DS().requests.begin(), DS().requests.end(), from_slot) == DS().requests.end())
            DS().requests.push_back(from_slot);
        return;
    }
    case dsummon::Kind::Room: {
        dsummon::Room r;
        bool refused = false;
        std::string err;
        const bool ok = dsummon::DecodeRoom(body, &r, &refused, &err);
        std::lock_guard<std::mutex> lk(DS().mu);
        if (ok) DS().room = r;
        const dsummon::Busy busy = refused ? dsummon::WaitOf(body) : dsummon::Busy::None;
        dsummon::OnRoom(DS().g, Now(), !ok, busy);
        if (busy != dsummon::Busy::None) party::bridge::OnHostBusy(dsummon::BusyText(busy, false));  // A11
        else if (ok) party::bridge::OnHostBusy(std::string());  // a wait (prologue / map) is over
        Log("direct summon: the host's answer: %s%s%s", ok ? "its room" : err.c_str(),
            busy != dsummon::Busy::None ? " - " : "", dsummon::BusyText(busy, false).c_str());
        return;
    }
    case dsummon::Kind::Done: {
        bool ok = false;
        dsummon::DecodeDone(body, &ok);
        std::string name;
        {
            std::lock_guard<std::mutex> lk(DS().mu);
            if (DS().names.count(from_slot)) name = DS().names[from_slot];
        }
        {
            std::lock_guard<std::mutex> lk(DS().mu);
            DS().done_at = Now();
            DS().done_ok = ok;
        }
        if (!name.empty()) {
            bbnet::party::FromApi::instance().hold_signs(name, ok ? 3000 : 1);
            if (!ok) bbnet::party::FromApi::instance().renew_signs(name);
        }
        Log("direct summon: slot %d (%s) reports %s", from_slot, name.c_str(),
            ok ? "it joined" : "a failure: its signs are offered again");
        return;
    }
    default:
        return;
    }
}

void TitleJoinFail(const char* why) {
    const int role = Role();
    if (role == 4 || role == RoleClient) {
        using Fn = u8(BB_COOP_SYSV*)(u64);
        reinterpret_cast<Fn>(Guest(kBeginLeave))(Slot(kSlotSession));
    }
    g_rejoin = false;
    std::lock_guard<std::mutex> lk(TJ().mu);
    TJ().phase = TitlePhase::Failed;
    Log("title join: FAILED (%s; role %d, flow %d); Continue loads the own world as before", why, role,
        Field<int>(Slot(kSlotFlow), 0x84, -1));
}

bool TitleJoinStart() {
    TitleJoin& t = TJ();
    std::vector<u8> room;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        room = t.room;
    }
    if (room.size() <= 8) {
        TitleJoinFail("the host sent no room");
        return false;
    }
    using Ctor = void(BB_COOP_SYSV*)(u64);
    using Read = u8(BB_COOP_SYSV*)(u64, u64);
    using Join = u8(BB_COOP_SYSV*)(u64, u64);
    alignas(16) u8 obj[0x200] = {};
    reinterpret_cast<Ctor>(Guest(kRoomCtor))(u64(obj));
    // DLMemoryReader as 0x1872360 builds it over the descriptor (after the 8-byte head).
    alignas(16) u64 r[6] = {};
    r[0] = u64(Guest(kReaderVtable));
    r[1] = room.size() - 8;
    r[2] = u64(room.data() + 8);
    r[3] = 0;
    reinterpret_cast<u8*>(r)[0x24] = 1;
    const bool read = reinterpret_cast<Read>(Guest(kRoomRead))(u64(obj), u64(r)) & 1;
    bool joined = false;
    if (read) {
        const u64 gsm = Slot(kSlotGsm);
        u32 h0 = 0, h1 = 0;
        std::memcpy(&h0, room.data(), 4);
        std::memcpy(&h1, room.data() + 4, 4);
        const u8 slot = u8(h1);
        SafeWrite(gsm + 0x16f0, &h0, 4);
        SafeWrite(gsm + 0x16f4, &slot, 1);
        g_rejoin = true;  // K5 / K6 quiet: no Lua join start at the title
        // 0x1789010 without its Lua part (SprjLuaEventMan does not exist at the title: its
        // singleton check panics): the join 0x1ED0670 and the reset of the host-world blobs.
        joined = reinterpret_cast<Join>(Guest(0x1ed0670))(Slot(kSlotSession), u64(obj)) & 1;
        for (u64 off : {u64(0x16a0), u64(0x1684)}) {
            const u32 zero = 0;
            if (Field<u64>(gsm, off + 8) == 0) {
                SafeWrite(gsm + off, &zero, 4);
            }
        }
        Log("title join: descriptor read (%zu bytes, head %08x %08x); JoinSession -> %d (role %d)", room.size(), h0, h1,
            int(joined), Role());
    }
    reinterpret_cast<Ctor>(Guest(kRoomDtor))(u64(obj));
    if (!read) {
        TitleJoinFail("the descriptor did not parse");
        return false;
    }
    if (!joined) {
        TitleJoinFail("JoinSession refused");
        return false;
    }
    std::lock_guard<std::mutex> lk(t.mu);
    t.phase = TitlePhase::Joining;
    t.since = Now();
    return true;
}

/// The flow update at the title (nothing else runs it there): only while the game's own does not
/// advance the flow timer (+0x98).
void TitleFlowUpdate(double now) {
    TitleJoin& t = TJ();
    const u64 flow = Slot(kSlotFlow);
    const float timer = Field<float>(flow, 0x98, -1.0f);
    const double dt = t.last_update > 0 ? std::min(0.1, std::max(0.0, now - t.last_update)) : 1.0 / 30.0;
    t.last_update = now;
    if (timer != t.last_timer || !flow) {
        t.last_timer = timer;
        return;  // the game updated it this frame (or no flow)
    }
    using Upd = void(BB_COOP_SYSV*)(float, u64);
    reinterpret_cast<Upd>(Guest(kFlowUpdate))(float(dt), flow);
    t.last_timer = Field<float>(flow, 0x98, -1.0f);
    if (++t.updates == 1) {
        Log("title join: running the flow update at the title (the game does not)");
    }
}

/// Guest, after Continue: the host-world load of a title join, then its arrival (flow 4).
void TitleJoinGuestTick(const GameSnapshot& s, double now) {
    TitleJoin& t = TJ();
    TitlePhase phase;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        phase = t.phase;
    }
    // The session type of a co-op summon (7: net chr type 1, a white phantom), which the summon load
    // passes and Continue's does not. Written only once the host's chr stands here (the first sync
    // is done): written at the arrival, the host's task for this guest stalled at
    // FirstSyncCompleteWait (live runs 9-11).
    // The session ended under a title-joined guest: it has no own world loaded (that load never
    // happened), so it goes home with a lamp warp to its own last lamp (GSM +0x1528): an own-world
    // load (flow is no longer 3).
    // Only when no home load of the game's own follows (a Leave or a send-home loads by itself,
    // with the return spot written below).
    static bool sent_home = false;
    static double ended_at = 0;
    const bool stranded = phase == TitlePhase::Done && !sent_home && s.world_up && !s.loading && Role() != RoleClient &&
                          Role() != 4 && Field<u8>(Slot(kSlotGsm), 0x1592) == 0;
    if (!stranded) {
        ended_at = 0;
    } else if (ended_at == 0) {
        ended_at = now;
    }
    if (stranded && now - ended_at > (g_title_home_now.load() ? 0.5 : 5.0)) {
        g_title_home_now = false;
        g_title_joined = false;
        sent_home = true;
        const u32 own = Field<u32>(Slot(kSlotGsm), 0x1528);
        if (own != 0 && own != kTravelNone && TravelLampKnown(own)) {
            Log("title join: the session ended (role %d); home to the own last lamp %u", Role(), own);
            // The world is still flagged the host's (GSM +0x1592 = 0): the lamp warp does nothing
            // there (live run 21). Own world from here on.
            const u8 one = 1;
            SafeWrite(Slot(kSlotGsm) + 0x1592, &one, 1);
            g_force_load_warp = true;  // the loaded world is the host's copy: a real load home
            RunLampWarpNotRest(own, "title join: going home");  // leaving the party is no rest
            g_force_load_warp = false;
        } else {
            Log("title join: the session ended (role %d); no own last lamp (%u) to go home to", Role(), own);
        }
    }
    static double last_diag = 0;
    if (((phase == TitlePhase::Done && now - t.since < 40.0) ||
         (!TitleJoinOn() && Role() == RoleClient && s.world_up && !s.loading)) &&
        now - last_diag > 3.0) {
        last_diag = now;
        const u64 gd = Field<u64>(Slot(kSlotGameData), 8);
        Log("title join: diag: flow %d, flow+0x10 %#llx, GameData+0x10 %d, +0xa4 %d, +0x5d8 %d; tasks:%s",
            Field<int>(Slot(kSlotFlow), 0x84, -1), ull(Field<u64>(Slot(kSlotFlow), 0x10)), Field<int>(gd, 0x10, -9),
            Field<int>(gd, 0xa4, -9), Field<int>(gd, 0x5d8, -9), DescribeTasks().c_str());
    }
    // The summoned guest's GameData (+0x10 2: the 0x168 record and the first-sync 0xC are sent
    // only with it; +0xA4 1 white phantom; +0x5D8 7 co-op) as soon as the load is in the host's
    // world (flow 4, the end of the MoveMapStep), not at the loading screen's end: the members'
    // data requests come then (four_players.md: a headless guest was counted ~48 s late).
    static bool gd_set = false;
    if ((phase == TitlePhase::Loading || phase == TitlePhase::Done) && !gd_set && Role() == RoleClient &&
        Field<int>(Slot(kSlotFlow), 0x84, -1) == 4 && Field<u64>(Slot(kSlotWorldChrMan), 0x60)) {
        if (const u64 gd = Field<u64>(Slot(kSlotGameData), 8)) {
            gd_set = true;
            const int two = 2, one = 1, seven = 7;
            SafeWrite(gd + 0x10, &two, 4);
            SafeWrite(gd + 0xa4, &one, 4);
            SafeWrite(gd + 0x5d8, &seven, 4);
            Log("title join: own GameData as a summoned guest's (+0x10 2, +0xA4 1, +0x5D8 7) at flow 4 (%s)",
                s.loading ? "loading screen still up" : "in the world");
        }
    }
    static bool typed = false;
    if (phase == TitlePhase::Done && !typed && s.world_up && !s.loading && Role() == RoleClient) {
        bool host_chr = false;
        for (const TaskInfo& k : Tasks(false)) {
            host_chr = host_chr || (k.mode == 2 && PlayerForPeer(k.task + kTaskPeer) && k.step == kStepUpdate);
        }
        (void)host_chr;
        typed = true;
        // The guest's own save spot back into the forced placement (what its save records as the
        // spawn: a later Continue starts there) and the return spot.
        bool own_have;
        u32 own_map;
        alignas(16) float own_pos[4], own_rot[4];
        {
            std::lock_guard<std::mutex> lk(t.mu);
            own_have = t.own_have;
            own_map = t.own_map;
            std::memcpy(own_pos, t.own_pos, sizeof own_pos);
            std::memcpy(own_rot, t.own_rot, sizeof own_rot);
        }
        if (own_have) {
            const u64 gsm = Slot(kSlotGsm);
            using PtrFn = void(BB_COOP_SYSV*)(const void*);
            reinterpret_cast<PtrFn>(Guest(0x156cf10))(&own_map);
            reinterpret_cast<PtrFn>(Guest(0x156cf20))(own_pos);
            reinterpret_cast<PtrFn>(Guest(0x156cf40))(own_rot);
            SafeWrite(gsm + 0x14a0, own_pos, 16);
            SafeWrite(gsm + 0x14b0, own_rot, 16);
            SafeWrite(gsm + 0x14c0, &own_map, 4);
            Log("title join: forced placement and return spot = own save spot map %#x (%.1f, %.1f, %.1f)", own_map,
                double(own_pos[0]), double(own_pos[1]), double(own_pos[2]));
        }
        // The return spot a summon writes from the sign (GSM +0x14A0 pos, +0x14B0 rot, +0x14C0 map;
        // 0x1872360): the guest's own last lamp (GSM +0x1528, known once the save is loaded). Without
        // it the guest's Leave loaded nothing and fell back to the title (live run 18).
        if (!own_have) {
            const u64 gsm = Slot(kSlotGsm);
            const u32 own = Field<u32>(gsm, 0x1528);
            float spot[4];
            if (own != 0 && own != kTravelNone && ids::ReturnSpot(own, spot)) {
                const float pos[4] = {spot[0], spot[1], spot[2], 1.0f};
                const float rot[4] = {0.0f, spot[3], 0.0f, 0.0f};
                const u32 map = ids::EntityMap(own);
                SafeWrite(gsm + 0x14a0, pos, 16);
                SafeWrite(gsm + 0x14b0, rot, 16);
                SafeWrite(gsm + 0x14c0, &map, 4);
                Log("title join: return spot = own last lamp %u (map %#x, %.1f, %.1f, %.1f)", own, map, pos[0], pos[1], pos[2]);
                // The forced placement too (what the guest's save records as its spawn): with the own
                // spot unknown (a save in the host's map; the record held m24_01 (0, 0, 0)) a later
                // solo Continue started at the map origin (live cont c4 fell through m24_01, c6 stood
                // at (0, 0, 0) in the Dream). The own last lamp is always a valid spawn.
                if (!(std::getenv("BB_PARTY_TITLE_OWN_LAMP_SPOT") && std::getenv("BB_PARTY_TITLE_OWN_LAMP_SPOT")[0] == '0')) {
                    alignas(16) float p16[4] = {pos[0], pos[1], pos[2], 1.0f}, r16[4] = {rot[0], rot[1], rot[2], rot[3]};
                    u32 m = map;
                    using PtrFn = void(BB_COOP_SYSV*)(const void*);
                    reinterpret_cast<PtrFn>(Guest(0x156cf10))(&m);
                    reinterpret_cast<PtrFn>(Guest(0x156cf20))(p16);
                    reinterpret_cast<PtrFn>(Guest(0x156cf40))(r16);
                    Log("title join: forced placement = own last lamp %u (the own save spot is not known)", own);
                }
            } else {
                Log("title join: no own last lamp (%u) for the return spot", own);
            }
        }
        // A summoned guest's own GameData as the summon load leaves it (live, vanilla join):
        // +0x10 2 (the 0x168 record and the first-sync packet 0xC are sent only with it set),
        // +0xA4 1 (white phantom, packet 0xB), +0x5D8 7 (session type).
        if (const u64 gd = Field<u64>(Slot(kSlotGameData), 8)) {
            const int two = 2, one = 1, seven = 7;
            SafeWrite(gd + 0x10, &two, 4);
            SafeWrite(gd + 0xa4, &one, 4);
            SafeWrite(gd + 0x5d8, &seven, 4);
            Log("title join: own GameData as a summoned guest's (+0x10 2, +0xA4 1, +0x5D8 7)");
        }
    }
    // Ready (Continue pressed, the load not started): the save's spawn is the forced placement
    // (GSM +0x14F0 map, +0x1500 pos, +0x1510 rot, +0x1520 on); a Continue loaded the guest's own
    // map (e.g. the Dream) in host-world mode, apart from the host (hostdeath agent). Every frame
    // until the load starts: a value that is not ours is the save's own spot (kept for the return
    // and for the save), then the host's spot beside it goes in.
    if (phase == TitlePhase::Ready && !s.loading) {
        const u64 gsm = Slot(kSlotGsm);
        u32 stage;
        float pos[4], rot[4];
        {
            std::lock_guard<std::mutex> lk(t.mu);
            stage = t.stage;
            std::memcpy(pos, t.pos, sizeof pos);
            std::memcpy(rot, t.rot, sizeof rot);
        }
        BesideHost(pos, rot, g_local_slot.load() > 0 ? g_local_slot.load() : 1);
        pos[3] = 1.0f;
        const u32 cur = Field<u32>(gsm, 0x14f0);
        if (cur != stage && cur != 0 && cur != 0xffffffffu) {
            std::lock_guard<std::mutex> lk(t.mu);
            t.own_have = true;
            t.own_map = cur;
            SafeRead(gsm + 0x1500, t.own_pos, 16);
            SafeRead(gsm + 0x1510, t.own_rot, 16);
            Log("title join: own save spot: map %#x (%.1f, %.1f, %.1f)", cur, double(t.own_pos[0]), double(t.own_pos[1]),
                double(t.own_pos[2]));
        } else if (cur == stage && cur != 0 && cur != 0xffffffffu) {
            // The own save is in the host's map (e.g. both in the Dream): the forced placement is
            // still the save's own spot (no host spot written over it yet). Without this the record
            // below (+0x15E0; m24_01 at (0, 0, 0) in a Dream save, live cont run) went into the
            // forced placement and the guest's next solo Continue fell through the world.
            alignas(16) float p0[4], r0[4];
            SafeRead(gsm + 0x1500, p0, 16);
            SafeRead(gsm + 0x1510, r0, 16);
            std::lock_guard<std::mutex> lk(t.mu);
            if (!t.own_have && (p0[0] != 0.0f || p0[1] != 0.0f || p0[2] != 0.0f)) {
                t.own_have = true;
                t.own_map = cur;
                std::memcpy(t.own_pos, p0, sizeof p0);
                std::memcpy(t.own_rot, r0, sizeof r0);
                Log("title join: own save spot (in the host's map): map %#x (%.1f, %.1f, %.1f)", cur, double(p0[0]),
                    double(p0[1]), double(p0[2]));
            }
        }
        {
            // The save's own spot record (+0x15B0 pos, +0x15C0 rot, +0x15E0 map; hostdeath's pos scan)
            // when the forced placement holds none.
            const u32 rec = Field<u32>(gsm, 0x15e0);
            std::lock_guard<std::mutex> lk(t.mu);
            alignas(16) float rp[4];
            SafeRead(gsm + 0x15b0, rp, 16);
            // A record at (0, 0, 0) is no spot (a Dream save held m24_01 (0, 0, 0) there).
            if (!t.own_have && rec != 0 && rec != 0xffffffffu && rec != stage &&
                (rp[0] != 0.0f || rp[1] != 0.0f || rp[2] != 0.0f)) {
                t.own_have = true;
                t.own_map = rec;
                SafeRead(gsm + 0x15b0, t.own_pos, 16);
                SafeRead(gsm + 0x15c0, t.own_rot, 16);
                Log("title join: own save spot (record): map %#x (%.1f, %.1f, %.1f)", rec, double(t.own_pos[0]),
                    double(t.own_pos[1]), double(t.own_pos[2]));
            }
        }
        if (Field<u32>(gsm, 0x15e0) != stage && t.own_have) {
            // The record is what Continue loads (its map: the guest's own, e.g. the Dream, with
            // the forced placement alone; live Dream run): the host's spot there too.
            SafeWrite(gsm + 0x15b0, pos, 16);
            SafeWrite(gsm + 0x15c0, rot, 16);
            SafeWrite(gsm + 0x15e0, &stage, 4);
            Log("title join: spawn record = the host's map %#x", stage);
        }
        if (cur != stage) {
            alignas(16) float p16[4], r16[4];
            std::memcpy(p16, pos, sizeof p16);
            std::memcpy(r16, rot, sizeof r16);
            using PtrFn = void(BB_COOP_SYSV*)(const void*);
            using VoidFn = void(BB_COOP_SYSV*)();
            reinterpret_cast<PtrFn>(Guest(0x156cf10))(&stage);
            reinterpret_cast<PtrFn>(Guest(0x156cf20))(p16);
            reinterpret_cast<PtrFn>(Guest(0x156cf40))(r16);
            reinterpret_cast<VoidFn>(Guest(0x156cf60))();
            Log("title join: forced placement = the host's map %#x beside the host (%.1f, %.1f, %.1f)", stage, double(pos[0]),
                double(pos[1]), double(pos[2]));
        }
    }
    if (phase == TitlePhase::Ready && s.loading) {
        std::lock_guard<std::mutex> lk(t.mu);
        t.phase = TitlePhase::Loading;
        Log("title join: loading (flow %d, GSM+0x1592 %u)", Field<int>(Slot(kSlotFlow), 0x84, -1),
            unsigned(Field<u8>(Slot(kSlotGsm), 0x1592)));
        return;
    }
    if (phase == TitlePhase::Loading && s.world_up && !s.loading) {
        g_rejoin = false;
        {
            std::lock_guard<std::mutex> lk(t.mu);
            t.phase = TitlePhase::Done;
            g_title_joined = true;
            // K5 (OnMatchingCheck: a member's chr missing) quiet for 15 s while the host builds
            // this guest's chr (live verification run 1: the guest left 0.1 s after arriving).
            g_split_change_at = now;
            t.done_load = now - t.since;
            t.since = now;
        }
        {
            u32 stage;
            float pos[4], rot[4];
            {
                std::lock_guard<std::mutex> lk(t.mu);
                stage = t.stage;
                std::memcpy(pos, t.pos, sizeof pos);
                std::memcpy(rot, t.rot, sizeof rot);
            }
            if (Field<u32>(s.player, 0x3f8) == stage) {
                BesideHost(pos, rot, g_local_slot.load() > 0 ? g_local_slot.load() : 1);
                WarpLocal(pos, rot[1], "title join: beside the host");
            }
        }
        Log("title join: in the world after %.1f s: map %s, flow %d, GSM+0x1592 %u, role %d, cooperators %d",
            t.done_load, MapName(s.map_id).c_str(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            unsigned(Field<u8>(Slot(kSlotGsm), 0x1592)), Role(), s.cooperators);
    }
}

} // namespace

std::string NrFindTimeouts(u64 base, std::size_t span) {
    std::vector<std::uint32_t> buf(span / 4);
    for (std::size_t chunk = 0; chunk < buf.size(); chunk += 256)
        if (!SafeRead(base + chunk * 4, buf.data() + chunk, 1024)) {
            buf.resize(chunk);
            break;
        }
    std::string out;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        const std::uint32_t v = buf[i];
        if (v == 25000 || v == 20000 || v == 25000000 || v == 20000000 || v == 3000 || v == 5000 || v == 6000 || v == 200) {
            char x[40];
            std::snprintf(x, sizeof x, " +%#zx=%u", i * 4, v);
            out += x;
        }
    }
    return out;
}

std::vector<NrTransport> NrTransports() {
    std::lock_guard<std::mutex> lk(g_nr_mu);
    return {g_nr.begin(), g_nr.end()};
}

// ---- C6: a crashed member's game connection, dropped through the transport's own release ----
namespace {
constexpr u64 kNrVtableUdpP2p = 0x52c51e0, kNrVtableUdp = 0x52c5160;  // FUN_027192e0 / FUN_02718d40
std::atomic<std::int64_t> g_nrd_at{0};  // steady ms from which to look (0 = nothing asked)
std::mutex g_nrd_mu;
std::string g_nrd_name;  // g_nrd_mu
std::atomic<bool> g_nrd_busy{false};
std::atomic<bool> g_nrd_outage{false};  // F5: the member is back after an outage (one scan)
bool g_nrd_listed = false;  // under g_nrd_busy

std::int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// On the game's thread, at a receive on one of its P2P sockets (bbnet::set_p2p_recv_hook).
void NrRecvHook(int sock, int where) {
    const std::int64_t at = g_nrd_at.load(std::memory_order_relaxed);
    if (!at) return;
    const std::int64_t now = SteadyMs();
    if (now < at) return;
    static const char* const kWhere[] = {"receive", "epoll wait", "send"};
    static std::atomic<int> logged[3];
    const bool outage = g_nrd_outage.load();
    if (logged[where].fetch_add(1) == 0)
        Log("C6: first %s (socket %d) since the %s", kWhere[where], sock, outage ? "outage" : "crash");
    if (where == 2) return;  // inside the transport's own update: not a safe place to release
    bool expect = false;
    if (!g_nrd_busy.compare_exchange_strong(expect, true)) return;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(g_nrd_mu);
        name = g_nrd_name;
    }
    if (now > at + 5000) {  // the hold (26 s) stays: the game times the connection out itself
        g_nrd_at.store(0);
        Log("C6: no game connection of %s found to drop; it times out by itself", name.c_str());
        g_nrd_busy.store(false);
        return;
    }
    int dropped = 0, seen = 0, transports = 0;
    std::size_t silent_n = 0;
    for (const NrTransport& t : NrTransports()) {
        u64 vt = 0, arr = 0;
        std::int32_t fd = -1;
        std::uint32_t cnt = 0;
        if (!t.transport || !SafeRead(t.transport, &vt, 8) || !SafeRead(t.transport + 0x120, &fd, 4) || fd < 0) continue;
        if (vt != Guest(kNrVtableUdpP2p) && vt != Guest(kNrVtableUdp)) continue;
        if (!SafeRead(t.transport + 0xd0, &arr, 8) || !SafeRead(t.transport + 0x6c, &cnt, 4) || !arr || cnt > 64) continue;
        u64 release = 0;
        if (!SafeRead(vt + 0x28, &release, 8) || !release) continue;
        ++transports;
        // After an outage the member's last datagram is as old as the outage (up to the 130 s slot
        // keep plus the reconnect), so the window is wider.
        const std::vector<nrdrop::Source> silent =
            nrdrop::SilentSources(bbnet::p2p_heard(fd), now, 1500, outage ? 180000 : 60000);
        silent_n += silent.size();
        for (std::uint32_t k = 0; k < cnt; ++k) {
            const u64 el = arr + k * 0xc0ull;
            std::uint8_t used = 0;
            std::uint8_t key[nrdrop::kKeySize] = {};
            if (!SafeRead(el + 0x2c, &used, 1) || !used || !SafeRead(el + 0x10, key, sizeof key)) continue;
            ++seen;
            std::uint16_t port = 0;
            std::memcpy(&port, key + 0x18, 2);
            const bool match = nrdrop::KeyMatchesAny(key, silent);
            if (match || !g_nrd_listed) Log("C6: socket %d connection %u to %u.%u.%u.%u:%u%s (at %s)", fd, k, key[0x13], key[0x12], key[0x11], key[0x10],
                static_cast<unsigned>(port), match ? (outage ? ": silent since the outage, released" : ": silent since the crash, released") : "",
                kWhere[where]);
            if (!match) continue;
            using Fn = void(BB_COOP_SYSV*)(u64, u64);
            reinterpret_cast<Fn>(release)(t.transport, el);
            ++dropped;
            if (!outage) {
                // C6 (parfin3): the restarted game comes back from this address and port; the F5
                // outage replay must not send it the dead session's records (title join refused).
                for (const nrdrop::Source& s : silent) {
                    if (!nrdrop::KeyMatchesAny(key, {s})) continue;
                    const std::size_t n = bbnet::p2p_forget_source(
                        std::uint64_t(s.addr) | (std::uint64_t(s.port_nbo) << 32));
                    Log("C6: its outage-replay state forgotten (%zu entr%s): no replay into its restarted game", n,
                        n == 1 ? "y" : "ies");
                }
            }
        }
    }
    g_nrd_listed = true;
    if (dropped) {
        g_nrd_at.store(0);
        Log("%s: %s %s: %d game connection(s) released (%zu silent source(s), %d in use)", outage ? "F5" : "C6",
            name.c_str(), outage ? "left our game's session during the outage" : "crashed", dropped, silent_n, seen);
        // Its restarted game can be summoned now: the sign hold ends in 1 s.
        bbnet::party::FromApi::instance().hold_signs(name, outage ? 1 : 1000);
    } else if (outage) {
        // One scan: an alive member sends again as soon as the network is back (not silent), a
        // member whose game left has no connection here any more; nothing to wait for either way.
        g_nrd_at.store(0);
        if (transports > 0) {
            bbnet::party::FromApi::instance().hold_signs(name, 1);
            Log("F5: %s is back after the outage: no silent game connection of it here (%d in use, %zu silent "
                "source(s)): its sign hold ends",
                name.c_str(), seen, silent_n);
        } else {
            Log("F5: %s is back after the outage: no game transport seen; its sign hold runs out by itself", name.c_str());
        }
    }
    g_nrd_busy.store(false);
}
}  // namespace

void NrDropSilentPeers(const std::string& name, int delay_ms) {
    {
        std::lock_guard<std::mutex> lk(g_nrd_mu);
        g_nrd_name = name;
    }
    g_nrd_outage = false;
    g_nrd_listed = false;
    bbnet::set_p2p_recv_hook(&NrRecvHook);
    Log("C6: %s crashed: looking for its game connection in %.1f s (%zu transport(s) seen)", name.c_str(),
        delay_ms / 1000.0, NrTransports().size());
    // The other members keep sending; the crashed one has been silent since the crash (the link
    // reset comes ~0.6 s after it). Its restarted game (same address) takes >3 s to send again. A
    // guest calls this once the restarted game is at its title (long silent already): delay 0.
    g_nrd_at.store(SteadyMs() + delay_ms);
}

void NrDropAfterOutage(const std::string& name) {
    {
        std::lock_guard<std::mutex> lk(g_nrd_mu);
        g_nrd_name = name;
    }
    g_nrd_listed = false;
    g_nrd_outage = true;
    bbnet::set_p2p_recv_hook(&NrRecvHook);
    Log("F5: %s is back after an outage while its sign hold runs: looking for a silent game connection of it in "
        "1.5 s (%zu transport(s) seen)",
        name.c_str(), NrTransports().size());
    // The network came back for the game's UDP too (with the link or before it): a member still in
    // our session sends again within a fraction of a second; one that left stays silent.
    g_nrd_at.store(SteadyMs() + 1500);
}

bool NrDropPending() {
    return g_nrd_at.load(std::memory_order_relaxed) != 0;
}

void SetTitleContinuing(bool on) {
    g_title_continuing = on;
}

bool TitleJoinPending() {
    std::lock_guard<std::mutex> lk(TJ().mu);
    return !TJ().requests.empty();
}

bool TitleJoinRequested() {
    return TitleJoinOn() && Requested();
}

bool TitleJoinHold(const GameSnapshot& s) {
    if (!TitleJoinOn() || !g_on.load()) {
        return false;
    }
    TitleJoin& t = TJ();
    const double now = Now();
    TitlePhase phase;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        phase = t.phase;
    }
    const int role = Role();
    const int fstate = Field<int>(Slot(kSlotFlow), 0x84, -1);
    if (role != t.last_role || fstate != t.last_state) {
        Log("title join: %s: role %d -> %d, flow %d -> %d (%.1f s)", TitlePhaseName(phase), t.last_role, role, t.last_state,
            fstate, t.since > 0 ? now - t.since : 0.0);
        t.last_role = role;
        t.last_state = fstate;
    }
    switch (phase) {
    case TitlePhase::Idle: {
        if (coop::SavedPrologue()) {
            // Playtest 1 #19: this save was still in its prologue: a title join would load it
            // straight into the host's world. Plain Continue into its own world; the bell summons
            // it once everybody is ready.
            static bool said = false;
            if (!said) {
                said = true;
                Log("title join: skipped: the save being continued is still in its prologue (#19); plain Continue");
            }
            return false;
        }
        Send(0, "{\"op\":\"title_req\"}");
        std::lock_guard<std::mutex> lk(t.mu);
        t.phase = TitlePhase::Requested;
        t.since = now;
        Log("title join: asking the host for its room");
        return true;
    }
    case TitlePhase::Requested: {
        bool have;
        {
            std::lock_guard<std::mutex> lk(t.mu);
            have = t.have_reply;
        }
        if (have && Role() >= 0 && Slot(kSlotGsm)) {
            TitleJoinStart();
            return true;
        }
        bool acked;
        {
            std::lock_guard<std::mutex> lk(t.mu);
            acked = t.acked;
        }
        if (!acked && now - t.since > 6.0) {
            TitleJoinFail("the host does not answer title joins (an older build, or off there)");
            return false;
        }
        // The host is not in its world yet (still at its title or loading): Continue the usual way
        // after 10 s (own world, then the bell joins once the host is up) instead of holding the
        // title for up to a minute.
        int host_state;
        {
            std::lock_guard<std::mutex> lk(t.mu);
            host_state = t.host_state;
        }
        // 10 s while the host still sits at its title; a host that is loading (its Continue
        // pressed) or in its world opens its room soon: up to 40 s then.
        if (now - t.since > (host_state == 1 ? 40.0 : 10.0)) {
            Send(0, "{\"op\":\"title_cancel\"}");
            TitleJoinFail(host_state == 1 ? "no room from the host within 40 s" : "the host is still at its title (10 s)");
            return false;
        }
        if (now - t.since > 70.0) {
            TitleJoinFail("no answer from the host within 70 s");
            return false;
        }
        return true;
    }
    case TitlePhase::Joining:
    case TitlePhase::Snapshot: {
        TitleFlowUpdate(now);
        const int st2 = Field<int>(Slot(kSlotFlow), 0x84, -1);
        if (phase == TitlePhase::Joining && Role() == RoleClient && st2 == 1) {
            std::lock_guard<std::mutex> lk(t.mu);
            t.phase = TitlePhase::Snapshot;
            Log("title join: client, waiting for the host's snapshot (%.1f s)", now - t.since);
        }
        if (st2 == 2) {
            // The snapshot is applied: the own-world serializer 0x131E5C0 has nothing to save (no
            // world); host-world load next with the summoned placement.
            const u64 gsm = Slot(kSlotGsm);
            const u64 flow = Slot(kSlotFlow);
            u32 stage;
            float pos[4], rot[4];
            {
                std::lock_guard<std::mutex> lk(t.mu);
                stage = t.stage;
                std::memcpy(pos, t.pos, sizeof pos);
                std::memcpy(rot, t.rot, sizeof rot);
                t.phase = TitlePhase::Ready;
            }
            BesideHost(pos, rot, g_local_slot.load() > 0 ? g_local_slot.load() : 1);
            const int three = 3;
            SafeWrite(gsm + 0x14c4, &stage, 4);
            SafeWrite(gsm + 0x14d0, pos, 16);
            SafeWrite(gsm + 0x14e0, rot, 16);
            SafeWrite(flow + 0x84, &three, 4);
            // GSM +0x1521 (what the Lua OnJoinMutiplay sets): the summoned placement +0x14C4 / +0x14D0
            // / +0x14E0 applies (else a Continue from another map put the guest at its own save's
            // coordinates inside the host's map). (The host-side stall seen with it in runs 9 / 10
            // was the missing GameData +0x10, fixed at the arrival.)
            const u8 one = 1;
            SafeWrite(gsm + 0x1521, &one, 1);
            Log("title join: host snapshot applied after %.1f s; flow 3, placement stage %#x pos (%.1f, %.1f, %.1f): "
                "Continue now loads the host's world",
                now - t.since, stage, pos[0], pos[1], pos[2]);
            return false;
        }
        if (Role() != 4 && Role() != RoleClient && now - t.since > 3.0) {
            TitleJoinFail("the join ended");
            return false;
        }
        // A snapshot comes ~1 s after the join (h5); 12 s leaves room for a slow link and costs 8 s
        // less when the join failed (the guest Continues into its own world and is summoned).
        if (now - t.since > 12.0) {
            TitleJoinFail("no snapshot within 12 s");
            return false;
        }
        return true;
    }
    case TitlePhase::Ready:
        return false;
    default:
        return false;
    }
}

bool TakeHostIrregularLeave() { return g_host_irregular_leave.exchange(false); }

bool Requested() {
    // Default in party mode (BB_PARTY set): B2, with B1 as the automatic fallback. BB_PARTY_B2=0: B1
    // only; BB_PARTY_B2=1 also without BB_PARTY (test fixtures).
    const char* v = std::getenv("BB_PARTY_B2");
    if (v && v[0]) {
        return EnvOn("BB_PARTY_B2");
    }
    return bb_party_enabled() != 0;
}

void SetLocalSlot(int slot) {
    g_local_slot = slot;
}

bool GuestPreRing(const GameSnapshot& s, std::uint32_t host_map) {
    return g_on.load() && PreRingSpacing(s, host_map);
}

bool HostHasMemberTasks() {
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode == 1) {
            return true;
        }
    }
    return false;
}

bool InOtherWorld() {
    const u64 gsm = Slot(kSlotGsm);
    return gsm && Field<u8>(gsm, 0x1592) == 0;
}

bool Enabled() {
    return g_on.load();
}

namespace {
bool SameMapLampWarp(std::uint32_t id, bool alone);
bool SameMapStageWarp(std::uint32_t map, std::uint32_t point);
} // namespace

void Init() {
    if (!Requested() || g_on.load()) {
        return;
    }
    if (!Image()) {
        Log("no image; B2 off");
        return;
    }
    g_force = EnvOn("BB_PARTY_B2_FORCE");
    // K1: the shadp2p XML retarget (call 0x1ED00A0, "is a session up") already keeps the room.
    if (Matches(kDtorLeaveCall, {0xe8, 0xea, 0x8e, 0x58, 0x00})) {
        g_k1_xml = true;
        Log("K1: XML \"Keep session on map reload\" already retargets +0x%llx", ull(kDtorLeaveCall));
    } else {
        InstallCall(kDtorLeaveCall, kBeginLeave, reinterpret_cast<const void*>(&DtorLeaveHook), "B2 K1 unload leave");
    }
    InstallCall(kDtorResetCall, kMultiPlayReset, reinterpret_cast<const void*>(&ResetHook), "B2 K2 CSMultiPlayMan reset");
    InstallCall(kUidLeaveCall, kBeginLeave, reinterpret_cast<const void*>(&UidLeaveHook), "B2 K3 stage-UID leave");
    InstallCall(kUidDropCall, kDropPresentation, reinterpret_cast<const void*>(&UidDropHook), "B2 K4 stage-UID drop");
    InstallCall(kMatchCheckCall, kBeginLeave, reinterpret_cast<const void*>(&MatchCheckHook),
                "B2 K5 OnMatchingCheck leave");
    InstallCall(kJoinStartCall, kJoinStartLua, reinterpret_cast<const void*>(&JoinStartHook), "B2 K6 OnBeJoinStart");
    InstallCall(kHostDeathReturnCall, kRequestReturn, reinterpret_cast<const void*>(&HostDeathReturnHook),
                "B2 K8 host death return-all");
    InstallCall(0x13858f5, kBeginLeave, reinterpret_cast<const void*>(&BossClearLeaveHook), "B2 K10 boss-clear leave (a)");
    InstallCall(kNrCreateCall, kNrCreate, reinterpret_cast<const void*>(&NrCreateHook), "C6 NR transport create");
    InstallCall(0x1385a95, kBeginLeave, reinterpret_cast<const void*>(&BossClearLeaveHook), "B2 K10 boss-clear leave (b)");
    InstallCall(kLockKickCall, kSessionKick, reinterpret_cast<const void*>(&LockKickHook), "B2 K10b cleared-session join kick");
    InstallCall(kLockSessionCall, kLockSession, reinterpret_cast<const void*>(&LockSessionHook), "B2 K10d boss-clear session lock");
    InstallCall(kFarewellCall, kLuaEventSchedule, reinterpret_cast<const void*>(&BossFarewellHook),
                "B2 K10c boss-clear farewell message (guest)");
    // Not B2-specific (B1 bell joins stream too): installed whenever B2 is.
    if (!HookPrologue(0x18cb0e0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                  0xec, 0xf8, 0x07, 0x00, 0x00},
                      &ChrPreStepHook, "remote chr stale map hit guard (0x18CB0E0)")) {
        Log("remote chr stale map hit guard: NOT installed (bytes differ)");
    }
    SetSendHomeVeto(&SendHomeVeto);
    SetGuestDeathHold(&HoldGuestDeath);
    SetHostDeathIntercept(&HostDeathIntercept);
    // Every BeginLeaveSession with a session logs its caller (which game path ends a party).
    // push rbp; mov rbp, rsp; push r15 (6 bytes, position-independent).
    if (!ReplacePrologue(kBeginLeave, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                         reinterpret_cast<const void*>(&LeaveEntryHook), &g_leave_orig, "B2 leave trace (0x1ED07A0)")) {
        g_leave_orig = nullptr;
    }
    InstallCall(0x17851ce, 0x13beca0, reinterpret_cast<const void*>(&FlagApplyHook), "B2 K11 flag snapshot apply");
    InstallBannerFilter();
    // F5: the session manager's send 0x17894E0 queues what the transport cannot take yet and raises
    // OnIrregularLeaveSession once the queues hold more than 1000 messages (cmp 0x3E9 at 0x17897AE;
    // network_robustness.md 7b): ~30 s of a stalled link. The limit is 30000 (a 50 s+ outage keeps
    // the session). Idea from deadinside's BBSC notes (docs/upstream/deadinside_0.6-pre1.md).
    // BB_PARTY_SEND_QUEUE_LIMIT=0: the game's 1001.
    if (!(std::getenv("BB_PARTY_SEND_QUEUE_LIMIT") && std::getenv("BB_PARTY_SEND_QUEUE_LIMIT")[0] == '0')) {
        PatchBytes(0x17897ae, {0xe9, 0x03, 0x00, 0x00}, {0x30, 0x75, 0x00, 0x00},
                   "F5 send queue limit 1001 -> 30000 (irregular leave)");
    }
    InstallEmptyStainSkip();
    SetEmevdSkipPredicate(&ClientIntroSkip);
    SetSameMapWarp(&SameMapLampWarp);
    SetSameMapStage(&SameMapStageWarp);
    EnsureEmevdFilter();
    Log("K7 funnel branch +0x%llx %s", ull(kFunnelLeaveJne),
        Matches(kFunnelLeaveJne, {0x75, 0x6c}) ? "verified (75 6c), written only around the guest's own warp"
                                                : "MISMATCH: guest re-join will leave the room");
    g_on = true;
    Log("on%s", g_force.load() ? " (BB_PARTY_B2_FORCE: K2 keeps tasks without a session)" : "");
}

/// A rewound task whose requested step never got applied (+0x54 != +0x50 for 2 s): the game's own
/// task update (0x1E556F0 from the flow update 0x1789B60) did not run it. Run one update of that
/// task ourselves, as 0x1E556F0 does: vfunc +0xD0 = 0x1E52FE0(task, &{0x52EFD40, dt}).
void KickStalledTasks(const GameSnapshot& s, double now) {
    static double pending_since = 0, last_kick = 0;
    if (!s.world_up || s.loading) {
        pending_since = 0;
        return;
    }
    bool stalled = false;
    for (const TaskInfo& t : Tasks(false)) {
        stalled = stalled || ((t.mode == 1 || t.mode == 2) && t.next == kStepCreate && t.step != kStepCreate);
    }
    if (!stalled) {
        pending_since = 0;
        return;
    }
    if (pending_since == 0) {
        pending_since = now;
        return;
    }
    const u64 remo = Field<u64>(Slot(0x5540058), 8);
    const bool remo_on = remo && (Field<u8>(remo, 0x168) & 1);
    // While SprjRemo's +0x168 bit 0 is set the flow update skips every task (0x1789B60); live run 8
    // showed it stuck at 1 on the guest after its host-world load (no cutscene visible), so after
    // 3 s we run the update ourselves anyway.
    if (now - pending_since < (remo_on ? 3.0 : 2.0) || now - last_kick < 1.0) {
        return;
    }
    last_kick = now;
    struct alignas(8) Dt {
        u64 vt;
        float dt;
    } dt{Guest(0x52efd40), 1.0f / 30.0f};
    for (const TaskInfo& t : Tasks(false)) {
        if ((t.mode == 1 || t.mode == 2) && t.next == kStepCreate && t.step != kStepCreate) {
            Log("kick: task mode %d stalled at step %d next %d for %.1f s (remo %d); running its update", t.mode,
                t.step, t.next, now - pending_since, int(remo_on));
            using Fn = void(BB_COOP_SYSV*)(u64, const Dt*);
            reinterpret_cast<Fn>(Guest(kTaskUpdate))(t.task, &dt);
        }
    }
}

/// BB_PARTY_B2_TEST_BOSS=<Lua event name> (e.g. BlockClear2): the guest raises it once the host's
/// chr has stood in its world for 5 s - the boss-clear path (K10 and the send-home veto) without
/// a boss fight. Logs the dispatcher's answer.
void TestBossTick(const GameSnapshot& s, double now) {
    static const char* env = std::getenv("BB_PARTY_B2_TEST_BOSS");
    static bool done = false;
    static double since = 0;
    if (!env || !env[0] || done || Role() != RoleClient || !s.world_up || s.loading) {
        return;
    }
    bool ready = false;
    for (const TaskInfo& t : Tasks(false)) {
        ready = ready || (t.mode == 2 && t.step == kStepUpdate && PlayerForPeer(t.task + kTaskPeer));
    }
    if (!ready) {
        since = 0;
        return;
    }
    if (since == 0) {
        since = now;
        return;
    }
    if (now - since < 5.0) {
        return;
    }
    done = true;
    if (std::strncmp(env, "hp=", 3) == 0) {
        // BB_PARTY_B2_TEST_BOSS=hp=<n>: the guest's HP set to n (to see it kept through a B2 load).
        const int hp = std::atoi(env + 3);
        const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
        if (dm && hp > 0) {
            SafeWrite(dm + 0xf8, &hp, 4);
        }
        Log("test hp: guest HP set to %d (max %d)", hp, Field<int>(dm, 0xfc, -1));
        return;
    }
    std::int64_t r = -1;
    const bool ok = LuaEventRaiseNow(env, &r);
    Log("test boss: Lua event %s raised (%s, dispatcher %lld); role %d", env, ok ? "ok" : "no event manager",
        static_cast<long long>(r), Role());
}

/// BB_PARTY_B2_TEST_WARP=<lamp id> | die: single-instance check of K2 - once an NPC task has existed
/// for 15 s in a steady world, warp once more (0x13CDF30) so the reset path runs with a task.
void TestWarpTick(const GameSnapshot& s, double now) {
    static const char* env = std::getenv("BB_PARTY_B2_TEST_WARP");
    static bool done = false;
    static double npc_since = 0;
    if (!env || !env[0] || done) {
        return;
    }
    // BB_PARTY_B2_TEST_HOSTWARP=1: the host instead, once a guest's chr has stood in its world.
    static const bool host_mode = EnvOn("BB_PARTY_B2_TEST_HOSTWARP");
    bool ready = !Tasks(true).empty();
    if (host_mode) {
        // BB_PARTY_B2_TEST_MEMBERS=<n> (default 1): wait for n guests' chrs in the host's world.
        static const int want = [] {
            const char* v = std::getenv("BB_PARTY_B2_TEST_MEMBERS");
            return v && v[0] ? std::atoi(v) : 1;
        }();
        int have = 0;
        if (Role() == RoleHost) {
            for (const TaskInfo& t : Tasks(false)) {
                have += (t.mode == 1 && t.step == kStepUpdate && PlayerForPeer(t.task + kTaskPeer)) ? 1 : 0;
            }
        }
        ready = have >= want;
    }
    if (!s.world_up || s.loading || !ready) {
        npc_since = 0;
        return;
    }
    if (npc_since == 0) {
        npc_since = now;
        return;
    }
    static const double delay = [] {
        const char* v = std::getenv("BB_PARTY_B2_TEST_DELAY");
        return v && v[0] ? std::atof(v) : 5.0;
    }();
    if (now - npc_since < delay) {
        return;
    }
    done = true;
    // BB_PARTY_B2_TEST_WARP=die: the local player dies instead (Lua ForceDead 0x13320C0(ctx, entity
    // 10000 = the local player): GetChrByEntityId, clear +0x1E3 bit 5, then the chr kill 0x18BF9A0).
    if (std::strcmp(env, "die") == 0) {
        if (!Matches(0x13320c0, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x05})) {
            Log("test die: ForceDead 0x13320C0 bytes differ; not run");
            return;
        }
        Log("test die: ForceDead(10000) with tasks:%s", DescribeTasks().c_str());
        using Fn = void(BB_COOP_SYSV*)(u64, u32);
        reinterpret_cast<Fn>(Guest(0x13320c0))(0, 10000);
        return;
    }
    const u32 id = u32(std::strtoul(env, nullptr, 10));
    Log("test warp: lamp %u with tasks:%s", id, DescribeTasks().c_str());
    using Fn = void(BB_COOP_SYSV*)(u32);
    reinterpret_cast<Fn>(Guest(0x13cdf30))(id);
}

void NoteOwnAppearanceChanged() {
    g_own_appearance_changed = true;
}

void Tick(const GameSnapshot& s) {
    if (!g_on.load()) {
        return;
    }
    const double now = Now();
    // Test aid (batch 6, BB_PARTY_TEST_TRAVEL_ON_SERVE=<lamp>): the host warps to that lamp 1.5 s
    // after it first served a room (a host travel while a member loads in, playtest 1 H1:3630).
    {
        static const u32 lamp = std::getenv("BB_PARTY_TEST_TRAVEL_ON_SERVE")
                                    ? static_cast<u32>(std::strtoul(std::getenv("BB_PARTY_TEST_TRAVEL_ON_SERVE"), nullptr, 10))
                                    : 0u;
        static bool done = false;
        double served = -1e9;
        if (lamp && !done && Role() == RoleHost && s.world_up && !s.loading) {
            std::lock_guard<std::mutex> lk(DS().mu);
            served = DS().served_at;
        }
        if (lamp && !done && served > 0 && now - served >= 1.5) {
            done = true;
            Log("test: travel on serve: the host warps to lamp %u %.1f s after it served its room", lamp, now - served);
            RunLampWarp(lamp);
        }
    }
    // A4 watchdog (guest): the game applied the host's snapshot (flow 2) and ran its Lua join start,
    // but no host-world load began. SummonReloadStart (0x1389440) loads only with presentation count
    // (GSM +0x16F8 +0x18) >= 2 and the own chr alive; otherwise it clears the summon type (+0x16F0)
    // and the join sits in flow 2 until the session times out (the 0.5 s bell timer stall,
    // join_timing.md). 1.5 s after that start, with the count at 2 now: the type back and the same
    // Lua start once more (twice at most per join). BB_PARTY_JOIN_RESTART=0 off.
    {
        static const bool on = !EnvOff("BB_PARTY_JOIN_RESTART");
        static double handled = -1;
        static int tries = 0;
        const double started = g_join_start_at.load();
        const u64 gsm = Slot(kSlotGsm);
        const int flow = Field<int>(Slot(kSlotFlow), 0x84, -1);
        if (on && Role() == RoleClient && flow == 2 && !s.loading && started > 0 && !g_rejoin.load() &&
            !g_carry_hold.load() && now - started > 1.5) {
            if (handled != started) {
                handled = started;
                tries = 0;
            }
            const u64 pres = Field<u64>(gsm, 0x16f8);
            const int count = Field<int>(pres, 0x18, -1);
            const u64 lua = g_join_lua.load();
            const bool alive = s.player && Field<int>(Field<u64>(Field<u64>(s.player, 0x3b0), 0x20), 0xf8, 0) > 0;
            if (tries < 2 && lua && count >= 2 && alive && now - started > 1.5 * (tries + 1)) {
                ++tries;
                const int type = Field<int>(gsm, 0x16f0, 0);
                const int want = g_join_type.load() ? g_join_type.load() : -1;
                if (type == 0) SafeWrite(gsm + 0x16f0, &want, 4);
                Log("A4: the join sits in flow 2 with no load %.1f s after the Lua join start (type %d, presentation "
                    "count %d now): the game's join start again (%d), type %d", now - started, type, count, tries, want);
                using Fn = void(BB_COOP_SYSV*)(u64);
                reinterpret_cast<Fn>(Guest(kJoinStartLua))(lua);
                g_join_start_at = now;
                handled = now;
            }
        }
    }
    // A4 join trace (BB_PARTY_JOIN_TRACE=1, guest): every 0.25 s from the client role until the
    // host-world load starts (at most 12 s): flow, summon type, presentation count, loading.
    {
        static const bool trace = EnvOn("BB_PARTY_JOIN_TRACE");
        static double client_since = -1, last = 0;
        static std::string last_line;
        const int role = Role();
        if (trace && role == RoleClient) {
            if (client_since < 0) client_since = now;
            if (now - client_since <= 12.0 && now - last >= 0.25) {
                last = now;
                const u64 gsm = Slot(kSlotGsm);
                const u64 pres = Field<u64>(gsm, 0x16f8);
                char b[200];
                std::snprintf(b, sizeof b, "flow %d, type %d, pres count %d used %d, loading %d, world %d, GSM+0x1592 %u",
                              Field<int>(Slot(kSlotFlow), 0x84, -1), Field<int>(gsm, 0x16f0, 0),
                              Field<int>(pres, 0x18, -1), Field<int>(pres, 0x14, -1), int(s.loading), int(s.world_up),
                              unsigned(Field<u8>(gsm, 0x1592)));
                if (b != last_line) {
                    last_line = b;
                    Log("join trace +%.2f s: %s", now - client_since, b);
                }
            }
        } else if (role != RoleClient) {
            client_since = -1;
            last_line.clear();
        }
    }
    static double last_status = 0;
    if ((Role() == RoleHost || Role() == RoleClient) && s.world_up && !s.loading && now - last_status >= 10.0) {
        last_status = now;
        const u64 remo = Field<u64>(Slot(0x5540058), 8);
        Log("status: local %s (%#llx), role %d, flow %d (timer %.2f, GSM+0x1521 %u, remo %d), rejoin %d, cooperators %d; "
            "tasks:%s; presence %s; %s",
            DescribeChr(s.player).c_str(), ull(s.player), Role(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            Field<float>(Slot(kSlotFlow), 0x98), unsigned(Field<u8>(Slot(kSlotGsm), 0x1521)),
            remo ? int(Field<u8>(remo, 0x168) & 1) : -1, int(g_rejoin.load()), s.cooperators, DescribeTasks().c_str(),
            DescribePresence(true).c_str(), DescribePresentation().c_str());
        Log("status: where%s; apart %d; enterleave%s", DescribeWhere().c_str(), g_apart.load(), DescribeEnterLeave().c_str());
        // Render-state probe (the invisible remote chr: "the health bar but no model"): raw bytes of
        // each remote chr and of its +0x3B0 modules' first entries, to diff a visible against an
        // invisible one (docs/party/split_travel.md 7).
        static const bool probe = std::getenv("BB_PARTY_RENDER_PROBE") && std::getenv("BB_PARTY_RENDER_PROBE")[0] == '1';
        for (const TaskInfo& t : probe ? Tasks(false) : std::vector<TaskInfo>{}) {
            const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
            if (!c) {
                continue;
            }
            auto hex = [](u64 base, u64 n) {
                std::string h;
                h.reserve(n * 2);
                for (u64 o = 0; o < n; o += 4) {
                    char b[12];
                    std::snprintf(b, sizeof b, "%08x", Field<u32>(base, o));
                    h += b;
                }
                return h;
            };
            const u64 mods = Field<u64>(c, 0x3b0);
            Log("render probe: chr %#llx mode %d chr[0..0x600] %s", ull(c), t.mode, hex(c, 0x600).c_str());
            for (u64 m = 0; m < 0x60; m += 8) {
                const u64 mod = Field<u64>(mods, m);
                if (mod) {
                    Log("render probe: chr %#llx module +0x%llx %#llx [0..0x200] %s", ull(c), ull(m), ull(mod), hex(mod, 0x200).c_str());
                }
            }
        }
    }
    {
        static double last_hit_log = 0;
        if (s.world_up && !s.loading && now - last_hit_log >= 0.2 && now - g_k3_at.load() < 15.0) {
            last_hit_log = now;
            for (const TaskInfo& t : Tasks(false)) {
                if (t.mode == 1 || t.mode == 2) {
                    if (const u64 c = PlayerForPeer(t.task + kTaskPeer)) {
                        Log("hit watch: remote chr %#llx %s", ull(c), DescribeHit(c).c_str());
                    }
                }
            }
            Log("hit watch: local %s", DescribeHit(s.player).c_str());
        }
    }
    {
        // A stale "host gone" timer: the session event handler 0x178B5E0 arms network flow +0xF4 =
        // 10 s when a member leaves a guest in the host's world (0x178D35D); the flow update
        // 0x1789B60 counts it down and then leaves (0x178A6BE). A guest that left on its own (a
        // Hunter's Mark) and is summoned again within those 10 s carried it into the new session
        // and was dropped 3 s after joining (live, edgetravel). A (re)join starts with it off
        // (-1, the flow ctor's value).
        static int prev_role = -1;
        const int role = Role();
        if ((role == 4 || role == 6) && prev_role != 4 && prev_role != 6) {
            const u64 flow = Slot(kSlotFlow);
            const float t = Field<float>(flow, 0xf4, -1.0f);
            if (flow && t > 0.0f) {
                const float off = -1.0f;
                SafeWrite(flow + 0xf4, &off, 4);
                Log("join: stale leave timer (flow +0xF4 = %.1f s from the last session) cleared", double(t));
            }
        }
        prev_role = role;
    }
    if (const u32 n = g_stale_hits.exchange(0, std::memory_order_relaxed)) {
        Log("remote chr on a streamed-out block: stale map hit cleared %u time(s); tasks:%s", n, DescribeTasks().c_str());
    }
    ProbeTick(s);
    DirectSummonTick(s, now);
    if (TitleJoinOn() || EnvOn("BB_PARTY_TITLE_JOIN_PROBE")) {
        TitleJoinHostTick(s);
        TitleJoinGuestTick(s, now);
    }
    TestWarpTick(s, now);
    KickStalledTasks(s, now);
    TestBossTick(s, now);
    ArrivalWatchTick(s, now);
    MemberSessionTypeTick(s);
    GuestChrTypeTick(s);
    BannerRejoinTick();
    EmptyStainTick();
    HostMemberWatchTick(s, now);
    FallbackCleanupTick(s);
    FallRecoveryTick(s, now);
    HeldDeathTick(s, now);
    LampSpotTick(s);
    HostDownTick(s, now);
    HostRevivedTick(s);
    JoinBesideTick(s, Now());
    CarryArrivalTick(s, Now());
    CarryGameDataTick(s, Now());
    SpectatorRemoveTick(s);
    SpectateTick(s, Now());
    OwnStainLogTick(s);
    OwnStainWatchTick(s);
    NgGuardTick(s);
    GuestStainTick(s);
    GuestDownNoticeTick(s);
    HostRestLoadTick(s, now);
    PinOwnSpotTick(s);
    PosScanTick(s, Now());
    RemoteChrRenderWatch(s, Now());
    MemberRevivedTick(s);
    AppearanceTick(s);
    ReviveRearmTick(s);
    BossClearInvincibleTick(s, now);
    DeadAfterClearTick(s, now);
    {
        GuestPhase gp;
        {
            std::lock_guard<std::mutex> lk(S().mu);
            gp = S().gphase;
        }
        if (gp == GuestPhase::Idle && S().hphase == HostPhase::Idle && !g_rejoin.load()) {
            SplitTick(s, now);
        }
        if (s.world_up && !s.loading) {
            if (SessionUp()) {
                RemoteVisibleTick(now);
            }
            RemovalTick(now);
            if (SessionUp()) {
                StaleMemberChrTick(now);
            }
            if (g_arrived_at > 0 && now >= g_arrived_at) {
                g_arrived_at = 0;
                Send(0, "{\"op\":\"arrived\"}");
            }
            if (g_record_request.exchange(false) && SessionUp()) {
                RebroadcastOwnRecord("a member asked (meet)");
            }
            if (SplitEnemiesOn()) {
                ApplyDeadEnemies(s);
                EnemyWatchTick(s, now);
            }
        }
    }
    if (Role() == RoleClient) {
        GuestTick(s, now);
    } else {
        GuestPhase gp;
        {
            std::lock_guard<std::mutex> lk(S().mu);
            gp = S().gphase;
        }
        if (gp != GuestPhase::Idle && Role() != RoleHost) {
            FallbackToB1("the session ended during the re-join");
        }
        HostTick(s, now);
    }
}

namespace {
/// The guest's B2 follow / re-join: flow state 1, then GuestTick. `main_thread`: state 1 now
/// (after dropping stale snapshot packets); else the next GuestTick enters it (the link thread
/// must not call the game). `solo`: the guest's own travel (split travel).
bool BeginRejoin(const TravelIntent& t, bool main_thread, bool solo) {
    State& st = S();
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.gphase != GuestPhase::Idle) {
            if (st.gphase == GuestPhase::AwayLoading || st.solo || solo) {
                return solo ? false : true;  // the guest's own travel runs: it keeps it
            }
            st.intent = t;  // newer destination; the running re-join takes it
            return true;
        }
        st.intent = t;
        st.have_intent = true;
        st.have_dest = false;
        st.screen_done = false;
        st.blobs_saved = false;
        st.solo = solo;
        st.gphase = GuestPhase::AwaitSnapshot;
        st.since = Now();
        st.enter_state1 = !main_thread;
    }
    g_rejoin = true;
    if (main_thread) {
        // Back to "joined, awaiting the host's snapshot" (event 8/9 state); the host's 0x11 / 0x10
        // are read by 0x1783160 only in state 1 (it also clears the 60 s state timer).
        DrainSnapshotQueue(solo ? "own travel" : "follow");
        const u64 flow = Slot(kSlotFlow);
        const int one = 1;
        const float zero = 0.0f;
        SafeWrite(flow + 0x84, &one, 4);
        SafeWrite(flow + 0x108, &zero, 4);
    }
    Log("guest: %s (%s); waiting in flow state 1 for the host's snapshot", solo ? "travels alone into the host's area"
                                                                               : "host travels",
        DescribeTravel(t).c_str());
    return true;
}
} // namespace

bool OnHostTravel(const TravelIntent& t) {
    if (!g_on.load() || Role() != RoleClient) {
        return false;
    }
    if (t.alone) {
        // Split travel: the host goes alone; this guest stays and does not load. Its chr here
        // goes (main thread, SplitTick); the two meet again when they share an area.
        {
            std::lock_guard<std::mutex> lk(S().mu);
            if (S().gphase != GuestPhase::Idle && !S().solo && S().gphase != GuestPhase::AwayLoading) {
                Log("guest: the host travels alone (%s) during a re-join; the re-join goes on", DescribeTravel(t).c_str());
                return true;
            }
        }
        g_host_left = true;
        g_host_alone_seen = Now();
        NoteAway(0);
        Log("guest: the host travels alone (%s); staying here (no load)", DescribeTravel(t).c_str());
        return true;
    }
    if (t.kind == TravelKind::HostDeath && !g_dead_follow.load()) {
        // B5: the host died in another area than this guest's (split up): its respawn load is
        // the host's own; this guest plays on where it is (ERSC: each respawns at their own grace,
        // the others play on). The two meet again when they share an area. In the host's area the
        // vanilla respawn load for everyone stays (C2: HostDead state, world reset).
        const GameSnapshot g = ReadGameState(false);
        const u32 host = HostMap();
        const bool idle = [] {
            std::lock_guard<std::mutex> lk(S().mu);
            return S().gphase == GuestPhase::Idle;
        }();
        if (g.world_up && !g.loading && deathrules::StayApartAtHostDeath(false, idle, AreaOf(host), AreaOf(g.map_id)) &&
            !deathrules::OptIn(std::getenv("BB_PARTY_SPLIT_FOLLOW_DEATH"))) {
            g_host_left = true;
            g_host_alone_seen = Now();
            NoteAway(0);
            Log("guest: the host died in %s, this guest is apart in %s: it respawns alone; staying here (no load)",
                MapName(host).c_str(), MapName(g.map_id).c_str());
            return true;
        }
    }
    {
        // The host's destination is the block this guest stands in (a host death or a stage warp
        // inside the map): no load for the guest (B4 / H1), only a move beside the host.
        const GameSnapshot g = ReadGameState(false);
        const std::uint32_t dest = DestMapOf(t);
        // BB_PARTY_INWORLD=1 (opt-in since playtest 1): that move without a load. Default: the
        // guest loads too (a host death in this map: the vanilla respawn load for everyone, which
        // also ends the guest's HostDead state and resets the world; triage bugs 6, 7).
        static const bool inworld = deathrules::OptIn(std::getenv("BB_PARTY_INWORLD"));
        if (inworld && dest != 0 && g.world_up && !g.loading && g.map_id != 0xffffffffu && (dest >> 16) == (g.map_id >> 16) &&
            !g_dead_follow.load() && !BossFightActive()) {  // during a boss fight: a real load (C7)
            std::lock_guard<std::mutex> lk(S().mu);
            if (S().gphase == GuestPhase::Idle) {
                S().intent = t;
                S().have_intent = true;
                S().have_dest = false;
                S().gphase = GuestPhase::InWorldFollow;
                S().since = Now();
                g_rejoin = true;
                Log("guest: host travels inside this map (%s): no load here, a move beside it once it is in",
                    DescribeTravel(t).c_str());
                return true;
            }
        }
    }
    return BeginRejoin(t, false, false);
}

void OnLinkEvent(int from_slot, const std::string& body) {
    if (!g_on.load()) {
        return;
    }
    if (body.find("\"hostng\"") != std::string::npos) {
        int ng = -1;
        const char* p = std::strstr(body.c_str(), "\"ng\":");
        if (Role() != RoleHost && p && std::sscanf(p, "\"ng\":%d", &ng) == 1 && ng >= 0 && ng <= 7) {
            if (g_link_host_ng.exchange(ng) != ng) {
                Log("NG guard: the host's NG cycle is %d", ng);
            }
        }
        return;
    }
    if (dsummon::KindOf(body) != dsummon::Kind::None) {
        DirectSummonEvent(from_slot, body);
        return;
    }
    if (body.find("\"title_req\"") != std::string::npos) {
        if (!TitleJoinOn()) {
            return;  // the guest's ack timeout sends it the usual way
        }
        Send(from_slot, "{\"op\":\"title_ack\"}");
        std::lock_guard<std::mutex> lk(TJ().mu);
        TJ().requests.push_back(from_slot);
        return;
    }
    if (body.find("\"title_cancel\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(TJ().mu);
        auto& r = TJ().requests;
        r.erase(std::remove(r.begin(), r.end(), from_slot), r.end());
        Log("title join: slot %d stopped waiting (Continue the usual way)", from_slot);
        return;
    }
    if (body.find("\"title_wait\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(TJ().mu);
        TJ().host_state = body.find("\"host\":0") != std::string::npos ? 0 : 1;
        return;
    }
    if (body.find("\"title_ack\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(TJ().mu);
        TJ().acked = true;
        return;
    }
    if (body.find("\"title_room\"") != std::string::npos) {
        const char* b = body.c_str();
        const char* pr = std::strstr(b, "\"room\":\"");
        std::vector<u8> room;
        const bool ok = pr && Unhex(pr + 8, &room);
        u32 stage = 0;
        float p[4] = {}, r[4] = {};
        const char* ps = std::strstr(b, "\"stage\":");
        const char* pp = std::strstr(b, "\"pos\":[");
        const char* pq = std::strstr(b, "\"rot\":[");
        const bool where = ps && pp && pq && std::sscanf(ps, "\"stage\":%u", &stage) == 1 &&
                           std::sscanf(pp, "\"pos\":[%f,%f,%f,%f]", &p[0], &p[1], &p[2], &p[3]) == 4 &&
                           std::sscanf(pq, "\"rot\":[%f,%f,%f,%f]", &r[0], &r[1], &r[2], &r[3]) == 4 &&
                           TravelMapKnown(stage) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
                           std::fabs(p[0]) < 1e5f && std::fabs(p[1]) < 1e5f && std::fabs(p[2]) < 1e5f;
        std::lock_guard<std::mutex> lk(TJ().mu);
        TJ().have_reply = true;
        TJ().room = ok && where ? room : std::vector<u8>{};
        TJ().stage = stage;
        std::memcpy(TJ().pos, p, sizeof p);
        std::memcpy(TJ().rot, r, sizeof r);
        const dsummon::Busy busy = dsummon::WaitOf(body);  // A11: refused for a boss fight / cutscene
        if (busy != dsummon::Busy::None) party::bridge::OnHostBusy(dsummon::BusyText(busy, false));
        Log("title join: the host's room: %zu bytes, stage %#x pos (%.1f, %.1f, %.1f)%s%s", room.size(), stage, p[0], p[1], p[2],
            ok && where ? "" : " (refused)", busy != dsummon::Busy::None ? (" - " + dsummon::BusyText(busy, false)).c_str() : "");
        return;
    }
    if (body.find("\"dest\"") != std::string::npos) {
        u32 stage = 0;
        float p[4] = {}, r[4] = {};
        const char* b = body.c_str();
        const char* ps = std::strstr(b, "\"stage\":");
        const char* pp = std::strstr(b, "\"pos\":[");
        const char* pr = std::strstr(b, "\"rot\":[");
        if (ps && pp && pr && std::sscanf(ps, "\"stage\":%u", &stage) == 1 &&
            std::sscanf(pp, "\"pos\":[%f,%f,%f,%f]", &p[0], &p[1], &p[2], &p[3]) == 4 &&
            std::sscanf(pr, "\"rot\":[%f,%f,%f,%f]", &r[0], &r[1], &r[2], &r[3]) == 4 &&
            TravelMapKnown(stage) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
            std::fabs(p[0]) < 1e5f && std::fabs(p[1]) < 1e5f && std::fabs(p[2]) < 1e5f) {
            std::lock_guard<std::mutex> lk(S().mu);
            S().have_dest = true;
            S().dest_stage = stage;
            std::memcpy(S().dest_pos, p, sizeof p);
            std::memcpy(S().dest_rot, r, sizeof r);
            Log("guest: host destination stage %#x pos (%.1f, %.1f, %.1f)", stage, p[0], p[1], p[2]);
        } else {
            Log("guest: bad destination event refused");
        }
        return;
    }
    if (body.find("\"record\"") != std::string::npos) {
        g_record_request = true;  // main thread: RebroadcastOwnRecord
        return;
    }
    if (body.find("\"dead\"") != std::string::npos) {
        unsigned map = 0;
        const char* pm = std::strstr(body.c_str(), "\"map\":");
        const char* pi = std::strstr(body.c_str(), "\"ids\":[");
        if (!pm || !pi || std::sscanf(pm, "\"map\":%u", &map) != 1 || !TravelMapKnown(map & 0xffff0000u)) {
            Log("split: bad dead-enemies event refused");
            return;
        }
        std::vector<u32> list;
        for (const char* p = pi + 7; *p && *p != ']' && list.size() < 128;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 10);
            if (end == p) {
                break;
            }
            if (EnemyReconcilable(u32(v), map)) {
                list.push_back(u32(v));
            }
            p = *end == ',' ? end + 1 : end;
        }
        std::sort(list.begin(), list.end());
        std::lock_guard<std::mutex> lk(g_enemy_dead_mu);
        if (g_dead_in.size() < 8) {
            g_dead_in.emplace_back(map, std::move(list));
        }
        return;
    }
    if (body.find("\"warp\"") != std::string::npos) {
        // The host's in-world party move (HostPartyWarpNoLoad): run on the main thread.
        unsigned id = 0;
        const char* pl = std::strstr(body.c_str(), "\"lamp\":");
        if (pl && std::sscanf(pl, "\"lamp\":%u", &id) == 1 && TravelLampKnown(id)) {
            std::lock_guard<std::mutex> lk(S().mu);
            S().warp_lamp = id;
            Log("guest: the host moved to lamp %u inside its map (no load); following", id);
        }
        return;
    }
    if (body.find("\"away\"") != std::string::npos) {
        float p[3] = {0, 0, 0};
        const char* pp = std::strstr(body.c_str(), "\"pos\":[");
        const bool have = pp && std::sscanf(pp, "\"pos\":[%f,%f,%f]", &p[0], &p[1], &p[2]) == 3 &&
                          std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        std::lock_guard<std::mutex> lk(S().mu);
        S().away_request = true;
        NoteAway(from_slot);
        S().away_have_pos = have;
        std::memcpy(S().away_pos, p, sizeof p);
        Log("host: slot %d travels alone into another area", from_slot);
        return;
    }
    if (body.find("\"rejoin\"") != std::string::npos || body.find("\"respawn\"") != std::string::npos) {
        const bool live = body.find("\"rejoin\"") != std::string::npos;
        float p[3] = {0, 0, 0};
        const char* pp = std::strstr(body.c_str(), "\"pos\":[");
        const bool have = pp && std::sscanf(pp, "\"pos\":[%f,%f,%f]", &p[0], &p[1], &p[2]) == 3 &&
                          std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        unsigned rest = 0;
        const char* pr = std::strstr(body.c_str(), "\"rest\":");
        if (live && pr && std::sscanf(pr, "\"rest\":%u", &rest) == 1 && rest != 0 && rest != kTravelNone) {
            // pt1polish: its rest rides on the re-join - the rest load runs first, this re-join after.
            g_rest_req_carry = true;
            g_rest_carry_at = Now();
            HostRestRequest(from_slot, rest);
        } else {
            rest = 0;
        }
        std::lock_guard<std::mutex> lk(S().mu);
        S().respawn_request = true;
        S().respawn_live = live;
        S().respawn_have_pos = have;
        std::memcpy(S().respawn_pos, p, sizeof p);
        Log("host: slot %d %s; re-sending the snapshot%s", from_slot,
            live ? "travels alone into this area" : "died alone and respawns in this world",
            rest ? " after the rest load it carries" : "");
        return;
    }
    if (body.find("\"spectating\"") != std::string::npos) {
        // A dead member in a boss-fight limbo follows a living one with its (dissolved) chr: its
        // copy here is removed meanwhile (no plate drifting along); its revive builds it again.
        float p[3] = {0, 0, 0};
        const char* pp = std::strstr(body.c_str(), "\"pos\":[");
        if (pp && std::sscanf(pp, "\"pos\":[%f,%f,%f]", &p[0], &p[1], &p[2]) == 3 && std::isfinite(p[0]) &&
            std::isfinite(p[1]) && std::isfinite(p[2])) {
            std::lock_guard<std::mutex> lk(g_member_revived_mu);
            if (g_spectators.size() < 8) {
                g_spectators.push_back({p[0], p[1], p[2]});
            }
        }
        Log("slot %d spectates (boss-fight limbo); its dead copy here is hidden until it respawns", from_slot);
        return;
    }
    if (body.find("\"op\":\"appearance\"") != std::string::npos) {
        float p[3] = {0, 0, 0};
        const char* pp = std::strstr(body.c_str(), "\"pos\":[");
        if (pp && std::sscanf(pp, "\"pos\":[%f,%f,%f]", &p[0], &p[1], &p[2]) == 3 && std::isfinite(p[0]) &&
            std::isfinite(p[1]) && std::isfinite(p[2])) {
            std::lock_guard<std::mutex> lk(g_member_revived_mu);
            if (g_appearance_todo.size() < 8) g_appearance_todo.push_back({Now() + 1.5, {p[0], p[1], p[2]}});
        }
        Log("slot %d changed its appearance: its chr here is rebuilt from its new record in 1.5 s", from_slot);
        return;
    }
    if (body.find("\"member_revived\"") != std::string::npos) {
        float p[3] = {0, 0, 0};
        const char* pp = std::strstr(body.c_str(), "\"pos\":[");
        if (pp && std::sscanf(pp, "\"pos\":[%f,%f,%f]", &p[0], &p[1], &p[2]) == 3 && std::isfinite(p[0]) &&
            std::isfinite(p[1]) && std::isfinite(p[2])) {
            std::lock_guard<std::mutex> lk(g_member_revived_mu);
            if (g_member_revived.size() < 8) {
                g_member_revived.push_back({p[0], p[1], p[2]});
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_dead_mu);
            g_dead_members.erase(from_slot);
        }
        Log("slot %d was revived in place (no load)", from_slot);
        return;
    }
    if (body.find("\"host_revived\"") != std::string::npos) {
        g_host_revived_note = true;
        Log("guest: the host was revived in place (no load)");
        return;
    }
    if (body.find("\"member_down\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(g_dead_mu);
        if (from_slot > 0 && from_slot < 16) {
            g_dead_members[from_slot] = Now();
        }
        g_last_down_at = Now();
        Log("host: slot %d is down (%zu member(s) down)", from_slot, g_dead_members.size());
        return;
    }
    if (body.find("\"arrived\"") != std::string::npos) {
        {
            std::lock_guard<std::mutex> lk(g_dead_mu);
            g_dead_members.erase(from_slot);
        }
        std::lock_guard<std::mutex> lk(S().mu);
        S().arrived = true;
        Log("host: slot %d arrived", from_slot);
    }
}

bool LampTravelIntent(std::uint32_t id, TravelIntent* t) {
    return g_on.load() && LampIntent(id, t);
}

bool HostHasMembers() {
    return Role() == RoleHost && HostHasMemberTasks();
}

namespace {
/// party_travel's same-map hook (every lamp warp): a lamp of the block the player stands in is an
/// in-world move - none at all when it already stands there (within 2 m: only the last-lamp record)
/// - and in a party the host's move takes the guests of this map along unless it travels alone.
/// A dead player keeps the game's death load (no revive here).
/// A scripted stage warp (EMEVD 2003[14] / Lua WarpNextStage) to warp point `point` of the loaded
/// map: an in-world move to the point's MSB region (ids::WarpSpot; the same region the game puts
/// the player at after the load). BB_PARTY_STAGE_INWORLD=1 (default off: not verified on a real
/// scripted warp yet). Not with members in the session (the party would not follow), not in a boss
/// fight, not for a point that is not in the loaded block's MSB.
bool SameMapStageWarp(std::uint32_t map, std::uint32_t point) {
    // 1: alone only; 2: also a host with members (the host moves alone, the guests stay in the map).
    static const int mode = std::getenv("BB_PARTY_STAGE_INWORLD") ? std::atoi(std::getenv("BB_PARTY_STAGE_INWORLD")) : 0;
    const bool on = mode > 0;
    float spot[4];
    if (!on || !g_on.load() || g_force_load_warp.load() || point == 0 || point == kTravelNone ||
        !ids::WarpSpot(point, spot) || !InLoadedBlock(map) || (ids::EntityMap(point) >> 16) != (map >> 16)) {
        return false;
    }
    if (Role() == RoleClient || (Role() == RoleHost && HostHasMemberTasks() && mode < 2)) {
        Log("stage warp to point %u of the loaded map %s: with a party, the game's own load", point, MapName(map).c_str());
        return false;
    }
    if (std::string boss; BossFightActive(&boss)) {
        return false;
    }
    const u64 player = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    if (!player || Field<int>(Field<u64>(Field<u64>(player, 0x3b0), 0x20), 0x128, 0) <= 0) {
        return false;
    }
    char why[64];
    std::snprintf(why, sizeof why, "stage warp, point %u", point);
    return WarpLocal(spot, spot[3], why);
}

bool SameMapLampWarp(std::uint32_t id, bool alone) {
    float spot[4];
    if (!g_on.load() || g_force_load_warp.load() || !LampInLoadedBlock(id, spot)) {
        return false;
    }
    // #22: nobody to travel with (a guest in its own world, its host never answered, or a host
    // alone): the lamp works as in single player - its own loading screen, the world resets.
    const int role = Role();
    if (!deathrules::PartyTravelActive(role == RoleClient, role == RoleHost, role == RoleClient, HostHasMembers())) {
        static std::atomic<std::uint32_t> logged{0};
        if (logged.exchange(id) != id) {
            Log("lamp %u in the loaded map: no party here (session role %d): the vanilla lamp warp", id, role);
        }
        return false;
    }
    const u64 player = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    if (!player || Field<int>(Field<u64>(Field<u64>(player, 0x3b0), 0x20), 0x128, 0) <= 0) {
        return false;
    }
    float p[3] = {};
    ChrPos(player, p);
    const float dx = p[0] - spot[0], dy = p[1] - spot[1], dz = p[2] - spot[2];
    TravelIntent t;
    if (dx * dx + dy * dy + dz * dz < 4.0f) {
        if (LampIntent(id, &t)) {
            SetOwnLastLamp(t);
        }
        Log("lamp %u: already standing at it (%.1f m): no warp, no load", id, double(std::sqrt(dx * dx + dy * dy + dz * dz)));
        return true;
    }
    if (Role() == RoleHost && !alone && HostHasMemberTasks()) {
        return HostPartyWarpNoLoad(id);
    }
    return WarpToLampNoLoad(id, false);
}
} // namespace

bool AtLampSpot(std::uint32_t id) {
    float spot[4], p[3] = {};
    const u64 player = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    if (!g_on.load() || !player || !LampInLoadedBlock(id, spot) || !ChrPos(player, p)) {
        return false;
    }
    const float dx = p[0] - spot[0], dy = p[1] - spot[1], dz = p[2] - spot[2];
    return dx * dx + dy * dy + dz * dz < 4.0f;
}

bool LampInLoadedMap(std::uint32_t id) {
    float spot[4];
    return g_on.load() && id != 0 && id != kTravelNone && LampInLoadedBlock(id, spot);
}

bool GuestSoloLampLoad(std::uint32_t id, bool mark, bool rest, bool* rest_carried) {
    TravelIntent t;
    if (!g_on.load() || Role() != RoleClient || !LampIntent(id, &t)) {
        return false;
    }
    Log("guest: lamp %u is in the loaded map: this guest's own loading screen (the host does not move)%s", id,
        rest ? "; a rest: the host's world gets a rest load first" : "");
    return TravelAlone(t, mark ? AloneWhy::HuntersMark : id == 2102950 ? AloneWhy::Lamp : AloneWhy::Headstone,
                       rest ? id : 0, rest_carried);
}

void HostRestRequest(int slot, std::uint32_t lamp) {
    if (slot < 0) {
        return;
    }
    g_rest_req_lamp = lamp;
    g_rest_req_at = Now();
    g_rest_req_slot = slot + 1;
    Log("host: slot %d rested at lamp %u (travels alone): a rest load is queued", slot, lamp);
}

bool WarpToLampNoLoad(std::uint32_t id, bool beside) {
    if (!g_on.load()) {
        return false;
    }
    // C7 / design decision 4: a rest during a boss fight is a real rest - a load resets the fight
    // and the enemies and takes the others out of the boss room - not an in-world move.
    if (std::string boss; BossFightActive(&boss)) {
        Log("lamp %u during a boss fight (%s): a real load (the fight and the enemies reset)", id, boss.c_str());
        return false;
    }
    if (!WarpToLampInWorld(id, beside, beside ? "party move" : "own move")) {
        return false;
    }
    TravelIntent t;
    if (LampIntent(id, &t)) {
        SetOwnLastLamp(t);  // a lamp reached in-world is the last lamp too (a Mark: unchanged anyway)
    }
    return true;
}

bool HostPartyWarpNoLoad(std::uint32_t id) {
    if (!g_on.load() || Role() != RoleHost) {
        return false;
    }
    if (std::string boss; BossFightActive(&boss)) {
        Log("host: lamp %u during a boss fight (%s): the party moves with a real load (fight and enemies reset)", id,
            boss.c_str());
        return false;
    }
    if (!WarpToLampInWorld(id, false, "the host's party move")) {
        return false;
    }
    if (TravelIntent t; LampIntent(id, &t)) {
        SetOwnLastLamp(t);
    }
    char b[64];
    std::snprintf(b, sizeof b, "{\"op\":\"warp\",\"lamp\":%u}", id);
    Send(-1, b);
    Log("host: party move to lamp %u inside the loaded map: no load; guests in this map follow in-world", id);
    return true;
}

void SetWhere(const std::vector<Where>& members) {
    g_where = members;
}

int MembersApart() {
    return g_apart.load();
}

const char* AloneWhyName(AloneWhy w) {
    switch (w) {
    case AloneWhy::Lamp: return "lamp";
    case AloneWhy::Headstone: return "headstone";
    case AloneWhy::HuntersMark: return "hunters_mark";
    case AloneWhy::Death: return "death";
    case AloneWhy::Join: return "join";
    }
    return "?";
}

void HostTravelAloneNext() {
    g_host_alone_at = Now();
    g_host_alone_next = true;
    SetNextHostTravelAlone(true);
}

/// The destination map of an intent: the packed map, else the lamp / last-lamp record's map
/// (ReturnPointParam ids AAB295k: map mAA_0B), 0 unknown.
std::uint32_t DestMapOf(const TravelIntent& t) {
    if (t.packed_map != kTravelNone && t.packed_map != 0 && TravelMapKnown(t.packed_map & 0xffff0000u)) {
        return t.packed_map;
    }
    std::uint32_t id = t.lamp_id;
    if (id == kTravelNone || id == 0) {
        id = static_cast<std::uint32_t>(t.last_lamp);
    }
    if (id == kTravelNone || id == 0) {
        return 0;
    }
    return ((id / 100000) << 24) | (((id / 10000) % 10) << 16);
}

bool TravelAlone(const TravelIntent& t, AloneWhy why, std::uint32_t rest_lamp, bool* rest_carried) {
    if (rest_carried) {
        *rest_carried = false;
    }
    if (!g_on.load() || Role() != RoleClient) {
        return false;
    }
    const GameSnapshot s = ReadGameState(false);
    const std::uint32_t dest = DestMapOf(t);
    if (!s.world_up || s.loading || !s.player || dest == 0) {
        Log("guest: travel alone (%s) refused: %s", AloneWhyName(why), dest == 0 ? "no destination map" : "not in the world");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(S().mu);
        if (S().gphase != GuestPhase::Idle) {
            Log("guest: travel alone (%s) refused: a re-join is running", AloneWhyName(why));
            return false;
        }
    }
    const std::uint32_t host = HostMap();
    float p[3] = {0, 0, 0};
    ChrPos(s.player, p);
    char pos[96];
    std::snprintf(pos, sizeof pos, "\"pos\":[%.3f,%.3f,%.3f]", p[0], p[1], p[2]);
    g_dead_follow = why == AloneWhy::Death;
    if (why == AloneWhy::Lamp || why == AloneWhy::Headstone) {
        SetOwnLastLamp(t);
    }
    if (host != 0 && AreaOf(host) == AreaOf(dest)) {
        // Into the host's area (the shared Dream included): a re-join, one load for this guest.
        if (!BeginRejoin(t, true, true)) {
            g_dead_follow = false;
            return false;
        }
        // pt1polish: a rest rides on the re-join (deathrules::RestRidesRejoin): the host runs its
        // rest load first, then sends the snapshot - one loading screen here, into the reset world.
        const bool carry = deathrules::RestRidesRejoin(rest_lamp != 0, AreaOf(dest), AreaOf(host));
        char rest[32] = "";
        if (carry) {
            std::snprintf(rest, sizeof rest, ",\"rest\":%u", rest_lamp);
            if (rest_carried) {
                *rest_carried = true;
            }
        }
        Send(0, std::string("{\"op\":\"rejoin\",") + pos + rest + "}");
        Log("guest: travels alone (%s) to %s, the host's area (%s): a re-join with the host's snapshot%s",
            AloneWhyName(why), MapName(dest).c_str(), MapName(host).c_str(),
            carry ? " after the host's rest load (one loading screen)" : "");
        return true;
    }
    // Elsewhere: the alone load, host-world mode, no snapshot. The destination as an exact
    // transform (the lamp's MSB revive spot, party_ids ReturnSpot): the lamp warp itself landed
    // the guest at the map origin of a non-existent block (live split3: m24_9999 asked for,
    // (0, -5, 0), an endless fall) - without the snapshot states 1-2 a host-world load does not
    // resolve the lamp's map. The transform is the path every B2 load uses.
    TravelIntent tt = t;
    {
        const std::uint32_t lamp = t.lamp_id != kTravelNone && t.lamp_id != 0 ? t.lamp_id
                                                                                : static_cast<std::uint32_t>(t.last_lamp);
        float spot[4];
        if (why == AloneWhy::Death || lamp / 100000 == 29 || !ids::ReturnSpot(lamp, spot)) {
            Log("guest: travel alone (%s) to %u refused: %s", AloneWhyName(why), lamp,
                why == AloneWhy::Death ? "a death outside the host's area is not an alone load yet"
                                       : "no known spot for that lamp");
            g_dead_follow = false;
            return false;
        }
        tt.kind = TravelKind::Transform;
        tt.has_pos = true;
        tt.pos_map = ids::EntityMap(lamp);
        tt.pos[0] = spot[0], tt.pos[1] = spot[1], tt.pos[2] = spot[2], tt.pos[3] = 1.0f;
        tt.rot[0] = 0.0f, tt.rot[1] = spot[3], tt.rot[2] = 0.0f, tt.rot[3] = 0.0f;
    }
    State& st = S();
    const u64 flow = Slot(kSlotFlow);
    if (Field<int>(flow, 0x84, -1) != 4) {
        Log("guest: travel alone refused: flow %d (not in the host's world)", Field<int>(flow, 0x84, -1));
        g_dead_follow = false;
        return false;
    }
    DrainSnapshotQueue("alone load");
    const u64 gsm = Slot(kSlotGsm);
    const u8 was = Field<u8>(gsm, 0x1521), zero = 0;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.intent = t;
        st.have_intent = true;
        st.solo = true;
        st.saved_1521 = was ? int(was) : -1;
        st.gphase = GuestPhase::AwayLoading;
        st.since = Now();
        st.load_seen = false;
        st.stable = 0;
    }
    if (was) {
        SafeWrite(gsm + 0x1521, &zero, 1);
    }
    {
        // GSM +0x1593 / +0x1594 are the host's packet-0x19 state (a summoned load's descriptor
        // takes the host's NG cycle +0x1594 while +0x1593 is set); +0x1598 / +0x159C are the own
        // cycle the summoned load saved for the next own-world load (0x1937570, 0x1943F10). An
        // alone load to a map the host is not in with them set built no map (live split5: a grey
        // screen in the Dream), so both are cleared for it; the NG guard (finding 3) puts the own
        // restore value back once this load is done (g_ng_alone_pending).
        g_ng_alone_pending = true;
        g_ng_alone_at = Now();
        Log("guest: alone load: GSM +0x1593 %u +0x1594 %#x +0x1598 %u +0x159c %#x, GameDataMan +0x68 %#x",
            unsigned(Field<u8>(gsm, 0x1593)), Field<u32>(gsm, 0x1594), unsigned(Field<u8>(gsm, 0x1598)),
            Field<u32>(gsm, 0x159c), Field<u32>(Slot(kSlotGameData), 0x68));
        static const bool keep = std::getenv("BB_PARTY_SPLIT_KEEP_1598") != nullptr;
        if (!keep) {
            SafeWrite(gsm + 0x1598, &zero, 1);
            SafeWrite(gsm + 0x1593, &zero, 1);
        }
    }
    g_rejoin = true;
    RestoreHostBlobs();
    {
        // The summoned placement GSM +0x14C4 stage / +0x14D0 position / +0x14E0 rotation still held
        // the host's last spot (m24_01) and a host-world load takes its stage from it whatever the
        // forced transform says (live split7 / split9: PlayerIns stage 0x18010000 at the Dream's
        // coordinates, area-24 resources, no world). The destination's goes there too.
        alignas(16) float p16[4] = {tt.pos[0], tt.pos[1], tt.pos[2], 1.0f};
        alignas(16) float r16[4] = {tt.rot[0], tt.rot[1], tt.rot[2], tt.rot[3]};
        Log("guest: alone load: summoned placement stage %#x -> %#x", Field<u32>(gsm, 0x14c4), tt.pos_map);
        WriteSummonedPlacement(gsm, &tt.pos_map, p16, r16);
    }
    const int three = 3;
    SafeWrite(flow + 0x84, &three, 4);
    Send(0, std::string("{\"op\":\"away\",") + pos + "}");
    bool ok;
    {
        FunnelKeepRoom keep;
        Log("guest: travels alone (%s) to %s, the host is in %s: alone load in host-world mode (K7 %s)", AloneWhyName(why),
            MapName(dest).c_str(), host ? MapName(host).c_str() : "an unknown map", keep.armed ? "armed" : "NOT armed");
        // The guest's own save keeps the FORCED placement (GSM +0x14F0 / +0x1500 / +0x1510) as its
        // spawn spot: a forced transform here made a later Continue start at the alone travel's
        // lamp (b2_seamless.md 6.13 (3)). As the B2 arrival does since then: the lamp warp itself
        // (the stage comes from the summoned placement +0x14C4 written above, which is what made it
        // land in a void before), no forced transform, and a chr warp onto the lamp's spot after the
        // arrival. BB_PARTY_SPLIT_ALONE_FORCED=1: the old forced transform.
        static const bool forced = std::getenv("BB_PARTY_SPLIT_ALONE_FORCED") &&
                                   std::getenv("BB_PARTY_SPLIT_ALONE_FORCED")[0] == '1';
        if (forced || t.lamp_id == kTravelNone || t.lamp_id == 0) {
            ok = ReplayTravelNow(tt);
        } else {
            g_alone_spot_valid = true;
            std::memcpy(g_alone_spot, tt.pos, sizeof g_alone_spot);
            g_alone_spot_yaw = tt.rot[1];
            TravelIntent lamp = t;
            lamp.kind = TravelKind::Lamp;
            lamp.has_pos = false;
            ok = ReplayTravelNow(lamp);
            Log("guest: alone load by the lamp warp itself (no forced transform: the own save spot stays)");
        }
    }
    if (!ok) {
        const int four = 4;
        SafeWrite(flow + 0x84, &four, 4);
        if (was) {
            SafeWrite(gsm + 0x1521, &was, 1);
        }
        g_rejoin = false;
        g_dead_follow = false;
        std::lock_guard<std::mutex> lk(st.mu);
        st.gphase = GuestPhase::Idle;
        st.solo = false;
        st.saved_1521 = -1;
        Log("guest: travel alone: no usable destination in %s", DescribeTravel(t).c_str());
        return false;
    }
    return true;
}

bool RespawnAloneAtOwnLamp() {
    const u32 own = Field<u32>(Slot(kSlotGsm), 0x1528);
    TravelIntent t;
    if (!g_on.load() || own == 0 || own == kTravelNone || !LampIntent(own, &t)) {
        Log("guest: own-lamp respawn refused (own last lamp %u)", own);
        return false;
    }
    return TravelAlone(t, AloneWhy::Death);
}

bool ReloadAtHostLamp(const char* why) {
    // The host's last lamp (into the host's area: a re-join beside the party, one loading screen);
    // its own last lamp when the host's is not known. Live A5 (pard4): a reload at the own lamp in
    // the boss's area restarted the fight there from the mirrored host-entered flag - a loop.
    const u32 host = GuestDeathRedirectLamp();
    const u32 own = Field<u32>(Slot(kSlotGsm), 0x1528);
    const u32 lamp = host != 0 && host != kTravelNone ? host : own;
    TravelIntent t;
    if (!g_on.load() || lamp == 0 || lamp == kTravelNone || !LampIntent(lamp, &t)) {
        Log("guest: %s: no alone load (host's last lamp %u, own %u)", why, host, own);
        return false;
    }
    Log("guest: %s: an alone load to %s lamp %u (a loading screen)", why, lamp == host ? "the host's last" : "its own last",
        lamp);
    return TravelAlone(t, AloneWhy::Lamp);
}

std::uint32_t MemberMap(int slot) {
    const Where* w = WhereOf(slot);
    return w && w->age_ms < 5000 ? w->map : 0u;
}

bool MemberApart(int slot) {
    const std::uint32_t m = MemberMap(slot);
    const std::uint32_t mine = LocalMap();
    return m != 0 && m != 0xffffffffu && mine != 0xffffffffu && AreaOf(m) != AreaOf(mine);
}

bool KillEnemyForTest(std::uint32_t id) {
    bool done = false;
    ForEachMapChr([&](u32 e, u64 chr) {
        const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
        if (e == id && dm && Field<int>(dm, 0x128, 0) > 0) {
            const int zero = 0;
            done = SafeWrite(dm + 0x128, &zero, 4);
        }
    });
    Log("test: enemy %u HP set to 0 %s", id, done ? "(the game's own death follows)" : "- not found alive");
    return done;
}

std::uint32_t NearestEnemyForTest(float max_m) {
    const GameSnapshot s = ReadGameState(false);
    if (!g_on.load() || !s.world_up || s.loading) {
        return 0;
    }
    int all = 0, ours = 0, alive = 0;
    std::string sample;
    ForEachMapChr([&](u32 id, u64 chr) {
        ++all;
        if (all <= 12) {
            float p[3] = {};
            ChrPos(chr, p);
            char b[64];
            std::snprintf(b, sizeof b, " %u(hp %d at %.0f,%.0f,%.0f)", id,
                          Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0x128, -1), p[0], p[1], p[2]);
            sample += b;
        }
        if (EnemyReconcilable(id, s.map_id)) {
            ++ours;
            alive += Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0x128, 0) > 0 ? 1 : 0;
        }
    });
    const u32 id = NearestEnemy(s, max_m);
    g_watch_enemy = id;
    g_watch_until = Now() + 4.0;
    Log("test: map chrs %d, of %s %d (%d alive); nearest within %.0f m: %u; first:%s", all, MapName(s.map_id).c_str(), ours,
        alive, double(max_m), id, sample.c_str());
    return id;
}

bool PopOutgoing(int* slot, std::string* body) {
    std::lock_guard<std::mutex> lk(S().mu);
    if (S().out.empty()) {
        return false;
    }
    *slot = S().out.front().first;
    *body = std::move(S().out.front().second);
    S().out.pop_front();
    return true;
}

bool NativeWarpLocal(std::uint64_t player, const float* pos16, const float* rot16, const char* why) {
    return NativeWarpLocalImpl(player, pos16, rot16, why);
}
bool LocalChrWarp(std::uint64_t player, const float* pos16, const float* rot16, const char* why) {
    return LocalChrWarpImpl(player, pos16, rot16, why);
}

} // namespace coop::b2

namespace coop::b2 {
void DirectSummonRing() {
    if (!DirectSummonOn() || !g_on.load() || Role() == RoleHost || Role() == RoleClient) return;
    std::lock_guard<std::mutex> lk(DS().mu);
    const dsummon::Phase p = DS().g.phase;
    if (p == dsummon::Phase::Asked || p == dsummon::Phase::HaveRoom || p == dsummon::Phase::Delivered) return;
    dsummon::OnAsked(DS().g, Now());
    DS().asked_at = Now();
    Send(0, dsummon::EncodeReq());
    Log("direct summon: asked the host for its room (rang the bell)");
}
bool DirectSummonTakeGather() {
    std::lock_guard<std::mutex> lk(DS().mu);
    if (DS().g.phase != dsummon::Phase::Failed || !DS().g.gather) return false;
    DS().g.gather = false;
    return true;
}
bool DirectSummonHoldsHostBell(double waiting_s) {
    std::lock_guard<std::mutex> lk(DS().mu);
    dsummon::HostBellView v;
    v.direct_on = DirectSummonOn() && g_on.load();
    v.waiting_s = waiting_s;
    const double now = Now();
    v.since_req = now - DS().req_at;
    v.since_served = now - DS().served_at;
    v.since_done = now - DS().done_at;
    v.done_ok = DS().done_ok;
    v.refused_wait = DS().refused_wait;
    v.since_refused = now - DS().refused_at;
    return dsummon::HostBellHeld(v);
}
double NowSeconds() {
    return Now();
}
double TitleRoomServedAt() {
    return g_title_served_at.load();
}
double LastMemberDownAt() {
    return g_last_down_at.load();
}
bool HostRejoinRunning() {
    return g_on.load() && Role() == RoleHost && g_rejoin.load();
}
bool RejoinRunning() {
    return g_on.load() && g_rejoin.load();
}
int LocalHp() {
    const u64 player = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    return player ? ChrHp(player) : -1;
}
bool HostTravelHeldForJoin(std::string* who) {
    if (!g_on.load() || Role() != RoleHost) return false;
    dsummon::HostTravelHoldView v;
    v.direct_on = DirectSummonOn();
    const double now = Now();
    {
        std::lock_guard<std::mutex> lk(DS().mu);
        v.since_served = now - DS().served_at;
        v.since_done = now - DS().done_at;
        v.done_ok = DS().done_ok;
    }
    v.since_title = now - g_title_served_at.load();
    int members = 0, missing = 0;
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode == 1) {
            ++members;
            missing += PlayerForPeer(t.task + kTaskPeer) ? 0 : 1;
        }
    }
    v.joiner_in_world = members > 0 && missing == 0;
    if (party::PartyLink* link = party::runtime_link()) {
        for (const party::RosterEntry& r : link->roster()) {
            if (r.slot != party::kHostSlot && r.connected &&
                (r.state == party::MemberState::Joining || r.state == party::MemberState::Loading)) {
                v.joiner_loading = true;
            }
        }
    }
    const bool held = dsummon::HostTravelHeld(v);
    if (held && who) {
        char b[160];
        if (v.since_title < v.since_served) {
            std::snprintf(b, sizeof b, "a member is joining from its title (room sent %.1f s ago, %d of %d member chr(s) missing%s)",
                          v.since_title, missing, members, v.joiner_loading ? ", its loading screen is up" : "");
        } else {
            std::snprintf(b, sizeof b, "a member is joining (room sent %.1f s ago, %s, %d of %d member chr(s) missing)",
                          v.since_served, v.since_done <= v.since_served ? (v.done_ok ? "joined" : "failed") : "no answer yet",
                          missing, members);
        }
        *who = b;
    }
    return held;
}
bool DirectSummonCarries() {
    std::lock_guard<std::mutex> lk(DS().mu);
    return !dsummon::GatherFirst(DirectSummonOn() && g_on.load(), DS().g.phase, DS().g.waited);
}
void HostBusyChanged(bool boss_bar_up, bool cutscene_playing) {
    const dsummon::Busy b = dsummon::HostBusy(boss_bar_up, cutscene_playing);
    party::bridge::OnHostBusy(dsummon::BusyText(b, true));
    Send(-1, dsummon::EncodeBusy(b));  // every guest: why it waits (or that it ended)
}

void DirectSummonName(int slot, const std::string& name) {
    std::lock_guard<std::mutex> lk(DS().mu);
    DS().names[slot] = name;
}
}  // namespace coop::b2
