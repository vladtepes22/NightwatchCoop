// SPDX-License-Identifier: GPL-3.0-or-later
// Invasions into open public games (0.7.8, CL-4cc63c): the allow / deny table, the rate limit, the block book, how an
// invasion ends and the wire (gpu/shim/party/party_invade_rules.h).
// Build and run: ninja -C out/gpu party-invade-test && out/gpu/party-invade-test.exe
#include <cmath>
#include "party/party_invade_rules.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace coop::invade;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static HostView Good() {
    HostView h;
    h.allow = true;
    h.world_players = 2;  // the host and one guest
    return h;
}

static void AllowDenyTable() {
    CHECK(Admit(Good(), false, false) == Deny::None);
    // The host's switch decides first, whatever else is true.
    HostView h = Good();
    h.allow = false;
    CHECK(Admit(h, false, false) == Deny::Off);
    CHECK(Admit(h, true, true, true) == Deny::Off);
    struct Row {
        const char* what;
        void (*set)(HostView&);
        Deny want;
    };
    const Row rows[] = {
        {"host stays", [](HostView& v) { v.host_stays = true; }, Deny::HostStays},
        {"no world", [](HostView& v) { v.world_up = false; }, Deny::NotInWorld},
        {"prologue", [](HostView& v) { v.prologue = true; }, Deny::NotInWorld},
        {"loading", [](HostView& v) { v.loading = true; }, Deny::Loading},
        {"cutscene", [](HostView& v) { v.cutscene = true; }, Deny::Cutscene},
        {"boss fog", [](HostView& v) { v.boss_fight = true; }, Deny::BossFight},
        {"host dead", [](HostView& v) { v.alive = false; }, Deny::HostDead},
        {"travelling", [](HostView& v) { v.travelling = true; }, Deny::Travelling},
        {"an invader in", [](HostView& v) { v.invaders = 1; }, Deny::InvaderPresent},
        {"four in the world", [](HostView& v) { v.world_players = 4; }, Deny::NoSlot},
    };
    for (const Row& r : rows) {
        HostView v = Good();
        r.set(v);
        const Deny d = Admit(v, false, false);
        if (d != r.want) std::fprintf(stderr, "row '%s': got %s\n", r.what, DenyWord(d));
        CHECK(d == r.want);
        CHECK(!DenyText(d).empty());
    }
    // The invader's own state and the books come before the host's moment.
    HostView v = Good();
    v.boss_fight = true;
    CHECK(Admit(v, true, false) == Deny::Blocked);
    CHECK(Admit(v, false, true) == Deny::RateLimited);
    CHECK(Admit(v, true, true) == Deny::Blocked);
    CHECK(Admit(v, false, false, true) == Deny::InvaderStays);
    CHECK(Admit(v, false, false) == Deny::BossFight);
    CHECK(DenyText(Deny::None).empty());
}

static void FourPlayers() {
    // Host + two guests = 3 in the world: the fourth slot is the invader's.
    HostView h = Good();
    h.world_players = 3;
    CHECK(Admit(h, false, false) == Deny::None);
    h.world_players = 4;  // no invader slot
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    // 0.8.0 (CL-10e56b): a party of 5 (the link's max) has the engine's five seats: host + 3 guests leave one for an invader,
    // host + 4 guests none; nothing is ever more than five.
    h.party_max = 5;
    CHECK(Capacity(5) == 5 && Capacity(6) == 6 && Capacity(9) == 6 && Capacity(4) == 4 && kEngineSeats == 6);
    CHECK(Admit(h, false, false) == Deny::None);  // world_players 4 (host + 3), party of 5
    h.world_players = 5;
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    // two invaders in a party of 5: host + 2 guests + 2 invaders fills it, host + 3 guests + 1 invader too
    h.max_invaders = 2, h.world_players = 3, h.invaders = 1;
    CHECK(Admit(h, false, false) == Deny::None);
    h.world_players = 4, h.invaders = 1;
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    CHECK(Invadable(true, 4, 0, 5) && !Invadable(true, 5, 0, 5) && !Invadable(true, 4, 0, 4));
    h = Good();
    // A party set to two players: host + guest fill it.
    h = Good();
    h.party_max = 2;
    CHECK(Capacity(2) == 2);
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    h.world_players = 1;
    CHECK(Admit(h, false, false) == Deny::None);
    // An invader already in blocks the next one before the slot count does.
    h = Good();
    h.invaders = 1;
    CHECK(Admit(h, false, false) == Deny::InvaderPresent);
}

