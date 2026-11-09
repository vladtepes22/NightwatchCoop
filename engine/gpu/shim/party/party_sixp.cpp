// SPDX-License-Identifier: GPL-3.0-or-later
// Six players: the patch set and the read-only probe (party_sixp.h, docs/party/six_players.md).
#include "party_sixp.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "../../party_env.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace coop::sixp {

namespace {

std::atomic<bool> g_active{false};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party 6p: ") + line + "\n");
}

constexpr std::uint64_t kWorldChrMan = 0x553e878, kGameDataMan = 0x553b130, kNetFlow = 0x5556678;
unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }

// Stage 2: the GameDataMan init 0x14e4e80(this) builds the rows; its allocation size and count are patched to 5 rows, this
// post-init hook constructs the 5th row (PlayerGameData ctor 0x14f1510(row, 0, -1), the same call as the four unrolled ones) and
// replaces the 4-byte occupancy array at +0x18 by a zeroed 8-byte one (slot 4 = byte 4). All fresh at this point.
using GdmInit = void(BB_COOP_SYSV*)(std::uint64_t);
using RowCtor = void(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t);
using Alloc = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t);
GdmInit g_gdm_original = nullptr;
BB_COOP_SYSV void GdmInitHook(std::uint64_t gdm) {
    g_gdm_original(gdm);
    std::uint64_t rows = 0;
    if (!SafeGet(gdm + 0x10, &rows) || !rows) {
        Log("GameDataMan init: no rows pointer; the 5th row is not made");
        return;
    }
    const std::uint64_t row4 = rows + 4 * 0x6a0;
    reinterpret_cast<RowCtor>(Guest(0x14f1510))(row4, 0, 0xffffffffull);
    std::uint64_t alloc_obj = 0, vtbl = 0, fn = 0;
    SafeGet(Guest(0x5540408), &alloc_obj);
    if (alloc_obj) SafeGet(alloc_obj, &vtbl);
    if (vtbl) SafeGet(vtbl + 0x58, &fn);
    std::uint64_t occ = fn ? reinterpret_cast<Alloc>(fn)(alloc_obj, 8, 1) : 0;
    if (!occ) {
        Log("GameDataMan init: the occupancy array could not be allocated");
        return;
    }
    const std::uint64_t zero = 0;
    SafeWrite(occ, &zero, 8);
    SafeWrite(gdm + 0x18, &occ, 8);
    Log("GameDataMan: row 4 built at %#llx, occupancy array %#llx (5 rows)", ull(row4), ull(occ));
}

// Stage 3: the presentation object. Slot 5 (+0x80..+0x93) takes {handle -1, type 1, state 0, -1, -1} like every empty slot, the tracked
// handles (+0xb0..+0xbf) are -1 and their count (+0xc0) 0.
void InitExtra(std::uint64_t obj) {
    const std::uint32_t slot5[5] = {0xffffffffu, 1u, 0u, 0xffffffffu, 0xffffffffu};
    const std::uint32_t handles[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    const std::uint32_t zero = 0;
    SafeWrite(obj + 0x80, slot5, sizeof slot5);
    SafeWrite(obj + 0xb0, handles, sizeof handles);
    SafeWrite(obj + 0xc0, &zero, 4);
}
using Ctor = void(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t);
Ctor g_pres_ctor_original = nullptr;
BB_COOP_SYSV void PresCtorHook(std::uint64_t obj, std::uint64_t arg) {
    g_pres_ctor_original(obj, arg);
    InitExtra(obj);
}
using Reset = void(BB_COOP_SYSV*)(std::uint64_t);
BB_COOP_SYSV void PresResetHook(std::uint64_t obj) {
    reinterpret_cast<Reset>(Guest(0x15bcfc0))(obj);
    InitExtra(obj);
}
// The game keeps the slots packed (readers stop at the first empty one): its release 0x15bc080 compacts slots 0-3 from later
// ones; slot 5 into slot 4 is the one move it has no block for. After the original: slot 4 empty and slot 5 used -> move it.
using Release = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t);
Release g_release_original = nullptr;
BB_COOP_SYSV std::uint64_t PresReleaseHook(std::uint64_t obj, std::uint64_t handle) {
    const std::uint64_t r = g_release_original(obj, handle);
    static const bool skip = std::getenv("BB_PARTY_SIX_NOCOMPACT") != nullptr;  // A/B test aid (round 6)
    if (skip) return r;
    std::uint32_t s4 = 0, s5 = 0;
    if (SafeGet(obj + 0x6c, &s4) && SafeGet(obj + 0x80, &s5) && s4 == 0xffffffffu && s5 != 0xffffffffu) {
        std::uint32_t slot[5] = {};
        if (SafeRead(obj + 0x80, slot, sizeof slot)) {
            SafeWrite(obj + 0x6c, slot, sizeof slot);
            const std::uint32_t empty[5] = {0xffffffffu, 1u, 0u, 0xffffffffu, 0xffffffffu};
            SafeWrite(obj + 0x80, empty, sizeof empty);
            reinterpret_cast<Reset>(Guest(0x15bce10))(obj);  // the member counts again
            Log("presentation: slot 5 moved to slot 4 (handle %#x)", slot[0]);
        }
    }
    return r;
}

} // namespace

