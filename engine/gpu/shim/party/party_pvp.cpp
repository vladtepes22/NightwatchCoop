// SPDX-License-Identifier: GPL-3.0-or-later
// I1, friendly fire / PvP, the game side (party_pvp.h; the tables and teams: party_pvp_rules.h,
// docs/party/pvp.md). Main thread: the director's tick, once a frame in party mode.
#include "party_pvp.h"
#include "party_pvp_rules.h"
#include "party_balance.h"
#include "party_status.h"
#include "party_async_io.h"
#include "coop_hooks.h"
#include "../bbport_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" void runtime_pad_director_press(std::uint32_t buttons);  // src/runtime_pad.c

namespace coop::pvp {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

constexpr u64 kSlotWorldChrMan = 0x553e878;   // +0x30 n players, +0x38 entries (0x38 each), +0x60 local
constexpr u64 kRelationTablePtr = 0x53e4560;  // -> 0x5127b90 (data): the relation table
constexpr u64 kRelationTable = 0x5127b90;     // the pointer's 1.09 target (checked)
constexpr u64 kEffectTable = 0x4731560;       // rodata: the SpEffect target-enemy table
constexpr u64 kChrTeam = 0x88;                // ChrIns team type (u32)
constexpr u64 kChrType = 0x78;                // ChrIns chr type (0 body, 1 white, 2 black, 8 grey)
constexpr u32 kPadR1 = 0x800;
constexpr u32 kPadL2 = 0x100;      // the firearm (left hand)
constexpr u32 kPadSquare = 0x8000; // the selected quick item

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    asyncio::Print(std::string("Party: pvp: ") + line + "\n");
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

bool Disabled() {
    static const bool off = [] {
        const char* v = std::getenv("BB_PARTY_PVP");
        return v && v[0] == '0';
    }();
    return off;
}

/// One byte of a game table, written through the page protection (the effect table is rodata).
bool WriteByte(u64 addr, u8 v) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
    if (writable) return SafeWrite(addr, &v, 1);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 1, (mbi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE,
                        &old))
        return false;
    const bool ok = SafeWrite(addr, &v, 1);
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, old, &unused);
    return ok;
#else
    return SafeWrite(addr, &v, 1);
#endif
}

struct Cell {
    u64 addr = 0;
    u8 orig = 0;
    int a = 0, b = 0;
    const char* table = "";
};

struct State {
    int last_rule = -1;  // -1 unknown, else 0/1 (the toast on a change)
    std::vector<std::pair<int, int>> pairs;
    std::vector<Cell> cells;
    std::string teams_line;
    // Evidence while active: the local HP, Echoes, Insight.
    u64 player = 0;
    int hp = -1, echoes = -1, insight = -1;
    // BB_PARTY_PVP_TEST
    bool test_parsed = false;
    std::vector<TestStep> steps;
    std::size_t next = 0;
    bool session_steady = false;
    Clock::time_point step_at{};
    int taps_left = 0;
    u32 tap_button = 0;
    bool pressed = false;
    Clock::time_point tap_at{};
};
State& S() {
    static State s;
    return s;
}

// ---- guests hit NPCs as the host does (0.7.5, "NPC attacks beyond #13") ----
//
// The SpEffect target table (an attack's effects, damage included) has 1 for the host's body (row
// 19) against the friendly NPC teams 8, 12, 26, 28 and the hostile 9, 27; a cooperator (row 20) has
// 0 there (read from the 1.09 ELF), so a guest's blow passes through a friendly NPC while the host's
// hurts it (and turns it hostile). The relation table (lock-on) has the same gap at 9 and 27. A
// party session opens row 20 wherever row 19 is 1 (columns 19 / 20, the party's own teams, are left
// to friendly fire), so a guest is a full player against the host world's NPCs. BB_PARTY_NPC_HIT=0
// turns it off (test aid).
u64 RelationTable();
struct NpcCells {
    std::vector<Cell> cells;
};
NpcCells& N() {
    static NpcCells n;
    return n;
}

bool NpcHitDisabled() {
    static const bool off = [] {
        const char* v = std::getenv("BB_PARTY_NPC_HIT");
        return v && v[0] == '0';
    }();
    return off;
}

