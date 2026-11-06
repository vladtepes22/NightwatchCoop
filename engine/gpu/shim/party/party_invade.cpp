// SPDX-License-Identifier: GPL-3.0-or-later
// Invasions into open public games (0.7.8, CL-4cc63c), the game side (party_invade.h). Main thread: Tick; the gate runs on the
// link's IO thread and reads a snapshot the tick keeps.
#include "party_invade.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_b2.h"
#include "party_balance.h"
#include "party_director.h"
#include "party_display_name.h"
#include "party_hud.h"
#include "party_ids.h"
#include "lamp_travel_rules.h"
#include "party_levelsync_rules.h"
#include "party_link.h"
#include "party_runtime.h"
#include "party_status.h"
#include "party_stayhome.h"
#include "party_story.h"
#include "seamless_rules.h"
#include "../bbport_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace coop::invade {

namespace {

using Clock = std::chrono::steady_clock;

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party: invade: ") + line + "\n");
}

std::string IpText(std::uint32_t ip_nbo) {
    char b[24];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip_nbo & 0xff, (ip_nbo >> 8) & 0xff, (ip_nbo >> 16) & 0xff, ip_nbo >> 24);
    return b;
}

// ---- host: what the gate reads, kept by the tick ----
struct Shared {
    std::mutex mu;
    HostView view;
    double view_at = -1e9;
    RateBook rate;   // invasions started per address (kRateMax per window)
    RateBook spam;   // knocks per address (kSpamMax per window)
    BlockBook blocks;
    std::vector<std::pair<std::string, double>> admitted;  // names the gate passed, not yet seen in the roster
    Where where;
    InvaderBook book;          // every game: the invaders the host announced (name, state, lamp), at most Max invaders (0.7.9: up to 2)
    int host_full_hp = -1;     // invader side: the host's phantom health for this invasion (Msg::full_hp), -1 not said
    std::string own_name;      // invader side: this game's own member name (from the roster)
    std::string host_name;     // guest side: the host's name (display)
    struct EndNote {
        std::string name;
        End why = End::None;
    };
    std::vector<EndNote> ends;  // guest side: invasions that just ended (name, why), shown by the tick
};
std::atomic<bool> g_block_request{false};
std::mutex g_test_mu;
std::string g_test_view;  // "", boss, cutscene, loading, full
Shared& G() {
    static Shared g;
    return g;
}

constexpr double kViewFreshS = 2.5;  // a view older than this (the tick stalled, a loading screen) counts as "not in a world"

// ---- invader side ----
bool g_fell_sent = false, g_logged_join = false;
std::string g_last_notice;
double g_state_sent_at = 0;

// ---- host side ----
struct Invasion {
    int slot = -1;
    std::string name;
    double since = 0;
    int announced = 0;  // 0 nothing, 1 pending sent, 2 in sent
    double sent_at = 0;
    bool fell = false;  // the invader said it fell
    double away_since = -1;  // the invader's state says it is not in the world (Home / Title) since then
};
std::map<int, Invasion> g_invs;  // host: by invader slot
std::mutex g_fell_mu;
std::vector<int> g_fell_slots;  // from the callback thread

std::string Shown(const std::string& real) {
    return bbname::Display(real);
}

/// The invader `name` is over for everybody's book (the host's own copy; the guests' follow the broadcast).
void ForgetInvader(const std::string& name) {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    g.book.Apply(Msg{0, name, End::None});
    if (g.book.Empty()) g.host_full_hp = -1;
}

void Notice(const std::string& text) {
    if (text == g_last_notice && false) return;
    g_last_notice = text;
    party::status::SetLastEvent(text);
    Log("notice: %s", text.c_str());
}


// ---- rewards (party_invade_rules.h RewardFor): the game's own bonus routine, once per outcome, on the receiver's own game ----
struct PendingReward {
    bool set = false;
    Role role = Role::Guest;
    End end = End::None;
    std::string name;
    double since = 0;
};
std::vector<PendingReward> g_pending;  // 0.7.9: two invaders can end together: one entry each
RewardOnce g_reward_once;

bool RewardsOn() {  // BB_PARTY_INVADE_REWARDS=0 turns the payouts off
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_INVADE_REWARDS");
        return !(v && v[0] == '0');
    }();
    return on;
}

