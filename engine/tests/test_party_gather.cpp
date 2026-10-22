// SPDX-License-Identifier: GPL-3.0-or-later
// I5 "Gather the party": who may gather, who may come, how, the wire (gpu/shim/party/party_gather_rules.h).
// Build and run: ninja -C out/gpu party-gather-test && out/gpu/party-gather-test.exe
#include "party/party_gather_rules.h"

#include <cstdio>

using namespace coop::gather;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static HostView ReadyHost() {
    HostView v;
    v.host = true;
    v.guests_connected = 2;
    v.world_up = true;
    v.now = 1000;
    return v;
}

static void HostRules() {
    HostView v = ReadyHost();
    int wait = -1;
    CHECK(HostRefusal(v, &wait) == Refusal::None && wait == 0);
    { HostView w = v; w.enabled = false; CHECK(HostRefusal(w) == Refusal::Off); }
    { HostView w = v; w.host = false; CHECK(HostRefusal(w) == Refusal::NotHost); }
    { HostView w = v; w.loading = true; CHECK(HostRefusal(w) == Refusal::Loading); }
    { HostView w = v; w.world_up = false; CHECK(HostRefusal(w) == Refusal::Loading); }
    { HostView w = v; w.alive = false; CHECK(HostRefusal(w) == Refusal::Dead); }
    { HostView w = v; w.boss_fight = true; CHECK(HostRefusal(w) == Refusal::BossFight); }
    { HostView w = v; w.cutscene = true; CHECK(HostRefusal(w) == Refusal::Cutscene); }
    { HostView w = v; w.travelling = true; CHECK(HostRefusal(w) == Refusal::Travelling); }
    { HostView w = v; w.guests_connected = 0; CHECK(HostRefusal(w) == Refusal::NoGuests); }
    // A boss fight wins over the cooldown (the reason the player can act on).
    { HostView w = v; w.boss_fight = true; w.last_gather_at = w.now - 1; CHECK(HostRefusal(w) == Refusal::BossFight); }
    // Cooldown: 30 s; the seconds left rounded up.
    v.last_gather_at = v.now - 10.2;
    CHECK(HostRefusal(v, &wait) == Refusal::Cooldown && wait == 20);
    CHECK(HostRefusalText(Refusal::Cooldown, wait) == "Gather the party: wait 20 s");
    v.last_gather_at = v.now - kCooldownS;
    CHECK(HostRefusal(v, &wait) == Refusal::None);
    CHECK(HostRefusalText(Refusal::BossFight, 0) == "Gather the party: not during a boss fight");
}

static void GuestRules() {
    GuestView v;
    v.world_up = true;
    CHECK(GuestRefusal(v) == Refusal::None);
    { GuestView w = v; w.loading = true; CHECK(GuestRefusal(w) == Refusal::Loading); }
    { GuestView w = v; w.world_up = false; CHECK(GuestRefusal(w) == Refusal::Loading); }
    { GuestView w = v; w.alive = false; CHECK(GuestRefusal(w) == Refusal::Dead); }
    { GuestView w = v; w.alive = false; w.spectating = true; CHECK(GuestRefusal(w) == Refusal::Spectating); }
    { GuestView w = v; w.boss_fight = true; CHECK(GuestRefusal(w) == Refusal::BossFight); }
    { GuestView w = v; w.cutscene = true; CHECK(GuestRefusal(w) == Refusal::Cutscene); }
    { GuestView w = v; w.menu = true; CHECK(GuestRefusal(w) == Refusal::Menu); }
    { GuestView w = v; w.dialog = true; CHECK(GuestRefusal(w) == Refusal::Dialog); }
    { GuestView w = v; w.travelling = true; CHECK(GuestRefusal(w) == Refusal::Travelling); }
    // Dead in a menu: dead is what counts (the menu closes by itself, the death does not).
    { GuestView w = v; w.alive = false; w.menu = true; CHECK(GuestRefusal(w) == Refusal::Dead); }
    // A boss fight with the menu open: the fight.
    { GuestView w = v; w.boss_fight = true; w.menu = true; CHECK(GuestRefusal(w) == Refusal::BossFight); }
    CHECK(GuestRefusalText(Refusal::Menu) == "The host gathers the party: not now (in a menu)");
}