void NpcRestore(const char* why) {
    NpcCells& n = N();
    int restored = 0;
    for (const Cell& c : n.cells) {
        u8 now = 0;
        if (SafeGet(c.addr, &now) && now == 1 && now != c.orig && WriteByte(c.addr, c.orig)) ++restored;
    }
    if (!n.cells.empty()) Log("npc hit: closed %d table cell(s) (%s)", restored, why);
    n.cells.clear();
}

void NpcApply() {
    NpcCells& n = N();
    if (!n.cells.empty()) return;  // opened for this session
    const u64 rel = RelationTable(), eff = Guest(kEffectTable);
    if (!rel || !eff) return;
    std::string opened;
    for (int t = 0; t < 2; ++t) {
        const u64 base = t == 0 ? rel : eff;
        for (int b = 0; b < kTeams; ++b) {
            const bool party_team = b == kHostTeam || b == kGuestTeam;
            u8 host_row = 0, guest_row = 0;
            if (party_team || !SafeGet(base + u64(kHostTeam) * kTeams + u64(b), &host_row) ||
                !SafeGet(base + u64(kGuestTeam) * kTeams + u64(b), &guest_row) || host_row != 1 || guest_row == 1)
                continue;
            Cell c;
            c.addr = base + u64(kGuestTeam) * kTeams + u64(b);
            c.orig = guest_row, c.a = kGuestTeam, c.b = b, c.table = t == 0 ? "relation" : "effect";
            if (!WriteByte(c.addr, 1)) continue;
            n.cells.push_back(c);
            char b2[40];
            std::snprintf(b2, sizeof b2, " %s[%d][%d]", c.table, kGuestTeam, b);
            opened += b2;
        }
    }
    Log("npc hit: a guest hits what the host hits: opened%s", opened.empty() ? " nothing" : opened.c_str());
}

std::atomic<bool> g_rule{false};
std::atomic<bool> g_active{false};

// ---- NPC hunters stay friends (0.7.7, "bell-summoned NPC hunters turn on the host") ----
//
// A bell-summoned NPC hunter is a cooperator like a guest: team 20. The tables are indexed by team, so
// opening team 19 <-> 20 for the host / guest duel (and 20 -> 20 for two guests) also made every NPC
// hunter an enemy of the host (and of the guests). Friendly fire is for the party's player chrs only:
// while it is on, the NPC chrs that sit on a party team are moved to a spare team (22 or 5: no chr
// uses them, every row / column of them is 0 in the three tables) whose row and column are a copy of
// team 20's, before PvP opened anything. They keep fighting team 23 and stay allies of 19, 20 and
// each other; the players' duel cells never touch them. Back to team 20 when the rule goes off.
constexpr int kSpareTeams[] = {22, 5};
constexpr u64 kFriendTable = 0x4731960;  // rodata: the SpEffect "friend" twin of the target table

struct SpareTeam {
    int team = 0;             // 0: none in use
    std::vector<Cell> cells;  // the bytes written (orig = before)
    struct Moved {
        u64 chr = 0;
        u32 entity = 0;
        int from = 0;
    };
    std::vector<Moved> moved;
    Clock::time_point scan_at{};
    bool failed = false;  // no spare team usable: logged once
};
SpareTeam& SP() {
    static SpareTeam s;
    return s;
}

/// The party's player chrs on this game: the local one first, then the others (WorldChrMan's
/// player set).
std::vector<u64> PlayerChrs(u64 wcm) {
    std::vector<u64> out;
    const u64 local = Field<u64>(wcm, 0x60);
    if (local) out.push_back(local);
    const int n = Field<int>(wcm, 0x30, 0);
    const u64 entries = Field<u64>(wcm, 0x38);
    for (int i = 0; entries && n > 0 && n <= 8 && i < n; ++i) {
        const u64 c = Field<u64>(entries + u64(i) * 0x38, 0);
        if (c && c != local) out.push_back(c);
    }
    return out;
}

u64 RelationTable() {
    const u64 p = Slot(kRelationTablePtr);
    return p == Guest(kRelationTable) ? p : 0;  // 1.09's table only
}

/// The original byte of a cell friendly fire / the NPC-hit rule opened (the tables as the game ships them).
u8 OrigByte(u64 addr, u8 now) {
    for (const Cell& c : S().cells)
        if (c.addr == addr) return c.orig;
    for (const Cell& c : N().cells)
        if (c.addr == addr) return c.orig;
    return now;
}