static void ListedAsInvadable() {
    CHECK(Invadable(true, 1, 0, 4));
    CHECK(!Invadable(false, 1, 0, 4));  // off: never listed
    CHECK(!Invadable(true, 4, 0, 4));   // full
    CHECK(!Invadable(true, 3, 1, 4));   // an invader in already
    CHECK(Invadable(true, 3, 0, 4));
    CHECK(!Invadable(true, 2, 0, 2));
}

static void RateLimit() {
    RateBook b;
    CHECK(b.Allow("a", 0));
    CHECK(b.Allow("a", 10));
    CHECK(b.Allow("a", 20));
    CHECK(b.Limited("a", 30));
    CHECK(!b.Allow("a", 30));  // the 4th inside the window
    CHECK(b.Allow("b", 30));   // another address is its own key
    CHECK(!b.Allow("a", kRateWindowS - 1));
    CHECK(b.Allow("a", kRateWindowS + 1));  // the first aged out
    b.Clear();
    CHECK(!b.Limited("a", 0));
    // A refused attempt is not recorded: waiting for the oldest to age out is enough.
    RateBook c;
    for (int i = 0; i < kRateMax; ++i) CHECK(c.Allow("k", i));
    for (int i = 0; i < 20; ++i) CHECK(!c.Allow("k", 100 + i));
    CHECK(c.Allow("k", kRateWindowS + 1));
    // A different limit per book: knocking is looser than starting invasions.
    RateBook e;
    for (int i = 0; i < kSpamMax; ++i) CHECK(e.Allow("k", i, kSpamMax));
    CHECK(!e.Allow("k", 50, kSpamMax) && e.Limited("k", 50, kSpamMax) && !e.Limited("other", 50, kSpamMax));
    // A flood of keys does not grow the book without bound.
    RateBook d;
    for (int i = 0; i < 2000; ++i) d.Allow("k" + std::to_string(i), i * 1000.0);
    CHECK(d.Allow("fresh", 3e6));
}

static void Blocking() {
    BlockBook k;
    CHECK(!k.Blocked("Bad", "10.0.0.5"));
    k.Block("Bad", "10.0.0.5");
    CHECK(k.Blocked("Bad", "10.0.0.9"));    // the name changes address
    CHECK(k.Blocked("Other", "10.0.0.5"));  // the address changes name
    CHECK(!k.Blocked("Other", "10.0.0.6"));
    CHECK(!k.Blocked("", ""));
    k.Block("", "");
    CHECK(k.Size() == 1);
    k.Clear();
    CHECK(!k.Blocked("Bad", "10.0.0.5"));
}

static void Ending() {
    Live l;
    CHECK(CheckEnd(l) == End::None);
    l.invader_alive = false;
    CHECK(CheckEnd(l) == End::InvaderDied);
    l.host_alive = false;  // both: the host's death wins (the invader's side)
    CHECK(CheckEnd(l) == End::HostDied);
    l = Live{};
    l.host_alive = false;
    CHECK(CheckEnd(l) == End::HostDied);
    l = Live{};
    l.host_loading = true;
    CHECK(CheckEnd(l) == End::Travel);
    l = Live{};
    l.boss_fight = true;
    CHECK(CheckEnd(l) == End::BossFight);
    l = Live{};
    l.allow = false;
    CHECK(CheckEnd(l) == End::Disabled);
    l.blocked = true;
    CHECK(CheckEnd(l) == End::Blocked);
    l = Live{};
    l.link_up = false;
    CHECK(CheckEnd(l) == End::HostGone);
    // Pending: only the timeout ends it; the host's world events do not count before the invader is in.
    l = Live{};
    l.in_world = false;
    l.pending_s = 5;
    l.host_loading = true;
    CHECK(CheckEnd(l) == End::None);
    l.pending_s = kPendingMaxS + 1;
    CHECK(CheckEnd(l) == End::Timeout);
    for (int i = 1; i <= static_cast<int>(End::LevelRange); ++i) {
        const End e = static_cast<End>(i);
        End back = End::None;
        CHECK(ParseEnd(EndWord(e), &back) && back == e);
        CHECK(!EndTextForHost(e, "Bob").empty());
        CHECK(!EndTextForInvader(e, "Host").empty());
    }
    End junk = End::None;
    CHECK(!ParseEnd("nope", &junk));
    CHECK(EndTextForHost(End::None, "x").empty());
}