/// Main thread. Remembers the outcome; PayTick pays it when this game's world is up (the invader's: its own world, after the
/// send-home).
void QueueReward(Role role, End end, const std::string& name) {
    if (!RewardFor(role, end).pay) return;
    if (g_pending.size() >= 8) g_pending.erase(g_pending.begin());
    g_pending.push_back({true, role, end, name, Now()});
}

/// The game's own big banner: SetTextEffect (the Lua's native, 0x13360B0): the type goes into the HUD's queue.
void ShowBanner(int type, const char* who, const char* end) {
    using Fn = void(BB_COOP_SYSV*)(std::uint64_t, int);
    reinterpret_cast<Fn>(Guest(0x13360b0))(0, type);
    Log("banner: %s, %s: text effect %d", who, end, type);
}

/// Test aid BB_PARTY_BANNER_TEST=<type>[,<type>...]: 25 s after the world is up, every 10 s, the next banner (the live check
/// of which text a type shows).
void BannerTestTick(const GameSnapshot& s) {
    static const char* env = std::getenv("BB_PARTY_BANNER_TEST");
    static std::vector<int> types;
    static double t0 = -1;
    static std::size_t next = 0;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        for (const char* c = env; env && *c;) {
            types.push_back(std::atoi(c));
            while (*c && *c != ',') ++c;
            if (*c == ',') ++c;
        }
    }
    if (types.empty() || !s.world_up || s.loading || !s.player) return;
    const double now = Now();
    if (t0 < 0) t0 = now + 25.0;
    if (now < t0 || next >= types.size()) return;
    ShowBanner(types[next++], "test", "test");
    t0 = now + 10.0;
}

void PayOne(const GameSnapshot& s, const PendingReward& p, double now);

void PayTick(const GameSnapshot& s) {
    BannerTestTick(s);
    if (g_pending.empty()) return;
    const double now = Now();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now - it->since > 90.0) {
            Log("reward: dropped, this game's world did not come up in 90 s");
            it = g_pending.erase(it);
        } else {
            ++it;
        }
    }
    if (g_pending.empty() || !s.world_up || s.loading || !s.player || !s.player_rec) return;
    std::vector<PendingReward> todo;
    todo.swap(g_pending);
    for (const PendingReward& p : todo) PayOne(s, p, now);
}

void PayOne(const GameSnapshot& s, const PendingReward& p, double now) {
    const Reward r = RewardFor(p.role, p.end);
    const char* who = p.role == Role::Invader ? "invader" : p.role == Role::Host ? "host" : "guest";
    if (!r.pay) return;
    if (!g_reward_once.Take(p.name, p.end, now)) {
        Log("reward: %s, %s: already paid for this outcome", who, EndWord(p.end));
        return;
    }
    if (r.banner) ShowBanner(r.banner, who, EndWord(p.end));
    if (!RewardsOn()) {
        Log("reward: %s, %s: off (BB_PARTY_INVADE_REWARDS=0)", who, EndWord(p.end));
        return;
    }
    int id = 0, echoes0 = -1, insight0 = -1;
    SafeGet(s.player_rec + 0x10, &id);
    SafeGet(s.player_rec + 0x94, &echoes0);
    SafeGet(s.player_rec + 0x84, &insight0);
    // CalcExcuteMultiBonus(rate, player id, Insight) = 0x131d470: Blood Echoes at the rate of the receiver's level, Insight +N
    // (the Lua's call in OnDeadEvent_HostDead / OnDeadEvent_NormalBlackDead; the Insight is capped at 99 by the game).
    using Fn = void(BB_COOP_SYSV*)(float, std::uint64_t, int, int);
    reinterpret_cast<Fn>(Guest(0x131d470))(r.rate, 0, id, r.insight);
    int echoes1 = -1, insight1 = -1;
    SafeGet(s.player_rec + 0x94, &echoes1);
    SafeGet(s.player_rec + 0x84, &insight1);
    Log("reward: %s, %s: rate %.2f, Insight +%d: Blood Echoes %d -> %d, Insight %d -> %d", who, EndWord(p.end), double(r.rate), r.insight,
        echoes0, echoes1, insight0, insight1);
}

