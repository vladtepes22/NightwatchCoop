// SPDX-License-Identifier: GPL-3.0-or-later
// Invasions into open public games (0.7.8, CL-4cc63c), the game side (party_invade.h). Main thread: Tick; the gate runs on the
// link's IO thread and reads a snapshot the tick keeps.
#include "party_invade.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_b2.h"
#include "party_director.h"
#include "party_display_name.h"
#include "party_hud.h"
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
    std::string invader_name;  // real member name; "" none
    int invader_state = 0;     // 0 none, 1 pending, 2 in
    int host_full_hp = -1;     // invader side: the host's phantom health for this invasion (Msg::full_hp), -1 not said
    std::string host_name;     // guest side: the host's name (display)
    std::string end_name;      // guest side: an invasion that just ended (name, why), shown by the tick
    End end_why = End::None;
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
Invasion g_inv;
std::mutex g_fell_mu;
std::vector<int> g_fell_slots;  // from the callback thread

std::string Shown(const std::string& real) {
    return bbname::Display(real);
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
PendingReward g_pending;
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
    g_pending = {true, role, end, name, Now()};
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

void PayTick(const GameSnapshot& s) {
    BannerTestTick(s);
    if (!g_pending.set) return;
    const double now = Now();
    if (now - g_pending.since > 90.0) {
        Log("reward: dropped, this game's world did not come up in 90 s");
        g_pending = {};
        return;
    }
    if (!s.world_up || s.loading || !s.player || !s.player_rec) return;
    const PendingReward p = g_pending;
    g_pending = {};
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
        if (m.why != End::None) g.end_name = m.name, g.end_why = m.why;
        g.invader_name.clear();
        g.invader_state = 0;
        g.host_full_hp = -1;
    } else {
        g.invader_name = m.name;
        g.invader_state = m.state;
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
    return g.invader_state ? g.invader_name : std::string();
}

bool LocalIsInvader() {
    return JoinAsInvader();
}

int HostFullHp() {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.invader_state ? g.host_full_hp : -1;
}

Where Announced() {
    Shared& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.where;
}

namespace {

/// Guest side (everyone but the host): the notices for the invader coming, in and gone, from the host's "invade" events.
struct GuestSeen {
    std::string name;
    int state = 0;
};
GuestSeen g_seen;

void GuestNotices(party::PartyLink* link) {
    Shared& g = G();
    std::string name, end_name;
    int state;
    End end_why;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        name = g.invader_name;
        state = g.invader_state;
        end_name = g.end_name;
        end_why = g.end_why;
        g.end_name.clear();
        g.end_why = End::None;
    }
    if (end_why != End::None) {
        std::string host_name = "the host";
        for (const party::RosterEntry& e : link->roster())
            if (e.slot == party::kHostSlot) host_name = Shown(e.name);
        Notice(JoinAsInvader() ? EndTextForInvader(end_why, host_name) : EndTextForHost(end_why, Shown(end_name)));
        QueueReward(JoinAsInvader() ? Role::Invader : Role::Guest, end_why, end_name);
    }
    if (state == g_seen.state && name == g_seen.name) return;
    const std::string was = g_seen.name;
    const int was_state = g_seen.state;
    g_seen = {name, state};
    std::string host_name = "the host";
    for (const party::RosterEntry& e : link->roster())
        if (e.slot == party::kHostSlot) host_name = Shown(e.name);
    const bool me = JoinAsInvader();
    if (state == 2) {
        Notice(me ? "You invade " + host_name + "'s world" : "Invader " + Shown(name) + " has entered the world: fight!");
    } else if (state == 1) {
        Notice(me ? "Invading " + host_name + ": waiting to be let in" : "An invader is coming: " + Shown(name));
    }
    (void)was;
    (void)was_state;
}

}  // namespace

