// SPDX-License-Identifier: GPL-3.0-or-later
// Lamp travel voting (0.7.3, user: "everybody has to vote yes"), the pure rules: who votes, when a
// vote is over and how it ended, the wire and the player texts. Game side: party_vote.cpp; unit
// test tests/test_party_vote.cpp.
//
//   With the host's rule on (System > Party rules: World "Party votes on travel", bbport.ini
//   party_travel_vote, off by default) a party travel by lamp, headstone or Hunter's Mark runs only
//   once every other member said YES:
//     host   its YES to "Travel with the party?" starts a vote among the connected guests;
//     guest  its YES there (where a guest can start party travel today: its lamp request, its
//            Hunter's Mark with the party) becomes a vote among the host and the other guests;
//            the starter's own YES counts.
//   Each voter gets the game's own YES / NO dialog. One NO, or no answer within kTimeoutS, cancels
//   it for everyone, with a notice naming who declined or did not answer. Rule off, or nobody else
//   connected: the travel runs at once, as before.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::vote {

constexpr const char* kEventName = "travel_vote";
constexpr double kTimeoutS = 20.0;          // a voter that has not answered by then: "did not answer"
constexpr std::int64_t kAskMaxAgeMs = 15000; // an ask this old (a replay after a reconnect) is dropped

enum class Reply { Yes, No, Unable };
inline const char* ReplyWord(Reply r) {
    switch (r) {
    case Reply::Yes: return "yes";
    case Reply::No: return "no";
    case Reply::Unable: return "unable";
    }
    return "?";
}

struct Voter {
    int slot = -1;
    std::string name;
    bool answered = false;
    Reply reply = Reply::No;
};

/// Who votes: every connected member but the one who started the travel (its YES counts).
struct Member {
    int slot = -1;
    std::string name;
    bool connected = false;
};
inline std::vector<Voter> Voters(const std::vector<Member>& roster, int starter_slot) {
    std::vector<Voter> v;
    for (const Member& m : roster) {
        if (!m.connected || m.slot == starter_slot || m.slot < 0) continue;
        Voter x;
        x.slot = m.slot;
        x.name = m.name.empty() ? "slot " + std::to_string(m.slot) : m.name;
        v.push_back(x);
    }
    return v;
}

/// The vote is needed: the host's rule is on and somebody else can vote. Off / nobody: the travel
/// runs at once (as before the rule).
inline bool Needed(bool rule_on, const std::vector<Voter>& voters) {
    return rule_on && !voters.empty();
}

enum class Outcome { Waiting, Go, Declined, TimedOut };
inline const char* OutcomeWord(Outcome o) {
    switch (o) {
    case Outcome::Waiting: return "waiting";
    case Outcome::Go: return "go";
    case Outcome::Declined: return "declined";
    case Outcome::TimedOut: return "timeout";
    }
    return "?";
}

/// Any NO (or "unable": the question could not be shown) ends it at once; all YES: go; past
/// `timeout_s` with someone silent: timed out.
inline Outcome Decide(const std::vector<Voter>& v, double since_s, double timeout_s = kTimeoutS) {
    bool all = true;
    for (const Voter& x : v) {
        if (x.answered && x.reply != Reply::Yes) return Outcome::Declined;
        if (!x.answered) all = false;
    }
    if (all) return Outcome::Go;
    return since_s >= timeout_s ? Outcome::TimedOut : Outcome::Waiting;
}

/// A voter `slot` left the party mid-vote: it no longer counts (the vote is among those present).
inline void DropVoter(std::vector<Voter>* v, int slot) {
    for (std::size_t i = 0; i < v->size(); ++i) {
        if ((*v)[i].slot == slot) {
            v->erase(v->begin() + std::ptrdiff_t(i));
            return;
        }
    }
}

/// Records `slot`'s answer; false when it is not a voter or answered already (the first counts).
inline bool Record(std::vector<Voter>* v, int slot, Reply r) {
    for (Voter& x : *v) {
        if (x.slot == slot && !x.answered) {
            x.answered = true;
            x.reply = r;
            return true;
        }
    }
    return false;
}

/// The notice when it is cancelled: "Party travel cancelled: Ann said no; Bob did not answer".
inline std::string CancelText(const std::vector<Voter>& v) {
    std::string no, silent, unable;
    auto add = [](std::string& list, const std::string& item) { list += (list.empty() ? "" : ", ") + item; };
    for (const Voter& x : v) {
        if (!x.answered) add(silent, x.name);
        else if (x.reply == Reply::No) add(no, x.name);
        else if (x.reply == Reply::Unable) add(unable, x.name);
    }
    std::string out = "Party travel cancelled:";
    if (!no.empty()) out += " " + no + " said no;";
    if (!unable.empty()) out += " " + unable + " could not answer;";
    if (!silent.empty()) out += " " + silent + " did not answer;";
    if (out.back() == ';') out.pop_back();
    return out;
}

/// The starter's notice while the others vote: "Waiting for the party's vote (2)".
inline std::string WaitingText(std::size_t voters) {
    return "Waiting for the party's vote (" + std::to_string(voters) + (voters == 1 ? " player)" : " players)");
}