static void Wire() {
    Msg m;
    CHECK(Decode(Encode({2, "Bob_1", End::None}), &m) && m.state == 2 && m.name == "Bob_1" && m.why == End::None);
    CHECK(Decode(Encode({1, "Bob", End::None}), &m) && m.state == 1);
    CHECK(Decode(Encode({0, "Bob", End::HostDied}), &m) && m.state == 0 && m.why == End::HostDied);
    CHECK(Decode(Encode({0, "Bob", End::None}), &m) && m.state == 0 && m.why == End::None);
    // 0.7.9: the host's phantom health for the invasion ("h"); a 0.7.8 message without it reads as -1
    {
        Msg h{2, "Bob", End::None};
        h.full_hp = 1;
        CHECK(Encode(h) == "{\"s\":2,\"n\":\"Bob\",\"h\":1}");
        CHECK(Decode(Encode(h), &m) && m.state == 2 && m.full_hp == 1);
        h.state = 1, h.full_hp = 0;
        CHECK(Decode(Encode(h), &m) && m.state == 1 && m.full_hp == 0);
        CHECK(Decode("{\"s\":2,\"n\":\"Bob\"}", &m) && m.full_hp == -1);
        CHECK(!Decode("{\"s\":2,\"n\":\"Bob\",\"h\":2}", &m));
        CHECK(!Decode("{\"s\":0,\"n\":\"Bob\",\"h\":1}", &m));  // only pending / in carry it
        h.state = 0, h.why = End::HostDied, h.full_hp = 1;
        CHECK(Encode(h) == "{\"s\":0,\"n\":\"Bob\",\"w\":\"host_died\"}");
    }
    CHECK(!Decode("", &m));
    CHECK(!Decode("{\"s\":3,\"n\":\"Bob\"}", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"\"}", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"Bob Smith\"}", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"Bob\"} x", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"Bob\",\"w\":\"nope\"}", &m));
    CHECK(!Decode("{\"s\":1,\"n\":\"Bob\",\"w\":\"host_died\"}", &m));  // an end word only with state 0
    CHECK(!Decode("{\"s\":2,\"n\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}", &m));
    CHECK(NameOk("a-b_9") && !NameOk("a b") && !NameOk(""));
    CHECK(RefusedText(DenyText(Deny::BossFight)) == "invasion refused: a boss fight is on in the host's world");
}

static void Script() {
    std::vector<TestStep> st;
    std::string bad;
    CHECK(ParseTest("allow=0@2,view=boss@50,rejoin@20?home,die@45?in,die@40?in#2,view=none@1", &st, &bad));
    CHECK(st.size() == 6);
    CHECK(st[0].kind == TestStep::Kind::Allow && st[0].arg == 0 && st[0].delay == 2);
    CHECK(st[1].kind == TestStep::Kind::View && st[1].word == "boss");
    CHECK(st[2].kind == TestStep::Kind::Rejoin && st[2].cond == 2 && st[2].delay == 20);
    CHECK(st[3].kind == TestStep::Kind::Die && st[3].cond == 1 && st[3].nth == 0);
    CHECK(st[4].cond == 1 && st[4].nth == 2);
    CHECK(ParseTest("die@5?other,rejoin@3?other#2", &st, &bad) && st.size() == 2 && st[0].cond == 3 && st[0].delay == 5 && st[1].nth == 2);
    CHECK(ParseTest(nullptr, &st) && st.empty());
    CHECK(ParseTest("", &st) && st.empty());
    for (const char* b : {"allow=2", "view=maybe", "rejoin=1", "die#2", "die?out", "jump@1", "die@x", "die@-1", "die?in#0", "die?in#21"})
        CHECK(!ParseTest(b, &st, &bad));
    CHECK(bad == "die?in#21");
}