BB_COOP_SYSV std::uint64_t MenuIdFilter(std::uint64_t obj, std::uint64_t id);

bool SixpActive() {
    return g_active.load();
}

void SixpInit() {
    if (const char* pr = std::getenv("BB_PARTY_SIX_PROBE"); pr && pr[0] == '1' && bb_party_enabled() && Image()) {
        if (!HookValueFilter(0x1bf993e, {0x4c, 0x63, 0xb4, 0x88, 0xc8, 0x15, 0x00, 0x00}, {0x48, 0x89, 0xc7, 0x4c, 0x89, 0xf6},
                             &MenuIdFilter, 0x1bf9946, "6p probe menu event id")) {
            Log("menu id probe not installed");
        }
    }
    if (!bb_party_enabled() || !EnabledFromEnv()) {
        return;
    }
    if (!Image()) {
        Log("no image; the 6-seat set is off");
        return;
    }
    const int stage = StageFromEnv();
    const std::vector<Site> sites = AllSites(stage);
    std::string why;
    if (!SitesSane(sites, &why)) {
        Log("the site list is not sane (%s); the 6-seat set is off", why.c_str());
        return;
    }
    // Whole or not at all: every site must hold the 1.09 bytes (or already the patched ones) before the first write.
    for (const Site& s : sites) {
        if (!Matches(s.off, s.from.data(), s.from.size()) && !Matches(s.off, s.to.data(), s.to.size())) {
            Log("site %#llx (%s) differs from 1.09: the 6-seat set is off (the party stays at 5 seats)", ull(s.off), s.what);
            return;
        }
    }
    if (stage >= 2) {  // the hook first: its prologue is compared by ReplacePrologue, nothing is written when it differs
        void* original = nullptr;
        if (!ReplacePrologue(0x14e4e80, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                             reinterpret_cast<const void*>(&GdmInitHook), &original, "6p GameDataMan init (5th row)")) {
            Log("the GameDataMan init differs from 1.09: the 6-seat set is off");
            return;
        }
        g_gdm_original = reinterpret_cast<GdmInit>(original);
    }
    if (stage >= 3) {
        const std::initializer_list<std::uint8_t> p13 = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
        void* o1 = nullptr;
        void* o2 = nullptr;
        if (!ReplacePrologue(0x15ba630, p13, reinterpret_cast<const void*>(&PresCtorHook), &o1, "6p presentation ctor (slot 5, tracked handles)") ||
            !ReplacePrologue(0x15bc080, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53},
                             reinterpret_cast<const void*>(&PresReleaseHook), &o2, "6p presentation release (slot 5 -> 4)") ||
            !HookCallSite(0x1942312, 0x15bcfc0, reinterpret_cast<const void*>(&PresResetHook), "6p presentation reset (slot 5, tracked handles)")) {
            Log("a presentation hook differs from 1.09: the 6-seat set is off");
            return;
        }
        g_pres_ctor_original = reinterpret_cast<Ctor>(o1);
        g_release_original = reinterpret_cast<Release>(o2);
    }
    int n = 0;
    for (const Site& s : sites) {
        if (!PatchBytes(s.off, s.from.data(), s.to.data(), s.from.size(), s.what)) {
            Log("write of %#llx (%s) failed after the check: the 6-seat set is half in, the party must not run", ull(s.off), s.what);
            return;
        }
        ++n;
    }
    g_active.store(true);
    Log("%d site(s) patched, stage %d (%s)", n, stage, kRulesTag);
}

