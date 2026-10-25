// SPDX-License-Identifier: GPL-3.0-or-later
// Four-player parties (see party_fourp.h and docs/party/four_players.md). Every site was read in
// the 1.09 eboot (capstone over out/eboot.elf); each install compares the bytes first.
#include "party_fourp.h"
#include "party_async_io.h"
#include "party_perf.h"

#include "../../party_env.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#ifndef BB_PARTY_FOURP_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_runtime.h"
#include "seamless_rules.h"
#include "../net/bbnet_internal.h"
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long skip, unsigned long n,
                                                                                    void** frames, unsigned long* hash);
#endif

#include <algorithm>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#endif

namespace coop::fourp {

namespace {

bool EnvIsZero(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

} // namespace

// ---- Pure rules ----

Config ConfigFromEnv() {
    Config c;
    c.on = bb_party_enabled() && !EnvIsZero("BB_PARTY_FOURP");
    c.scaling = !EnvIsZero("BB_PARTY_FOURP_SCALING");
    c.npc_signs = !EnvIsZero("BB_PARTY_FOURP_NPC_SIGNS");
    c.recruit = !EnvIsZero("BB_PARTY_FOURP_RECRUIT");
    int m = 3;
    if (const char* v = std::getenv("BB_PARTY_MAX"); v && *v) {
        m = std::atoi(v);
    }
    c.local_max = m < 2 ? 2 : m > kMaxPlayers ? kMaxPlayers : m;
    return c;
}

std::string RulesTag(const Config& c) {
    if (!c.on) {
        return "4p:off";
    }
    std::string t = "4p:v2:H1,H5";
    if (c.scaling) {
        t += ",H2,H3,H4";
    }
    if (c.npc_signs) {
        t += ",E6";
    }
    if (c.local_max >= 5) {
        t += ",H6";  // the SOS filter's guest literal 4 -> 5 (a 4th cooperator)
    }
    return t;
}

std::string FourpRulesTag() {
    return RulesTag(ConfigFromEnv());
}

int EffectiveMaxPlayers(int local_max, bool guest, bool connected, int host_max) {
    int m = guest && connected && host_max > 0 ? host_max : local_max;
    return m < 2 ? 2 : m > kMaxPlayers ? kMaxPlayers : m;
}

int SelectorMaxCoop(int max_players) {
    // The SOS filter 0x1874710 refuses a cooperator request when max <= pending + cooperators,
    // the selector update 0x1876b10 stops at pending + cooperators >= max: vanilla 3 = 2
    // cooperators + the one being summoned. A party of N needs N (N - 1 cooperators + 1 pending).
    return max_players >= 4 ? max_players : 3;
}

bool LiftCoopSlot(int rec0_status, int coop_count, int max_players) {
    // Status 2 = used; 3 (unavailable: no host, blocked area) and 0 (free) stay as they are.
    return max_players >= 4 && rec0_status == 2 && coop_count >= 0 && coop_count < max_players - 1;
}

int BossCount(int count) {
    return count > 2 ? 2 : count;
}

int DopingSpEffect(int sp_effect, int coop_count) {
    return sp_effect == 7501 && coop_count >= 3 ? 7502 : sp_effect;
}

bool IsDopingSpEffect(int sp_effect) {
    return sp_effect >= 7500 && sp_effect <= 7502;
}

DopingAction DopingApply(int sp_effect, int coop_count, int doping_held, bool dedupe) {
    if (!IsDopingSpEffect(sp_effect)) {
        return DopingAction::Keep;
    }
    if (dedupe && doping_held > 0) {
        return DopingAction::Skip;
    }
    return DopingSpEffect(sp_effect, coop_count) != sp_effect ? DopingAction::Upgrade : DopingAction::Keep;
}

void SpotOffset(int spot, float d, float* right, float* forward) {
    // Side by side in a line on the host's right axis (upstream 12490f5: wider offsets or spots
    // behind put guests into walls at narrow lamp spots): 1 right, 2 left, 3 2x right; 4 2x left
    // for a larger party. Neighbours d apart, nobody on the host (spot 0).
    static const float kRight[5] = {0, 1, -1, 2, -2};
    if (spot < 0 || spot >= 5) {
        spot = 3;
    }
    *right = kRight[spot] * d;
    *forward = 0;
}

int PickFreeSpot(int slot, float d, float hx, float hz, float yaw, const float (*others)[2], int n,
                 float min_gap) {
    // Candidate offsets along the host's right axis, in spacings: the own spot first.
    static const float kLine[6] = {1, -1, 2, -2, 3, -3};
    float own_r = 0, own_f = 0;
    SpotOffset(slot, d, &own_r, &own_f);
    const float rx = std::cos(yaw), rz = -std::sin(yaw);
    const auto free_at = [&](float r) {
        const float x = hx + rx * r, z = hz + rz * r;
        for (int i = 0; i < n; ++i) {
            const float dx = x - others[i][0], dz = z - others[i][1];
            if (dx * dx + dz * dz < min_gap * min_gap) return false;
        }
        return true;
    };
    if (free_at(own_r)) return -1;  // the own spot
    for (int k = 0; k < 6; ++k) {
        if (free_at(kLine[k] * d)) return k;
    }
    return -1;
}

float FreeSpotRight(int pick, int slot, float d) {
    static const float kLine[6] = {1, -1, 2, -2, 3, -3};
    if (pick >= 0 && pick < 6) return kLine[pick] * d;
    float r = 0, f = 0;
    SpotOffset(slot, d, &r, &f);
    return r;
}

int PlayerNumberForSlot(int slot) {
    return slot <= 0 ? 1 : slot < 0x7ffe ? slot + 1 : 2;
}

bool PlayerNumberValid(int n) {
    return n == 1 || (n >= 2 && n <= 0x7ffe);
}

int SpotForTry(int slot, int attempt) {
    if (slot < 1 || slot >= 5) {
        slot = 1;
    }
    static const int kOther[5] = {0, 2, 1, 4, 3}; // the mirrored spot
    if (attempt <= 0) {
        return slot;
    }
    if (attempt == 1) {
        return kOther[slot];
    }
    return slot == 3 ? 1 : 3;
}

bool IsNpcSignEvent(int event_id) {
    if (event_id == 12906962) {
        return true;
    }
    if (event_id < 10000000) {
        return false;
    }
    const int low = event_id % 10000;
    return low >= 4400 && low <= 4406;
}

int NpcSignCount(const std::uint8_t a[4], int max_players) {
    // [group][client type 0 = cooperators][comparison 3 = "<"][count]
    if (a[1] != 0 || a[2] != 3) {
        return -1;
    }
    if (max_players >= 4 && a[3] == 2) {
        return 3;
    }
    if (max_players < 4 && a[3] == 3) {
        return 2; // only for bytes we rewrote ourselves (the caller checks)
    }
    return -1;
}

#ifndef BB_PARTY_FOURP_NO_GAME

// ---- Game side ----

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party 4p: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

// Our offsets (1.09).
constexpr u64 kSlotSummary = 0x186fe40;    // SessionSlotSummary(out, selector, debug)
constexpr u64 kCountKind = 0x18796c0;      // selector members of a session kind
constexpr u64 kBossCountCall = 0x17bf187;  // EMEVD 1003[109]: call 0x15bdc20
constexpr u64 kCooperatorCount = 0x15bdc20;
constexpr u64 kMultiDopingJne = 0x138bcbf; // cmp eax,2; jne
constexpr u64 kApplySpEffect = 0x18c6db0;  // chr vfunc +0x3f0
constexpr u64 kRecruitNum = 0x1e97770;     // mov ecx,2
constexpr u64 kNetFlow = 0x5556678;        // +0x16f8 the session slot table
constexpr u64 kWorldChrMan = 0x553e878;
constexpr u64 kSessionMan = 0x5540290;

Config g_cfg;
std::atomic<int> g_max{3};
bool g_init = false;
bool g_h1 = false, g_h2 = false, g_h3 = false, g_h4 = false, g_e6 = false, g_p5 = false, g_h6 = false;

std::atomic<u64> g_h5_writes{0};
std::atomic<u64> g_h1_lifts{0}, g_h1_calls{0}, g_h2_clamps{0}, g_h4_upgrades{0}, g_e6_rewrites{0},
    g_e6_reverts{0};

using SummaryFn = u64(BB_COOP_SYSV*)(u64 out, u64 sel, u64 dbg);
using CountKindFn = i32(BB_COOP_SYSV*)(u64 sel, i32 kind);
using CountFn = i32(BB_COOP_SYSV*)(u64 slots);
SummaryFn g_summary_original = nullptr;

// The game's cooperator count without the world checks' cost: set by FourpTick (main thread).
std::atomic<int> g_coop_now{-1};
int CooperatorsNowFast() {
    return g_coop_now.load(std::memory_order_relaxed);
}

// H1 diagnostics: what the summary holds next to the per-kind counts (status 3 unavailable, 0
// free, 2 used at +4 / +0x10 / +0x1c / +0x28; +0x33 members counted) and the game's cooperator
// count, each time that shape changes (first 40). The lift counts on the per-kind sum; this shows
// whether one phantom is counted under two kinds.
std::mutex g_shape_mu;
std::string g_shape_last;
int g_shape_logs = 0;
void LogSummaryShape(u64 out, const i32 per_kind[5], int coop_now) {
    i32 st[4] = {-1, -1, -1, -1};
    u8 total = 0xff;
    SafeGet(out + 4, &st[0]);
    SafeGet(out + 0x10, &st[1]);
    SafeGet(out + 0x1c, &st[2]);
    SafeGet(out + 0x28, &st[3]);
    SafeGet(out + 0x33, &total);
    char b[200];
    std::snprintf(b, sizeof b,
                  "status rec0..3 %d %d %d %d, counted %u; kinds 7:%d 1:%d 0x15:%d 0x16:%d 0x17:%d; game cooperators %d",
                  st[0], st[1], st[2], st[3], total, per_kind[0], per_kind[1], per_kind[2], per_kind[3], per_kind[4],
                  coop_now);
    std::lock_guard<std::mutex> lk(g_shape_mu);
    if (g_shape_logs >= 40 || g_shape_last == b) {
        return;
    }
    g_shape_last = b;
    ++g_shape_logs;
    Log("H1 summary: %s", b);
}

// ---- A7: the per-peer packet queues (four_players.md 9) ----
// SprjNetMan (*0x553D6D0) +0x28: a std::list of peer entries {+8 PlayerId, +0x30 queue per packet
// type (0x44): +8 list, +0x10 count}. A received packet is pushed by 0x178D510 -> 0x1790C20 and
// popped by 0x1789200 -> 0x17910F0 (memcpy, free, unlink); neither takes a lock. The chr threads
// (CSChrThread1 / 2) pop the remote chrs' type-6 updates (0x1A6D390) while other threads push and
// pop: a 4-player rejoin crashed two guests there (null memcpy source, "heap not found" on the
// free). Every push and pop now runs under one lock; the first overlaps it saw are logged.
constexpr u64 kNetMan = 0x553d6d0;
constexpr u64 kPushCall = 0x178d5cc, kPushFn = 0x1790c20;  // in 0x178D510: (entry, reliable, data, size)
constexpr u64 kPopJmp = 0x1789364, kPopFn = 0x17910f0;     // tail jump in 0x1789200: (entry, type, buf, size)
constexpr u64 kChrReadCall = 0x1a6d449, kPacketRead = 0x1789200;
std::mutex g_q_mu;
bool g_q_on = false;
struct QBusy {
    std::atomic<u64> entry{0};
    std::atomic<u32> tid{0};
    std::atomic<u32> type{0};
    std::atomic<int> op{0};  // 1 push, 2 pop
    std::atomic<const char*> name{nullptr};  // the owner's thread_local name
};
QBusy g_q_busy[32];
std::atomic<u64> g_q_pushes{0}, g_q_pops{0}, g_q_overlaps{0}, g_q_chr_reads{0};
std::atomic<int> g_q_overlap_logs{0};
thread_local u64 t_chr_peer = 0;  // the PlayerId a chr thread's 0x1A6D390 is reading for

extern "C" int runtime_win_thread_name(std::uint32_t thread_id, char* out, std::size_t size);
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
#endif
u32 ThisTid() {
#ifdef _WIN32
    return u32(GetCurrentThreadId());
#else
    return 0;
#endif
}
/// This thread's name (the runtime's guest thread name; "tid N" if it has none).
const char* ThisName() {
    thread_local char name[40] = {};
    if (!name[0]) {
        if (runtime_win_thread_name(0, name, sizeof name) != 0 || !name[0]) {
            std::snprintf(name, sizeof name, "tid %u", ThisTid());
        }
    }
    return name;
}

/// Marks this thread busy on (entry, type); logs (first 12) another thread busy on the same entry.
int QEnter(u64 entry, u32 type, int op) {
    const u32 me = ThisTid();
    int mine = -1;
    for (int i = 0; i < 32; ++i) {
        QBusy& b = g_q_busy[i];
        const u64 e = b.entry.load(std::memory_order_acquire);
        if (e == entry && e && b.tid.load() != me) {
            g_q_overlaps.fetch_add(1);
            if (g_q_overlap_logs.fetch_add(1) < 12) {
                Log("A7: packet queue overlap%s: %s %s entry %#llx type %u while %s %s type %u (chr peer %#llx); "
                    "serialised",
                    type == b.type.load() || op == 1 || b.op.load() == 1 ? " (same queue)" : "",
                    ThisName(), op == 1 ? "pushes to" : "pops", ull(entry), type,
                    b.name.load() ? b.name.load() : "?", b.op.load() == 1 ? "pushes" : "pops", b.type.load(),
                    ull(t_chr_peer));
            }
        }
    }
    for (int i = 0; i < 32 && mine < 0; ++i) {
        u32 z = 0;
        if (g_q_busy[i].tid.compare_exchange_strong(z, me)) {  // claim the slot, then publish the entry
            g_q_busy[i].type = type;
            g_q_busy[i].op = op;
            g_q_busy[i].name = ThisName();
            g_q_busy[i].entry.store(entry ? entry : 1, std::memory_order_release);
            mine = i;
        }
    }
    return mine;
}
void QLeave(int i) {
    if (i >= 0) {
        g_q_busy[i].entry.store(0, std::memory_order_release);
        g_q_busy[i].tid.store(0, std::memory_order_release);
    }
}

BB_COOP_SYSV u64 QueuePush(u64 entry, u64 reliable, u64 data, u64 size) {
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64);
    const int slot = QEnter(entry, 0xffffffffu, 1);
    g_q_pushes.fetch_add(1, std::memory_order_relaxed);
    u64 r;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        r = reinterpret_cast<Fn>(Guest(kPushFn))(entry, reliable, data, size);
    }
    QLeave(slot);
    return r;
}

