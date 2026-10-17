// SPDX-License-Identifier: GPL-3.0-or-later
// NPC-summon test fixture (see party_npc_test.h; recipe: docs/party/npc_peer.md sections 2 and 5).
// Every address is our offset, each function read in the 1.09 decompilation:
//   GetChrByEntityId 0x13c97a0(entity, entity, 0)   (only reads WorldChrMan's tables)
//   ChrIns_SetDisable 0x18c6390(chr, on)
//   SosSel_BuildRequestFromChr 0x1878d90(sel, st, entity, summonFlag, pos*, rot*, eventId*,
//       dismissFlag, useNetPos): pos = 3 floats, rot: only rot[1] (yaw) is read; it drops the
//       request when desc[st]+4 & the SOS status mask is 0 and runs the SOS filter 0x1874710(sel,
//       req, 1); sel = *(*(*(SprjEventMan 0x553b108)+0x60)+0x30) (as 0x13deaf0 reads it)
//   CSMultiPlayMan 0x5540230: human tasks +0x10/+0x18, NPC tasks +0x38/+0x40;
//       EnsureNpcTask 0x1e54c30(man, ci): ci {u32 handle; u8 st; f32 pos[3] @8; f32 rot[3] @0x14;
//       i32 init @0x20; i32 end @0x24; u8 flags @0x28} (the SOS tick 0x1872360 builds it so)
//   CSMultiNPCPlayerInsTask (ctor 0x1e4ae90): +0x50 step, +0xb8 step name (wchar*), +0xc8 handle,
//       +0xd0 st, +0xec init flag, +0xf0 end flag, +0x128 lifecycle flags
//   NetworkFlow slot table *(*(0x5556678)+0x16f8): +8/+0xc/+0x10 per-kind counts, +0x14 count,
//       5 x 0x14 at +0x1c {handle, kind, state, init, end}; Register 0x15bc590(slots, handle, team,
//       kind, init, end, summonparam) -> bool (false: count >= *(*(0x553d6d0)+0xc), or no free
//       entry); ReturnNpc 0x15be2a0(slots, handle, died); cooperator count 0x15bdc20(slots)
//   SessionTypeDesc 0x553d750 + st*0x80: +4 capability mask, +0xc summonparam type, +0x10 team,
//       +0x15 net chr type (counted as cooperator when in {1,5,7,19})
//   SprjEventFlagMan 0x553b100: GetEventFlagValue 0x13cfd80(man, id, bits)
//   area boss-cleared check 0x131d7b0(SprjLuaEventMan 0x553b0c8, &area) (what 0x1874710 asks)
#include "party_npc_test.h"
#include "party_async_io.h"

#include "coop_hooks.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace coop {
namespace {

using Clock = std::chrono::steady_clock;
using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u64 kGetChrByEntityId = 0x13c97a0;
constexpr u64 kSetDisable = 0x18c6390;
constexpr u64 kBuildRequest = 0x1878d90;
constexpr u64 kEnsureNpcTask = 0x1e54c30;
constexpr u64 kRegister = 0x15bc590;
constexpr u64 kReturnNpc = 0x15be2a0;
constexpr u64 kCooperatorCount = 0x15bdc20;
constexpr u64 kGetEventFlagValue = 0x13cfd80;
constexpr u64 kAreaBossCleared = 0x131d7b0;
constexpr u64 kLampWarp = 0x13cdf30; // void(u32 WarpParam id), travel.md 1.1

constexpr u64 kSlotEventMan = 0x553b108;
constexpr u64 kSlotMultiPlayMan = 0x5540230;
constexpr u64 kSlotNetFlow = 0x5556678;
constexpr u64 kSlotSessionCfg = 0x553d6d0;
constexpr u64 kSlotEventFlagMan = 0x553b100;
constexpr u64 kSlotLuaEventMan = 0x553b0c8;
constexpr u64 kSlotWorldChrMan = 0x553e878;
constexpr u64 kSessionTypeDesc = 0x553d750;

// Runtime copies of two "Party: ..." XML patches (seamless_rules.md), for the filter A/B/C test:
//   group 1 "Party: Bells after boss defeated" (SOS filter 0x1874710): boss-cleared / negative-area rejections
//   group 2 "Party: Bells anywhere", status producer 0x186fe40 only: its area-derived restriction -> 0
//     (with the restriction set, 0x1878d90 clears capability bit 0x80 and drops the request before the filter)
struct PatchSite {
    int group;
    u64 off;
    int len;
    u8 orig[16];
    u8 patched[16];
};
constexpr PatchSite kPatchSites[] = {
    {1, 0x18749e8, 6, {0x0f, 0x85, 0x48, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}}, // jne boss cleared
    {1, 0x18749f0, 6, {0x0f, 0x88, 0x40, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}}, // js area < 0
    {2, 0x18700d3, 16,
     {0x84, 0xc0, 0x41, 0xbd, 0xff, 0xff, 0xff, 0xff, 0x44, 0x0f, 0x44, 0xeb, 0x41, 0xc1, 0xed, 0x1f},
     {0x45, 0x31, 0xed, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}}, // xor r13d,r13d
};

// EMEVD summon type -> session type (0x4733a10).
constexpr int kTypeToSession[8] = {26, 28, 29, 30, 31, 32, 33, 27};

// NPC summon entities of the shipped EMEVD (npc_peer.md 1.1) and their EMEVD summon type.
struct KnownNpc {
    int entity;
    int emevd_type;
};
constexpr KnownNpc kKnown[] = {
    {2300740, 0}, {2300930, 0}, {2300931, 5}, {2400910, 0}, {2410158, 7}, {2410740, 0}, {2420910, 0},
    {2700920, 5}, {2700921, 5}, {2800910, 5}, {2800911, 0}, {3200910, 0}, {3200911, 5}, {3200912, 5},
    {3400921, 0}, {3400922, 6}, {3400923, 6}, {3400924, 5}, {3400925, 6}, {3400926, 6}, {3500940, 5},
};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party NPC: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

double Seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

u64 Slot(u64 off) {
    u64 v = 0;
    const u64 at = Guest(off);
    return at && SafeGet(at, &v) ? v : 0;
}
template <class T>
T Field(u64 base, u64 off, T fallback) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}
u64 VFunc(u64 obj, u64 off) {
    const u64 vt = Field<u64>(obj, 0, 0);
    return Field<u64>(vt, off, 0);
}