// Crash hunt (fp6, 2026-11-07): the menu singleton 0x5562878 holds an event queue of 17 ints at +0x15c8 (values 1..17, read/write index
// +0x160c / +0x1610); a 6th-seat run died reading the id 503 from it. With BB_PARTY_SIX_PROBE=1 every tick checks it and logs the
// first bad state with the bytes around it, so the writer can be found.
void CheckMenuQueue() {
    static int logged = 0;
    std::uint64_t menu = 0;
    if (!SafeGet(Guest(0x5562878), &menu) || !menu) return;
    std::int32_t q[17] = {}, rd = 0, wr = 0;
    if (!SafeRead(menu + 0x15c8, q, sizeof q) || !SafeGet(menu + 0x160c, &rd) || !SafeGet(menu + 0x1610, &wr)) return;
    // every change of the queue state (first 300): the last lines before a crash show the id that killed it
    static std::int32_t last[19] = {};
    static int changes = 0;
    std::int32_t now[19];
    std::memcpy(now, q, sizeof q);
    now[17] = rd;
    now[18] = wr;
    if (changes < 300 && std::memcmp(now, last, sizeof now) != 0) {
        std::memcpy(last, now, sizeof now);
        ++changes;
        Log("menu queue: read %d write %d [%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d]", rd, wr, q[0], q[1], q[2], q[3], q[4], q[5], q[6],
            q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15], q[16]);
    }
    bool bad = rd < 0 || rd > 16 || wr < 0 || wr > 16;
    for (int v : q) bad = bad || v < 0 || v > 17;
    if (!bad || logged >= 3) return;
    ++logged;
    std::uint8_t raw[0xa0] = {};
    SafeRead(menu + 0x1580, raw, sizeof raw);
    char hex[0xa0 * 2 + 1];
    for (int i = 0; i < 0xa0; ++i) std::snprintf(hex + i * 2, 3, "%02x", raw[i]);
    Log("MENU QUEUE CORRUPT: object %#llx read %d write %d queue %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d; bytes +0x1580: %s", ull(menu), rd, wr,
        q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15], q[16], hex);
}

// Fast sampler for the same queue (BB_PARTY_SIX_PROBE=1): the main-thread tick misses ids the menu thread consumes within a frame.
void QueueSampler() {
    int logs = 0;
    std::int32_t prev[17] = {};
    while (logs < 40) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::uint64_t menu = 0;
        if (!SafeGet(Guest(0x5562878), &menu) || !menu) continue;
        std::int32_t q[17] = {}, rd = 0, wr = 0;
        if (!SafeRead(menu + 0x15c8, q, sizeof q) || !SafeGet(menu + 0x160c, &rd) || !SafeGet(menu + 0x1610, &wr)) continue;
        for (int i = 0; i < 17; ++i) {
            if (q[i] != prev[i] && (q[i] > 17 || q[i] < 0)) {
    ++logs;
                Log("menu queue SAMPLER: slot %d now %d (was %d), read %d write %d", i, q[i], prev[i], rd, wr);
            }
            prev[i] = q[i];
        }
    }
}