// ---- wire (EVENT travel_vote) ----
/// Text put on the wire: no quote, backslash or control character (names are the roster's).
inline std::string Clean(const std::string& s, std::size_t max = 120) {
    std::string o;
    for (char c : s) {
        if (o.size() >= max) break;
        if (c == '"' || c == '\\') o += '\'';
        else if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o;
}

struct Ask {
    std::uint32_t round = 0;
    std::uint32_t lamp = 0;   // the ReturnPointParam row the party would travel to
    int starter = -1;         // the slot whose YES started it
    std::string who;          // its name (the question says who wants to travel)
    std::int64_t t = 0;       // the host clock when sent
};

inline std::string EncodeAsk(const Ask& a) {
    char b[256];
    std::snprintf(b, sizeof b, "{\"op\":\"ask\",\"round\":%u,\"lamp\":%u,\"by\":%d,\"t\":%lld,\"who\":\"%s\"}", a.round,
                  a.lamp, a.starter, (long long)a.t, Clean(a.who, 64).c_str());
    return b;
}

/// The string after `key` up to the next quote ("" when missing).
inline std::string StrField(const char* s, const char* key) {
    const char* p = std::strstr(s, key);
    if (!p) return {};
    p += std::strlen(key);
    const char* e = std::strchr(p, '"');
    return e && e - p <= 200 ? std::string(p, e) : std::string();
}

inline bool DecodeAsk(const std::string& body, Ask* out) {
    const char* s = body.c_str();
    if (!std::strstr(s, "\"op\":\"ask\"")) return false;
    const char* pr = std::strstr(s, "\"round\":");
    const char* pl = std::strstr(s, "\"lamp\":");
    const char* pb = std::strstr(s, "\"by\":");
    Ask a;
    if (!pr || !pl || !pb || std::sscanf(pr, "\"round\":%u", &a.round) != 1 || std::sscanf(pl, "\"lamp\":%u", &a.lamp) != 1 ||
        std::sscanf(pb, "\"by\":%d", &a.starter) != 1 || a.round == 0 || a.lamp == 0) {
        return false;
    }
    if (const char* pt = std::strstr(s, "\"t\":")) {
        long long t = 0;
        if (std::sscanf(pt, "\"t\":%lld", &t) == 1) a.t = t;
    }
    a.who = StrField(s, "\"who\":\"");
    *out = a;
    return true;
}

struct Answer {
    std::uint32_t round = 0;
    Reply reply = Reply::No;
};

inline std::string EncodeAnswer(const Answer& a) {
    char b[96];
    std::snprintf(b, sizeof b, "{\"op\":\"answer\",\"round\":%u,\"reply\":\"%s\"}", a.round, ReplyWord(a.reply));
    return b;
}

inline bool DecodeAnswer(const std::string& body, Answer* out) {
    const char* s = body.c_str();
    if (!std::strstr(s, "\"op\":\"answer\"")) return false;
    const char* pr = std::strstr(s, "\"round\":");
    Answer a;
    if (!pr || std::sscanf(pr, "\"round\":%u", &a.round) != 1 || a.round == 0) return false;
    const std::string r = StrField(s, "\"reply\":\"");
    if (r == "yes") a.reply = Reply::Yes;
    else if (r == "no") a.reply = Reply::No;
    else if (r == "unable") a.reply = Reply::Unable;
    else return false;
    *out = a;
    return true;
}

/// The host's end of a round, to every member: go (the travel follows) or cancelled with the notice.
struct End {
    std::uint32_t round = 0;
    bool go = false;
    std::string text;
};

inline std::string EncodeEnd(const End& e) {
    char b[320];
    std::snprintf(b, sizeof b, "{\"op\":\"end\",\"round\":%u,\"go\":%d,\"text\":\"%s\"}", e.round, e.go ? 1 : 0,
                  Clean(e.text, 200).c_str());
    return b;
}

inline bool DecodeEnd(const std::string& body, End* out) {
    const char* s = body.c_str();
    if (!std::strstr(s, "\"op\":\"end\"")) return false;
    const char* pr = std::strstr(s, "\"round\":");
    const char* pg = std::strstr(s, "\"go\":");
    End e;
    int go = 0;
    if (!pr || !pg || std::sscanf(pr, "\"round\":%u", &e.round) != 1 || std::sscanf(pg, "\"go\":%d", &go) != 1 ||
        e.round == 0) {
        return false;
    }
    e.go = go != 0;
    e.text = StrField(s, "\"text\":\"");
    *out = e;
    return true;
}

/// Test aid (BB_PARTY_VOTE_TEST=yes,no,none,...): the answer to the `n`-th question (1-based; the
/// last one repeats; "none" leaves it unanswered). Empty: no scripted answer.
inline std::string TestAnswer(const std::string& list, int n) {
    if (list.empty() || n < 1) return {};
    std::size_t at = 0;
    for (int k = 1; k < n; ++k) {
        const std::size_t c = list.find(',', at);
        if (c == std::string::npos) break;
        at = c + 1;
    }
    const std::size_t c = list.find(',', at);
    return list.substr(at, c == std::string::npos ? std::string::npos : c - at);
}

} // namespace coop::vote