// G3 test aid (`BB_PARTY_TEST_DROP_PACKET=TYPE[:COUNT]`, test runs only): this game throws away
// the next COUNT (default 1) received game packets of TYPE, as if the link had lost them; with
// the aid set, the first packets of every type popped are logged (type, size, first words).
struct DropPacket {
    u32 type = 0xffffffffu;
    int left = 0;
};
DropPacket& DropPacketCfg() {
    static DropPacket d = [] {
        DropPacket a;
        if (const char* v = std::getenv("BB_PARTY_TEST_DROP_PACKET"); v && *v) {
            a.type = u32(std::strtoul(v, nullptr, 0));
            const char* c = std::strchr(v, ':');
            a.left = c ? std::atoi(c + 1) : 1;
        }
        return a;
    }();
    return d;
}
std::atomic<int> g_drop_logs[0x44];
double QNow();
// A timed window (`BB_PARTY_TEST_DROP_FILE=PATH`): when the file's text ("TYPE[,TYPE...][@VALUE] MS")
// changes, every packet of those types (carrying the u32 VALUE, if given) popped in the next MS is
// thrown away. A network-synced event's state goes to the other games as packet 0x31 (24 bytes:
// 00010000 00000008 EVENT SLOT|... STATE; live: 12400750 = 0x00BD386E from the guest's door press),
// which is how the host's copy of the Cathedral Ward door event follows a guest's action: on the
// host `0x31@12400750 8000` loses exactly that action.
std::atomic<u64> g_drop_win_types{0};  // bit per packet type (< 0x40)
std::atomic<u32> g_drop_win_type{0xffffffffu};
std::atomic<u32> g_drop_win_value{0};  // only packets carrying this u32 (0: all)
std::atomic<double> g_drop_win_until{0};
std::atomic<int> g_drop_win_count{0};
void DropFilePoll() {
    static const char* path = std::getenv("BB_PARTY_TEST_DROP_FILE");
    if (!path || !*path) return;
    static double next = 0;
    static std::string last;
    const double now = QNow();
    if (now < next) return;
    next = now + 0.1;
    FILE* f = std::fopen(path, "rb");
    if (!f) return;
    char b[64] = {};
    const size_t n = std::fread(b, 1, sizeof b - 1, f);
    std::fclose(f);
    const std::string text(b, n);
    if (text == last) return;
    last = text;
    // "TYPE[,TYPE...] MS"
    char* end = b;
    u64 types = 0;
    u32 first = 0xffffffffu;
    for (;;) {
        char* e2 = nullptr;
        const u32 t = u32(std::strtoul(end, &e2, 0));
        if (e2 == end) break;
        if (t < 0x40) types |= u64(1) << t;
        if (first == 0xffffffffu) first = t;
        end = e2;
        if (*end != ',') break;
        ++end;
    }
    u32 value = 0;
    if (*end == '@') value = u32(std::strtoul(end + 1, &end, 0));
    const double ms = std::strtod(end, nullptr);
    if (!types || ms <= 0) return;
    g_drop_win_value = value;
    const int dropped = g_drop_win_count.exchange(0);
    for (auto& c : g_drop_logs) c = 0;  // log what comes in this window
    g_drop_win_types = types;
    g_drop_win_type = first;
    g_drop_win_until = now + ms / 1000.0;
    Log("G3 test: dropping every packet of types mask %#llx%s%u for %.0f ms (BB_PARTY_TEST_DROP_FILE; %d dropped in "
        "the last window)",
        ull(types), value ? " carrying " : "", value, ms, dropped);
}