// Crash hunt: the menu update 0x1bf7c20 takes an event id from the queue (movsxd r14, [rax + rcx*4 + 0x15c8] at 0x1bf993e) and indexes an
// 18-entry handler table with it; the 6th guest dies there with ids like 503 / 100. This filter logs every id outside 0..17 with the
// queue, the read index and what the neighbours hold, and hands the menu object back (the stub continues with rax = our result).
BB_COOP_SYSV std::uint64_t MenuIdFilter(std::uint64_t obj, std::uint64_t id) {
    static std::atomic<int> logs{0};
    const std::int64_t v = static_cast<std::int64_t>(id);
    if ((v < 0 || v > 17) && logs.fetch_add(1) < 6) {
        std::int32_t q[24] = {}, rd = -1, wr = -1;
        SafeRead(obj + 0x15c8, q, sizeof q);
        SafeGet(obj + 0x160c, &rd);
        SafeGet(obj + 0x1610, &wr);
        Log("MENU ID %lld (read %d write %d): queue+neighbours [%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d | %d %d %d %d %d %d %d]", static_cast<long long>(v), rd, wr,
            q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15], q[16], q[17], q[18], q[19], q[20], q[21], q[22], q[23]);
    }
    if ((v < 0 || v > 17) && logs.load() <= 2) {
        std::int32_t pre[0x80] = {};
        SafeRead(obj + 0x13c8, pre, sizeof pre);
        char b[0x80 * 9 + 8];
        int n = 0;
        for (int i = 0; i < 0x80; ++i) n += std::snprintf(b + n, sizeof b - n, "%x ", static_cast<unsigned>(pre[i]));
        Log("MENU ID context: bytes +0x13c8..+0x15c8 as ints: %s", b);
    }
    return obj;
}

#ifdef _WIN32
// Hardware write watchpoints (BB_PARTY_SIX_PROBE=1) on the menu object's queue head (+0x15c8) and the gauge record's max HP (+0x15b8): the
// writer of the HP values that land in the menu event queue. Debug registers are per thread: every thread gets them, new ones on the next tick.
std::atomic<std::uint64_t> g_watch0{0}, g_watch1{0};
std::atomic<int> g_watch_logs{0};
LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const std::uint64_t dr6 = ep->ContextRecord->Dr6;
    if (!(dr6 & 3)) return EXCEPTION_CONTINUE_SEARCH;
    const std::uint64_t addr = (dr6 & 1) ? g_watch0.load() : g_watch1.load();
    std::int32_t v = 0;
    std::memcpy(&v, reinterpret_cast<void*>(addr), 4);
    if (g_watch_logs.load() < 24 && ((dr6 & 2) || v > 17 || v < 0)) {
        g_watch_logs.fetch_add(1);
        const std::uint64_t rip = ep->ContextRecord->Rip, base = Guest(0);
        char st[600];
        int n = 0;
        const std::uint64_t* sp = reinterpret_cast<const std::uint64_t*>(ep->ContextRecord->Rsp);
        for (int i = 0; i < 40 && n < 540; ++i) {
            const std::uint64_t q = sp[i];
            if (q > base && q < base + 0x6000000) n += std::snprintf(st + n, sizeof st - n, "+%llx ", ull(q - base));
        }
        Log("WATCH write %#llx = %d at rip +%llx (guest %s); rax %llx rcx %llx rdx %llx; stack: %s", ull(addr), v,
            ull(rip - base), rip > base && rip < base + 0x6000000 ? "yes" : "no", ull(ep->ContextRecord->Rax), ull(ep->ContextRecord->Rcx),
            ull(ep->ContextRecord->Rdx), st);
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void ArmWatch(std::uint64_t a0, std::uint64_t a1) {
    static bool veh = false;
    if (!veh) {
        veh = true;
        AddVectoredExceptionHandler(1, WatchVeh);
    }
    g_watch0 = a0;
    g_watch1 = a1;
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c) && c.Dr0 != a0) {
                c.Dr0 = a0;
                c.Dr1 = a1;
                c.Dr7 = (c.Dr7 & ~0xffull) | 0x1 | 0x4 | (0x1ull << 16) | (0x3ull << 18) | (0x1ull << 20) | (0x3ull << 22);  // write, 4 bytes, DR0 and DR1
                SetThreadContext(th, &c);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE cur = GetCurrentThread();
    if (GetThreadContext(cur, &c) && c.Dr0 != a0) {
        c.Dr0 = a0;
        c.Dr1 = a1;
        c.Dr7 = (c.Dr7 & ~0xffull) | 0x1 | 0x4 | (0x1ull << 16) | (0x3ull << 18) | (0x1ull << 20) | (0x3ull << 22);
        SetThreadContext(cur, &c);
    }
}
#endif

