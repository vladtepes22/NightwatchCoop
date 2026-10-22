// SPDX-License-Identifier: GPL-3.0-or-later
// Lamp travel voting (0.7.3), the game side (party_vote.h). Main thread: HostBegin and Tick;
// OnEvent on the link's callback thread only queues.
#include "party_vote.h"

#include "game_state.h"
#include "lamp_travel_rules.h"
#include "party_async_io.h"
#include "party_hud.h"
#include "party_link.h"
#include "party_split.h"
#include "party_status.h"
#include "../bbport_game_menu.h"
#include "../bbport_game_menu_pages.h"
#include "../bbport_settings.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop::vote {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kPadCross = 0x4000, kPadRight = 0x20, kPadCircle = 0x2000;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party vote: ") + line + "\n");
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double TimeoutS() {
    static const double t = [] {
        const char* v = std::getenv("BB_PARTY_VOTE_TIMEOUT");
        const double s = v && v[0] ? std::atof(v) : 0.0;
        return s >= 3.0 && s <= 120.0 ? s : kTimeoutS;
    }();
    return t;
}

std::mutex g_mu;
std::deque<std::pair<int, std::string>> g_in;  // under g_mu

// ---- host: the running round ----
struct Round {
    bool open = false;
    u32 round = 0;
    u32 lamp = 0;
    int starter = 0;
    std::string what;
    std::function<void()> go;
    std::vector<Voter> voters;
    double at = 0;
    bool sent = false;  // the ask is out (the first tick with the link)
};
Round g_round;     // main thread
u32 g_next_round = 0;

// ---- any role: this game's own question ----
struct Question {
    bool active = false;
    Ask ask;
    double since = 0;
    double until = 0;   // give up opening it by then
    u64 dialog = 0;
    int test_step = 0;
    double test_at = 0;
};
Question g_q;            // main thread
int g_asked = 0;         // questions shown (BB_PARTY_VOTE_TEST answers them in order)
double g_release_at = 0; // a test / cancel press is a tap

void Press(u32 buttons, double now) {
    runtime_pad_director_press(buttons);
    g_release_at = now + 0.25;  // held long enough for a slow frame (split travel's test aid)
}

std::string RosterName(party::PartyLink* link, int slot) {
    if (link) {
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot == slot && !e.name.empty()) return e.name;
        }
    }
    return slot == 0 ? "The host" : "slot " + std::to_string(slot);
}

/// The lamp's name as the game's headstones show it (EventText of its name flag).
std::u16string LampName(u32 lamp) {
    lamptravel::Lamp l{};
    const u32 flag = lamp == lamptravel::kDreamReturnPoint ? lamptravel::kDreamNameFlag
                     : lamptravel::LampOf(lamp, &l)       ? lamptravel::NameFlag(l)
                                                          : 0;
    if (const char16_t* t = flag ? BbGameMenu::GameText(30, flag) : nullptr) return t;
    std::u16string out = u"lamp ";
    for (char c : std::to_string(lamp)) out += char16_t(c);
    return out;
}

/// Closes this game's open question (the dialog's own cancel), if it is still up.
void CloseQuestion(double now, const char* why) {
    if (!g_q.active) return;
    const bool up = g_q.dialog && split::QuestionResult(g_q.dialog) == -1 && split::DialogUp();
    if (up) Press(kPadCircle, now);
    Log("round %u: this game's question closed (%s)%s", g_q.ask.round, why, up ? ", dialog cancelled" : "");
    g_q = Question{};
}

void StartQuestion(const Ask& a, double now) {
    g_q = Question{};
    g_q.active = true;
    g_q.ask = a;
    g_q.since = now;
    g_q.until = now + TimeoutS();
}

void HostRecord(int slot, Reply r, party::PartyLink* link);