bool TeamIsDead(u64 rel, u64 eff, u64 fr, int t) {
    for (const u64 base : {rel, eff, fr}) {
        for (int k = 0; k < kTeams; ++k) {
            u8 a = 1, b = 1;
            if (!SafeGet(base + u64(t) * kTeams + u64(k), &a) || !SafeGet(base + u64(k) * kTeams + u64(t), &b)) return false;
            if (a || b) return false;
        }
    }
    return true;
}

void SpareRestoreChrs(const char* why) {
    SpareTeam& sp = SP();
    int back = 0;
    for (const auto& m : sp.moved) {
        // Only a chr that is still the same one (its entity id) and still on the spare team.
        if (Field<u32>(m.chr, 0x1f0, 0) == m.entity && Field<u32>(m.chr, kChrTeam, 0) == u32(sp.team)) {
            const u32 t = u32(m.from);
            if (SafeWrite(m.chr + kChrTeam, &t, 4)) ++back;
        }
    }
    if (!sp.moved.empty()) Log("npc teams: %d of %zu NPC chr(s) back on their team (%s)", back, sp.moved.size(), why);
    sp.moved.clear();
}

void SpareRestore(const char* why) {
    SpareTeam& sp = SP();
    SpareRestoreChrs(why);
    int restored = 0;
    for (const Cell& c : sp.cells) {
        u8 now = 0;
        if (SafeGet(c.addr, &now) && now != c.orig && WriteByte(c.addr, c.orig)) ++restored;
    }
    if (!sp.cells.empty()) Log("npc teams: spare team %d closed: %d table cell(s) put back (%s)", sp.team, restored, why);
    sp.cells.clear();
    sp.team = 0;
}

/// Spare team X as a copy of team 20 in the three tables (target, friend twin, relation), as the game
/// ships them. False: no dead team to use.
bool SpareOpen() {
    SpareTeam& sp = SP();
    if (sp.team) return true;
    const u64 rel = RelationTable(), eff = Guest(kEffectTable), fr = Guest(kFriendTable);
    if (!rel || !eff || !fr) return false;
    int x = 0;
    for (int t : kSpareTeams)
        if (TeamIsDead(rel, eff, fr, t)) {
            x = t;
            break;
        }
    if (!x) {
        if (!sp.failed) Log("npc teams: no spare team is free (22, 5): the NPC hunters stay on team %d", kGuestTeam);
        sp.failed = true;
        return false;
    }
    int written = 0;
    for (const u64 base : {rel, eff, fr}) {
        const auto at = [&](int a, int b) { return base + u64(a) * kTeams + u64(b); };
        u8 row[kTeams]{}, col[kTeams]{};
        bool ok = true;
        for (int k = 0; k < kTeams && ok; ++k) {
            u8 r = 0, c = 0;
            ok = SafeGet(at(kGuestTeam, k), &r) && SafeGet(at(k, kGuestTeam), &c);
            row[k] = OrigByte(at(kGuestTeam, k), r);
            col[k] = OrigByte(at(k, kGuestTeam), c);
        }
        if (!ok) continue;
        for (int k = 0; k < kTeams; ++k) {
            // Row x: what team 20 does to k; column x: what k does to team 20 (x itself counts as 20).
            const int kk = k == x ? kGuestTeam : k;
            for (int dir = 0; dir < 2; ++dir) {
                if (dir == 1 && k == x) continue;  // [x][x] is written in the row pass
                Cell c;
                c.addr = dir == 0 ? at(x, k) : at(k, x);
                const u8 want = dir == 0 ? row[kk] : col[kk];
                if (!SafeGet(c.addr, &c.orig) || c.orig == want) continue;
                c.a = dir == 0 ? x : k, c.b = dir == 0 ? k : x;
                c.table = base == rel ? "relation" : base == eff ? "effect" : "friend";
                if (!WriteByte(c.addr, want)) continue;
                sp.cells.push_back(c);
                ++written;
            }
        }
    }
    sp.team = x;
    Log("npc teams: spare team %d is a copy of team %d (%d table cell(s) written)", x, kGuestTeam, written);
    return true;
}