static void Routes() {
    const std::uint32_t m24_01 = 0x18010000, m24_00 = 0x18000000, dream = 0x15000000;
    Place p;
    p.client = true;
    p.own_map = m24_01;
    p.host_map = m24_01;
    p.host_chr_here = true;
    p.distance = 130;
    CHECK(RouteFor(p) == Route::InWorld);           // the same map, far away: no load
    p.distance = 1.0f;
    CHECK(RouteFor(p) == Route::Here);              // already beside the host
    p.distance = 130;
    p.host_chr_here = false;
    CHECK(RouteFor(p) == Route::Travel);            // the host's chr not here (yet): a load
    p.host_chr_here = true;
    p.own_map = m24_00;
    CHECK(RouteFor(p) == Route::Travel);            // a streamed neighbour block: another map, a load
    p.own_map = dream;
    CHECK(RouteFor(p) == Route::Travel);            // another area
    p.own_map = 0xffffffffu;
    CHECK(RouteFor(p) == Route::Travel);            // unknown: never an in-world warp
    p.own_map = m24_01;
    p.client = false;
    CHECK(RouteFor(p) == Route::Rejoin);            // its own world: the Rejoin
    CHECK(SameBlock(0x18010000, 0x18010001) && !SameBlock(0x18010000, 0x18000000) && !SameBlock(0, 0));
}

static void Wire() {
    Ask a;
    a.round = 3;
    a.map = 0x18010000;
    a.pos[0] = -36.8f, a.pos[1] = -40.75f, a.pos[2] = 84.3f;
    a.yaw = 1.25f;
    a.t = 123456789012;
    Ask b;
    CHECK(DecodeAsk(EncodeAsk(a), &b) && b.round == 3 && b.map == a.map && std::fabs(b.pos[2] - 84.3f) < 1e-3f &&
          std::fabs(b.yaw - 1.25f) < 1e-3f && b.t == 123456789012);
    CHECK(!DecodeAsk("{\"op\":\"ask\",\"round\":0,\"map\":402718720,\"pos\":[1,2,3],\"yaw\":0}", &b));      // round 0
    CHECK(!DecodeAsk("{\"op\":\"ask\",\"round\":1,\"map\":0,\"pos\":[1,2,3],\"yaw\":0}", &b));              // no map
    CHECK(!DecodeAsk("{\"op\":\"ask\",\"round\":1,\"map\":402718720,\"pos\":[1e9,2,3],\"yaw\":0}", &b));    // far out
    CHECK(!DecodeAsk("{\"op\":\"ask\",\"round\":1,\"map\":402718720,\"pos\":[nan,2,3],\"yaw\":0}", &b));
    CHECK(!DecodeAsk("{\"op\":\"answer\",\"round\":1}", &b));
    Answer y;
    y.round = 3;
    y.reply = Reply::Yes;
    y.route = Route::Travel;
    Answer z;
    CHECK(DecodeAnswer(EncodeAnswer(y), &z) && z.round == 3 && z.reply == Reply::Yes && z.route == Route::Travel);
    y.reply = Reply::Refused;
    y.why = Refusal::Menu;
    CHECK(DecodeAnswer(EncodeAnswer(y), &z) && z.reply == Reply::Refused && z.why == Refusal::Menu);
    CHECK(!DecodeAnswer("{\"op\":\"answer\",\"round\":2,\"reply\":\"maybe\"}", &z));
    CHECK(!DecodeAnswer("{\"op\":\"answer\",\"reply\":\"yes\"}", &z));
    for (int i = 0; i <= int(Refusal::Failed); ++i) CHECK(RefusalFromWord(RefusalWord(Refusal(i))) == Refusal(i));
}

static void Rounds() {
    std::vector<MemberAnswer> m(3);
    m[0].name = "Ann";
    m[1].name = "Bob";
    m[2].name = "Cid";
    CHECK(!RoundOver(m, 1.0));
    CHECK(RoundOver(m, kAnswerTimeoutS + kRoundSlackS));
    m[0].answered = true;
    m[0].answer.reply = Reply::Yes;
    m[1].answered = true;
    m[1].answer.reply = Reply::No;
    m[2].answered = true;
    m[2].answer.reply = Reply::Refused;
    m[2].answer.why = Refusal::Menu;
    CHECK(RoundOver(m, 1.0));
    CHECK(Summary(m) == "Gather: Ann coming; Bob said no; not now: Cid (in a menu)");
    m[1].answer.reply = Reply::Yes;
    m[2].answered = false;
    CHECK(Summary(m) == "Gather: Ann, Bob coming; not now: Cid (no answer)");
    CHECK(Summary({}) == "Gather: nobody answered");
}