BB_COOP_SYSV u64 QueuePop(u64 entry, u64 type, u64 buf, u64 size) {
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64);
    const int slot = QEnter(entry, u32(type), 2);
    g_q_pops.fetch_add(1, std::memory_order_relaxed);
    u64 r;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        r = reinterpret_cast<Fn>(Guest(kPopFn))(entry, type, buf, size);
        DropPacket& d = DropPacketCfg();
        const u32 t = u32(type);
        while ((d.type != 0xffffffffu || g_drop_win_type.load() != 0xffffffffu) && t < 0x44 && u32(r) > 0 && buf) {
            u32 words[5] = {};
            const u32 n = std::min<u32>(u32(r), sizeof words) / 4;
            for (u32 i = 0; i < n; ++i) SafeGet(buf + 4 * i, &words[i]);
            bool win = t < 0x40 && (g_drop_win_types.load() >> t & 1) && QNow() < g_drop_win_until.load();
            if (win && g_drop_win_value.load()) {
                bool carries = false;
                for (u32 i = 0; i < n; ++i) carries = carries || words[i] == g_drop_win_value.load();
                win = carries;
            }
            const bool drop = win || (t == d.type && d.left > 0);
            if (win) {
                const int c = g_drop_win_count.fetch_add(1);
                if (t != 6 || c < 3) {
                    Log("G3 test: packet 0x%02X (%u bytes) dropped (window): %08x %08x %08x %08x", t, u32(r), words[0],
                        words[1], words[2], words[3]);
                }
                r = reinterpret_cast<Fn>(Guest(kPopFn))(entry, type, buf, size);
                continue;
            }
            if (drop || g_drop_logs[t].fetch_add(1) < (t == 6 ? 2 : 12)) {
                Log("G3 test: packet 0x%02X (%u bytes) from peer entry %#llx: %08x %08x %08x %08x %08x%s", t, u32(r),
                    ull(entry), words[0], words[1], words[2], words[3], words[4],
                    drop ? " -> DROPPED (BB_PARTY_TEST_DROP_PACKET: as if the link lost it)" : "");
            }
            if (!drop) {
                break;
            }
            --d.left;
            r = reinterpret_cast<Fn>(Guest(kPopFn))(entry, type, buf, size);  // the next one, if any
        }
    }
    {
        // BB_PARTY_PKT_PROBE=1 (diagnostic, off by default, 700 lines at most): every popped packet but the chr
        // updates (type 6) with its first words, to find what reaches a lamp / headstone event from a peer.
        static const bool probe = [] { const char* v = std::getenv("BB_PARTY_PKT_PROBE"); return v && v[0] == '1'; }();
        static std::atomic<int> logged{0};
        if (probe && u32(r) > 0 && buf && u32(type) != 6 && u32(type) < 0x44 && logged.fetch_add(1) < 700) {
            u32 w[5] = {};
            for (u32 i = 0; i < std::min<u32>(u32(r), sizeof w) / 4; ++i) SafeGet(buf + 4 * i, &w[i]);
            Log("packet probe: type 0x%02X (%u bytes): %08x %08x %08x %08x %08x", u32(type), u32(r), w[0], w[1], w[2],
                w[3], w[4]);
        }
    }
    QLeave(slot);
    return r;
}

