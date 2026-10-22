// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.3 lamp travel voting: who votes, when it is over, the notices, the wire
// (gpu/shim/party/party_vote_rules.h).
// Build and run: ninja -C out/gpu party-vote-test && out/gpu/party-vote-test.exe
#include "party/party_vote_rules.h"

#include <cstdio>

using namespace coop::vote;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static std::vector<Member> Roster() {
    return {{0, "Host", true}, {1, "Ann", true}, {2, "Bob", true}, {3, "Cid", false}};
}

static void WhoVotes() {
    // The host's travel: every connected guest (Cid's slot is kept but lost: no vote).
    std::vector<Voter> v = Voters(Roster(), 0);
    CHECK(v.size() == 2 && v[0].slot == 1 && v[1].slot == 2 && v[0].name == "Ann");
    // A guest's travel: the host and the other guests; the starter's YES counts.
    v = Voters(Roster(), 2);
    CHECK(v.size() == 2 && v[0].slot == 0 && v[1].slot == 1);
    // No name: "slot N".
    v = Voters({{0, "", true}, {4, "", true}}, 0);
    CHECK(v.size() == 1 && v[0].name == "slot 4");
    // The rule: off, or nobody else -> the travel runs at once (as before the rule).
    CHECK(!Needed(false, Voters(Roster(), 0)));
    CHECK(Needed(true, Voters(Roster(), 0)));
    CHECK(!Needed(true, Voters({{0, "Host", true}}, 0)));
    CHECK(!Needed(true, Voters({{0, "Host", true}, {1, "Ann", false}}, 0)));
}

static void Outcomes() {
    std::vector<Voter> v = Voters(Roster(), 0);
    CHECK(Decide(v, 0) == Outcome::Waiting);
    CHECK(Record(&v, 1, Reply::Yes));
    CHECK(!Record(&v, 1, Reply::No));   // the first answer counts
    CHECK(!Record(&v, 3, Reply::Yes));  // not a voter
    CHECK(Decide(v, 5) == Outcome::Waiting);
    CHECK(Decide(v, kTimeoutS) == Outcome::TimedOut);
    CHECK(Decide(v, 19.9) == Outcome::Waiting);
    CHECK(kTimeoutS == 20.0);
    {   // all YES: go
        std::vector<Voter> w = v;
        CHECK(Record(&w, 2, Reply::Yes) && Decide(w, 1) == Outcome::Go);
        CHECK(Decide(w, 100) == Outcome::Go);  // answered in time; late ticks do not turn it into a timeout
    }
    {   // one NO ends it at once, also with others still silent
        std::vector<Voter> w = Voters(Roster(), 0);
        CHECK(Record(&w, 2, Reply::No) && Decide(w, 1) == Outcome::Declined);
        CHECK(CancelText(w) == "Party travel cancelled: Bob said no; Ann did not answer");
    }
    {   // the question could not be shown: declined, named
        std::vector<Voter> w = Voters(Roster(), 0);
        Record(&w, 1, Reply::Unable);
        Record(&w, 2, Reply::Yes);
        CHECK(Decide(w, 1) == Outcome::Declined);
        CHECK(CancelText(w) == "Party travel cancelled: Ann could not answer");
    }
    {   // the timeout names who did not answer
        std::vector<Voter> w = Voters(Roster(), 0);
        Record(&w, 1, Reply::Yes);
        CHECK(Decide(w, 20.5) == Outcome::TimedOut);
        CHECK(CancelText(w) == "Party travel cancelled: Bob did not answer");
        std::vector<Voter> x = Voters(Roster(), 0);
        CHECK(CancelText(x) == "Party travel cancelled: Ann, Bob did not answer");
    }
    {   // a voter who leaves no longer counts: the others' YES carries it
        std::vector<Voter> w = Voters(Roster(), 0);
        Record(&w, 1, Reply::Yes);
        DropVoter(&w, 2);
        CHECK(w.size() == 1 && Decide(w, 1) == Outcome::Go);
        DropVoter(&w, 7);  // not there: nothing
        CHECK(w.size() == 1);
        DropVoter(&w, 1);
        CHECK(w.empty() && Decide(w, 0) == Outcome::Go);  // nobody left: as without the rule
    }
    CHECK(WaitingText(1) == "Waiting for the party's vote (1 player)");
    CHECK(WaitingText(3) == "Waiting for the party's vote (3 players)");
}

static void Wire() {
    Ask a;
    a.round = 7;
    a.lamp = 2402950;
    a.starter = 1;
    a.who = "Ann \"the\" \\Hunter";
    a.t = 123456789012LL;
    Ask b;
    CHECK(DecodeAsk(EncodeAsk(a), &b));
    CHECK(b.round == 7 && b.lamp == 2402950 && b.starter == 1 && b.t == 123456789012LL);
    CHECK(b.who == "Ann 'the' 'Hunter");  // quotes and backslashes never break the event
    CHECK(!DecodeAsk("{\"op\":\"ask\",\"round\":0,\"lamp\":1,\"by\":0}", &b));
    CHECK(!DecodeAsk("{\"op\":\"ask\",\"round\":1,\"lamp\":0,\"by\":0}", &b));
    CHECK(!DecodeAsk("{\"op\":\"answer\",\"round\":1,\"reply\":\"yes\"}", &b));
    for (Reply r : {Reply::Yes, Reply::No, Reply::Unable}) {
        Answer x{3, r}, y;
        CHECK(DecodeAnswer(EncodeAnswer(x), &y) && y.round == 3 && y.reply == r);
    }
    Answer y;
    CHECK(!DecodeAnswer("{\"op\":\"answer\",\"round\":3,\"reply\":\"maybe\"}", &y));
    CHECK(!DecodeAnswer("{\"op\":\"answer\",\"round\":0,\"reply\":\"yes\"}", &y));
    End e{9, false, "Party travel cancelled: Bob said no"}, f;
    CHECK(DecodeEnd(EncodeEnd(e), &f) && f.round == 9 && !f.go && f.text == e.text);
    e.go = true;
    CHECK(DecodeEnd(EncodeEnd(e), &f) && f.go);
    CHECK(!DecodeEnd(EncodeAsk(a), &f));
    CHECK(Clean(std::string("a\nb\x01" "c")) == "abc");
}

static void TestAid() {
    CHECK(TestAnswer("", 1).empty());
    CHECK(TestAnswer("yes,no,none", 1) == "yes");
    CHECK(TestAnswer("yes,no,none", 2) == "no");
    CHECK(TestAnswer("yes,no,none", 3) == "none");
    CHECK(TestAnswer("yes,no,none", 9) == "none");  // the last one repeats
    CHECK(TestAnswer("yes", 0).empty());
}

int main() {
    WhoVotes();
    Outcomes();
    Wire();
    TestAid();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-vote-test: all passed\n");
    return 0;
}