/// This game's question: opened once no menu / dialog is up, answered by the player (or the test
/// aid); the answer to the host (a guest) or into the round (the host). Main thread.
void QuestionTick(const GameSnapshot& s, party::PartyLink* link, bool host, double now) {
    if (!g_q.active) return;
    if (!g_q.dialog) {
        if (now > g_q.until) {
            Log("round %u: the question could not be shown in time (menu, dialog or loading)", g_q.ask.round);
            g_q = Question{};
            return;  // no answer: the host's timeout names this player
        }
        if (!s.world_up || s.loading || party::hud::GameMenu() || split::DialogUp()) return;
        BbGameMenu::Pages::SetTravelVoteQuestion(g_q.ask.who, LampName(g_q.ask.lamp));
        g_q.dialog = split::AskYesNo(BbGameMenu::Pages::TravelVoteQuestionText());
        if (g_q.dialog) {
            ++g_asked;
            g_q.test_at = now;
            Log("round %u: question open: %s wants the party to travel to lamp %u (dialog %#llx)", g_q.ask.round,
                g_q.ask.who.c_str(), g_q.ask.lamp, static_cast<unsigned long long>(g_q.dialog));
        }
        return;
    }
    const int r = split::QuestionResult(g_q.dialog);
    static const std::string test = [] {
        const char* v = std::getenv("BB_PARTY_VOTE_TEST");
        return std::string(v ? v : "");
    }();
    const std::string want = TestAnswer(test, g_asked);
    if (r == -1 && (want == "yes" || want == "no") && now - g_q.test_at > 1.5 && g_release_at == 0 && g_q.test_step < 6) {
        // YES = Cross; NO = Right, then Cross (the cursor starts on YES).
        const u32 b = want == "yes" || g_q.test_step % 2 == 1 ? kPadCross : kPadRight;
        Press(b, now);
        g_q.test_at = now;
        ++g_q.test_step;
        Log("test: vote answer %s: press %#x (step %d)", want.c_str(), b, g_q.test_step);
    }
    if (r == -1) return;
    const Reply reply = r == 1 ? Reply::Yes : Reply::No;
    Log("round %u: this player answered %s (result %d)", g_q.ask.round, ReplyWord(reply), r);
    const u32 round = g_q.ask.round;
    g_q = Question{};
    if (host) {
        if (g_round.open && g_round.round == round) HostRecord(0, reply, link);
    } else if (link) {
        Answer a;
        a.round = round;
        a.reply = reply;
        link->post_event(party::kHostSlot, kEventName, EncodeAnswer(a));
    }
    party::status::SetLastEvent(reply == Reply::Yes ? "You voted yes: waiting for the others" : "You voted no");
}

void HostRecord(int slot, Reply r, party::PartyLink* link) {
    if (!Record(&g_round.voters, slot, r)) {
        Log("round %u: answer %s from slot %d dropped (not a voter, or answered already)", g_round.round, ReplyWord(r),
            slot);
        return;
    }
    Log("round %u: %s (slot %d) votes %s", g_round.round, RosterName(link, slot).c_str(), slot, ReplyWord(r));
}

void HostEnd(party::PartyLink* link, Outcome o, double now) {
    Round rd = std::move(g_round);
    g_round = Round{};
    End e;
    e.round = rd.round;
    e.go = o == Outcome::Go;
    e.text = e.go ? "The party travels" : CancelText(rd.voters);
    if (link) link->post_event(party::kBroadcast, kEventName, EncodeEnd(e));
    if (g_q.active && g_q.ask.round == rd.round) CloseQuestion(now, OutcomeWord(o));
    Log("round %u over (%.1f s, %s): lamp %u %s", rd.round, now - rd.at, OutcomeWord(o), rd.lamp,
        e.go ? "-> the party travels" : ("cancelled: " + e.text).c_str());
    party::status::SetLastEvent(e.text);
    if (e.go && rd.go) rd.go();
}

void HostTick(const GameSnapshot& s, party::PartyLink* link, const std::vector<std::pair<int, std::string>>& in,
              double now) {
    for (const auto& [slot, body] : in) {
        Answer a;
        if (!DecodeAnswer(body, &a)) {
            Log("host: bad event from slot %d dropped", slot);
            continue;
        }
        if (!g_round.open || a.round != g_round.round) {
            Log("host: slot %d's answer for round %u dropped (the open round is %u%s)", slot, a.round, g_round.round,
                g_round.open ? "" : ", closed");
            continue;
        }
        HostRecord(slot, a.reply, link);
    }
    if (!g_round.open) return;
    if (!link || !link->is_host() || s.session_role != RoleHost) {
        Log("round %u: the party session ended during the vote; nothing travels", g_round.round);
        g_round = Round{};
        if (g_q.active) CloseQuestion(now, "the session ended");
        return;
    }
    if (!g_round.sent) {
        // The voters: every connected member but the starter (the host votes on a guest's travel).
        std::vector<Member> roster;
        bool host_listed = false;
        for (const party::RosterEntry& e : link->roster()) {
            roster.push_back({e.slot, e.name, e.slot == 0 ? true : e.connected});
            host_listed = host_listed || e.slot == 0;
        }
        if (!host_listed) roster.push_back({0, "", true});
        g_round.voters = Voters(roster, g_round.starter);
        for (Voter& v : g_round.voters) v.name = RosterName(link, v.slot);
        g_round.sent = true;
        g_round.at = now;
        if (g_round.voters.empty()) {
            Log("round %u: nobody else to ask; the travel runs", g_round.round);
            HostEnd(link, Outcome::Go, now);
            return;
        }
        Ask a;
        a.round = g_round.round;
        a.lamp = g_round.lamp;
        a.starter = g_round.starter;
        a.who = RosterName(link, g_round.starter);
        a.t = link->host_clock_ms();
        link->post_event(party::kBroadcast, kEventName, EncodeAsk(a));
        std::string names;
        for (const Voter& v : g_round.voters) names += (names.empty() ? "" : ", ") + v.name;
        Log("round %u: %s: %s wants the party to travel to lamp %u; asking %zu (%s), %.0f s", a.round,
            g_round.what.c_str(), a.who.c_str(), a.lamp, g_round.voters.size(), names.c_str(), TimeoutS());
        if (g_round.starter == 0) {
            party::status::SetLastEvent(WaitingText(g_round.voters.size()));
        } else {
            StartQuestion(a, now);  // the host votes on a guest's travel: its own dialog
        }
        return;
    }
    // A voter who left the party no longer counts (the host itself always does).
    for (std::size_t i = g_round.voters.size(); i-- > 0;) {
        const int slot = g_round.voters[i].slot;
        if (slot == 0) continue;
        bool here = false;
        for (const party::RosterEntry& e : link->roster()) here = here || (e.slot == slot && e.connected);
        if (!here) {
            Log("round %u: %s (slot %d) left the party: no longer votes", g_round.round, g_round.voters[i].name.c_str(), slot);
            DropVoter(&g_round.voters, slot);
        }
    }
    const Outcome o = Decide(g_round.voters, now - g_round.at, TimeoutS());
    if (o != Outcome::Waiting) HostEnd(link, o, now);
}