// Which chr reads which peer's type-6 queue. Every remote player chr's update (0x1A6D390, chr
// threads) pops the type-6 queue of the peer entry its PlayerId matches (0x1789380 walks the list
// with 0xCA1C70: equal by the member, not by the object). A member that leaves and rejoins while
// others stay leaves its old player entry on the bystanders (parab3 a7t1: presence count 4 -> 5
// with 4 players); its old chr and the new one then match the same entry and two chr threads pop
// one queue at once (the 4-player crash), and each gets half the updates. The newest reader of an
// entry keeps it; an older one reading the same entry within 1 s gets no packet.
constexpr u64 kPeerEntryOf = 0x1789380;  // entry (netman, const PlayerId*)
struct QReader {
    u64 entry = 0, id = 0;
    double first = 0, last = 0;
};
std::mutex g_q_readers_mu;
std::vector<QReader> g_q_readers;
std::atomic<u64> g_q_stale_skips{0};
std::atomic<int> g_q_stale_logs{0};
double QNow() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

BB_COOP_SYSV u64 ChrPacketRead(u64 man, u64 peer, u64 type, u64 buf, u64 size) {
    using Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64);
    using EntryFn = u64(BB_COOP_SYSV*)(u64, u64);
    u64 obj = 0;
    SafeGet(peer, &obj);
    t_chr_peer = obj;
    g_q_chr_reads.fetch_add(1, std::memory_order_relaxed);
    const u64 entry = man && obj ? reinterpret_cast<EntryFn>(Guest(kPeerEntryOf))(man, peer) : 0;
    if (entry) {
        const double now = QNow();
        std::lock_guard<std::mutex> lk(g_q_readers_mu);
        g_q_readers.erase(std::remove_if(g_q_readers.begin(), g_q_readers.end(),
                                         [&](const QReader& r) { return now - r.last > 3.0; }),
                          g_q_readers.end());
        QReader* me = nullptr;
        for (QReader& r : g_q_readers) {
            if (r.entry == entry && r.id == peer) me = &r;
        }
        if (!me) {
            int others = 0;
            for (const QReader& r : g_q_readers) others += r.entry == entry ? 1 : 0;
            g_q_readers.push_back({entry, peer, now, now});
            me = &g_q_readers.back();
            Log("A7: %s: the chr with PlayerId %#llx (peer object %#llx) reads peer entry %#llx (%d other reader(s) of "
                "it)",
                ThisName(), ull(peer), ull(obj), ull(entry), others);
        }
        me->last = now;
        const QReader mine = *me;
        for (const QReader& r : g_q_readers) {
            if (r.entry == entry && r.id != peer && r.first > mine.first && now - r.last < 1.0) {
                g_q_stale_skips.fetch_add(1, std::memory_order_relaxed);
                if (g_q_stale_logs.fetch_add(1) < 6) {
                    Log("A7: two chrs read peer entry %#llx: the older one (PlayerId %#llx) gets no packet, the newer "
                        "(PlayerId %#llx) keeps the updates",
                        ull(entry), ull(peer), ull(r.id));
                }
                t_chr_peer = 0;
                return 0;
            }
        }
    }
    const u64 r = reinterpret_cast<Fn>(Guest(kPacketRead))(man, peer, type, buf, size);
    t_chr_peer = 0;
    return r;
}

// C6 RE aid (`BB_PARTY_TRACE_FROM_UPDATE=N`): logs the first N calls of SprjNetworkClientMan's update
// 0x1E7DF20(man, dt*) (its timers, then the HTTP client update 0xFD10D0 that sends queued requests and
// the completions): how often the FROM client runs decides how fast the title's sign-in steps go.
std::atomic<int> g_from_upd_left{0};
BB_COOP_SYSV void FromUpdateEntry(u64 man, u64 dt, u64, u64, u64, u64) {
    if (g_from_upd_left.fetch_sub(1) <= 0) return;
    float d = -1;
    SafeGet(dt + 8, &d);
    static double last = 0;
    const double now = QNow();
    Log("C6 trace: FROM client update (dt %.3f s, %.3f s since the last call) on %s", double(d),
        last ? now - last : 0.0, ThisName());
    last = now;
}