enum class Mode { Summon, Cap, Filter, LogOnly };
enum class Path { Faithful, Direct };

struct Target {
    int entity = -1;
    int st = 27;
    bool st_given = false;
};

struct Summoned {
    int entity;
    u32 handle;
    Clock::time_point at;
    bool returned;
};

struct Fixture {
    bool on = false, configured = false;
    Mode mode = Mode::Summon;
    Path path = Path::Faithful;
    bool autolist = false;
    std::vector<Target> targets;
    double delay = 5.0, return_after = -1.0;
    bool log_every_second = true;
    bool patch_all = false; // BB_PARTY_TEST_NPC_PATCH=1: both patch groups on for the whole run
    int warp_id = -1;       // BB_PARTY_TEST_NPC_WARP: lamp warp to the NPC map first (once)
    bool warp_done = false, warp_pending = false;

    // Per load.
    bool steady = false;
    Clock::time_point steady_at{};
    u32 map = 0xffffffffu;
    std::vector<Target> live; // the targets to work through in this load
    std::size_t next = 0;
    Clock::time_point last_action{};
    std::vector<Summoned> summoned;
    bool reported_missing = false, cap_summary = false, done = false;
    Clock::time_point work_done_at{};
    bool work_done = false;
    int cap_registered = 0, cap_refused = 0;
    std::string last_state;
    Clock::time_point last_log{};

    // Filter A/B (once per process).
    int filter_phase = 0; // 0 not started, 1 A (1.09), 2 B (boss filter patched), 3 C (+ status), 4 done
    Clock::time_point filter_at{};
    bool filter_patched = false;
};

Fixture& F() {
    static Fixture f;
    return f;
}

int DefaultSessionType(int entity) {
    for (const KnownNpc& k : kKnown) {
        if (k.entity == entity) {
            return kTypeToSession[k.emevd_type & 7];
        }
    }
    return 27;
}

void Configure(Fixture& f) {
    f.configured = true;
    const char* list = std::getenv("BB_PARTY_TEST_NPC");
    if (!list || !list[0]) {
        return;
    }
    f.on = true;
    std::string s = list;
    if (s == "auto") {
        f.autolist = true;
    } else {
        std::size_t at = 0;
        while (at <= s.size()) {
            std::size_t end = s.find(',', at);
            if (end == std::string::npos) {
                end = s.size();
            }
            const std::string item = s.substr(at, end - at);
            at = end + 1;
            if (item.empty()) {
                continue;
            }
            Target t;
            t.entity = std::atoi(item.c_str());
            const std::size_t colon = item.find(':');
            if (colon != std::string::npos) {
                t.st = std::atoi(item.c_str() + colon + 1);
                t.st_given = true;
            } else {
                t.st = DefaultSessionType(t.entity);
            }
            if (t.entity > 0 && t.st >= 0 && t.st < 0x22) {
                f.targets.push_back(t);
            } else {
                Log("BB_PARTY_TEST_NPC: '%s' is not <entity>[:<session type 0..33>]; ignored", item.c_str());
            }
        }
    }
    if (const char* m = std::getenv("BB_PARTY_TEST_NPC_MODE"); m && m[0]) {
        const std::string mode = m;
        f.mode = mode == "cap" ? Mode::Cap : mode == "filter" ? Mode::Filter : mode == "log" ? Mode::LogOnly : Mode::Summon;
    }
    f.path = f.mode == Mode::Cap ? Path::Direct : Path::Faithful;
    if (const char* p = std::getenv("BB_PARTY_TEST_NPC_PATH"); p && p[0]) {
        f.path = (p[0] == 'b' || p[0] == 'B') ? Path::Direct : Path::Faithful;
    }
    if (const char* d = std::getenv("BB_PARTY_TEST_NPC_DELAY"); d && d[0]) {
        f.delay = std::atof(d);
    }
    if (const char* r = std::getenv("BB_PARTY_TEST_NPC_RETURN"); r && r[0]) {
        f.return_after = std::atof(r);
    }
    if (const char* pa = std::getenv("BB_PARTY_TEST_NPC_PATCH"); pa && pa[0] == '1') {
        f.patch_all = true;
    }
    if (const char* w = std::getenv("BB_PARTY_TEST_NPC_WARP"); w && w[0]) {
        f.warp_id = std::atoi(w);
    }
    if (const char* l = std::getenv("BB_PARTY_TEST_NPC_LOG"); l && l[0] == '0') {
        f.log_every_second = false;
    }
    std::string desc;
    for (const Target& t : f.targets) {
        char one[32];
        std::snprintf(one, sizeof one, "%s%d:%d", desc.empty() ? "" : ",", t.entity, t.st);
        desc += one;
    }
    Log("fixture on: %s, mode %s, path %s, delay %.1f s, return %s", f.autolist ? "auto (known NPC summon entities)" : desc.c_str(),
        f.mode == Mode::Cap ? "cap" : f.mode == Mode::Filter ? "filter" : f.mode == Mode::LogOnly ? "log" : "summon",
        f.path == Path::Direct ? "B (direct)" : "A (faithful, 0x1878d90)", f.delay,
        f.return_after >= 0 ? std::to_string(f.return_after).c_str() : "never");
}