// ---- the smoke's script (BB_PARTY_INVADE_TEST, party_invade_rules.h ParseTest) ----
struct ScriptState {
    bool parsed = false;
    std::vector<TestStep> steps;
    std::size_t next = 0;
    double ready_at = -1;          // when the previous step ran (the first: when the world came up)
    bool was_in = false, was_home = false;
    int in_edges = 0, home_edges = 0;
    double in_since = -1, home_since = -1;
};
ScriptState g_script;

}  // namespace

void TestView(const std::string& word);

namespace {
void ScriptTick(const GameSnapshot& s, bool in_state, bool home_state, double now) {
    ScriptState& sc = g_script;
    if (!sc.parsed) {
        sc.parsed = true;
        std::string bad;
        if (!ParseTest(std::getenv("BB_PARTY_INVADE_TEST"), &sc.steps, &bad)) {
            Log("test: BB_PARTY_INVADE_TEST item '%s' not understood: no script", bad.c_str());
            sc.steps.clear();
        } else if (!sc.steps.empty()) {
            Log("test: script of %zu step(s)", sc.steps.size());
        }
    }
    if (sc.steps.empty()) return;
    if (in_state && !sc.was_in) ++sc.in_edges, sc.in_since = now;
    if (home_state && !sc.was_home) ++sc.home_edges, sc.home_since = now;
    sc.was_in = in_state, sc.was_home = home_state;
    if (sc.next >= sc.steps.size()) return;
    const bool world = s.world_up && !s.loading && s.player;
    if (!world) return;
    if (sc.ready_at < 0) sc.ready_at = now;
    const TestStep& st = sc.steps[sc.next];
    double from = sc.ready_at;
    if (st.cond == 1) {
        if (!in_state || sc.in_edges < std::max(1, st.nth)) return;
        from = std::max(from, sc.in_since);
    } else if (st.cond == 2) {
        if (!home_state || sc.home_edges < std::max(1, st.nth)) return;
        from = std::max(from, sc.home_since);
    }
    if (now - from < st.delay) return;
    ++sc.next;
    sc.ready_at = now;
    switch (st.kind) {
    case TestStep::Kind::Allow:
        BbSettings::Get().party_invaders.store(st.arg != 0);
        Log("test: script step %zu: Allow invaders -> %d", sc.next, st.arg);
        break;
    case TestStep::Kind::View:
        TestView(st.word);
        break;
    case TestStep::Kind::Rejoin:
        Log("test: script step %zu: rejoin -> %d", sc.next, party::runtime_rejoin());
        break;
    case TestStep::Kind::Die:
        Log("test: script step %zu: this player dies now", sc.next);
        TestDieLocal();
        break;
    }
}
}  // namespace

bool JoinAsInvader() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_INVADE");
        return v && v[0] == '1';
    }();
    return on;
}

bool HostAllows() {
    return BbSettings::Get().party_invaders.load();
}

bool ApartAnyLamp() {  // test aid BB_PARTY_INVADE_APART_ANY=1: every lamp of the block counts as lit (a save that lit none)
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_INVADE_APART_ANY");
        return v && v[0] == '1';
    }();
    return on;
}

bool ApartOn() {  // BB_PARTY_INVADE_APART=0: the invader stays beside the host (the 0.7.9 direct-summon placement)
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_INVADE_APART");
        return !(v && v[0] == '0');
    }();
    return on;
}

std::atomic<int> g_own_level{0};
std::mutex g_host_name_mu;
std::string g_host_name_shown = "the host";  // the host's display name (masked), set by the host's tick, read by the gates
std::string HostShown() {
    std::lock_guard<std::mutex> lk(g_host_name_mu);
    return g_host_name_shown;
}
std::string OpenGate(int level) {
    const int host = g_own_level.load();
    if (OpenLevelOk(host, level)) {
        if (host <= 0) Log("gate: the host's level is not known: no level check for an open-party join");
        return {};
    }
    Log("gate: open-party join level %d refused (host level %d: range %d-%d)", level, host, LevelLower(host), OpenLevelUpper(host));
    return LevelText(HostShown(), host, level, false, LevelLower(host), OpenLevelUpper(host));
}
int OwnLevel() {
    if (const char* e = std::getenv("BB_PARTY_INVADE_LEVEL")) {  // test aid: the level the HELLO says
        const int v = std::atoi(e);
        if (v > 0) return v;
    }
    return g_own_level.load();
}