/// Moves the NPC chrs on a party team (not the party's player chrs) to the spare team. Called every
/// frame while friendly fire is applied; scans 4 times a second (a chr the game put back is moved again).
void SpareScan(u64 wcm, const std::vector<u64>& players) {
    SpareTeam& sp = SP();
    const auto now = Clock::now();
    if (sp.team && now - sp.scan_at < std::chrono::milliseconds(250)) return;
    if (!sp.team && !SpareOpen()) return;
    sp.scan_at = now;
    const int sets = std::min(Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2, 64);
    int fresh = 0, again = 0;
    std::string ids;
    for (int set = 0; set < sets; ++set) {
        const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
        const int count = Field<int>(tab, 0, 0);
        const u64 entries = Field<u64>(tab, 8);
        if (!tab || !entries || count <= 0 || count > 4096) continue;
        for (int i = 0; i < count; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            if (!chr || std::find(players.begin(), players.end(), chr) != players.end()) continue;
            const int team = static_cast<int>(Field<u32>(chr, kChrTeam, 0));
            if (team != kGuestTeam && team != kHostTeam) continue;
            const u32 to = u32(sp.team);
            if (!SafeWrite(chr + kChrTeam, &to, 4)) continue;
            const u32 ent = Field<u32>(chr, 0x1f0, 0);
            bool known = false;
            for (auto& m : sp.moved)
                if (m.chr == chr && m.entity == ent) known = true;
            if (known) {
                ++again;  // the game put it back to its team and it is moved again
                continue;
            }
            sp.moved.push_back({chr, ent, team});
            ++fresh;
            if (fresh <= 6) {
                char b[48];
                std::snprintf(b, sizeof b, " %u(team %d)", ent, team);
                ids += b;
            }
        }
    }
    if (fresh || again) Log("npc teams: %d NPC chr(s) moved to team %d:%s%s", fresh, sp.team, ids.c_str(), again ? " (+ some moved again)" : "");
}

void Restore(State& st, const char* why) {
    int restored = 0;
    for (const Cell& c : st.cells) {
        u8 now = 0;
        if (SafeGet(c.addr, &now) && now == 1 && now != c.orig && WriteByte(c.addr, c.orig)) ++restored;
    }
    SpareRestore(why);
    if (!st.cells.empty()) Log("closed %d table cell(s) (%s): party members are allies again", restored, why);
    st.cells.clear();
    st.pairs.clear();
    g_active = false;
}

void Apply(State& st, const std::vector<std::pair<int, int>>& pairs, const std::string& teams) {
    Restore(st, "team change");
    const u64 rel = RelationTable(), eff = Guest(kEffectTable);
    if (!rel || !eff) {
        Log("the team tables are not 1.09's (relation table pointer %llx): friendly fire NOT applied",
            static_cast<unsigned long long>(Slot(kRelationTablePtr)));
        return;
    }
    std::string opened;
    for (const auto& [a, b] : pairs) {
        for (int t = 0; t < 2; ++t) {
            Cell c;
            c.addr = (t == 0 ? rel : eff) + u64(a) * kTeams + u64(b);
            c.a = a, c.b = b;
            c.table = t == 0 ? "relation" : "effect";
            if (!SafeGet(c.addr, &c.orig)) continue;
            if (c.orig == 1) continue;  // already enemies there
            if (!WriteByte(c.addr, 1)) {
                Log("cell %s[%d][%d] not writable", c.table, a, b);
                continue;
            }
            st.cells.push_back(c);
            char b2[40];
            std::snprintf(b2, sizeof b2, " %s[%d][%d]", c.table, a, b);
            opened += b2;
        }
    }
    st.pairs = pairs;
    g_active = true;
    Log("friendly fire applied (%s): opened%s", teams.c_str(), opened.empty() ? " nothing (already open)" : opened.c_str());
}

/// Non-player chrs sharing a party team (evidence that nothing else changes side).
std::string Collateral(u64 wcm, const std::vector<int>& party_teams, const std::vector<u64>& players) {
    const int sets = std::min(Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2, 64);
    int n = 0;
    std::string ids;
    for (int set = 0; set < sets; ++set) {
        const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
        const int count = Field<int>(tab, 0, 0);
        const u64 entries = Field<u64>(tab, 8);
        if (!tab || !entries || count <= 0 || count > 4096) continue;
        for (int i = 0; i < count; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            if (!chr || std::find(players.begin(), players.end(), chr) != players.end()) continue;
            const int team = static_cast<int>(Field<u32>(chr, kChrTeam, 0));
            if (std::find(party_teams.begin(), party_teams.end(), team) == party_teams.end()) continue;
            if (++n <= 6) {
                char b[48];
                std::snprintf(b, sizeof b, " %u(team %d)", Field<u32>(chr, 0x1f0, 0), team);
                ids += b;
            }
        }
    }
    char line[400];
    std::snprintf(line, sizeof line, "%d other chr(s) on a party team%s", n, ids.c_str());
    return line;
}