// ---- Game calls (main thread only) ----

u64 ChrByEntity(int entity) {
    using Fn = u64(BB_COOP_SYSV*)(int, u64, u64);
    return reinterpret_cast<Fn>(Guest(kGetChrByEntityId))(entity, u64(unsigned(entity)), 0);
}

u64 SlotTable() {
    return Field<u64>(Slot(kSlotNetFlow), 0x16f8, 0);
}

int MemberCap() {
    return Field<i32>(Slot(kSlotSessionCfg), 0xc, -1);
}

int EventFlag(u32 id) {
    const u64 man = Slot(kSlotEventFlagMan);
    if (!man) {
        return -1;
    }
    using Fn = u32(BB_COOP_SYSV*)(u64, u32, int);
    return int(reinterpret_cast<Fn>(Guest(kGetEventFlagValue))(man, id, 1));
}

int CooperatorCount(const GameSnapshot& s) {
    const u64 table = SlotTable();
    if (!table || !s.world_up || s.loading) {
        return -1;
    }
    using Fn = i32(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kCooperatorCount))(table);
}

/// The area id the SOS filter checks for the character (chr +0x278, or +0x27c when vfunc +0x628).
int ChrArea(u64 chr) {
    bool alt = false;
    if (const u64 fn = VFunc(chr, 0x628)) {
        using Fn = bool(BB_COOP_SYSV*)(u64);
        alt = reinterpret_cast<Fn>(fn)(chr);
    }
    return Field<i32>(chr, alt ? 0x27c : 0x278, -1);
}

int AreaBossCleared(int area) {
    const u64 lua = Slot(kSlotLuaEventMan);
    if (!lua) {
        return -1;
    }
    int a = area;
    using Fn = u64(BB_COOP_SYSV*)(u64, int*);
    return int(reinterpret_cast<Fn>(Guest(kAreaBossCleared))(lua, &a) & 1);
}

bool IsNpcPlayer(u64 chr) {
    const u64 fn = VFunc(chr, 0x1a8);
    using Fn = bool(BB_COOP_SYSV*)(u64);
    return fn && reinterpret_cast<Fn>(fn)(chr);
}

int ChrHp(u64 chr) {
    // *(*(chr+0x3b0)+0x20)+0xf8, as 0x1878d90 reads it (plVar5[0x76] = chr+0x3b0).
    return Field<i32>(Field<u64>(Field<u64>(chr, 0x3b0, 0), 0x20, 0), 0xf8, -1);
}

bool ChrDisabled(u64 chr) {
    return Field<u8>(Field<u64>(chr, 0x18, 0), 0x20, 0) & 1;
}

/// Position (+0x1e0) and rotation (+0x1d0) of a character, as the debug summon 0x1879a60 reads them.
bool ChrTransform(u64 chr, float pos[4], float rot[4]) {
    const u64 a = Field<u64>(Field<u64>(Field<u64>(chr, 0x58, 0), 8, 0), 0x3b0, 0);
    const u64 phys = Field<u64>(a, 0x68, 0);
    return phys && SafeRead(phys + 0x1e0, pos, 16) && SafeRead(phys + 0x1d0, rot, 16);
}