static void Look() {
    CHECK(kCoopLookEffect == 9006 && kInvaderLookEffect == 9026);
    // a remote copy of the invader: a cooperator (9006) -> swap
    LookStep a = LookNow(true, false);
    CHECK(a.remove_coop && a.add_invader);
    // its own chr: only 9026
    a = LookNow(false, false);
    CHECK(!a.remove_coop && a.add_invader);
    // already done: nothing
    a = LookNow(false, true);
    CHECK(!a.remove_coop && !a.add_invader);
    // the end puts back only what was changed
    LookBack b = LookEnd(true, true, true, false);
    CHECK(b.remove_invader && b.add_coop);
    b = LookEnd(true, false, true, false);  // the invader's own game: no 9006 to put back
    CHECK(b.remove_invader && !b.add_coop);
    b = LookEnd(false, false, true, true);  // another mod's 9026: left alone
    CHECK(!b.remove_invader && !b.add_coop);
    b = LookEnd(true, true, false, true);  // the game already did it
    CHECK(!b.remove_invader && !b.add_coop);
}

static void NameColour() {
    const float blue[3] = {0.1f, 0.2f, 0.9f};
    CHECK(IsInvaderName("Hunter2", "Hunter2"));
    CHECK(!IsInvaderName("Hunter1", "Hunter2"));
    CHECK(!IsInvaderName("Hunter1", ""));  // no invasion: nobody is red
    CHECK(!IsInvaderName("", ""));
    CHECK(NameRgb("Hunter2", "Hunter2", blue) == kInvaderNameRgb);
    CHECK(NameRgb("Hunter1", "Hunter2", blue) == blue);
    CHECK(NameRgb("Hunter2", "", blue) == blue);
    CHECK(kInvaderNameRgb[0] > 0.8f && kInvaderNameRgb[1] < 0.3f && kInvaderNameRgb[2] < 0.3f);
}

static void Rewards() {
    const Reward w = RewardFor(Role::Invader, End::HostDied);
    CHECK(w.pay && w.rate == 0.1f && w.insight == 1);
    const Reward h = RewardFor(Role::Host, End::InvaderDied);
    CHECK(h.pay && h.rate == 0.5f && h.insight == 0);
    const Reward g = RewardFor(Role::Guest, End::InvaderDied);
    CHECK(g.pay && g.rate == 0.25f && g.insight == 0);
    // every other pairing pays nothing
    for (End e : {End::None, End::InvaderDied, End::HostDied, End::Travel, End::BossFight, End::Disabled, End::Blocked, End::HostGone,
                  End::Timeout, End::LevelRange})
        for (Role r : {Role::Host, Role::Guest, Role::Invader}) {
            const bool paid = (r == Role::Invader && e == End::HostDied) || (r != Role::Invader && e == End::InvaderDied);
            CHECK(RewardFor(r, e).pay == paid);
        }
    // the banners: the invader that won reads TargetClear (5), the host and guests that won BlackClear (7), nobody else a banner
    CHECK(w.banner == 5 && h.banner == 7 && g.banner == 7);
    for (End e : {End::None, End::InvaderDied, End::HostDied, End::Travel, End::BossFight, End::Disabled, End::Blocked, End::HostGone,
                  End::Timeout, End::LevelRange})
        for (Role r : {Role::Host, Role::Guest, Role::Invader})
            CHECK((RewardFor(r, e).banner != 0) == RewardFor(r, e).pay);
    // an invader that lost / a host that fell is paid nothing, a guest is not paid when the host fell
    CHECK(!RewardFor(Role::Invader, End::InvaderDied).pay);
    CHECK(!RewardFor(Role::Host, End::HostDied).pay);
    CHECK(!RewardFor(Role::Guest, End::HostDied).pay);
    // once per outcome
    RewardOnce once;
    CHECK(once.Take("A", End::InvaderDied, 10.0));
    CHECK(!once.Take("A", End::InvaderDied, 11.0));
    CHECK(once.Take("A", End::HostDied, 12.0));
    CHECK(once.Take("B", End::HostDied, 13.0));
    CHECK(!once.Take("B", End::HostDied, 14.0));
    CHECK(once.Take("B", End::HostDied, 14.0 + kRewardOnceS + 1));  // a later invasion by the same name
}

