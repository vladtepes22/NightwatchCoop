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
    // CL-adeb50 Stay in my world: refused whatever else is up, and the host hears the reason
    { GuestView w = v; w.stay_home = true; CHECK(GuestRefusal(w) == Refusal::StayHome); }
    { GuestView w = v; w.stay_home = true; w.menu = true; CHECK(GuestRefusal(w) == Refusal::StayHome); }
    CHECK(std::string(RefusalReason(Refusal::StayHome)) == "staying in own world");
    CHECK(GuestRefusalText(Refusal::StayHome) == "The host gathers the party: not now (staying in own world)");
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

static void ChaliceRoutes() {
    const std::uint32_t m29 = 0x1d000000, m24_01 = 0x18010000, dream = 0x15000000;
    Place p;
    p.client = true;
    p.host_dungeon = true;
    p.host_map = m29;
    p.own_map = m29;
    p.host_chr_here = true;
    p.distance = 40;
    CHECK(RouteFor(p) == Route::InWorld);   // the same loaded dungeon: moved in place, no load
    p.distance = 1;
    CHECK(RouteFor(p) == Route::Here);
    p.host_chr_here = false;
    CHECK(RouteFor(p) == Route::Rejoin);    // m29 without the host's chr (not this dungeon): the summon, never a load
    p.distance = 40;
    p.own_map = m24_01;
    CHECK(RouteFor(p) == Route::Rejoin);    // apart in another map
    p.own_map = dream;
    CHECK(RouteFor(p) == Route::Rejoin);
    p.own_map = 0xffffffffu;
    CHECK(RouteFor(p) == Route::Rejoin);
    p.client = false;
    CHECK(RouteFor(p) == Route::Rejoin);
    // No dungeon: unchanged (a load).
    p.client = true;
    p.host_dungeon = false;
    p.host_map = m24_01;
    p.own_map = dream;
    CHECK(RouteFor(p) == Route::Travel);
}