std::string DescRow(int st) {
    const u64 row = Guest(kSessionTypeDesc + u64(st) * 0x80);
    char text[160];
    std::snprintf(text, sizeof text, "desc[%d]: mask 0x%x, summonparam %d, team %d, net chr type %u%s", st,
                  Field<u32>(row, 4, 0), Field<i32>(row, 0xc, 0), Field<i32>(row, 0x10, 0), Field<u8>(row, 0x15, 0),
                  ((0x800a2u >> (Field<u8>(row, 0x15, 0) & 31)) & 1) && Field<u8>(row, 0x15, 0) < 20
                      ? " (counted as cooperator)"
                      : " (NOT counted as cooperator)");
    return text;
}

/// Writes group `group`'s patched (on) or 1.09 (off) bytes; a site holding neither is left alone.
void PatchGroup(int group, bool on) {
    for (const PatchSite& site : kPatchSites) {
        if (site.group != group) {
            continue;
        }
        const u8* from = on ? site.orig : site.patched;
        const u8* to = on ? site.patched : site.orig;
        if (Matches(site.off, to, site.len)) {
            continue;
        }
        if (!Matches(site.off, from, site.len)) {
            Log("patch: 0x%llx does not hold the expected bytes; left alone", static_cast<unsigned long long>(site.off));
            continue;
        }
        u8* at = reinterpret_cast<u8*>(Guest(site.off));
#ifdef _WIN32
        DWORD old = 0, unused = 0;
        if (!VirtualProtect(at, site.len, PAGE_EXECUTE_READWRITE, &old)) {
            Log("patch: VirtualProtect failed at 0x%llx", static_cast<unsigned long long>(site.off));
            continue;
        }
        std::memcpy(at, to, site.len);
        VirtualProtect(at, site.len, old, &unused);
        FlushInstructionCache(GetCurrentProcess(), at, site.len);
#else
        const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(at) & ~std::uintptr_t(0xfff);
        mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
        std::memcpy(at, to, site.len);
        mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_EXEC);
#endif
    }
}

/// "boss-filter 1.09/patched, status 1.09/patched".
std::string FilterBytes() {
    std::string out;
    for (int g = 1; g <= 2; ++g) {
        bool orig = true, patched = true;
        for (const PatchSite& site : kPatchSites) {
            if (site.group == g) {
                orig = orig && Matches(site.off, site.orig, site.len);
                patched = patched && Matches(site.off, site.patched, site.len);
            }
        }
        out += g == 1 ? "boss-filter " : ", status-restriction ";
        out += orig ? "1.09" : patched ? "patched" : "mixed";
    }
    return out;
}

// ---- State ----

struct TaskView {
    u64 task;
    u32 handle;
    int step, st;
    u32 life;
    std::string name;
};

std::vector<TaskView> NpcTasks(int* humans) {
    std::vector<TaskView> out;
    const u64 man = Slot(kSlotMultiPlayMan);
    if (humans) {
        const u64 b = Field<u64>(man, 0x10, 0), e = Field<u64>(man, 0x18, 0);
        *humans = man && e >= b && e - b < 0x1000 ? int((e - b) / 8) : -1;
    }
    const u64 b = Field<u64>(man, 0x38, 0), e = Field<u64>(man, 0x40, 0);
    if (!man || e < b || e - b > 0x400) {
        return out;
    }
    for (u64 at = b; at < e; at += 8) {
        const u64 t = Field<u64>(at, 0, 0);
        if (!t) {
            continue;
        }
        TaskView v{t, Field<u32>(t, 0xc8, 0xffffffffu), Field<i32>(t, 0x50, -1), Field<u8>(t, 0xd0, 0xff),
                   u32(Field<u8>(t, 0x128, 0) & 0x1f), {}};
        const u64 wname = Field<u64>(t, 0xb8, 0);
        for (int i = 0; wname && i < 31; ++i) {
            std::uint16_t c = 0;
            if (!SafeGet(wname + u64(i) * 2, &c) || !c) {
                break;
            }
            v.name += (c >= 0x20 && c < 0x7f) ? char(c) : '?';
        }
        out.push_back(v);
    }
    return out;
}

std::string SlotsText(u64 table) {
    if (!table) {
        return "slots: none";
    }
    char text[512];
    int n = std::snprintf(text, sizeof text, "slots: count %d (kinds %d/%d/%d), cap %d [", Field<i32>(table, 0x14, -1),
                          Field<i32>(table, 8, -1), Field<i32>(table, 0xc, -1), Field<i32>(table, 0x10, -1), MemberCap());
    for (int i = 0; i < 5 && n > 0 && n < int(sizeof text); ++i) {
        const u64 e = table + 0x1c + u64(i) * 0x14;
        const u32 h = Field<u32>(e, 0, 0xffffffffu);
        if (h == 0xffffffffu) {
            n += std::snprintf(text + n, sizeof text - n, "%s-", i ? " " : "");
        } else {
            n += std::snprintf(text + n, sizeof text - n, "%s{h 0x%x kind %d state %d init %d end %d}", i ? " " : "", h,
                               Field<i32>(e, 4, -1), Field<i32>(e, 8, -1), Field<i32>(e, 0xc, -1), Field<i32>(e, 0x10, -1));
        }
    }
    if (n > 0 && n < int(sizeof text)) {
        std::snprintf(text + n, sizeof text - n, "]");
    }
    return text;
}

