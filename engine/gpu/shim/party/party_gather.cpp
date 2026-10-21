// SPDX-License-Identifier: GPL-3.0-or-later
// I5 "Gather the party", the game side (party_gather.h). Main thread: Tick; OnEvent on the link's
// callback thread only queues.
#include "party_gather.h"
#include "party_gather_rules.h"

#include "game_state.h"
#include "party_async_io.h"
#include "party_b2.h"
#include "party_hud.h"
#include "party_link.h"
#include "party_split.h"
#include "party_status.h"
#include "seamless_rules.h"
#include "../bbport_game_menu_pages.h"
#include "../bbport_settings.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop::gather {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kPadCross = 0x4000, kPadRight = 0x20, kPadCircle = 0x2000, kPadOptions = 0x8;
constexpr double kLateAnswerS = 60.0;   // a YES this late (the question left open) no longer moves
constexpr double kArrivalWaitS = 120.0; // after the travel: beside the host once it is in

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party gather: ") + line + "\n");
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The local chr's physics position (chr +0x58 -> +8 -> +0x3B0 -> +0x68, +0x1E0), as the director's
/// DescribeHere reads it.
bool OwnPos(u64 player, float p[3]) {
    u64 a = 0;
    return player && SafeGet(player + 0x58, &a) && a && SafeGet(a + 8, &a) && a && SafeGet(a + 0x3b0, &a) && a &&
           SafeGet(a + 0x68, &a) && a && SafeRead(a + 0x1e0, p, 12);
}

std::atomic<bool> g_request{false};
std::mutex g_mu;
std::deque<std::pair<int, std::string>> g_in;  // under g_mu

// ---- host ----
struct HostState {
    double last_gather_at = -1e9;
    u32 next_round = 0;
    bool open = false;
    u32 round = 0;
    double at = 0;
    std::vector<int> slots;            // the guests asked, as m
    std::vector<MemberAnswer> m;
};
HostState g_host;

// ---- guest ----
struct GuestState {
    bool active = false;   // a question for `ask`
    Ask ask;
    double since = 0;
    u64 dialog = 0;
    double open_until = 0; // the dialog must be up by then
    // after a travel: beside the host once it has arrived
    bool arriving = false;
    double arriving_since = 0;
    u32 arriving_map = 0;
    // test aid
    std::vector<std::string> test;
    std::size_t test_next = 0;
    int test_step = 0;
    double test_at = 0, release_at = 0;
    bool menu_opened = false;
    double menu_at = 0;
};
GuestState g_guest;

std::string RosterName(party::PartyLink* link, int slot) {
    if (link) {
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot == slot) return e.name;
        }
    }
    return "slot " + std::to_string(slot);
}