std::string Gate(const std::string& name, std::uint32_t ip, bool final_check, int invader_level) {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    const double now = Now();
    HostView v = g.view;
    v.allow = HostAllows();
    if (now - g.view_at > kViewFreshS) v.world_up = false;  // no fresh view of the host's world
    // Invaders the gate passed that the tick has not seen yet count too (two arrive within a frame).
    g.admitted.erase(std::remove_if(g.admitted.begin(), g.admitted.end(),
                                    [&](const std::pair<std::string, double>& a) { return now - a.second > 8.0; }),
                     g.admitted.end());
    int extra = 0;
    for (const auto& a : g.admitted)
        if (a.first != name) ++extra;
    v.invaders += extra;
    const std::string addr = IpText(ip);
    const bool blocked = g.blocks.Blocked(name, addr);
    // 0.7.9: a level-range refusal comes before the knock counter and never counts against the rate limit.
    const bool level_ok = LevelOk(v.host_level, invader_level);
    if (v.host_level <= 0) Log("gate: the host's level is not known: no level check");
    const bool knocking = level_ok && !final_check && !g.spam.Allow(addr, now, kSpamMax);
    Deny d = Admit(v, blocked, knocking, false, invader_level);
    // The abuse limit counts invasions started: an invader that was let in kRateMax times in the window waits.
    if (d == Deny::None && g.rate.Limited(addr, now)) d = Deny::RateLimited;
    if (d == Deny::None && final_check) {
        g.rate.Allow(addr, now);
        bool known = false;
        for (auto& a : g.admitted)
            if (a.first == name) a.second = now, known = true;
        if (!known) g.admitted.emplace_back(name, now);
    }
    // The log names the reason, never the address.
    Log("gate: invader %s%s: %s%s", name.empty() ? "?" : "(name hidden)", final_check ? " (final)" : "", DenyWord(d),
        d == Deny::None ? "" : (" - " + DenyText(d)).c_str());
    if (d == Deny::LevelRange) return LevelText(HostShown(), v.host_level, invader_level, true);
    return DenyText(d);
}

void OnEvent(int from_slot, const std::string& name, const std::string& body) {
    if (name == kFellEvent) {  // host side: the invader says it fell
        std::lock_guard<std::mutex> lk(g_fell_mu);
        g_fell_slots.push_back(from_slot);
        return;
    }
    Msg m;
    if (name != kEventName || !Decode(body, &m)) return;
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (m.state == 0) {
        if (m.why != End::None) {
            if (g.ends.size() >= 8) g.ends.erase(g.ends.begin());
            g.ends.push_back({m.name, m.why});
        }
        g.book.Apply(m);
        if (g.book.Empty()) g.host_full_hp = -1;
    } else {
        g.book.Apply(m);
        if (m.full_hp >= 0) g.host_full_hp = m.full_hp;
    }
    // The notice itself is shown by the tick (main thread), from the same fields.
}

void RequestBlock() {
    g_block_request = true;
}

void TestView(const std::string& word) {
    std::lock_guard<std::mutex> lk(g_test_mu);
    g_test_view = word == "none" ? std::string() : word;
    Log("test: the host's view is forced to '%s'", g_test_view.empty() ? "none" : g_test_view.c_str());
}

std::string InvaderName() {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.book.First();
}

std::vector<std::string> InvaderNames() {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.book.Names();
}

bool IsInvader(const std::string& name) {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.book.Has(name);
}

bool LocalIsInvader() {
    return JoinAsInvader();
}

int HostFullHp() {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return !g.book.Empty() ? g.host_full_hp : -1;
}

std::uint32_t ApartLamp() {
    if (!ApartOn()) return 0;
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.book.Apart(g.own_name);
}

Where Announced() {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.where;
}