std::string SelText() {
    const u64 sel = Field<u64>(Field<u64>(Slot(kSlotEventMan), 0x60, 0), 0x30, 0);
    if (!sel) {
        return "sel none";
    }
    char text[96];
    std::snprintf(text, sizeof text, "sel pending %d, quota team1 %d team2 %d", Field<i32>(sel, 0x1c8, -1),
                  Field<i32>(sel, 0x1fc, -1), Field<i32>(sel, 0x200, -1));
    return text;
}

void LogState(Fixture& f, const GameSnapshot& s, bool force) {
    int humans = -1;
    const std::vector<TaskView> tasks = NpcTasks(&humans);
    const u64 table = SlotTable();
    std::string line;
    char head[256];
    std::snprintf(head, sizeof head, "map %s, session %s (%d, sub %d), cooperators %d, flag6009 %d, tasks: human %d, npc %zu",
                  MapName(s.map_id).c_str(), SessionRoleName(s.session_role), s.session_role, s.session_sub_state,
                  CooperatorCount(s), EventFlag(6009), humans, tasks.size());
    line = head;
    for (const TaskView& t : tasks) {
        char one[160];
        static const char* const kSteps[] = {"Init", "SummonMsgWait", "SummonWait", "Summon",
                                             "Update", "ReturnWait", "Return", "Finish"};
        std::snprintf(one, sizeof one, " [npc h 0x%x st %d step %d %s flags 0x%x]", t.handle, t.st, t.step,
                      t.step >= 0 && t.step < 8 ? kSteps[t.step] : "?", t.life);
        line += one;
    }
    line += "; " + SlotsText(table) + "; " + SelText();
    if (force || f.log_every_second || line != f.last_state) {
        Log("state %s", line.c_str());
    }
    f.last_state = line;
}

// ---- Summons ----

u64 Prepare(const Target& t, const char* why) {
    const u64 chr = ChrByEntity(t.entity);
    if (!chr) {
        Log("%s: entity %d is not in the loaded maps (GetChrByEntityId -> NULL)", why, t.entity);
        return 0;
    }
    const int area = ChrArea(chr);
    Log("%s: entity %d -> chr 0x%llx handle 0x%x, NPC player %s, HP %d, disabled %d, area %d (boss cleared %d), "
        "chr type %d; %s",
        why, t.entity, static_cast<unsigned long long>(chr), Field<u32>(chr, 8, 0), IsNpcPlayer(chr) ? "yes" : "NO",
        ChrHp(chr), ChrDisabled(chr) ? 1 : 0, area, AreaBossCleared(area), Field<i32>(chr, 0x78, -1),
        DescRow(t.st).c_str());
    return chr;
}

bool SummonFaithful(const Target& t, u64 chr, u64 player) {
    const u64 sel = Field<u64>(Field<u64>(Slot(kSlotEventMan), 0x60, 0), 0x30, 0);
    if (!sel) {
        Log("summon %d: no SOS selection state (SprjEventMan+0x60+0x30)", t.entity);
        return false;
    }
    float pos[4] = {}, rot[4] = {};
    if (!ChrTransform(player, pos, rot)) {
        ChrTransform(chr, pos, rot);
    }
    using SetDisableFn = void(BB_COOP_SYSV*)(u64, u8);
    reinterpret_cast<SetDisableFn>(Guest(kSetDisable))(chr, 0);
    int evid = -1;
    using BuildFn = void(BB_COOP_SYSV*)(u64, u64, u32, i32, float*, float*, int*, i32, int);
    reinterpret_cast<BuildFn>(Guest(kBuildRequest))(sel, u64(t.st), u32(t.entity), -1, pos, rot, &evid, -1, 0);
    Log("summon %d: SetDisable(0) + 0x1878d90(sel 0x%llx, st %d, flags -1/-1) at (%.1f, %.1f, %.1f); filter bytes %s",
        t.entity, static_cast<unsigned long long>(sel), t.st, pos[0], pos[1], pos[2], FilterBytes().c_str());
    return true;
}