void HostTick(const GameSnapshot& s, party::PartyLink* link, const std::vector<std::pair<int, std::string>>& in) {
    HostState& h = g_host;
    const double now = Now();
    for (const auto& [slot, body] : in) {
        Answer a;
        if (!DecodeAnswer(body, &a)) {
            Log("host: bad event from slot %d dropped", slot);
            continue;
        }
        bool matched = false;
        for (std::size_t i = 0; h.open && a.round == h.round && i < h.slots.size(); ++i) {
            if (h.slots[i] == slot && !h.m[i].answered) {
                h.m[i].answered = true;
                h.m[i].answer = a;
                matched = true;
            }
        }
        Log("host: round %u: slot %d (%s) answered %s%s%s%s%s", a.round, slot, RosterName(link, slot).c_str(),
            ReplyWord(a.reply), a.reply == Reply::Yes ? ", " : "", a.reply == Reply::Yes ? RouteWord(a.route) : "",
            a.reply == Reply::Refused ? ": " : "", a.reply == Reply::Refused ? RefusalReason(a.why) : "");
        if (!matched) {
            Log("host: (the answer is for round %u; the open round is %u%s)", a.round, h.round, h.open ? "" : ", closed");
        }
    }
    if (h.open && RoundOver(h.m, now - h.at)) {
        h.open = false;
        const std::string sum = Summary(h.m);
        party::status::SetLastEvent(sum);
        Log("round %u over (%.1f s): %s", h.round, now - h.at, sum.c_str());
    }
    if (!g_request.exchange(false)) {
        return;
    }
    HostView v;
    v.enabled = Enabled();
    v.host = link && link->is_host();
    if (link) {
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot != 0 && e.connected) ++v.guests_connected;
        }
    }
    v.world_up = s.world_up;
    v.loading = s.loading;
    v.alive = b2::LocalHp() > 0;
    v.boss_fight = BossBarShown();
    v.cutscene = CutscenePlaying();
    v.travelling = b2::HostRejoinRunning() || b2::RejoinRunning();
    v.now = now;
    v.last_gather_at = h.last_gather_at;
    int wait = 0;
    const Refusal r = HostRefusal(v, &wait);
    if (r != Refusal::None) {
        const std::string text = HostRefusalText(r, wait);
        party::status::SetLastEvent(text);
        Log("Gather the party refused: %s (%s)", RefusalReason(r), text.c_str());
        return;
    }
    Ask a;
    a.round = ++h.next_round;
    a.map = s.map_id;
    OwnPos(s.player, a.pos);
    a.t = link->host_clock_ms();
    h.last_gather_at = now;
    h.open = true;
    h.round = a.round;
    h.at = now;
    h.slots.clear();
    h.m.clear();
    for (const party::RosterEntry& e : link->roster()) {
        if (e.slot == 0 || !e.connected) continue;
        h.slots.push_back(e.slot);
        MemberAnswer m;
        m.name = e.name;
        h.m.push_back(m);
    }
    link->post_event(party::kBroadcast, kEventName, EncodeAsk(a));
    party::status::SetLastEvent("Gathering the party: asking " + std::to_string(h.slots.size()) +
                                (h.slots.size() == 1 ? " guest" : " guests"));
    Log("round %u: Gather the party: %zu guest(s) asked; the host in %s at (%.1f, %.1f, %.1f)", a.round, h.slots.size(),
        MapName(a.map).c_str(), double(a.pos[0]), double(a.pos[1]), double(a.pos[2]));
}

void Press(u32 buttons, double now, GuestState& g) {
    runtime_pad_director_press(buttons);
    g.release_at = now + 0.25;  // held long enough for a slow frame (split travel's test aid)
}

void SendAnswer(party::PartyLink* link, const Answer& a) {
    if (link) link->post_event(party::kHostSlot, kEventName, EncodeAnswer(a));
}

/// The guest's view of itself now. `own_question`: our YES / NO box is (or was just) up - it is not
/// "another dialog".
GuestView ReadGuestView(const GameSnapshot& s, bool own_question) {
    GuestView v;
    v.world_up = s.world_up && s.player;
    v.loading = s.loading;
    v.alive = b2::LocalHp() > 0;
    v.spectating = b2::Spectating();
    v.boss_fight = BossBarShown();
    v.cutscene = CutscenePlaying();
    v.travelling = b2::RejoinRunning() || b2::GuestTravelPending();
    if (!own_question) {
        v.menu = party::hud::GameMenu();
        v.dialog = party::hud::GameDialog() || split::DialogUp();
    }
    return v;
}

void Refuse(party::PartyLink* link, const Ask& ask, Refusal why) {
    Answer a;
    a.round = ask.round;
    a.reply = Reply::Refused;
    a.why = why;
    SendAnswer(link, a);
    const std::string text = GuestRefusalText(why);
    party::status::SetLastEvent(text);
    Log("guest: round %u refused: %s", ask.round, RefusalReason(why));
}

/// YES: come to the host by RouteFor. False: could not move (the answer says Failed).
bool Come(const GameSnapshot& s, const Ask& ask, Route* route) {
    Place p;
    p.client = s.session_role == RoleClient;
    p.own_map = s.map_id;
    p.host_map = ask.map;
    float d = 1e9f;
    p.host_chr_here = p.client && b2::HostChrHere(&d);
    p.distance = d;
    *route = RouteFor(p);
    Log("guest: YES: route %s (own %s, the host's %s, the host's chr %s, %.1f m)", RouteWord(*route),
        MapName(p.own_map).c_str(), MapName(p.host_map).c_str(), p.host_chr_here ? "here" : "not here", double(d));
    switch (*route) {
    case Route::Here:
        party::status::SetLastEvent("Gathering: you are already with the host");
        return true;
    case Route::InWorld:
        if (b2::GatherBesideHost("gather")) {
            party::status::SetLastEvent("Gathered: beside the host");
            return true;
        }
        Log("guest: the in-world warp did not run; the travel instead");
        *route = Route::Travel;
        [[fallthrough]];
    case Route::Travel:
        if (b2::GatherTravelToHost()) {
            g_guest.arriving = true;
            g_guest.arriving_since = Now();
            g_guest.arriving_map = ask.map;
            party::status::SetLastEvent("Gathering: travelling to the host");
            return true;
        }
        return false;
    case Route::Rejoin:
        party::status::RequestRejoin();
        party::status::SetLastEvent("Gathering: rejoining the host");
        return true;
    }
    return false;
}