// 0.7.9: the level range, both bounds rounded down: lower floor(X*0.8-20) (clamped at 1), upper floor(X*1.1+10).
static void LevelRange() {
    CHECK(LevelLower(100) == 60 && LevelUpper(100) == 120);
    CHECK(LevelLower(55) == 24 && LevelUpper(55) == 70);
    CHECK(LevelLower(4) == 1 && LevelUpper(4) == 14);
    CHECK(LevelLower(1) == 1 && LevelUpper(1) == 11);
    CHECK(LevelLower(25) == 1 && LevelLower(26) == 1 && LevelLower(30) == 4);
    CHECK(LevelLower(101) == 60 && LevelUpper(101) == 121);  // 60.8 and 121.1 round down
    CHECK(LevelLower(999) == 779 && LevelUpper(999) == 1108);
    // brute force against the float formula the user gave (the .1 offsets keep the edges exact)
    for (int x = 1; x <= 999; ++x) {
        CHECK(LevelUpper(x) == static_cast<int>(std::floor(x * 1.1 + 10 + 1e-9)));
        CHECK(LevelLower(x) == std::max(1, static_cast<int>(std::floor(x * 0.8 - 20 + 1e-9))));
    }
    CHECK(LevelOk(100, 60) && LevelOk(100, 120) && !LevelOk(100, 59) && !LevelOk(100, 121));
    CHECK(LevelOk(4, 1) && LevelOk(4, 14) && !LevelOk(4, 15));
    CHECK(LevelOk(0, 500) && LevelOk(100, 0));  // unknown: not checked
    CHECK(LevelText("Bob", 100, 150) == "Can't invade Bob: your level 150 is outside their range (60-120)");
    CHECK(LevelText("Bob", 100, 150, false, LevelLower(100), OpenLevelUpper(100)) ==
          "Can't join Bob: your level 150 is outside their range (60-140)");
    // the open-party join range: floor both, vanilla 1.2 * X + 20 upper
    CHECK(LevelLower(100) == 60 && OpenLevelUpper(100) == 140);
    CHECK(LevelLower(4) == 1 && OpenLevelUpper(4) == 24);
    CHECK(LevelLower(55) == 24 && OpenLevelUpper(55) == 86);
    for (int x = 1; x <= 999; ++x) CHECK(OpenLevelUpper(x) == static_cast<int>(std::floor(x * 1.2 + 20 + 1e-9)));
    CHECK(OpenLevelOk(100, 60) && OpenLevelOk(100, 140) && !OpenLevelOk(100, 59) && !OpenLevelOk(100, 141));
    CHECK(OpenLevelOk(0, 999) && OpenLevelOk(100, 0));
    // 0.7.9 "Any level can join": the toggle skips the open range (both ends); off is the same as before
    CHECK(OpenLevelOk(100, 59, true) && OpenLevelOk(100, 141, true) && OpenLevelOk(100, 999, true) && OpenLevelOk(100, 1, true));
    CHECK(!OpenLevelOk(100, 59, false) && !OpenLevelOk(100, 141, false) && OpenLevelOk(100, 100, false));
    // an invader keeps the invasion range whatever the toggle: LevelOk / Admit take no such switch
    CHECK(!LevelOk(100, 150) && LevelOk(100, 120) && !LevelOk(100, 59));
    HostView h = Good();
    h.host_level = 100;
    CHECK(Admit(h, false, false, false, 150) == Deny::LevelRange);
    CHECK(Admit(h, false, false, false, 100) == Deny::None);
    CHECK(Admit(h, false, false, false, 0) == Deny::None);
    // the level refusal comes before the rate limit and the knock counter, after the switch and the block list
    CHECK(Admit(h, false, true, false, 150) == Deny::LevelRange);
    CHECK(Admit(h, true, false, false, 150) == Deny::Blocked);
    h.allow = false;
    CHECK(Admit(h, false, false, false, 150) == Deny::Off);
    CHECK(!DenyText(Deny::LevelRange).empty() && std::string(DenyWord(Deny::LevelRange)) == "level_range");
}