bool SummonDirect(const Target& t, u64 chr, u64 player, bool* registered) {
    const u64 man = Slot(kSlotMultiPlayMan);
    const u64 table = SlotTable();
    if (!man || !table) {
        Log("summon %d: no CSMultiPlayMan / slot table", t.entity);
        return false;
    }
    float pos[4] = {}, rot[4] = {};
    if (!ChrTransform(player, pos, rot)) {
        ChrTransform(chr, pos, rot);
    }
    using SetDisableFn = void(BB_COOP_SYSV*)(u64, u8);
    reinterpret_cast<SetDisableFn>(Guest(kSetDisable))(chr, 0);
    // What 0x1878d90 does for an NPC player: GameData (vfunc +0x1c8) +0x5d8 = session type.
    if (IsNpcPlayer(chr)) {
        if (const u64 fn = VFunc(chr, 0x1c8)) {
            using Fn = u64(BB_COOP_SYSV*)(u64);
            const u64 gd = reinterpret_cast<Fn>(fn)(chr);
            const u32 st = u32(t.st);
            if (gd) {
                SafeWrite(gd + 0x5d8, &st, 4);
            }
        }
    }
    alignas(16) u8 ci[0x40] = {};
    const u32 handle = Field<u32>(chr, 8, 0xffffffffu);
    const i32 none = -1;
    std::memcpy(ci + 0, &handle, 4);
    ci[4] = u8(t.st);
    std::memcpy(ci + 8, pos, 12);
    std::memcpy(ci + 0x14, rot, 12);
    std::memcpy(ci + 0x20, &none, 4);
    std::memcpy(ci + 0x24, &none, 4);
    ci[0x28] = 0;
    using EnsureFn = void(BB_COOP_SYSV*)(u64, void*);
    reinterpret_cast<EnsureFn>(Guest(kEnsureNpcTask))(man, ci);
    const u64 row = Guest(kSessionTypeDesc + u64(t.st) * 0x80);
    using RegisterFn = u64(BB_COOP_SYSV*)(u64, u32, u32, int, i32, i32, i32);
    const int before = Field<i32>(table, 0x14, -1);
    const bool ok = reinterpret_cast<RegisterFn>(Guest(kRegister))(table, handle, Field<u32>(row, 0x10, 0), 2, -1, -1,
                                                                   Field<i32>(row, 0xc, -1)) & 1;
    *registered = ok;
    Log("summon %d: direct EnsureNpcTask + Register(h 0x%x, team %d, summonparam %d) -> %s; slot count %d -> %d (cap %d)",
        t.entity, handle, Field<i32>(row, 0x10, 0), Field<i32>(row, 0xc, -1), ok ? "registered" : "REFUSED", before,
        Field<i32>(table, 0x14, -1), MemberCap());
    return true;
}

void SendHome(Summoned& m) {
    const u64 table = SlotTable();
    if (!table) {
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64, u32, int);
    reinterpret_cast<Fn>(Guest(kReturnNpc))(table, m.handle, 0);
    m.returned = true;
    Log("return: 0x15be2a0(slots, h 0x%x, 0) for entity %d", m.handle, m.entity);
}

void ResetLoad(Fixture& f) {
    if (f.filter_patched) {
        PatchGroup(1, false);
        PatchGroup(2, false);
        f.filter_patched = false;
        Log("filter: 1.09 bytes restored (load)");
    }
    f.steady = false;
    f.warp_pending = false;
    f.live.clear();
    f.next = 0;
    f.summoned.clear();
    f.reported_missing = false;
    f.cap_summary = false;
    f.done = f.work_done = false;
    f.cap_registered = f.cap_refused = 0;
    if (f.filter_phase >= 1 && f.filter_phase <= 3) {
        f.filter_phase = 0; // the request died with the load: run the A/B again in the next one
    }
}

void BuildLive(Fixture& f) {
    f.live.clear();
    if (!f.autolist) {
        f.live = f.targets;
        return;
    }
    std::string found;
    for (const KnownNpc& k : kKnown) {
        if (ChrByEntity(k.entity)) {
            Target t;
            t.entity = k.entity;
            t.st = kTypeToSession[k.emevd_type & 7];
            f.live.push_back(t);
            found += (found.empty() ? "" : ", ") + std::to_string(k.entity);
        }
    }
    Log("auto: known NPC summon entities in the loaded maps: %s", found.empty() ? "none" : found.c_str());
    if (f.mode != Mode::Cap && f.live.size() > 1) {
        f.live.resize(1);
    }
}

bool TaskFor(u32 handle) {
    for (const TaskView& t : NpcTasks(nullptr)) {
        if (t.handle == handle) {
            return true;
        }
    }
    return false;
}

void RunFilter(Fixture& f, const GameSnapshot& s, Clock::time_point now) {
    if (f.filter_phase >= 4 || f.live.empty()) {
        return;
    }
    const Target& t = f.live[0];
    static const char* const kStage[] = {"", "A (1.09 bytes)", "B (boss-filter NOPs 0x18749E8/0x18749F0)",
                                         "C (B + status restriction 0x18700D3 -> 0)"};
    if (f.filter_phase == 0) {
        PatchGroup(1, false);
        PatchGroup(2, false);
        f.filter_patched = false;
        const u64 chr = Prepare(t, "filter A (1.09 bytes)");
        if (!chr) {
            f.filter_phase = 4;
            return;
        }
        SummonFaithful(t, chr, s.player);
        f.summoned.push_back({t.entity, Field<u32>(chr, 8, 0xffffffffu), now, false});
        f.filter_phase = 1;
        f.filter_at = now;
        return;
    }
    if (Seconds(f.filter_at, now) < 4.0) {
        return;
    }
    const u64 chr = ChrByEntity(t.entity);
    const u32 handle = Field<u32>(chr, 8, 0xffffffffu);
    const bool task = chr && TaskFor(handle);
    const int area = chr ? ChrArea(chr) : -1;
    Log("filter %s result: %s (area %d, boss cleared %d; %s)", kStage[f.filter_phase],
        task ? "task created" : "NO task (request dropped)", area, AreaBossCleared(area), FilterBytes().c_str());
    if (task || f.filter_phase == 3 || !chr) {
        if (f.filter_patched) {
            PatchGroup(1, false);
            PatchGroup(2, false);
            f.filter_patched = false;
            Log("filter: 1.09 bytes restored (%s)", FilterBytes().c_str());
        }
        f.filter_phase = 4;
        return;
    }
    ++f.filter_phase;
    PatchGroup(1, true);
    if (f.filter_phase == 3) {
        PatchGroup(2, true);
    }
    f.filter_patched = true;
    char why[96];
    std::snprintf(why, sizeof why, "filter %s", kStage[f.filter_phase]);
    Prepare(t, why);
    SummonFaithful(t, chr, s.player);
    f.filter_at = now;
}