void GuestTick(const GameSnapshot& s, party::PartyLink* link, const std::vector<std::pair<int, std::string>>& in) {
    GuestState& g = g_guest;
    const double now = Now();
    if (g.release_at > 0 && now >= g.release_at) {
        runtime_pad_director_press(0);
        g.release_at = 0;
    }
    // Test aid: "menu" opens the in-game menu ahead of the next question.
    if (!g.test.empty() && g.test_next < g.test.size() && g.test[g.test_next] == "menu" && !g.menu_opened && !g.active &&
        !g.arriving && s.world_up && !s.loading && now - g.test_at > 3.0 && !b2::RejoinRunning()) {
        Press(kPadOptions, now, g);
        g.menu_opened = true;
        g.menu_at = now;
        Log("test: the in-game menu opened (Options) for the next question");
    }
    for (const auto& [slot, body] : in) {
        Ask a;
        if (!DecodeAsk(body, &a)) {
            Log("guest: bad event from slot %d dropped", slot);
            continue;
        }
        const std::int64_t age = link ? link->host_clock_ms() - a.t : 0;
        if (a.t != 0 && age > kAskMaxAgeMs) {
            Log("guest: round %u is %lld ms old (a replay after a reconnect): dropped", a.round, (long long)age);
            continue;
        }
        Log("guest: round %u: the host gathers the party (host in %s at (%.1f, %.1f, %.1f))", a.round, MapName(a.map).c_str(),
            double(a.pos[0]), double(a.pos[1]), double(a.pos[2]));
        if (!party::status::HostRuleOn(party::status::HostRule::Gather)) {  // the host's rule (late)
            Refuse(link, a, Refusal::Off);
            continue;
        }
        if (g.active) {
            Refuse(link, a, Refusal::Dialog);
            continue;
        }
        const Refusal r = GuestRefusal(ReadGuestView(s, false));
        if (r != Refusal::None) {
            Refuse(link, a, r);
            if (g.menu_opened && g.test_next < g.test.size() && g.test[g.test_next] == "menu") {
                ++g.test_next;  // test aid: close the menu again
                g.menu_opened = false;
                Press(kPadCircle, now, g);
                g.test_at = now;
                Log("test: menu refusal seen; closing the menu (Circle)");
            }
            continue;
        }
        g.active = true;
        g.ask = a;
        g.since = now;
        g.dialog = 0;
        g.open_until = now + 2.0;
        g.test_step = 0;
        g.test_at = now;
    }
    if (g.menu_opened && g.release_at == 0 && now - g.menu_at > 60.0) {
        // test aid: no question came; close it anyway
        g.menu_opened = false;
        Press(kPadCircle, now, g);
    }
    if (g.active) {
        if (!g.dialog) {
            g.dialog = split::AskYesNo(BbGameMenu::Pages::GatherQuestionText());
            if (g.dialog) {
                Log("guest: round %u: question open (dialog %#llx)", g.ask.round, static_cast<unsigned long long>(g.dialog));
                g.test_at = now;
            } else if (now > g.open_until) {
                g.active = false;
                Refuse(link, g.ask, Refusal::Dialog);
            }
            return;
        }
        const int r = split::QuestionResult(g.dialog);
        // Test answer: YES = Cross; NO = Right, then Cross (the cursor starts on YES).
        if (r == -1 && g.test_next < g.test.size() && (g.test[g.test_next] == "yes" || g.test[g.test_next] == "no") &&
            now - g.test_at > 1.5 && g.release_at == 0 && g.test_step < 6) {
            const bool yes = g.test[g.test_next] == "yes";
            const u32 b = yes || g.test_step % 2 == 1 ? kPadCross : kPadRight;
            Press(b, now, g);
            g.test_at = now;
            ++g.test_step;
            Log("test: answer %s: press %#x (step %d)", yes ? "yes" : "no", b, g.test_step);
        }
        if (r == -1) {
            return;
        }
        g.active = false;
        if (g.test_next < g.test.size() && (g.test[g.test_next] == "yes" || g.test[g.test_next] == "no")) {
            ++g.test_next;
        }
        Answer a;
        a.round = g.ask.round;
        if (r != 1) {
            a.reply = Reply::No;
            SendAnswer(link, a);
            party::status::SetLastEvent("Gathering: you stay where you are");
            Log("guest: round %u: NO (result %d)", a.round, r);
            return;
        }
        if (now - g.since > kLateAnswerS) {
            a.reply = Reply::NoAnswer;
            SendAnswer(link, a);
            party::status::SetLastEvent("The gather is over (answered too late)");
            Log("guest: round %u: YES %.0f s after the question: too late, no move", a.round, now - g.since);
            return;
        }
        // The state may have changed while the question was up (a boss fight, a death).
        const Refusal now_r = GuestRefusal(ReadGuestView(s, true));
        if (now_r != Refusal::None) {
            Refuse(link, g.ask, now_r);
            return;
        }
        Route route = Route::Here;
        if (Come(s, g.ask, &route)) {
            a.reply = Reply::Yes;
            a.route = route;
        } else {
            a.reply = Reply::Refused;
            a.why = Refusal::Failed;
            party::status::SetLastEvent(GuestRefusalText(Refusal::Failed));
        }
        SendAnswer(link, a);
        Log("guest: round %u: YES -> %s", a.round, a.reply == Reply::Yes ? RouteWord(a.route) : "could not move");
        return;
    }
    if (g.arriving) {
        if (now - g.arriving_since > kArrivalWaitS) {
            g.arriving = false;
            Log("guest: no arrival in the host's map within %.0f s", kArrivalWaitS);
            return;
        }
        if (!s.world_up || s.loading || b2::RejoinRunning() || !SameBlock(s.map_id, g.arriving_map)) {
            return;
        }
        float d = 0;
        if (!b2::HostChrHere(&d)) {
            return;  // the host's chr is built again after the arrival (the member task rewind)
        }
        g.arriving = false;
        g.test_at = now;
        if (d > kBesideM) {
            b2::GatherBesideHost("gather arrival");
        }
        party::status::SetLastEvent("Gathered: beside the host");
        Log("guest: arrived in the host's map %s %.1f s after the YES; the host %.1f m away%s", MapName(s.map_id).c_str(),
            now - g.arriving_since, double(d), d > kBesideM ? ": put beside it" : "");
    }
}

} // namespace