static void SpotRules() {
    CHECK(SpotAcceptable(10.0f, 10.0f) && SpotAcceptable(10.0f, 11.5f) && SpotAcceptable(10.0f, 8.5f));
    CHECK(!SpotAcceptable(10.0f, 11.6f) && !SpotAcceptable(10.0f, 8.4f));
    CHECK(NextTry(0) == 1 && NextTry(1) == 2 && NextTry(2) == kHostSpot && NextTry(kHostSpot) == -1);
    // Standing on the host's floor with ground: landed once the safe ground is in (kGroundSettleS).
    CHECK(LandingVerdict(0.5, 10, 10, 10, 0.1f, true, true, -1) == Verdict::Wait);
    CHECK(LandingVerdict(1.0, 10, 10, 10, 0.1f, true, true, -1) == Verdict::Landed);
    // On a roof 6 m up (random layout above the host): bad at once, not after the watch.
    CHECK(LandingVerdict(0.4, 10, 10, 16, 0.2f, true, true, -1) == Verdict::Bad);
    // 1.4 m up is a step, not a roof.
    CHECK(LandingVerdict(1.0, 10, 10, 11.4f, 0.2f, true, true, -1) == Verdict::Landed);
    // Shoved 5 m away by the geometry.
    CHECK(LandingVerdict(0.4, 10, 10, 10, 5.0f, true, true, -1) == Verdict::Bad);
    // No safe ground: wait, then bad.
    CHECK(LandingVerdict(1.5, 10, 10, 10, 0.1f, true, false, -1) == Verdict::Wait);
    CHECK(LandingVerdict(kGroundGiveUpS, 10, 10, 10, 0.1f, true, false, -1) == Verdict::Bad);
    // Safe ground unreadable: the old watch (kLandWatchS).
    CHECK(LandingVerdict(2.0, 10, 10, 10, 0.1f, false, false, -1) == Verdict::Wait);
    CHECK(LandingVerdict(kLandWatchS, 10, 10, 10, 0.1f, false, false, -1) == Verdict::Landed);
    // A fall is still the fall (back and travel / stay).
    CHECK(LandingVerdict(0.9, 10, 10, 5.5f, 0.1f, true, false, -1) == Verdict::Fell);
    // A ladder is standing.
    CHECK(LandingVerdict(0.5, 10, 10, 14, 0.1f, true, false, 2) == Verdict::Landed);
    CHECK(std::string(BadWhy(6, 0, true, true)).find("above") != std::string::npos);
    CHECK(std::string(BadWhy(0, 0, true, false)).find("no ground") != std::string::npos);
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
    for (int i = 0; i <= int(Refusal::StayHome); ++i) CHECK(RefusalFromWord(RefusalWord(Refusal(i))) == Refusal(i));
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

static void QuestionStates() {
    using namespace coop::gather;
    CHECK(QuestionState(1, true, 0.1, 0.1) == Question::Answered);
    CHECK(QuestionState(2, false, 0.1, 0.1) == Question::Answered);
    CHECK(QuestionState(-1, true, 5, 5) == Question::Open);
    CHECK(QuestionState(-1, false, 0.5, 0.6) == Question::Open);       // the box is still opening
    CHECK(QuestionState(-1, false, 3.0, 3.1) == Question::Dismissed);  // gone without a result: a NO
    CHECK(QuestionState(-1, true, 95, 96) == Question::Expired);
}

// CL-a3a9e6: a guest gathers the party.
static RequesterView ReadyGuest() {
    RequesterView v;
    v.allowed = true;
    v.client = true;
    v.guest.world_up = true;
    v.now = 1000;
    return v;
}

static void GuestGather() {
    int wait = -1;
    RequesterView v = ReadyGuest();
    CHECK(RequestRefusal(v, &wait) == Refusal::None && wait == 0);
    { RequesterView w = v; w.allowed = false; CHECK(RequestRefusal(w) == Refusal::GuestsOff); }
    { RequesterView w = v; w.client = false; CHECK(RequestRefusal(w) == Refusal::OwnWorld); }
    { RequesterView w = v; w.guest.stay_home = true; CHECK(RequestRefusal(w) == Refusal::StayHome); }
    // staying home wins over the own-world refusal (the guest is at home because it chose to be)
    { RequesterView w = v; w.guest.stay_home = true; w.client = false; CHECK(RequestRefusal(w) == Refusal::StayHome); }
    { RequesterView w = v; w.guest.loading = true; CHECK(RequestRefusal(w) == Refusal::Loading); }
    { RequesterView w = v; w.guest.boss_fight = true; CHECK(RequestRefusal(w) == Refusal::BossFight); }
    { RequesterView w = v; w.guest.cutscene = true; CHECK(RequestRefusal(w) == Refusal::Cutscene); }
    { RequesterView w = v; w.guest.alive = false; CHECK(RequestRefusal(w) == Refusal::Dead); }
    { RequesterView w = v; w.guest.travelling = true; CHECK(RequestRefusal(w) == Refusal::Travelling); }
    { RequesterView w = v; w.standing = false; CHECK(RequestRefusal(w) == Refusal::NotStanding); }
    // the request comes from the menu: an open menu or dialog is no reason
    { RequesterView w = v; w.guest.menu = true; w.guest.dialog = true; CHECK(RequestRefusal(w) == Refusal::None); }
    v.last_request_at = v.now - 10.2;
    CHECK(RequestRefusal(v, &wait) == Refusal::Cooldown && wait == 20);
    v.last_request_at = v.now - kCooldownS;
    CHECK(RequestRefusal(v, &wait) == Refusal::None);

    // the host's side: its rule, the shared cooldown, a round still open
    CHECK(RequestHostRefusal(true, true, false, 1000, -1e9, &wait) == Refusal::None);
    CHECK(RequestHostRefusal(true, false, false, 1000, -1e9) == Refusal::GuestsOff);
    CHECK(RequestHostRefusal(false, true, false, 1000, -1e9) == Refusal::NotHost);
    CHECK(RequestHostRefusal(true, true, false, 1000, 990, &wait) == Refusal::Cooldown && wait == 20);
    CHECK(RequestHostRefusal(true, true, true, 1000, -1e9, &wait) == Refusal::Cooldown && wait == 5);

    // members in another map are not moved there (no load, no computed point)
    CHECK(MemberAreaRefusal(0x18010000, 0x18010001) == Refusal::None);
    CHECK(MemberAreaRefusal(0x18000000, 0x18010000) == Refusal::OtherArea);
    CHECK(MemberAreaRefusal(0x15000000, 0x18010000) == Refusal::OtherArea);
    CHECK(MemberAreaRefusal(0xffffffffu, 0x18010000) == Refusal::OtherArea);
    CHECK(MemberAreaRefusal(0, 0) == Refusal::OtherArea);

    // texts
    CHECK(HostRefusalText(Refusal::GuestsOff, 0) == "Gather the party: the host has not allowed guests to gather");
    CHECK(HostRefusalText(Refusal::OwnWorld, 0) == "Gather the party: you are in your own world");
    CHECK(HostRefusalText(Refusal::StayHome, 0) == "Gather the party: staying in own world");
    CHECK(GuestRefusalText(Refusal::Menu, "Ann") == "Ann gathers the party: not now (in a menu)");
    CHECK(GuestRefusalText(Refusal::OtherArea, "Ann") == "Ann gathers the party: not now (in another area)");
    for (Refusal r : {Refusal::GuestsOff, Refusal::OwnWorld, Refusal::OtherArea}) CHECK(RefusalFromWord(RefusalWord(r)) == r);
    // the refusal reaches the host's summary with the same reason as for the host's own gather
    CHECK(Summary({{"Ann", true, {1, Reply::Refused, Refusal::StayHome, Route::Here}}}) == "Gather: not now: Ann (staying in own world)");

    // wire: the request and the ask carry the gathering guest's slot; the reply carries the host's verdict
    Ask a;
    a.round = 3;
    a.map = 0x18010000;
    a.pos[0] = 1.5f;
    a.pos[1] = -2.25f;
    a.pos[2] = 30.0f;
    a.yaw = 0.5f;
    a.t = 12345;
    a.standing = true;
    a.by = 2;
    Ask b;
    CHECK(DecodeAsk(EncodeAsk(a, "req"), &b, "req") && b.by == 2 && b.round == 3 && b.standing && b.t == 12345);
    CHECK(!DecodeAsk(EncodeAsk(a, "req"), &b));            // a request is not an ask
    CHECK(!DecodeAsk(EncodeAsk(a), &b, "req"));            // nor an ask a request
    CHECK(DecodeAsk(EncodeAsk(a), &b) && b.by == 2);
    a.by = 0;
    CHECK(DecodeAsk(EncodeAsk(a), &b) && b.by == 0);       // the host's own gather: by 0
    CHECK(DecodeAsk("{\"op\":\"ask\",\"round\":1,\"map\":402718720,\"pos\":[1,2,3],\"yaw\":0}", &b) && b.by == 0);  // older host
    RequestReply rp, rq;
    rp.round = 7;
    rp.ok = true;
    rp.asked = 2;
    CHECK(DecodeReply(EncodeReply(rp), &rq) && rq.ok && rq.asked == 2 && rq.round == 7);
    rp.ok = false;
    rp.why = Refusal::GuestsOff;
    CHECK(DecodeReply(EncodeReply(rp), &rq) && !rq.ok && rq.why == Refusal::GuestsOff);
    Answer an;
    CHECK(!DecodeAnswer(EncodeReply(rp), &an));            // the reply is no answer
    CHECK(!DecodeAsk(EncodeReply(rp), &b) && !DecodeAsk(EncodeReply(rp), &b, "req"));
}

int main() {
    GuestGather();
    HostRules();
    Standing();
    Spots();
    Landings();
    ChaliceRoutes();
    SpotRules();
    WireStand();
    GuestRules();
    Routes();
    Wire();
    Rounds();
    QuestionStates();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-gather-test: ok\n");
    return 0;
}