void SixpTick() {
    // Stage 1b: the game keeps its own copy of the room size at *(0x553d6d0)+0xc (5): 0x15bc590 and 1e50090 refuse a member when
    // the presentation count has reached it. Our Matching2 answer says 6, the game does not take it from there; keep the field at 6.
    if (g_active.load()) {
        std::uint64_t netman = 0;
        std::int32_t cur = -1;
        if (SafeGet(Guest(0x553d6d0), &netman) && netman && SafeGet(netman + 0xc, &cur) && cur == 5) {
            const std::int32_t six = 6;
            SafeWrite(netman + 0xc, &six, 4);
            static int logged = 0;
            if (logged++ < 4) Log("room size field +0xc of the session object %#llx: 5 -> 6", ull(netman));
        }
    }
    static const bool probe = [] {
        const char* e = std::getenv("BB_PARTY_SIX_PROBE");
        return e && e[0] == '1';
    }();
    if (!probe) {
        return;
    }
    static std::once_flag sampler_once;
    std::call_once(sampler_once, [] { std::thread(QueueSampler).detach(); });
    CheckMenuQueue();
#ifdef _WIN32
    {
        static std::chrono::steady_clock::time_point armed{};
        const auto t = std::chrono::steady_clock::now();
        std::uint64_t menu = 0;
        if (t - armed > std::chrono::seconds(1) && SafeGet(Guest(0x5562878), &menu) && menu) {
            armed = t;
            ArmWatch(menu + 0x15c8, menu + 0x15b8);
        }
    }
#endif
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last{};
    static int logs = 0;
    const auto now = Clock::now();
    if (logs >= 40 || now - last < std::chrono::seconds(10)) {
        return;
    }
    last = now;
    std::uint64_t wcm = 0, gdm = 0, flow = 0;
    SafeGet(Guest(kWorldChrMan), &wcm);
    SafeGet(Guest(kGameDataMan), &gdm);
    SafeGet(Guest(kNetFlow), &flow);
    std::int64_t row_count = -1;
    std::uint64_t chr_entries = 0, row_base = 0, occ_ptr = 0, pres = 0;
    std::int32_t chr_count = -1, pres_count = -1, pres_state = -1;
    std::uint64_t occ = ~0ull;
    if (wcm) {
        SafeGet(wcm + 0x30, &chr_count);
        SafeGet(wcm + 0x38, &chr_entries);
    }
    if (gdm) {
        SafeGet(gdm + 0x10, &row_base);
        SafeGet(gdm + 0x18, &occ_ptr);
        if (row_base) {
            SafeGet(row_base - 8, &row_count);
        }
        if (occ_ptr) {
            SafeGet(occ_ptr, &occ);
        }
    }
    if (flow) {
        SafeGet(flow + 0x16f8, &pres);
        if (pres) {
            SafeGet(pres + 0x14, &pres_count);
            SafeGet(pres + 0x98, &pres_state);
        }
    }
    char slots[160] = "";
    if (pres) {
        int n = 0;
        for (int i = 0; i < 6 && n < 150; ++i) {
            std::uint32_t h = 0xdeadbeefu;
            SafeGet(pres + 0x1c + i * 0x14, &h);
            n += std::snprintf(slots + n, sizeof slots - n, "%s%x", i ? " " : "", h);
        }
        std::uint32_t th[4] = {}, tc = 0;
        SafeRead(pres + 0xb0, th, sizeof th);
        SafeGet(pres + 0xc0, &tc);
        std::snprintf(slots + n, sizeof slots - n, " | tracked %x %x %x %x n%u", th[0], th[1], th[2], th[3], tc);
    }
    std::uint64_t netman = 0;
    std::int32_t room_max = -1;
    SafeGet(Guest(0x553d6d0), &netman);
    if (netman) SafeGet(netman + 0xc, &room_max);
    ++logs;
    Log("probe: WorldChrMan %#llx ChrSet count %d entries %#llx; GameDataMan %#llx rows %#llx row count %lld occupancy %#018llx; "
        "presentation %#llx members %d state %d slots [%s]; room max %d; set %s",
        ull(wcm), chr_count, ull(chr_entries), ull(gdm), ull(row_base), static_cast<long long>(row_count), ull(occ), ull(pres),
        pres_count, pres_state, slots, room_max, g_active.load() ? "ON" : "off");
}

} // namespace coop::sixp