/// Evidence (test "log" step): each non-player chr on a party team, with the table cells between the
/// host's team and its own (relation / effect, both directions): 1 = enemies.
std::string NpcView(u64 wcm, const std::vector<u64>& players) {
    const u64 rel = RelationTable(), eff = Guest(kEffectTable);
    const int sets = std::min(Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2, 64);
    std::string out;
    int n = 0;
    for (int set = 0; set < sets && rel && eff; ++set) {
        const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
        const int count = Field<int>(tab, 0, 0);
        const u64 entries = Field<u64>(tab, 8);
        if (!tab || !entries || count <= 0 || count > 4096) continue;
        for (int i = 0; i < count && n < 6; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            if (!chr || std::find(players.begin(), players.end(), chr) != players.end()) continue;
            const int team = static_cast<int>(Field<u32>(chr, kChrTeam, 0));
            if (team != kHostTeam && team != kGuestTeam && team != 22 && team != 5) continue;
            u8 r1 = 9, r2 = 9, e1 = 9, e2 = 9;
            SafeGet(rel + u64(kHostTeam) * kTeams + u64(team), &r1);
            SafeGet(rel + u64(team) * kTeams + u64(kHostTeam), &r2);
            SafeGet(eff + u64(kHostTeam) * kTeams + u64(team), &e1);
            SafeGet(eff + u64(team) * kTeams + u64(kHostTeam), &e2);
            char b[160];
            std::snprintf(b, sizeof b, " [npc %u team %d type %u: host->it rel %d eff %d, it->host rel %d eff %d]",
                          Field<u32>(chr, 0x1f0, 0), team, Field<u32>(chr, kChrType, 0), r1, e1, r2, e2);
            out += b;
            ++n;
        }
    }
    return n ? out : " [no npc chr on a party team]";
}

void Toast(bool on, bool host) {
    std::string t = ToastText(on);
    if (!host) t += on ? " (host's rule)" : "";
    party::status::SetLastEvent(t);
    Log("notice: %s", t.c_str());
}

// ---- BB_PARTY_PVP_TEST ----
/// Test aid: goods `id` (given if none is held) into the first quick-item slot, selected.
bool QuickItemFirst(u32 id, std::string* what) {
    // Enough of it for a status effect to build up (a Poison Knife adds 70 poison a hit).
    const int held = ItemCount(kItemGoods, id);
    if (held >= 0 && held < 10) GiveItem(kItemGoods, id, 10 - held);
    const bool ok = SetQuickItem(0, id, true);
    bool sel = false;
    const u32 now = QuickItemId(0, &sel);
    char b[120];
    std::snprintf(b, sizeof b, "quick slot 0 holds %u%s (held %d)", now, sel ? ", selected" : "", ItemCount(kItemGoods, id));
    *what = b;
    return ok;
}

/// `team`: the first other party player on that team (0: the first other one).
bool FaceOther(u64 wcm, int team, std::string* what) {
    const std::vector<u64> chrs = PlayerChrs(wcm);
    if (chrs.size() < 2) return false;
    const u64 me = chrs[0];
    u64 other = 0;
    for (std::size_t i = 1; i < chrs.size() && !other; ++i) {
        if (team <= 0 || static_cast<int>(Field<u32>(chrs[i], kChrTeam, 0)) == team) other = chrs[i];
    }
    if (!other) return false;
    float t[4] = {}, m[4] = {}, r[4] = {};
    u64 vt = 0, fn = 0;
    // The physics positions (as the party HUD reads them; a remote chr's +0xc0 is not kept up).
    const auto phys = [](u64 c) {
        return Field<u64>(Field<u64>(Field<u64>(Field<u64>(c, 0x58), 8), 0x3b0), 0x68);
    };
    const u64 pt = phys(other), pm = phys(me);
    if (!pt || !pm || !SafeRead(pt + 0x1e0, t, sizeof t) || !SafeRead(pm + 0x1e0, m, sizeof m) ||
        !SafeRead(pt + 0x1d0, r, sizeof r) || !SafeGet(me, &vt) || !SafeGet(vt + 0x5f8, &fn) || !fn)
        return false;
    float dx = m[0] - t[0], dz = m[2] - t[2];
    float h = std::sqrt(dx * dx + dz * dz);
    if (h < 0.3f) dx = -std::sin(r[1]), dz = -std::cos(r[1]), h = 1.0f;  // its front
    dx /= h, dz /= h;
    alignas(16) float pos[4] = {t[0] + dx * 1.1f, t[1], t[2] + dz * 1.1f, 1.0f};
    const float yaw = std::atan2(-(t[0] - pos[0]), -(t[2] - pos[2]));  // forward(yaw) = (-sin, -cos)
    alignas(16) float rot[4] = {0, yaw, 0, 0};
    using WarpFn = void(BB_COOP_SYSV*)(u64, const float*, const float*, u64);
    reinterpret_cast<WarpFn>(fn)(me, pos, rot, 0);
    char b[200];
    std::snprintf(b, sizeof b, "facing chr %x (team %u) at (%.2f, %.2f, %.2f) from (%.2f, %.2f, %.2f) yaw %.2f",
                  Field<u32>(other, 8, 0), Field<u32>(other, kChrTeam, 0), double(t[0]), double(t[1]), double(t[2]),
                  double(pos[0]), double(pos[1]), double(pos[2]), double(yaw));
    *what = b;
    return true;
}