namespace {

/// Guest side (everyone but the host): the notices for the invader coming, in and gone, from the host's "invade" events.
std::map<std::string, int> g_seen;  // invader name -> the state a notice was shown for

void GuestNotices(party::PartyLink* link) {
    Shared& g = G();
    std::string own, host_name = "the host";
    for (const party::RosterEntry& e : link->roster()) {
        if (e.slot == party::kHostSlot) host_name = Shown(e.name);
        if (e.slot == link->local_slot() && e.slot != party::kHostSlot) own = e.name;
    }
    std::vector<SeenInvader> now;
    std::vector<Shared::EndNote> ends;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.own_name = own;
        now = g.book.All();
        ends.swap(g.ends);
    }
    const bool invader = JoinAsInvader();
    for (const Shared::EndNote& e : ends) {  // 0.7.9: one notice, and one payout, per invader that ended
        const bool mine = invader && e.name == own;
        Notice(mine ? EndTextForInvader(e.why, host_name) : EndTextForHost(e.why, Shown(e.name)));
        if (mine || !invader) QueueReward(mine ? Role::Invader : Role::Guest, e.why, e.name);
    }
    for (const SeenInvader& iv : now) {
        const auto seen = g_seen.find(iv.name);
        if (seen != g_seen.end() && seen->second == iv.state) continue;
        g_seen[iv.name] = iv.state;
        const bool me = invader && iv.name == own;
        if (iv.state == 2) {
            Notice(me ? "You invade " + host_name + "'s world" : "Invader " + Shown(iv.name) + " has entered the world: fight!");
        } else if (iv.state == 1) {
            Notice(me ? "Invading " + host_name + ": waiting to be let in" : "An invader is coming: " + Shown(iv.name));
        }
    }
    for (auto it = g_seen.begin(); it != g_seen.end();) {
        bool still = false;
        for (const SeenInvader& iv : now) still = still || iv.name == it->first;
        it = still ? std::next(it) : g_seen.erase(it);
    }
}

}  // namespace

