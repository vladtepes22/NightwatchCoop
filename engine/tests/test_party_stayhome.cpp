// SPDX-License-Identifier: GPL-3.0-or-later
// "Stay in my world" (CL-adeb50): the wire, when a guest reports, the host's expiring record
// (gpu/shim/party/party_stayhome_rules.h), and the vote that never waits for a staying guest.
// Build and run: ninja -C out/gpu party-stayhome-test && out/gpu/party-stayhome-test.exe
#include "party/party_stayhome_rules.h"
#include "party/party_vote_rules.h"

#include <cstdio>

using namespace coop::stayhome;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static void Wire() {
    bool on = false;
    CHECK(Decode(Encode(true), &on) && on);
    CHECK(Decode(Encode(false), &on) && !on);
    CHECK(!Decode("", &on));
    CHECK(!Decode("{\"on\":2}", &on));
    CHECK(!Decode("{\"x\":1}", &on));
}

static void Reporting() {
    // Off and never said anything: silent (the host's default is "not staying").
    CHECK(!ShouldReport(false, false, false, 100));
    // First "on": at once; then every kReportEveryS s.
    CHECK(ShouldReport(true, false, false, 0));
    CHECK(!ShouldReport(true, true, true, 1.0));
    CHECK(ShouldReport(true, true, true, kReportEveryS));
    // Switched off after it was on: said once, then silent.
    CHECK(ShouldReport(false, true, true, 0.1));
    CHECK(!ShouldReport(false, false, true, 100));
    // Switched on again at once.
    CHECK(ShouldReport(true, false, true, 0.1));
}

static void HostBook() {
    Book b;
    CHECK(!b.Stays(1, 0));
    b.Set(1, true, 10);
    CHECK(b.Stays(1, 10) && b.Stays(1, 10 + kExpireS) && !b.Stays(1, 10 + kExpireS + 0.1));
    CHECK(!b.Stays(2, 10));
    b.Set(2, true, 12);
    CHECK(b.Count(13) == 2);
    b.Set(1, false, 14);  // "off" clears at once
    CHECK(!b.Stays(1, 14) && b.Stays(2, 14) && b.Count(14) == 1);
    // A keepalive extends it
    b.Set(2, true, 20);
    CHECK(b.Stays(2, 20 + kExpireS - 1));
    b.Forget(2);
    CHECK(!b.Stays(2, 21));
    b.Set(3, true, 1);
    b.Clear();
    CHECK(b.Count(1) == 0);
}

// The party travel vote never waits for a staying guest: the host builds the voters without it, so with only
// staying guests the travel runs at once, and with another guest it needs that guest's answer alone.
static void VoteSkipsStayers() {
    namespace v = coop::vote;
    Book b;
    b.Set(2, true, 0);
    std::vector<v::Member> roster;
    const struct { int slot; const char* name; } all[] = {{0, "Host"}, {1, "Ann"}, {2, "Bob"}};
    for (const auto& m : all) {
        if (m.slot != 0 && b.Stays(m.slot, 1)) continue;  // party_vote.cpp HostTick
        roster.push_back({m.slot, m.name, true});
    }
    std::vector<v::Voter> voters = v::Voters(roster, 0);
    CHECK(voters.size() == 1 && voters[0].slot == 1);
    CHECK(v::Decide(voters, 0.0) == v::Outcome::Waiting);
    v::Record(&voters, 1, v::Reply::Yes);
    CHECK(v::Decide(voters, 0.1) == v::Outcome::Go);
    // Bob alone staying: nobody left to ask, the travel runs.
    b.Set(1, true, 0);
    roster.clear();
    for (const auto& m : all) {
        if (m.slot != 0 && b.Stays(m.slot, 1)) continue;
        roster.push_back({m.slot, m.name, true});
    }
    CHECK(v::Voters(roster, 0).empty() && !v::Needed(true, v::Voters(roster, 0)));
}

int main() {
    Wire();
    Reporting();
    HostBook();
    VoteSkipsStayers();
    if (g_failures) {
        std::fprintf(stderr, "party-stayhome-test: %d failures\n", g_failures);
        return 1;
    }
    std::printf("party-stayhome-test: ok\n");
    return 0;
}