void RunSummons(Fixture& f, const GameSnapshot& s, Clock::time_point now) {
    const double gap = f.mode == Mode::Cap ? 3.0 : 0.5;
    if (f.next >= f.live.size()) {
        if (f.mode == Mode::Cap && !f.cap_summary && !f.live.empty() && Seconds(f.last_action, now) >= 3.0) {
            f.cap_summary = true;
            const u64 table = SlotTable();
            char reg[64] = "Register answers: path B only";
            if (f.path == Path::Direct) {
                std::snprintf(reg, sizeof reg, "%d registered, %d refused", f.cap_registered, f.cap_refused);
            }
            Log("cap summary (path %s): %zu NPCs tried, %s; slot count %d, member cap %d, cooperators %d, npc tasks %zu",
                f.path == Path::Direct ? "B" : "A", f.live.size(), reg, Field<i32>(table, 0x14, -1), MemberCap(),
                CooperatorCount(s), NpcTasks(nullptr).size());
        }
        return;
    }
    if (f.next > 0 && Seconds(f.last_action, now) < gap) {
        return;
    }
    const Target t = f.live[f.next++];
    f.last_action = now;
    char why[48];
    std::snprintf(why, sizeof why, "summon #%zu", f.next);
    const u64 chr = Prepare(t, why);
    if (!chr) {
        return;
    }
    bool ok = false, registered = false;
    if (f.path == Path::Direct) {
        ok = SummonDirect(t, chr, s.player, &registered);
        if (ok) {
            (registered ? f.cap_registered : f.cap_refused)++;
        }
    } else {
        ok = SummonFaithful(t, chr, s.player);
    }
    if (ok) {
        f.summoned.push_back({t.entity, Field<u32>(chr, 8, 0xffffffffu), now, false});
    }
}

// ---- SOS filter observer: 0x1874710(sel, req, from_event) -> accepted request or 0 ----
// Wrapped (ReplacePrologue) so each NPC request's fate is logged: req NULL = 0x1878d90 already
// dropped it (capability mask desc[st]+4 & the SOS status 0x186fe40); 0 = the filter rejected it.
constexpr u64 kSosFilter = 0x1874710;
using SosFilterFn = u64(BB_COOP_SYSV*)(u64, u64, int);
void* g_filter_orig = nullptr;

BB_COOP_SYSV u64 SosFilterWrap(u64 sel, u64 req, int from_event) {
    const u64 r = reinterpret_cast<SosFilterFn>(g_filter_orig)(sel, req, from_event);
    if (from_event) { // the request path (0x1878d90 passes 1; the sign list update passes 0)
        if (!req) {
            Log("SOS filter: request NULL (0x1878d90 dropped it before the filter: desc capability mask & SOS "
                "status 0x186fe40 = 0)");
        } else {
            const int st = Field<u8>(req, 0x22, 0xff);
            Log("SOS filter: request st %d entity %d (summon flag %d, dismiss %d) -> %s", st, Field<i32>(req, 0x88, -1),
                Field<i32>(req, 0x8c, -1), Field<i32>(req, 0x90, -1), r ? "ACCEPTED" : "REJECTED");
        }
    }
    return r;
}

void InstallFilterObserver() {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    // push rbp; mov rbp,rsp; push r15..r12, rbx; sub rsp,0x28 (17 bytes, no rip-relative operands)
    ReplacePrologue(kSosFilter, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                 0x83, 0xec, 0x28},
                    reinterpret_cast<const void*>(&SosFilterWrap), &g_filter_orig,
                    "NPC fixture: SOS filter observer (0x1874710)");
}

} // namespace

bool NpcTestRequested() {
    const char* v = std::getenv("BB_PARTY_TEST_NPC");
    return v && v[0];
}