namespace {
void TickImpl(const GameSnapshot& s, party::PartyLink* link, bool host);

/// Host: the lamp the invader appears at (party_invade_rules.h PickApart): a lamp of this map block that the host has lit,
/// 30..250 m from the host. 0: none (the invader stays beside the host), the placement off, or the host's spot unknown.
std::uint32_t ApartLampForInvader(const GameSnapshot& s) {
    if (!ApartOn() || !s.player || !s.world_up) return 0;
    float host[3];
    if (!balance::ChrPosition(s.player, host)) return 0;
    std::vector<ApartSpot> spots;
    for (const lamptravel::Lamp& l : lamptravel::Lamps()) {
        const std::uint32_t rp = lamptravel::ReturnPoint(l);
        float sp[4];
        bool lit = false;
        if ((lamptravel::MapOf(rp) >> 16) != (s.map_id >> 16) || !ids::ReturnSpot(rp, sp)) continue;
        if (!ApartAnyLamp() && (!ReadEventFlag(lamptravel::LitFlag(l), &lit) || !lit)) continue;
        spots.push_back({rp, sp[0], sp[1], sp[2]});
    }
    float dist = 0;
    const std::uint32_t id = PickApart(host, spots, kApartMinM, kApartMaxM, &dist);
    static std::uint32_t logged = 0;
    if (id != logged) {
        logged = id;
        Log("apart: %u lit lamp(s) in this block: the invader appears at lamp %u (%.0f m from the host)", unsigned(spots.size()), id, double(dist));
    }
    return id;
}

/// Host: one invader's invasion this frame (its admission, the block, the end checks, the state report).
void HostInvader(const GameSnapshot& s, party::PartyLink* link, const HostView& v, const party::RosterEntry& ent,
                 const std::vector<int>& fell, bool block, double now) {
    Shared& g = G();
    const int slot = ent.slot;
    auto found = g_invs.find(slot);
    if (found == g_invs.end()) {
        Invasion nv;
        nv.slot = slot;
        nv.name = ent.name;
        nv.since = now;
        found = g_invs.emplace(slot, nv).first;
        Log("an invader is admitted into slot %d (pending)", slot);
    }
    Invasion& inv = found->second;
    for (int f : fell)
        if (f == slot) inv.fell = true;
    if (block) {  // the host's "Block the invader": every invader in is blocked for the session by name and address
        std::uint32_t ip = link->member_ip(slot);
        {
            std::lock_guard<std::mutex> lk(g.mu);
            g.blocks.Block(ent.name, IpText(ip));
        }
        Msg m{0, ent.name, End::Blocked};
        link->post_event(party::kBroadcast, kEventName, Encode(m));
        Notice(EndTextForHost(End::Blocked, Shown(ent.name)));
        Log("the host blocked the invader in slot %d for this session", slot);
        link->kick(slot, EndTextForInvader(End::Blocked, Shown(link->roster().empty() ? "the host" : link->roster()[0].name)));
        g_invs.erase(found);
        ForgetInvader(ent.name);
        return;
    }
    // Once in, a loading screen of the invader's (its game loads while the host's world stays) does not make it "coming" again;
    // a state that says it is back home (Home / Title / Prologue) for 3 s ends the invasion (it left the session).
    const bool was_in = inv.announced == 2;
    const bool arrived = ent.state == party::MemberState::InHostWorld || (was_in && ent.state == party::MemberState::Loading);
    if (was_in && (ent.state == party::MemberState::Home || ent.state == party::MemberState::Title ||
                   ent.state == party::MemberState::Prologue)) {
        if (inv.away_since < 0) inv.away_since = now;
    } else {
        inv.away_since = -1;
    }
    const bool went_home = inv.away_since >= 0 && now - inv.away_since > 3.0;
    // The invader's own HP, from its vitals (the host's copy of it): 0 means it fell.
    bool invader_alive = !inv.fell;
    bool level_bad = false;
    int level_seen = 0;
    {
        party::MemberVitals mv[party::kMaxVitals];
        const int n = link->vitals(mv, party::kMaxVitals);
        for (int i = 0; i < n; ++i) {
            if (mv[i].slot != slot || !(mv[i].flags & party::kVitalsValid)) continue;
            if (mv[i].hp == 0 && arrived) invader_alive = false;
            // 0.7.9: the invader's level from its vitals (self-reported too, but the HELLO value could be a lie): the range
            // is checked again as soon as it is known and for as long as the invasion lasts (each invader on its own).
            if (mv[i].level > 0 && !LevelOk(v.host_level, mv[i].level)) {
                level_bad = true;
                level_seen = mv[i].level;
                Log("invader slot %d: its vitals say level %d, outside the host's range %d-%d", slot, int(mv[i].level),
                    LevelLower(v.host_level), LevelUpper(v.host_level));
            }
        }
    }
    Live l;
    l.allow = v.allow;
    l.blocked = false;
    l.in_world = arrived;
    l.pending_s = now - inv.since;
    l.host_alive = v.alive;
    l.invader_alive = invader_alive;
    l.host_loading = s.loading;
    l.boss_fight = v.boss_fight;
    l.link_up = ent.connected;
    End end = CheckEnd(l);
    if (end == End::None && level_bad) end = End::LevelRange;
    if (end == End::None && went_home) end = End::HostGone;  // it left the host's world by itself
    // Before the invader stands in the world, a moment that turned bad refuses it (boss fog, a cutscene, a travel ...).
    std::string refusal;
    if (end == End::None && !arrived) {
        HostView pv = v;
        pv.invaders = 0;
        pv.world_players = std::max(1, v.world_players);
        const Deny d = Admit(pv, false, false);
        if (d != Deny::None && d != Deny::NoSlot) refusal = RefusedText(DenyText(d));
    }
    if (!refusal.empty() || end != End::None) {
        const std::string why = !refusal.empty() ? refusal
                            : end == End::LevelRange ? LevelText(HostShown(), v.host_level, level_seen, true)
                                                     : EndTextForInvader(end, Shown(link->roster().empty() ? "the host" : link->roster()[0].name));
        Msg m{0, ent.name, end};
        link->post_event(party::kBroadcast, kEventName, Encode(m));
        Notice(!refusal.empty() ? "Invader " + Shown(ent.name) + " turned away: " + refusal : EndTextForHost(end, Shown(ent.name)));
        if (refusal.empty()) QueueReward(Role::Host, end, ent.name);
        Log("invasion of slot %d ends: %s", slot, !refusal.empty() ? refusal.c_str() : EndWord(end));
        link->drop_invader(slot, why);
        g_invs.erase(found);
        ForgetInvader(ent.name);
        return;
    }
    const int state = arrived ? 2 : 1;
    if (inv.announced != state || now - inv.sent_at >= kReportEveryS) {
        Msg m{state, ent.name, End::None};
        m.apart = ApartLampForInvader(s);
        // 0.7.9: the host's rules decide the phantom cut in an invasion (it is PvP); the invader follows this.
        m.full_hp = levelsync::PhantomCutOn(party::status::HostRuleOn(party::status::HostRule::PhantomHp), true,
                                            party::status::HostRuleOn(party::status::HostRule::PvpFullHp))
                        ? 0
                        : 1;
        link->post_event(party::kBroadcast, kEventName, Encode(m));
        if (inv.announced != state) {
            Notice(state == 2 ? "Invader " + Shown(ent.name) + " has entered your world: fight!"
                              : "Invader " + Shown(ent.name) + " is coming into your world");
            Log("invader slot %d: %s", slot, state == 2 ? "in the world" : "pending");
        }
        inv.announced = state;
        inv.sent_at = now;
        std::lock_guard<std::mutex> lk(g.mu);
        g.book.Apply(m);
        g.host_full_hp = m.full_hp;
    }
}
}