void TestTick(State& st, const GameSnapshot& s, bool session, bool host) {
    if (!st.test_parsed) {
        st.test_parsed = true;
        std::string bad;
        if (!ParseTestSteps(std::getenv("BB_PARTY_PVP_TEST"), &st.steps, &bad)) {
            Log("test: BB_PARTY_PVP_TEST item '%s' not understood: no test", bad.c_str());
            st.steps.clear();
        } else if (!st.steps.empty()) {
            Log("test: %zu step(s)", st.steps.size());
        }
    }
    if (st.steps.empty()) return;
    const auto now = Clock::now();
    // A tap in flight: R1 down 0.15 s, then the next one 1.3 s after the previous.
    if (st.pressed && now - st.tap_at >= std::chrono::milliseconds(150)) {
        runtime_pad_director_press(0);
        st.pressed = false;
    }
    if (st.taps_left > 0 && !st.pressed && now - st.tap_at >= std::chrono::milliseconds(1300)) {
        runtime_pad_director_press(st.tap_button ? st.tap_button : kPadR1);
        st.pressed = true;
        st.tap_at = now;
        --st.taps_left;
        if (st.taps_left == 0) st.step_at = now;
        return;
    }
    const bool steady = session && s.world_up && !s.loading && s.player;
    if (steady != st.session_steady) {
        st.session_steady = steady;
        st.step_at = now;
    }
    if (!steady || st.taps_left > 0 || st.next >= st.steps.size()) return;
    const TestStep& step = st.steps[st.next];
    if (std::chrono::duration<double>(now - st.step_at).count() < step.delay) return;
    ++st.next;
    st.step_at = now;
    const u64 wcm = Slot(kSlotWorldChrMan);
    switch (step.kind) {
    case TestStep::Kind::On:
    case TestStep::Kind::Off:
        BbSettings::Get().party_friendly_fire = step.kind == TestStep::Kind::On;
        Log("test: step %zu: %s: the setting is now %s%s", st.next, step.kind == TestStep::Kind::On ? "on" : "off",
            step.kind == TestStep::Kind::On ? "on" : "off", host ? "" : " (a guest's own setting: does not count)");
        break;
    case TestStep::Kind::Face: {
        std::string what;
        const bool ok = FaceOther(wcm, step.arg, &what);
        Log("test: step %zu: face: %s", st.next, ok ? what.c_str() : "FAILED (no other party player loaded)");
        break;
    }
    case TestStep::Kind::Hit:
    case TestStep::Kind::Shoot:
    case TestStep::Kind::Use: {
        const bool hit = step.kind == TestStep::Kind::Hit, shoot = step.kind == TestStep::Kind::Shoot;
        st.tap_button = hit ? kPadR1 : shoot ? kPadL2 : kPadSquare;
        st.taps_left = step.arg;
        st.tap_at = now - std::chrono::seconds(2);
        Log("test: step %zu: %d x %s", st.next, step.arg, hit ? "R1" : shoot ? "L2 (firearm)" : "Square (quick item)");
        break;
    }
    case TestStep::Kind::Item: {
        std::string what;
        const bool ok = QuickItemFirst(static_cast<u32>(step.arg), &what);
        Log("test: step %zu: item %d in the first quick-item slot: %s", st.next, step.arg, ok ? what.c_str() : what.empty() ? "FAILED" : what.c_str());
        break;
    }
    case TestStep::Kind::Hp: {
        const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
        const int before = Field<int>(dm, 0xf8, -1);
        const int v = step.arg;
        const bool ok = dm && before > 0 && SafeWrite(dm + 0xf8, &v, sizeof v);
        Log("test: step %zu: own HP %d -> %d%s", st.next, before, v, ok ? "" : " FAILED");
        break;
    }
    case TestStep::Kind::Log:
        Log("test: step %zu: %s; rule %s, active %s, %zu cell(s) open;%s", st.next, st.teams_line.c_str(),
            g_rule.load() ? "on" : "off", g_active.load() ? "yes" : "no", st.cells.size(),
            NpcView(wcm, PlayerChrs(wcm)).c_str());
        break;
    }
}

}  // namespace