void NpcTestTick(const GameSnapshot& s) {
    Fixture& f = F();
    if (!f.configured) {
        Configure(f);
    }
    if (!f.on || !Image()) {
        return;
    }
    // The SOS filter runs on this (the main) thread only: the tick is a safe point to wrap it.
    if (f.mode != Mode::LogOnly) {
        InstallFilterObserver();
    }
    const auto now = Clock::now();
    // Re-arm on every load: the NPC belongs to its map's ChrSet, the handle dies with it.
    if (!s.world_up || s.loading || s.map_id == 0xffffffffu) {
        if (f.steady) {
            Log("load / world down: re-arming (%zu summoned in the last load)", f.summoned.size());
            ResetLoad(f);
        }
        return;
    }
    if (!f.steady) {
        f.steady = true;
        f.steady_at = now;
        f.map = s.map_id;
        Log("world steady in %s: first action in %.1f s", MapName(s.map_id).c_str(), f.delay);
        LogState(f, s, true);
        f.last_log = now;
        return;
    }
    if (s.map_id != f.map) {
        Log("map %s -> %s without a loading screen: re-arming", MapName(f.map).c_str(), MapName(s.map_id).c_str());
        ResetLoad(f);
        return;
    }
    if (Seconds(f.last_log, now) >= 1.0) {
        f.last_log = now;
        LogState(f, s, false);
    }
    if (Seconds(f.steady_at, now) < f.delay) {
        return;
    }
    if (f.patch_all && f.mode != Mode::Filter) {
        static bool logged = false;
        PatchGroup(1, true);
        PatchGroup(2, true);
        if (!logged) {
            logged = true;
            Log("patch: boss filter and status restriction patched for the run (%s)", FilterBytes().c_str());
        }
    }
    if (f.warp_pending) {
        return; // until the load the warp starts
    }
    if (f.warp_id > 0 && !f.warp_done) {
        f.warp_done = true;
        const u32 want = u32(f.warp_id / 100000) << 24 | u32((f.warp_id / 10000) % 10) << 16;
        if ((s.map_id & 0xffff0000u) != want) {
            Log("warp: lamp warp 0x13cdf30(%d) to m%02u_%02u (from %s); the fixture re-arms after the load",
                f.warp_id, want >> 24, (want >> 16) & 0xff, MapName(s.map_id).c_str());
            using Fn = void(BB_COOP_SYSV*)(u32);
            reinterpret_cast<Fn>(Guest(kLampWarp))(u32(f.warp_id));
            f.warp_pending = true;
            return;
        }
    }
    if (!f.reported_missing) {
        f.reported_missing = true;
        // The call path itself: GetChrByEntityId(10000) is the local player.
        const u64 me = ChrByEntity(10000);
        Log("call path: GetChrByEntityId(10000) = 0x%llx (%s the local player 0x%llx)",
            static_cast<unsigned long long>(me), me == s.player ? "is" : "is NOT",
            static_cast<unsigned long long>(s.player));
        BuildLive(f);
        bool any = false;
        for (const Target& t : f.live) {
            any = any || ChrByEntity(t.entity) != 0;
        }
        if (!any) {
            Log("nothing to summon in %s; logging only. NPC summon entities by map (npc_peer.md 1.1): m23_00 2300740/"
                "2300930/2300931, m24_00 2400910, m24_01 2410740/2410158, m24_02 2420910, m27_00 2700920/2700921, "
                "m28_00 2800910/2800911, m32_00 3200910-3200912, m34_00 3400921-3400926 (six: cap tests), m35_00 3500940",
                MapName(s.map_id).c_str());
        }
        if (!any || f.mode == Mode::LogOnly) {
            for (const Target& t : f.live) {
                Prepare(t, "probe");
            }
            f.live.clear();
        }
    }
    if (f.mode == Mode::LogOnly) {
        return;
    }
    if (f.mode == Mode::Filter) {
        RunFilter(f, s, now);
    } else {
        RunSummons(f, s, now);
    }
    if (f.return_after >= 0) {
        for (Summoned& m : f.summoned) {
            if (!m.returned && Seconds(m.at, now) >= f.return_after) {
                SendHome(m);
            }
        }
    }
    // "done": this load's work is over (every summon tried, the filter A/B finished, every return
    // sent) and the NPC tasks are gone, or 30 s passed since: a marker for --until.
    bool work = f.mode == Mode::Filter ? (f.filter_phase >= 4 || f.live.empty()) : f.next >= f.live.size();
    for (const Summoned& m : f.summoned) {
        work = work && (f.return_after < 0 || m.returned);
    }
    if (work && !f.work_done) {
        f.work_done = true;
        f.work_done_at = now;
    }
    if (f.work_done && !f.done) {
        const std::size_t tasks = NpcTasks(nullptr).size();
        const double waited = Seconds(f.work_done_at, now);
        if ((tasks == 0 && waited >= 3.0) || waited >= 30.0) {
            f.done = true;
            LogState(f, s, true);
            Log("done in %s: %zu summoned, %zu NPC tasks left, cooperators %d, slot count %d", MapName(s.map_id).c_str(),
                f.summoned.size(), tasks, CooperatorCount(s), Field<i32>(SlotTable(), 0x14, -1));
        }
    }
}

} // namespace coop