void Tick(const GameSnapshot& s, party::PartyLink* link, bool host) {
    if (!link) {
        PayTick(s);
        return;
    }
    TickImpl(s, link, host);
    PayTick(s);
    // The smoke's script, after the tick has updated the state it reads.
    bool in_state, home_state;
    if (host) {
        in_state = false;
        for (const auto& kv : g_invs) in_state = in_state || kv.second.announced == 2;
        home_state = g_invs.empty();
    } else {
        std::lock_guard<std::mutex> lk(G().mu);
        const bool in_host = JoinAsInvader() && G().book.State(G().own_name) == 2 && link->state() == party::LinkState::Connected;
        in_state = in_host;
        home_state = JoinAsInvader() && !in_host && link->state() != party::LinkState::Connected;
    }
    ScriptTick(s, in_state, home_state, Now());
}

namespace {
void TickImpl(const GameSnapshot& s, party::PartyLink* link, bool host) {
    Shared& g = G();
    const double now = Now();
    if (!link) return;
    const bool in_world = s.world_up && !s.loading && s.player;
    if (s.level > 0) g_own_level.store(s.level);
    if (host) {
        // ---- the view the gate reads, and the numbers the lists announce ----
        HostView v;
        v.allow = HostAllows();
        v.host_stays = stayhome::Active();
        v.world_up = s.world_up && s.player && !LocalPrologueHold();
        v.prologue = false;
        v.loading = s.loading;
        v.cutscene = CutscenePlaying();
        v.boss_fight = BossBarShown();
        v.alive = b2::LocalHp() > 0;
        v.travelling = b2::HostRejoinRunning() || b2::RejoinRunning();
        v.party_max = link->max_players();
        v.max_invaders = ClampMaxInvaders(BbSettings::Get().party_max_invaders.load());
        v.host_level = s.level;
        for (const party::RosterEntry& e : link->roster())
            if (e.slot == party::kHostSlot) {
                std::lock_guard<std::mutex> lk(g_host_name_mu);
                g_host_name_shown = Shown(e.name);
            }
        int world_players = 1, invaders = 0;
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot == party::kHostSlot || !e.connected) continue;
            if (link->is_invader(e.slot)) ++invaders;
            else if (e.state == party::MemberState::Joining || e.state == party::MemberState::InHostWorld) ++world_players;
        }
        v.world_players = world_players;
        v.invaders = invaders;
        {
            std::lock_guard<std::mutex> lk(g_test_mu);
            if (g_test_view == "boss") v.boss_fight = true;
            else if (g_test_view == "cutscene") v.cutscene = true;
            else if (g_test_view == "loading") v.loading = true;
            else if (g_test_view == "full") v.world_players = std::max(v.world_players, kGameSlots - invaders);
        }
        Where w;
        w.allow = v.allow;
        const int tod = in_world ? WorldTodPhase() : -1;
        w.phase = static_cast<std::uint8_t>(tod >= 0 && tod <= 3 ? tod + 1 : 0);
        const int ng = NgCycleLive();
        w.ng = static_cast<std::uint8_t>(ng >= 0 && ng <= 7 ? ng : 255);
        w.area = s.world_up ? s.map_id : 0;
        w.boss = v.boss_fight;
        w.invadable = v.allow && Admit(v, false, false) == Deny::None;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            g.view = v;
            g.view_at = now;
            g.where = w;
        }
        // 0.7.9: a member that came in through the open-party level gate: its level from its vitals is checked as soon as it is
        // known (the HELLO value is self-reported). A resumed member is never gated (a guest that levelled up inside the
        // party stays), only the first admission and this recheck of it.
        if (s.level > 0) {
            party::MemberVitals mv[party::kMaxVitals];
            const int n = link->vitals(mv, party::kMaxVitals);
            for (int i = 0; i < n; ++i) {
                if (!(mv[i].flags & party::kVitalsValid) || mv[i].level == 0 || !link->joined_open(mv[i].slot)) continue;
                if (OpenLevelOk(s.level, mv[i].level)) continue;
                Log("open-party member in slot %d: its vitals say level %d, outside the host's range %d-%d: removed", mv[i].slot,
                    int(mv[i].level), LevelLower(s.level), OpenLevelUpper(s.level));
                link->drop_member(mv[i].slot, party::RejectCode::Level,
                                  LevelText(HostShown(), s.level, mv[i].level, false, LevelLower(s.level), OpenLevelUpper(s.level)));
            }
        }
        // ---- the invasions in progress (0.7.9: up to Max invaders at once, each with its own state) ----
        std::vector<int> fell;
        {
            std::lock_guard<std::mutex> lk(g_fell_mu);
            fell.swap(g_fell_slots);
        }
        std::vector<party::RosterEntry> ents;
        for (const party::RosterEntry& e : link->roster())
            if (e.slot != party::kHostSlot && link->is_invader(e.slot)) ents.push_back(e);
        for (auto it = g_invs.begin(); it != g_invs.end();) {  // an invader that is gone from the roster (its link dropped)
            bool there = false;
            for (const party::RosterEntry& e : ents) there = there || (e.slot == it->first && e.name == it->second.name);
            if (there) {
                ++it;
                continue;
            }
            Log("invader slot %d is gone (the link dropped)", it->first);
            Msg m{0, it->second.name, End::HostGone};
            link->post_event(party::kBroadcast, kEventName, Encode(m));
            Notice(EndTextForHost(End::HostGone, Shown(it->second.name)));
            ForgetInvader(it->second.name);
            it = g_invs.erase(it);
        }
        const bool block = g_block_request.exchange(false);
        for (const party::RosterEntry& ent : ents) HostInvader(s, link, v, ent, fell, block, now);
        return;
    }
    // ---- everyone else (a guest, the invader) ----
    GuestNotices(link);
    if (!JoinAsInvader()) return;
    if (link->state() == party::LinkState::Connected && !g_logged_join) {
        g_logged_join = true;
        Log("this game invades the host's world (hostile; no password, no shared progress)");
    }
    // 0.7.9: how far this invader stands from the host while the invasion is on (the placement's evidence), every 5 s.
    {
        static double dist_at = 0;
        int st = 0;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            st = g.book.State(g.own_name);
        }
        if (st == 2 && in_world && s.player && now - dist_at >= 5.0) {
            dist_at = now;
            float me[3];
            party::MemberVitals mv[party::kMaxVitals];
            const int n = link->vitals(mv, party::kMaxVitals);
            for (int i = 0; i < n; ++i) {
                if (mv[i].slot != party::kHostSlot || !(mv[i].flags & party::kVitalsHasPos) || mv[i].age_ms > 3000) continue;
                if (!balance::ChrPosition(s.player, me)) break;
                const float dx = mv[i].pos[0] - me[0], dz = mv[i].pos[2] - me[2];
                Log("apart: %.0f m from the host (host at %.1f, %.1f, %.1f; this game at %.1f, %.1f, %.1f)", double(std::sqrt(dx * dx + dz * dz)),
                    double(mv[i].pos[0]), double(mv[i].pos[1]), double(mv[i].pos[2]), double(me[0]), double(me[1]), double(me[2]));
                break;
            }
        }
    }
    // The invader falls: tell the host and go home at once (the vanilla send-home), not a revive at a lamp.
    if (in_world && link->state() == party::LinkState::Connected && b2::LocalHp() <= 0 && !g_fell_sent) {
        int state = 0;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            state = g.book.State(g.own_name);
        }
        if (state == 2) {
            g_fell_sent = true;
            link->post_event(party::kHostSlot, kFellEvent, "{}");
            Log("the invader fell: going home");
            Notice("You were defeated: back home");
            RequestGuestSendHome("the invader died");
        }
    }
    if (g_fell_sent && b2::LocalHp() > 0) g_fell_sent = false;  // home again, alive
}

}  // namespace

}  // namespace coop::invade