// The title's sign-in stages (lambdas of the title flow built by 0x1C37B30): when each starts and when
// the login answer is taken, against the HTTP sends (`http trace:`).
template <int K>
BB_COOP_SYSV void FromStageEntry(u64, u64, u64, u64, u64, u64) {
    static const char* const kNames[] = {"ss.info stage starts (0x1C492C0)", "login stage starts (0x1C48AA0)",
                                         "login answer taken (0x1C48CA0)", "ss.info answer taken (0x1E89850)"};
    static std::atomic<int> left{6};
    if (left.fetch_sub(1) > 0) Log("C6 trace: %s on %s", kNames[K], ThisName());
}

void InstallQueueGuard() {
    if (const char* v = std::getenv("BB_PARTY_TRACE_FROM_UPDATE"); v && std::atoi(v) > 0) {
        HookPrologue(0x1C492C0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53}, &FromStageEntry<0>,
                     "C6 trace: ss.info stage");
        HookPrologue(0x1C48AA0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &FromStageEntry<1>, "C6 trace: login stage");
        HookPrologue(0x1C48CA0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &FromStageEntry<2>, "C6 trace: login answer");
        HookPrologue(0x1E89850, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &FromStageEntry<3>, "C6 trace: ss.info answer");
        g_from_upd_left = std::atoi(v);
        HookPrologue(0x1E7DF20, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &FromUpdateEntry, "C6 trace: FROM client update (0x1E7DF20)");
    }
    if (const char* v = std::getenv("BB_PARTY_QUEUE_GUARD"); v && v[0] == '0' && !v[1]) {
        Log("A7: packet queue lock off (BB_PARTY_QUEUE_GUARD=0)");
        return;
    }
    const bool push = HookCallSite(kPushCall, kPushFn, reinterpret_cast<const void*>(&QueuePush),
                                   "A7 packet queue push lock (0x178D510)");
    const bool pop = push && HookTailJump(kPopJmp, kPopFn, reinterpret_cast<const void*>(&QueuePop),
                                          "A7 packet queue pop lock (0x1789200)");
    HookCallSite(kChrReadCall, kPacketRead, reinterpret_cast<const void*>(&ChrPacketRead),
                 "A7 remote chr packet read trace (0x1A6D390)");
    g_q_on = push && pop;
    Log("A7: packet queue lock %s (push %s, pop %s)", g_q_on ? "on" : "NOT installed", push ? "ok" : "no",
        pop ? "ok" : "no");
}

/// The SprjNetMan peer entries (main thread, logged when the list changes).
std::string g_q_list_last;
void QueueTick() {
    DropFilePoll();
    if (!g_q_on) {
        return;
    }
    u64 man = 0, head = 0;
    if (!SafeGet(Guest(kNetMan), &man) || !man || !SafeGet(man + 0x28, &head) || !head) {
        return;
    }
    std::string s;
    u64 node = 0;
    SafeGet(head, &node);
    for (int guard = 0; node && node != head && guard < 12; ++guard) {
        u64 e = 0, obj = 0;
        SafeGet(node + 0x10, &e);
        SafeGet(e + 8, &obj);
        char b[64];
        std::snprintf(b, sizeof b, " [%#llx id %#llx]", ull(e), ull(obj));
        s += b;
        if (!SafeGet(node, &node)) {
            break;
        }
    }
    if (s.empty()) {
        s = " none";
    }
    if (s != g_q_list_last) {
        g_q_list_last = s;
        Log("A7: peer queues:%s (pushes %llu, pops %llu, chr reads %llu, overlaps %llu, stale chr reads skipped %llu)",
            s.c_str(), ull(g_q_pushes.load()), ull(g_q_pops.load()), ull(g_q_chr_reads.load()), ull(g_q_overlaps.load()),
            ull(g_q_stale_skips.load()));
    }
}

// H1
BB_COOP_SYSV u64 SlotSummary(u64 out, u64 sel, u64 dbg) {
    const u64 r = g_summary_original(out, sel, dbg);
    perf::Scope z(perf::Zone::HookFourpSlot);
    g_h1_calls.fetch_add(1, std::memory_order_relaxed);
    const int max = g_max.load(std::memory_order_relaxed);
    // H5: the selector's own "max cooperators" (sel+0x1fc, ctor 0x1871259 writes 3), read by the SOS
    // filter 0x1874710 (pending + cooperators < max) and the selector update 0x1876b10. Live
    // (2026-10-11, 4 clients): with 2 cooperators in the world the 3rd one's request was refused
    // whenever its own entry was pending (1 + 2 >= 3): its invite was dropped in the handshake.
    // Only values the game or we wrote (3 / party size) are touched.
    if (sel) {
        const i32 want = SelectorMaxCoop(max);
        i32 cur = 0;
        if (SafeGet(sel + 0x1fc, &cur) && cur != want && (cur == 3 || (cur >= 4 && cur <= kMaxPlayers)) &&
            SafeWrite(sel + 0x1fc, &want, 4) && g_h5_writes.fetch_add(1) < 16) {
            Log("H5: selector max cooperators %d -> %d (max players %d)", cur, want, max);
        }
    }
    if (max < 4 || !sel || !out) {
        return r;
    }
    i32 rec0 = -1;
    if (!SafeGet(out + 4, &rec0) || rec0 != 2) {
        return r;
    }
    const auto count_kind = reinterpret_cast<CountKindFn>(Guest(kCountKind));
    int coop = 0;
    i32 per_kind[5] = {};
    int k = 0;
    for (const i32 kind : {7, 1, 0x15, 0x16, 0x17}) {
        per_kind[k] = count_kind(sel, kind);
        coop += per_kind[k++];
    }
    // The per-kind sum counts one more than the phantoms in the world (live, 3 instances: kind 7
    // = 2 with 1 cooperator, 3 with 2), so with 2 cooperators it never let the 3rd in. The
    // game's own cooperator count (0x15bdc20, read by FourpTick) decides; the sum minus one
    // only while that is not known yet.
    const int coop_now = CooperatorsNowFast();
    LogSummaryShape(out, per_kind, coop_now);
    coop = coop_now >= 0 ? coop_now : coop - 1;
    if (LiftCoopSlot(rec0, coop, max)) {
        const i32 free_status = 0;
        static std::atomic<int> logged_coop{-1};
        g_h1_lifts.fetch_add(1, std::memory_order_relaxed);
        if (SafeWrite(out + 4, &free_status, 4) && logged_coop.exchange(coop) != coop) {
            Log("H1: a cooperator slot opened (%d cooperators in the world, kind counts %d, max players %d)", coop,
                per_kind[0] + per_kind[1] + per_kind[2] + per_kind[3] + per_kind[4], max);
        }
    }
    return r;
}