void Apart() {
    const float host[3] = {0, 0, 0};
    std::vector<ApartSpot> sp = {{2302950, 10, 0, 0}, {2302951, 0, 0, 45}, {2302952, 100, 5, 100}, {2302953, 400, 0, 0}};
    float d = 0;
    CHECK(PickApart(host, sp, kApartMinM, kApartMaxM, &d) == 2302951 && d > 44.9f && d < 45.1f);  // 10 m is too near, 45 m the nearest
    sp.erase(sp.begin() + 1);
    CHECK(PickApart(host, sp) == 2302952);       // 141 m
    sp.erase(sp.begin() + 1);
    CHECK(PickApart(host, sp) == 0);             // 10 m too near, 400 m too far: none, the invader stays beside the host
    CHECK(PickApart(host, {}) == 0);
    CHECK(PickApart(host, {{0, 50, 0, 0}}) == 0);  // id 0 is not a lamp
    // the ground plane decides, not the height
    CHECK(PickApart(host, {{2302950, 5, 80, 5}}) == 0);
    CHECK(PickApart(host, {{2302950, 20, 80, 20}}) == 0);  // 28 m: too near
    CHECK(PickApart(host, {{2302950, 25, 0, 25}}) == 2302950);   // 35 m
    // ties: the lower id
    CHECK(PickApart(host, {{2302951, 40, 0, 0}, {2302950, 0, 0, 40}}) == 2302950);
    // the wire carries it (state 1 / 2 only, with the health byte)
    Msg m;
    Msg w{2, "Bob", End::None, 1, 2302951};
    CHECK(Encode(w) == "{\"s\":2,\"n\":\"Bob\",\"h\":1,\"a\":2302951}");
    CHECK(Decode(Encode(w), &m) && m.state == 2 && m.full_hp == 1 && m.apart == 2302951);
    CHECK(Decode("{\"s\":1,\"n\":\"Bob\",\"h\":0}", &m) && m.apart == 0);  // a 0.7.9 host without it
    CHECK(!Decode("{\"s\":1,\"n\":\"Bob\",\"h\":0,\"a\":0}", &m));
    CHECK(!Decode("{\"s\":0,\"n\":\"Bob\",\"h\":0,\"a\":5}", &m));
}