bool RuleOn() {
    return g_rule.load();
}

bool Active() {
    return g_active.load();
}

void Tick(const GameSnapshot& s, bool host) {
    State& st = S();
    balance::Rules hr;
    const bool known = !host && balance::HostRules(&hr);
    const bool rule = RuleInForce(host, BbSettings::Get().party_friendly_fire.load(), known, known && hr.friendly_fire);
    g_rule = rule;
    party::status::SetFriendlyFire(rule);
    if (st.last_rule < 0) {
        if (rule) Toast(true, host);
    } else if ((st.last_rule != 0) != rule) {
        Toast(rule, host);
    }
    if (st.last_rule != int(rule)) Log("rule: friendly fire %s (%s)", rule ? "ON" : "off", host ? "host's setting" : known ? "the host's rule" : "no host rule yet");
    st.last_rule = rule ? 1 : 0;

    const bool session = PartySession(s.session_role, s.cooperators);
    const bool in_world = s.world_up && !s.loading && s.player;
    TestTick(st, s, session, host);
    if (!NpcHitDisabled() && session && in_world) NpcApply();
    else if (!N().cells.empty()) NpcRestore(NpcHitDisabled() ? "BB_PARTY_NPC_HIT=0" : !session ? "no party session" : "loading");
    if (Disabled()) return;

    if (!rule || !session || !in_world) {
        if (!st.cells.empty() || g_active.load())
            Restore(st, !rule ? "the rule is off" : !session ? "no party session" : "loading");
        st.player = 0;
        return;
    }
    const u64 wcm = Slot(kSlotWorldChrMan);
    const std::vector<u64> chrs = PlayerChrs(wcm);
    std::vector<int> teams;
    std::string line;
    for (std::size_t i = 0; i < chrs.size(); ++i) {
        const int team = static_cast<int>(Field<u32>(chrs[i], kChrTeam, 0));
        teams.push_back(team);
        char b[80];
        std::snprintf(b, sizeof b, "%s%s chr %x type %u team %d", i ? ", " : "", i ? "other" : "local",
                      Field<u32>(chrs[i], 8, 0), Field<u32>(chrs[i], kChrType, 0), team);
        line += b;
    }
    st.teams_line = line;
    const auto pairs = TeamPairs(teams);
    if (pairs != st.pairs && chrs.size() >= 2) {
        std::vector<int> uniq = teams;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        Apply(st, pairs, line + "; " + Collateral(wcm, uniq, chrs));
    }
    if (g_active.load() && !st.cells.empty()) SpareScan(wcm, chrs);
    // Evidence: the local HP, a death, Echoes / Insight (no farming: party kills give nothing).
    const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
    const int hp = Field<int>(dm, 0xf8, -1);
    if (s.player == st.player) {
        if (hp >= 0 && st.hp > 0 && hp != st.hp && hp < st.hp)
            Log("own HP %d -> %d%s", st.hp, hp, hp == 0 ? " (dead)" : "");
        if (s.echoes >= 0 && st.echoes >= 0 && s.echoes != st.echoes) Log("own Echoes %d -> %d", st.echoes, s.echoes);
        if (s.insight >= 0 && st.insight >= 0 && s.insight != st.insight)
            Log("own Insight %d -> %d", st.insight, s.insight);
    }
    st.player = s.player;
    st.hp = hp;
    st.echoes = s.echoes;
    st.insight = s.insight;
}

}  // namespace coop::pvp
