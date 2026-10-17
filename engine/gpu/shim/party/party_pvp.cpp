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

std::atomic<bool> g_rule{false};
std::atomic<bool> g_active{false};

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

void Restore(State& st, const char* why) {
    int restored = 0;
    for (const Cell& c : st.cells) {
        u8 now = 0;
        if (SafeGet(c.addr, &now) && now == 1 && now != c.orig && WriteByte(c.addr, c.orig)) ++restored;
    }
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
        Log("test: step %zu: %s; rule %s, active %s, %zu cell(s) open", st.next, st.teams_line.c_str(),
            g_rule.load() ? "on" : "off", g_active.load() ? "yes" : "no", st.cells.size());
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