namespace {
void TickImpl(const GameSnapshot& s, party::PartyLink* link, bool host);
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
        in_state = g_inv.announced == 2;
        home_state = g_inv.slot < 0;
    } else {
        std::lock_guard<std::mutex> lk(G().mu);
        const bool in_host = JoinAsInvader() && G().invader_state == 2 && link->state() == party::LinkState::Connected;
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
        // ---- the invasion in progress ----
        std::vector<int> fell;
        {
            std::lock_guard<std::mutex> lk(g_fell_mu);
            fell.swap(g_fell_slots);
        }
        int slot = -1;
        party::RosterEntry ent;
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot != party::kHostSlot && link->is_invader(e.slot)) {
                slot = e.slot;
                ent = e;
            }
        }
        if (slot < 0) {
            if (g_inv.slot >= 0) {
                Log("invader slot %d is gone (the link dropped)", g_inv.slot);
                Msg m{0, g_inv.name, End::HostGone};
                link->post_event(party::kBroadcast, kEventName, Encode(m));
                Notice(EndTextForHost(End::HostGone, Shown(g_inv.name)));
                g_inv = Invasion{};
                std::lock_guard<std::mutex> lk(g.mu);
                g.invader_name.clear();
                g.invader_state = 0;
            }
            return;
        }
        if (g_inv.slot != slot || g_inv.name != ent.name) {
            g_inv = Invasion{};
            g_inv.slot = slot;
            g_inv.name = ent.name;
            g_inv.since = now;
            Log("an invader is admitted into slot %d (pending)", slot);
        }
        for (int f : fell)
            if (f == slot) g_inv.fell = true;
        if (g_block_request.exchange(false)) {  // the host's "Block the invader": blocked for the session by name and address
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
            g_inv = Invasion{};
            std::lock_guard<std::mutex> lk(g.mu);
            g.invader_name.clear();
            g.invader_state = 0;
            return;
        }
        // Once in, a loading screen of the invader's (its game loads while the host's world stays) does not make it "coming" again;
        // a state that says it is back home (Home / Title / Prologue) for 3 s ends the invasion (it left the session).
        const bool was_in = g_inv.announced == 2;
        const bool arrived = ent.state == party::MemberState::InHostWorld || (was_in && ent.state == party::MemberState::Loading);
        if (was_in && (ent.state == party::MemberState::Home || ent.state == party::MemberState::Title ||
                       ent.state == party::MemberState::Prologue)) {
            if (g_inv.away_since < 0) g_inv.away_since = now;
        } else {
            g_inv.away_since = -1;
        }
        const bool went_home = g_inv.away_since >= 0 && now - g_inv.away_since > 3.0;
        // The invader's own HP, from its vitals (the host's copy of it): 0 means it fell.
        bool invader_alive = !g_inv.fell;
        bool level_bad = false;
        int level_seen = 0;
        {
            party::MemberVitals mv[party::kMaxVitals];
            const int n = link->vitals(mv, party::kMaxVitals);
            for (int i = 0; i < n; ++i) {
                if (mv[i].slot != slot || !(mv[i].flags & party::kVitalsValid)) continue;
                if (mv[i].hp == 0 && arrived) invader_alive = false;
                // 0.7.9: the invader's level from its vitals (self-reported too, but the HELLO value could be a lie): the range
                // is checked again as soon as it is known and for as long as the invasion lasts.
                if (mv[i].level > 0 && !LevelOk(v.host_level, mv[i].level)) {
                    level_bad = true;
                    Log("invader slot %d: its vitals say level %d, outside the host's range %d-%d", slot, int(mv[i].level),
                        LevelLower(v.host_level), LevelUpper(v.host_level));
                }
            }
        }
        Live l;
        l.allow = v.allow;
        l.blocked = false;
        l.in_world = arrived;
        l.pending_s = now - g_inv.since;
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
            g_inv = Invasion{};
            std::lock_guard<std::mutex> lk(g.mu);
            g.invader_name.clear();
            g.invader_state = 0;
            return;
        }
        const int state = arrived ? 2 : 1;
        if (g_inv.announced != state || now - g_inv.sent_at >= kReportEveryS) {
            Msg m{state, ent.name, End::None};
            // 0.7.9: the host's rules decide the phantom cut in an invasion (it is PvP); the invader follows this.
            m.full_hp = levelsync::PhantomCutOn(party::status::HostRuleOn(party::status::HostRule::PhantomHp), true,
                                                party::status::HostRuleOn(party::status::HostRule::PvpFullHp))
                            ? 0
                            : 1;
            link->post_event(party::kBroadcast, kEventName, Encode(m));
            if (g_inv.announced != state) {
                Notice(state == 2 ? "Invader " + Shown(ent.name) + " has entered your world: fight!"
                                  : "Invader " + Shown(ent.name) + " is coming into your world");
                Log("invader slot %d: %s", slot, state == 2 ? "in the world" : "pending");
            }
            g_inv.announced = state;
            g_inv.sent_at = now;
            std::lock_guard<std::mutex> lk(g.mu);
            g.invader_name = ent.name;
            g.invader_state = state;
        }
        return;
    }
    // ---- everyone else (a guest, the invader) ----
    GuestNotices(link);
    if (!JoinAsInvader()) return;
    if (link->state() == party::LinkState::Connected && !g_logged_join) {
        g_logged_join = true;
        Log("this game invades the host's world (hostile; no password, no shared progress)");
    }
    // The invader falls: tell the host and go home at once (the vanilla send-home), not a revive at a lamp.
    if (in_world && link->state() == party::LinkState::Connected && b2::LocalHp() <= 0 && !g_fell_sent) {
        int state = 0;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            state = g.invader_state;
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