// A player's report (Old Yharnam): the host gathered half way up a ladder; the guest was put in the
// open shaft beside it and dropped 3 m. Guests are put on ground only.
static void Standing() {
    CHECK(HostStanding(-1, 0.0f));                  // no ladder, still
    CHECK(HostStanding(-1000, 0.0f));               // ladder state unread: by the height alone
    CHECK(HostStanding(-1, 3.0f));                  // stairs at a run
    CHECK(!HostStanding(0, 0.0f));                  // holding still on a ladder (the report)
    CHECK(!HostStanding(3, 1.2f));                  // climbing
    CHECK(!HostStanding(-1, -8.0f));                // falling
    CHECK(!HostStanding(-1, 4.5f));                 // a jump
    HostView v = ReadyHost();
    v.standing = false;
    CHECK(HostRefusal(v) == Refusal::NotStanding);
    CHECK(HostRefusalText(Refusal::NotStanding, 0) == "Gather the party: not on a ladder or in the air");
    // a boss fight is still named first; the cooldown after it
    { HostView w = v; w.boss_fight = true; CHECK(HostRefusal(w) == Refusal::BossFight); }
    { HostView w = v; w.last_gather_at = w.now - 1; CHECK(HostRefusal(w) == Refusal::NotStanding); }
    CHECK(RefusalFromWord(RefusalWord(Refusal::NotStanding)) == Refusal::NotStanding);
    CHECK(RefusalFromWord("failed") == Refusal::Failed);
}

static void Spots() {
    // the host asked from the ground: its chr still about there -> beside the chr
    CHECK(SpotFor(true, -75.8f, -75.8f) == Spot::Live);
    CHECK(SpotFor(true, -74.6f, -75.8f) == Spot::Live);   // stairs / a slope
    // its chr climbed a ladder (the report: 21 m up) or dropped since: beside the spot it asked from
    CHECK(SpotFor(true, -54.3f, -75.8f) == Spot::Asked);
    CHECK(SpotFor(true, -90.0f, -75.8f) == Spot::Asked);
    // an older host (no "stand"): as before, beside the chr
    CHECK(SpotFor(false, -54.3f, -75.8f) == Spot::Live);
}

static void Landings() {
    // standing on the spot: watched for 4 s, then landed
    CHECK(LandingCheck(0.5, -91.6f, -91.6f, -1) == Landing::Wait);
    CHECK(LandingCheck(kLandWatchS, -91.6f, -91.7f, -1) == Landing::Landed);
    // a step down / a 3 m drop to a floor (the report's ladder shaft) is not a fall
    CHECK(LandingCheck(1.0, -75.8f, -78.8f, -1) == Landing::Wait);
    CHECK(LandingCheck(4.5, -75.8f, -78.8f, -1) == Landing::Landed);
    // no ground there: 4 m below the spot
    CHECK(LandingCheck(0.9, -75.8f, -79.8f, -1) == Landing::Fell);
    CHECK(LandingCheck(0.9, -67.8f, -76.0f, -1) == Landing::Fell);   // the test aid's 8 m lift
    // it caught a ladder: stands on it
    CHECK(LandingCheck(0.9, -75.8f, -80.0f, 2) == Landing::Landed);
}

static void WireStand() {
    Ask a;
    a.round = 1;
    a.map = 0x17000000u;
    a.pos[1] = -75.8f;
    a.yaw = 1.25f;
    a.standing = true;
    Ask b;
    CHECK(DecodeAsk(EncodeAsk(a), &b) && b.standing && std::fabs(b.yaw - 1.25f) < 1e-3f);
    a.standing = false;
    CHECK(DecodeAsk(EncodeAsk(a), &b) && !b.standing);
    // an older host's ask: no "stand" -> not checked
    CHECK(DecodeAsk("{\"op\":\"ask\",\"round\":2,\"map\":385875968,\"pos\":[3.1,-75.8,-63.2],\"yaw\":0.0000,\"t\":5}", &b) &&
          !b.standing && b.round == 2);
}

int main() {
    HostRules();
    Standing();
    Spots();
    Landings();
    WireStand();
    GuestRules();
    Routes();
    Wire();
    Rounds();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-gather-test: ok\n");
    return 0;
}