// Diagnostic (BB_PARTY_FOURP_PROBE=1): the SOS request filter 0x1874710(sel, request, flag) - its
// verdict and every count it reads (slots +8 coop / +0xc invaders / +0x10 other / +0x14 all, the
// room max *(0x553d6d0)+0xc, sel+0x1c8 outgoing, sel+0x1fc / +0x200 max coop / invaders, the pending
// session entries of sel+0x1c0 by team), each time that tuple changes (first 200).
using SosFilterFn = u64(BB_COOP_SYSV*)(u64 sel, u64 req, u64 flag);
SosFilterFn g_sos_original = nullptr;
std::mutex g_sos_mu;
std::string g_sos_last;
int g_sos_logs = 0;
BB_COOP_SYSV u64 SosFilterProbe(u64 sel, u64 req, u64 flag) {
    const u64 r = g_sos_original(sel, req, flag);
    u64 flow = 0, slots = 0, roomcfg = 0;
    i32 c8 = -1, cc = -1, c10 = -1, c14 = -1, room = -1, out = -1, mc = -1, mi = -1;
    SafeGet(Guest(kNetFlow), &flow);
    if (flow) SafeGet(flow + 0x16f8, &slots);
    if (slots) {
        SafeGet(slots + 8, &c8);
        SafeGet(slots + 0xc, &cc);
        SafeGet(slots + 0x10, &c10);
        SafeGet(slots + 0x14, &c14);
    }
    SafeGet(Guest(0x553d6d0), &roomcfg);
    if (roomcfg) SafeGet(roomcfg + 0xc, &room);
    if (sel) {
        SafeGet(sel + 0x1c8, &out);
        SafeGet(sel + 0x1fc, &mc);
        SafeGet(sel + 0x200, &mi);
    }
    // sel+0x1c0: a list head; nodes {next, prev, data}; data+8 = the session kind (char), the
    // descriptor 0x553d760 + kind * 0x80 = its team (1 coop, 2 invader).
    int pend[3] = {0, 0, 0}, nodes = 0;
    u64 head = 0;
    if (sel && SafeGet(sel + 0x1c0, &head) && head) {
        u64 n = 0;
        SafeGet(head, &n);
        while (n && n != head && nodes < 32) {
            u64 data = 0;
            std::int8_t kind = -1;
            i32 team = -1;
            SafeGet(n + 0x10, &data);
            if (data && SafeGet(data + 8, &kind) && kind >= 0 && kind < 0x22) {
                SafeGet(Guest(0x553d760) + u64(kind) * 0x80, &team);
            }
            pend[team == 1 ? 1 : team == 2 ? 2 : 0]++;
            ++nodes;
            u64 next = 0;
            if (!SafeGet(n, &next)) break;
            n = next;
        }
    }
    u8 rq[16] = {};
    if (req) SafeRead(req, rq, sizeof rq);
    char b[400];
    std::snprintf(b, sizeof b,
                  "-> %llx (flag %llu); slots coop %d inv %d other %d all %d, room max %d; sel out %d, max coop %d inv %d; "
                  "pending coop %d inv %d other %d; game cooperators %d; req %02x%02x%02x%02x %02x%02x%02x%02x "
                  "%02x%02x%02x%02x %02x%02x%02x%02x",
                  ull(r), ull(flag & 0xff), c8, cc, c10, c14, room, out, mc, mi, pend[1], pend[2], pend[0],
                  CooperatorsNowFast(), rq[0], rq[1], rq[2], rq[3], rq[4], rq[5], rq[6], rq[7], rq[8], rq[9], rq[10],
                  rq[11], rq[12], rq[13], rq[14], rq[15]);
    std::lock_guard<std::mutex> lk(g_sos_mu);
    if (g_sos_logs < 200 && g_sos_last != b) {
        g_sos_last = b;
        ++g_sos_logs;
        Log("SOS filter probe: %s", b);
    }
    return r;
}

// Diagnostic (with the probe): vport-40 session packets (the summon handshake): direction, size,
// the first 32 bytes, and on a send the game's return addresses (our offsets).
std::atomic<int> g_pkt_logs{0};
void PacketTrace(bool send, unsigned src, unsigned dst, const unsigned char* d, std::size_t n) {
    if (src != 40 && dst != 40) return;
    if (g_pkt_logs.fetch_add(1, std::memory_order_relaxed) >= 3000) return;
    char hex[3 * 32 + 1] = {};
    for (std::size_t i = 0; i < n && i < 32; ++i) std::snprintf(hex + 3 * i, 4, "%02x ", d[i]);
    std::string stack;
#ifdef _WIN32
    if (send) {
        void* frames[40] = {};
        const unsigned short got = RtlCaptureStackBackTrace(0, 40, frames, nullptr);
        const u64 base = reinterpret_cast<u64>(Image()), size = ImageSize();
        int kept = 0;
        for (unsigned short i = 0; i < got && kept < 12; ++i) {
            const u64 a = reinterpret_cast<u64>(frames[i]);
            if (base && a >= base && a < base + size) {
                char f[24];
                std::snprintf(f, sizeof f, " +%llx", ull(a - base));
                stack += f;
                ++kept;
            }
        }
    }
#endif
    Log("pkt %s %u->%u %zu B: %s%s%s", send ? "send" : "recv", src, dst, n, hex, stack.empty() ? "" : "| stack",
        stack.c_str());
}

// H2
BB_COOP_SYSV i32 BossCooperatorCount(u64 slots) {
    const i32 n = reinterpret_cast<CountFn>(Guest(kCooperatorCount))(slots);
    const i32 m = BossCount(n);
    if (m != n && g_h2_clamps.fetch_add(1) == 0) {
        Log("H2: boss scaling sees %d cooperators as 2 (then H4: SpEffect 7501 -> 7502)", n);
    }
    return m;
}

// The game's cooperator count, guarded like game_state.cpp (0x15bdc20 asserts without WorldChrMan
// or SprjSessionManager); -1 when it cannot be asked.
int CooperatorsNow() {
    u64 wcm = 0, player = 0, sm = 0, flow = 0, slots = 0;
    if (!SafeGet(Guest(kWorldChrMan), &wcm) || !wcm || !SafeGet(wcm + 0x60, &player) || !player ||
        !SafeGet(Guest(kSessionMan), &sm) || !sm || !SafeGet(Guest(kNetFlow), &flow) || !flow ||
        !SafeGet(flow + 0x16f8, &slots) || !slots) {
        return -1;
    }
    return reinterpret_cast<CountFn>(Guest(kCooperatorCount))(slots);
}

