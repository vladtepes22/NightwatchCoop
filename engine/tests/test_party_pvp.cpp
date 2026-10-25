// SPDX-License-Identifier: GPL-3.0-or-later
// I1 friendly fire / PvP: the pure rules (party_pvp_rules.h) and the party rule's JSON
// (party_balance_rules.h friendly_fire). cmake --build out/gpu --target party-pvp-test
#include "party/party_pvp_rules.h"
#include "party/party_balance_rules.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    using namespace coop::pvp;

    // Only in a party session: a host with cooperators, a guest in the host's world.
    assert(!PartySession(3, 0));   // host alone
    assert(PartySession(3, 1));
    assert(PartySession(6, -1));   // client
    assert(!PartySession(0, 2));   // idle (own world)
    assert(!PartySession(4, 0));   // trying to join

    // The host's setting counts; a guest's own never does, only the host's rule once received.
    assert(RuleInForce(true, true, false, false));
    assert(!RuleInForce(true, false, true, true));
    assert(!RuleInForce(false, true, false, false));  // guest, own setting on, nothing received
    assert(!RuleInForce(false, true, true, false));   // guest, host says off
    assert(RuleInForce(false, false, true, true));    // guest, host says on

    // Team pairs: host 1 + cooperator 2 -> 1->2 and 2->1; two guests share team 2 -> 2->2 too.
    {
        const auto p = TeamPairs({1, 2});
        assert((p == std::vector<std::pair<int, int>>{{1, 2}, {2, 1}}));
        const auto q = TeamPairs({1, 2, 2});
        assert((q == std::vector<std::pair<int, int>>{{1, 2}, {2, 1}, {2, 2}}));
        // A guest's view: its own team first, the host's second - the same cells.
        assert(TeamPairs({2, 1}) == p);
        // Unread / out of range teams are skipped; one chr opens nothing.
        assert(TeamPairs({1, 0}).empty());
        assert(TeamPairs({1, 40}).empty());
        assert(TeamPairs({1}).empty());
        assert(TeamPairs({}).empty());
    }

    // Toast texts.
    assert(std::string(ToastText(true)).find("ON") != std::string::npos);
    assert(std::string(ToastText(false)) == "Friendly fire OFF");

    // Test steps.
    {
        std::vector<TestStep> st;
        std::string bad;
        assert(ParseTestSteps("log@5,face@20,hit=3@1,on@10,hp=40@2,off@3", &st, &bad));
        assert(st.size() == 6);
        assert(st[0].kind == TestStep::Kind::Log && st[0].delay == 5);
        assert(st[1].kind == TestStep::Kind::Face && st[1].delay == 20);
        assert(st[2].kind == TestStep::Kind::Hit && st[2].arg == 3 && st[2].delay == 1);
        assert(st[3].kind == TestStep::Kind::On);
        assert(st[4].kind == TestStep::Kind::Hp && st[4].arg == 40);
        assert(st[5].kind == TestStep::Kind::Off && st[5].delay == 3);
        assert(ParseTestSteps("hit@0", &st) && st.size() == 1 && st[0].arg == 1);
        assert(ParseTestSteps(nullptr, &st) && st.empty());
        assert(ParseTestSteps("", &st) && st.empty());
        assert(!ParseTestSteps("jump@1", &st, &bad) && bad == "jump@1");
        assert(!ParseTestSteps("hit=0@1", &st, &bad));
        assert(!ParseTestSteps("on@x", &st, &bad));
        assert(!ParseTestSteps("hp@1", &st, &bad));
        // Guest-guest (face a team), bullets, a quick item (thrown knife).
        assert(ParseTestSteps("face=20@2,shoot=2@1,item=1060@1,use=3@1,face@1", &st, &bad));
        assert(st.size() == 5);
        assert(st[0].kind == TestStep::Kind::Face && st[0].arg == 20);
        assert(st[1].kind == TestStep::Kind::Shoot && st[1].arg == 2);
        assert(st[2].kind == TestStep::Kind::Item && st[2].arg == 1060);
        assert(st[3].kind == TestStep::Kind::Use && st[3].arg == 3);
        assert(st[4].kind == TestStep::Kind::Face && st[4].arg == 0);
        assert(!ParseTestSteps("shoot=0@1", &st, &bad));
        assert(!ParseTestSteps("face=40@1", &st, &bad));
        assert(!ParseTestSteps("item@1", &st, &bad));
    }

    // The party rule travels in the "balance" event: off by default, round trip, older hosts
    // (no member) mean off.
    {
        using namespace coop::balance;
        Rules r;
        assert(!r.friendly_fire && !Vanilla().friendly_fire);
        r.friendly_fire = true;
        Rules back;
        assert(FromJson(ToJson(r), &back) && back.friendly_fire && back == r);
        Rules old;
        assert(FromJson("{\"rot\":true,\"boss_hp\":150}", &old) && !old.friendly_fire);
        Rules wrong;
        assert(FromJson("{\"friendly_fire\":1}", &wrong) && !wrong.friendly_fire);  // not a bool: ignored
        Rules a, b;
        b.friendly_fire = true;
        assert(a != b);
    }

    // pvpfix: dead hit bodies on a living chr.
    assert(coop::pvp::DeadHitBodiesOnLiving(719, 0x01000036));
    assert(!coop::pvp::DeadHitBodiesOnLiving(719, 0x01000008));
    assert(!coop::pvp::DeadHitBodiesOnLiving(0, 0x01000036));  // really dead

    std::puts("party-pvp-test: ok");
    return 0;
}