// 0.7.9 (CL-7e748c): Max invaders 1 or 2.
void TwoInvaders() {
    CHECK(ClampMaxInvaders(0) == 1 && ClampMaxInvaders(1) == 1 && ClampMaxInvaders(2) == 2 && ClampMaxInvaders(3) == 2 && ClampMaxInvaders(-5) == 1);
    HostView h;
    h.allow = true;
    h.party_max = 4;
    h.world_players = 2;  // the host and one guest
    // the default is the old behaviour: one invader, the second is refused as "another invader is already in"
    CHECK(h.max_invaders == 1);
    CHECK(Admit(h, false, false) == Deny::None);
    h.invaders = 1;
    CHECK(Admit(h, false, false) == Deny::InvaderPresent);
    // Max invaders 2: the second one is in, the third is not
    h.max_invaders = 2;
    CHECK(Admit(h, false, false) == Deny::None);          // host + guest + invader + the new one = 4
    h.invaders = 2;
    CHECK(Admit(h, false, false) == Deny::InvaderPresent);
    // both invaders count against Max players and the four slots
    h.invaders = 1, h.party_max = 3;                       // host + guest + invader = 3 = Max players: no room for the second
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    h.party_max = 4, h.world_players = 3;                  // host + 2 guests + invader = 4 slots: none left
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    h.world_players = 1, h.invaders = 1;                   // host alone + invader: the second fits (3 of 4)
    CHECK(Admit(h, false, false) == Deny::None);
    h.invaders = 2;
    CHECK(Admit(h, false, false) == Deny::InvaderPresent);
    // a setting out of range is clamped, never more than two
    h.max_invaders = 9, h.invaders = 2;
    CHECK(Admit(h, false, false) == Deny::InvaderPresent);
    // the level range applies to each: the same refusal for the second
    h.max_invaders = 2, h.invaders = 1, h.host_level = 100;
    CHECK(Admit(h, false, false, false, 150) == Deny::LevelRange);
    CHECK(Admit(h, false, false, false, 100) == Deny::None);
    // the list's flag: room for another invader
    CHECK(Invadable(true, 2, 0, 4, 1) && !Invadable(true, 2, 1, 4, 1) && Invadable(true, 2, 1, 4, 2) && !Invadable(true, 2, 2, 4, 2));
    CHECK(!Invadable(true, 2, 1, 3, 2) && !Invadable(false, 1, 0, 4, 2) && Invadable(true, 2, 0, 4));  // Max players 3 is full with one; the default is 1

    // the book each game keeps of the announced invaders
    InvaderBook b;
    CHECK(b.Empty() && b.First().empty() && b.State("A") == 0);
    CHECK(b.Apply({1, "A", End::None, 1, 0}) && b.Size() == 1 && b.State("A") == 1);
    CHECK(!b.Apply({1, "A", End::None, 0, 0}));            // the same state again: no change (the health byte is not kept here)
    CHECK(b.Apply({2, "A", End::None, 1, 2412951}) && b.State("A") == 2 && b.Apart("A") == 2412951);
    CHECK(b.Apply({1, "B", End::None, 1, 0}) && b.Size() == 2 && b.First() == "A");
    CHECK(!b.Apply({1, "C", End::None, 1, 0}) && !b.Has("C") && b.Size() == 2);   // never a third
    CHECK(b.Names().size() == 2 && b.Names()[1] == "B");
    CHECK(b.Apply({0, "A", End::InvaderDied}) && !b.Has("A") && b.First() == "B" && b.State("B") == 1);   // each ends on its own
    CHECK(!b.Apply({0, "A", End::None}));
    CHECK(b.Apply({0, "B", End::None}) && b.Empty());

    // one payout per invader: two invaders that end together are both paid, a resent one is not
    RewardOnce ro;
    CHECK(ro.Take("A", End::InvaderDied, 10.0) && ro.Take("B", End::InvaderDied, 10.5));
    CHECK(!ro.Take("A", End::InvaderDied, 11.0) && !ro.Take("B", End::InvaderDied, 11.0));
    CHECK(ro.Take("A", End::HostDied, 12.0));
    CHECK(ro.Take("A", End::InvaderDied, 10.0 + kRewardOnceS + 1.0));
}

// 0.7.9: the B1 fallback and the chr-type repair know the invade role.
void FallbackRole() {
    CHECK(FallbackPlanFor(true) == FallbackPlan::EndInvasion);
    CHECK(FallbackPlanFor(false) == FallbackPlan::ReplayTravel);
    CHECK(!ChrTypeRepairAllowed(true));
    CHECK(ChrTypeRepairAllowed(false));
}

int main() {
    FallbackRole();
    TwoInvaders();
    Apart();
    Look();
    NameColour();
    Rewards();
    Script();
    AllowDenyTable();
    FourPlayers();
    ListedAsInvadable();
    RateLimit();
    Blocking();
    Ending();
    Wire();
    LevelRange();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("party-invade-test: OK");
    return 0;
}