void GuestTick(party::PartyLink* link, const std::vector<std::pair<int, std::string>>& in, double now) {
    const int mine = link ? link->local_slot() : -1;
    for (const auto& [slot, body] : in) {
        Ask a;
        End e;
        if (DecodeAsk(body, &a)) {
            const std::int64_t age = link ? link->host_clock_ms() - a.t : 0;
            if (a.t != 0 && age > kAskMaxAgeMs) {
                Log("round %u is %lld ms old (a replay after a reconnect): dropped", a.round, (long long)age);
                continue;
            }
            if (a.starter == mine) {
                Log("round %u: the party votes on this player's travel to lamp %u", a.round, a.lamp);
                party::status::SetLastEvent("Waiting for the party's vote");
                continue;
            }
            if (g_q.active) CloseQuestion(now, "a newer vote");
            Log("round %u: %s wants the party to travel to lamp %u", a.round, a.who.c_str(), a.lamp);
            StartQuestion(a, now);
        } else if (DecodeEnd(body, &e)) {
            if (g_q.active && g_q.ask.round == e.round) CloseQuestion(now, e.go ? "everyone said yes" : "cancelled");
            Log("round %u over: %s", e.round, e.go ? "the party travels" : e.text.c_str());
            party::status::SetLastEvent(e.go ? "The party travels" : e.text);
        } else {
            Log("guest: bad event from slot %d dropped", slot);
        }
    }
}

} // namespace

bool Enabled() {
    return BbSettings::Get().party_travel_vote.load();
}

bool HostBegin(std::uint32_t lamp, int starter, std::function<void()> go, const char* what) {
    if (!Enabled()) return false;
    if (g_round.open) {
        Log("%s: a party vote (round %u, lamp %u) is already running; this travel does not run", what ? what : "travel",
            g_round.round, g_round.lamp);
        party::status::SetLastEvent("A party vote is already running");
        return true;
    }
    g_round = Round{};
    g_round.open = true;
    g_round.round = ++g_next_round;
    g_round.lamp = lamp;
    g_round.starter = starter;
    g_round.what = what ? what : "travel";
    g_round.go = std::move(go);
    g_round.at = Now();
    Log("round %u: %s to lamp %u (slot %d): the party votes first", g_round.round, g_round.what.c_str(), lamp, starter);
    return true;
}

bool Busy() {
    return g_round.open || g_q.active;
}

void OnEvent(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_in.size() < 16) g_in.emplace_back(slot, body);
}

void Tick(const GameSnapshot& s, party::PartyLink* link, bool host) {
    const double now = Now();
    if (g_release_at > 0 && now >= g_release_at) {
        runtime_pad_director_press(0);
        g_release_at = 0;
    }
    std::vector<std::pair<int, std::string>> in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in.assign(g_in.begin(), g_in.end());
        g_in.clear();
    }
    if (host) {
        HostTick(s, link, in, now);
    } else {
        if (g_round.open) g_round = Round{};  // only the host runs a round
        GuestTick(link, in, now);
    }
    QuestionTick(s, link, host, now);
}

} // namespace coop::vote