bool Enabled() {
    return BbSettings::Get().party_gather.load();
}

void RequestFromMenu() {
    g_request = true;
}

bool HotkeyAvailable() {
    if (!Enabled()) return false;
    const party::status::Board b = party::status::Snapshot();
    return b.enabled && b.role == party::status::Role::Host && b.state != party::status::State::Off;
}

bool RequestFromHotkey(const char* source) {
    if (!HotkeyAvailable()) return false;
    Log("Gather the party: %s pressed", source ? source : "hotkey");
    return party::status::RequestGather();  // the menu row's path: the same refusals and notices
}

void OnEvent(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_in.size() < 16) g_in.emplace_back(slot, body);
}

void Tick(const GameSnapshot& s, party::PartyLink* link, bool host) {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* v = std::getenv("BB_PARTY_GATHER_TEST"); v && v[0]) {
            std::string item;
            for (const char* p = v;; ++p) {
                if (*p == ',' || *p == 0) {
                    if (!item.empty()) g_guest.test.push_back(item);
                    item.clear();
                    if (*p == 0) break;
                } else {
                    item.push_back(*p);
                }
            }
            Log("test: %zu answer(s) scripted (%s)", g_guest.test.size(), v);
        }
    }
    std::vector<std::pair<int, std::string>> in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in.assign(g_in.begin(), g_in.end());
        g_in.clear();
    }
    if (host) {
        HostTick(s, link, in);
    } else {
        g_request = false;  // only the host gathers
        GuestTick(s, link, in);
    }
}

} // namespace coop::gather

extern "C" int bbgpu_gather_combo_enabled(void) {
    return coop::gather::HotkeyAvailable() ? 1 : 0;
}

extern "C" void bbgpu_gather_request(const char* source) {
    coop::gather::RequestFromHotkey(source);
}