// E6
std::mutex g_e6_mu;
std::set<u64> g_e6_written; // argument bytes we changed (EVD data may be shared)

void NpcSignRewriter(int event_id, int bank, int id, u64 event, u64 instr) {
    if (bank != 3 || id != 29 || !IsNpcSignEvent(event_id)) {
        return;
    }
    u32 size = 0;
    const u64 args = EmevdInstructionArgs(event, instr, &size);
    u8 a[4] = {};
    if (!args || size < 4 || !SafeRead(args, a, 4)) {
        return;
    }
    const int max = g_max.load(std::memory_order_relaxed);
    const int n = NpcSignCount(a, max);
    if (n < 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_e6_mu);
    if (n == 2 && !g_e6_written.count(args)) {
        return; // a "< 3" we did not write: the game's own
    }
    const u8 b = u8(n);
    if (!SafeWrite(args + 3, &b, 1)) {
        return;
    }
    if (n == 3) {
        g_e6_written.insert(args);
        if (g_e6_rewrites.fetch_add(1) < 32) {
            Log("E6: NPC sign event %d: 3[29] cooperators < 2 -> < 3", event_id);
        }
    } else {
        g_e6_written.erase(args);
        if (g_e6_reverts.fetch_add(1) < 32) {
            Log("E6: NPC sign event %d: back to cooperators < 2 (max players %d)", event_id, max);
        }
    }
}

struct TickState {
    Clock::time_point last_log{};
    u64 seen_h1 = 0, seen_h2 = 0, seen_h4 = 0, seen_e6 = 0;
};
TickState g_tick;

} // namespace
} // namespace coop::fourp

// H4 at the entry of 0x18c6db0 (chr ApplySpEffect, vfunc +0x3f0; returns al = applied). Arguments:
// rdi chr, esi SpEffect id, rdx the source chr, cl, r8b (1: a local-only apply, 0: the game sends it
// on to the other players after applying, 0x18c7080), r9, xmm0-4 and a stack argument - all kept;
// the stub saves the System V caller-saved registers (and xmm0-7) around the check (only for the
// doping rows 7500-7502), then jumps to the trampoline: 7501 -> 7502 with 3 cooperators, or
// returns 0 (not applied) for a second doping row on a chr that already holds one (D7 dedupe).
extern "C" {
__attribute__((used)) void* bb_fourp_spfx_original = nullptr;
__attribute__((used)) BB_COOP_SYSV int bb_fourp_spfx_upgrade(std::uint64_t chr, int id, std::uint64_t src,
                                                             std::uint64_t rcx, std::uint64_t r8,
                                                             std::uint64_t ret) {
    using namespace coop::fourp;
    using coop::Image;
    using coop::SafeGet;
    coop::perf::Scope z(coop::perf::Zone::HookFourpSpfx);
    (void)rcx;
    // No max-players gate: 3 cooperators exist only once H1 let them in, and a guest must scale
    // like its host whatever it was told.
    const int n = CooperatorsNow();
    int held = 0, held_id = 0;
    std::uint64_t list = 0, node = 0;
    if (chr && SafeGet(chr + 0x1c8, &list) && list && SafeGet(list + 8, &node)) {
        for (int guard = 0; node && guard < 256; ++guard) {
            std::uint32_t nid = 0;
            if (SafeGet(node + 0x40, &nid) && IsDopingSpEffect(int(nid))) {
                ++held;
                held_id = int(nid);
            }
            if (!SafeGet(node + 0x58, &node)) break;
        }
    }
    static const bool dedupe = !EnvIsZero("BB_PARTY_DOPING_DEDUPE");
    const DopingAction act = DopingApply(id, n, held, dedupe);
    if (g_h4_upgrades.fetch_add(1) < 64) {
        std::uint32_t handle = ~0u;
        int entity = -1;
        SafeGet(chr + 8, &handle);
        SafeGet(chr + 0x1f0, &entity);
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(Image());
        Log("H4: doping SpEffect %d on chr %x (entity %d, %d cooperators, source %s, %s, caller +0x%llx): "
            "%s",
            id, handle, entity, n, src == chr ? "itself" : src ? "another chr" : "none",
            (r8 & 0xff) ? "local apply" : "applied and sent on",
            static_cast<unsigned long long>(base && ret > base ? ret - base : ret),
            act == DopingAction::Skip      ? (held_id == id ? "SKIPPED: the chr already holds it (dedupe)"
                                                            : "SKIPPED: the chr already holds another doping row (dedupe)")
            : act == DopingAction::Upgrade ? "applied as 7502 (3 cooperators)"
                                           : "applied");
    }
    return act == DopingAction::Skip ? 2 : act == DopingAction::Upgrade ? 1 : 0;
}
void bb_fourp_spfx_stub(void);
}

asm(R"(
    .text
    .p2align 4
    .globl bb_fourp_spfx_stub
bb_fourp_spfx_stub:
    cmpl $7500, %esi
    jb 1f
    cmpl $7502, %esi
    ja 1f
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    subq $0x88, %rsp
    movdqu %xmm0, 0x00(%rsp)
    movdqu %xmm1, 0x10(%rsp)
    movdqu %xmm2, 0x20(%rsp)
    movdqu %xmm3, 0x30(%rsp)
    movdqu %xmm4, 0x40(%rsp)
    movdqu %xmm5, 0x50(%rsp)
    movdqu %xmm6, 0x60(%rsp)
    movdqu %xmm7, 0x70(%rsp)
    movq 0xc8(%rsp), %r9
    call bb_fourp_spfx_upgrade
    movdqu 0x00(%rsp), %xmm0
    movdqu 0x10(%rsp), %xmm1
    movdqu 0x20(%rsp), %xmm2
    movdqu 0x30(%rsp), %xmm3
    movdqu 0x40(%rsp), %xmm4
    movdqu 0x50(%rsp), %xmm5
    movdqu 0x60(%rsp), %xmm6
    movdqu 0x70(%rsp), %xmm7
    addq $0x88, %rsp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    cmpl $2, %eax
    je 2f
    testl %eax, %eax
    jz 1f
    movl $7502, %esi
1:
    jmpq *bb_fourp_spfx_original(%rip)
2:
    xorl %eax, %eax
    ret
)");

namespace coop::fourp {

void FourpInit() {
    if (g_init) {
        return;
    }
    g_cfg = ConfigFromEnv();
    g_max.store(g_cfg.local_max);
    if (bb_party_enabled() && Image()) {
        InstallQueueGuard();
    }
    if (!g_cfg.on) {
        Log("off (%s); rules tag %s", bb_party_enabled() ? "BB_PARTY_FOURP=0" : "BB_PARTY off", RulesTag(g_cfg).c_str());
        return;
    }
    if (!Image()) {
        Log("no image; off");
        return;
    }
    g_init = true;
    const std::initializer_list<u8> kPrologue13 = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                   0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
    void* original = nullptr;
    g_h1 = ReplacePrologue(kSlotSummary, kPrologue13, reinterpret_cast<const void*>(&SlotSummary), &original,
                           "4p H1 cooperator slots (SessionSlotSummary)");
    if (g_h1) {
        g_summary_original = reinterpret_cast<SummaryFn>(original);
    }
    if (const char* pr = std::getenv("BB_PARTY_FOURP_PROBE"); pr && pr[0] == '1') {
        void* sos_original = nullptr;
        if (ReplacePrologue(0x1874710, kPrologue13, reinterpret_cast<const void*>(&SosFilterProbe), &sos_original,
                            "4p SOS filter probe (0x1874710)")) {
            g_sos_original = reinterpret_cast<SosFilterFn>(sos_original);
        }
        bbnet::set_packet_trace(&PacketTrace);
    }
    if (g_cfg.scaling) {
        g_h2 = HookCallSite(kBossCountCall, kCooperatorCount, reinterpret_cast<const void*>(&BossCooperatorCount),
                            "4p H2 boss scaling count (EMEVD 1003[109])");
        // cmp eax,2; jne +0x32 -> cmp eax,2; jl +0x32
        g_h3 = PatchBytes(kMultiDopingJne, {0x83, 0xf8, 0x02, 0x75, 0x32}, {0x83, 0xf8, 0x02, 0x7c, 0x32},
                          "4p H3 MultiDoping count >= 2");
        void* sp_original = nullptr;
        g_h4 = ReplacePrologue(kApplySpEffect, kPrologue13, reinterpret_cast<const void*>(&bb_fourp_spfx_stub),
                               &sp_original, "4p H4 boss doping (chr ApplySpEffect: 7501 -> 7502, one doping row per chr)");
        if (g_h4) {
            bb_fourp_spfx_original = sp_original;
        }
    }
    if (g_cfg.npc_signs) {
        SetEmevdRewriter(&NpcSignRewriter);
        g_e6 = EnsureEmevdFilter();
        if (!g_e6) {
            SetEmevdRewriter(nullptr);
        }
    }
    // H6 (5 players: a 4th cooperator): the SOS filter 0x1874710 refuses a request once
    // cooperators + invaders + others + outgoing requests (slots +8/+0xc/+0x10, sel+0x1c8) reach 4:
    // `cmp esi, 4` at 0x1874bbe -> 5. Only with BB_PARTY_MAX >= 5 (every peer: the rules tag says H6).
    if (g_cfg.local_max >= 5) {
        g_h6 = PatchBytes(0x1874bbe, {0x83, 0xfe, 0x04}, {0x83, 0xfe, 0x05}, "4p H6 SOS filter guests < 5 (5 players)");
    }
    if (g_cfg.recruit && g_cfg.local_max >= 4) {
        g_p5 = PatchBytes(kRecruitNum, {0xb9, 0x02, 0x00, 0x00, 0x00}, {0xb9, 0x03, 0x00, 0x00, 0x00},
                          "4p P5 RecruitNum 2 -> 3");
    }
    Log("rules %s: H1 %s, H2 %s, H3 %s, H4 %s, E6 %s, P5 %s, H6 %s; max players %d (local BB_PARTY_MAX; a guest "
        "takes the host's)",
        RulesTag(g_cfg).c_str(), g_h1 ? "on" : "OFF", g_h2 ? "on" : "off", g_h3 ? "on" : "off",
        g_h4 ? "on" : "off", g_e6 ? "on" : "off", g_p5 ? "on" : "off", g_h6 ? "on" : "off", g_cfg.local_max);
    if (!g_h1 && g_cfg.local_max >= 4) {
        Log("H1 not installed: the game stays at 2 cooperators");
    }
}

int FourpEffectiveMax() {
    return g_max.load(std::memory_order_relaxed);
}

void FourpTick() {
    QueueTick();
    if (!g_init) {
        return;
    }
    // The host's max players on a connected guest (WELCOME); our own otherwise.
    bool guest = false, connected = false;
    int host_max = 0;
    if (party::PartyLink* link = party::runtime_link()) {
        guest = !link->is_host();
        connected = link->state() == party::LinkState::Connected;
        host_max = link->max_players();
    }
    const int m = EffectiveMaxPlayers(g_cfg.local_max, guest, connected, host_max);
    const int old = g_max.exchange(m);
    if (old != m) {
        Log("max players %d -> %d (%s)", old, m,
            guest && connected ? "the host's setting" : "local BB_PARTY_MAX");
    }
    const auto now = Clock::now();
    static Clock::time_point coop_read{};
    if (now - coop_read >= std::chrono::milliseconds(500)) {
        coop_read = now;
        g_coop_now.store(CooperatorsNow(), std::memory_order_relaxed);
    }
    if (now - g_tick.last_log >= std::chrono::seconds(30)) {
        g_tick.last_log = now;
        const u64 h1 = g_h1_lifts.load(), h2 = g_h2_clamps.load(), h4 = g_h4_upgrades.load(),
                  e6 = g_e6_rewrites.load() + g_e6_reverts.load();
        if (h1 != g_tick.seen_h1 || h2 != g_tick.seen_h2 || h4 != g_tick.seen_h4 || e6 != g_tick.seen_e6) {
            Log("max players %d; H1 slot lifts %llu (of %llu summaries), H2 clamps %llu, H4 upgrades %llu, E6 "
                "rewrites %llu",
                m, ull(h1), ull(g_h1_calls.load()), ull(h2), ull(h4), ull(e6));
            g_tick.seen_h1 = h1;
            g_tick.seen_h2 = h2;
            g_tick.seen_h4 = h4;
            g_tick.seen_e6 = e6;
        }
    }
}

#endif // BB_PARTY_FOURP_NO_GAME

} // namespace coop::fourp
