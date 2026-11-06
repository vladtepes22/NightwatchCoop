// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.8 give items to a party member: the goods rules, the wire and the two-phase transfer
// (gpu/shim/party/party_give_rules.h). Build and run: ninja -C out/gpu party-give-test && out/gpu/party-give-test.exe
#include "party/party_give_rules.h"

#include <cstdio>
#include <string>

using namespace coop::give;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static GoodsFacts Vial(int held = 12) {
    GoodsFacts f;
    f.id = 1000;  // Blood Vial
    f.held = held;
    f.max_num = 20;
    f.max_storage = 99;
    f.goods_type = 0;
    return f;
}

static void Allow() {
    CHECK(GiverVerdict(Vial(), 5) == Why::None);
    CHECK(GiverVerdict(Vial(), 12) == Why::None);
    CHECK(GiverVerdict(Vial(), 13) == Why::NotHeld);
    CHECK(GiverVerdict(Vial(), 0) == Why::NotHeld);
    CHECK(GiverVerdict(Vial(0), 1) == Why::NotHeld);
    CHECK(GiverVerdict(Vial(150), 100) == Why::NotHeld);  // more than one transfer carries
    { GoodsFacts f = Vial(); f.id = 4000; f.goods_type = 1; f.max_num = 1; CHECK(GiverVerdict(f, 1) == Why::KeyItem); }
    { GoodsFacts f = Vial(); f.goods_type = 1; f.max_num = 5; CHECK(GiverVerdict(f, 1) == Why::KeyItem); }
    // materials are goodsType 2 (Blood Stone Shard 3000, Twin 3010, Chunk 3020, Rock 3030, ritual materials 7xxx): allowed
    { GoodsFacts f = Vial(40); f.id = 3000; f.goods_type = 2; f.max_num = 99; CHECK(GiverVerdict(f, 40) == Why::None); }
    { GoodsFacts f = Vial(3); f.id = 3030; f.goods_type = 2; f.max_num = 99; CHECK(GiverVerdict(f, 3) == Why::None); CHECK(ReceiverVerdict(f, 3, 99) == Why::None); }
    { GoodsFacts f = Vial(3); f.id = 7200; f.goods_type = 2; f.max_num = 99; CHECK(GiverVerdict(f, 3) == Why::None); }
    { GoodsFacts f = Vial(3); f.goods_type = 3; CHECK(GiverVerdict(f, 3) == Why::KeyItem); }
    { GoodsFacts f = Vial(3); f.goods_type = 6; CHECK(GiverVerdict(f, 3) == Why::KeyItem); }
    // unique: a hunter tool / badge row that is goodsType 0 but maxNum 1
    { GoodsFacts f = Vial(1); f.id = 5555; f.max_num = 1; CHECK(GiverVerdict(f, 1) == Why::Unique); }
    // the story bands are denied even when the param row looks plain
    { GoodsFacts f = Vial(); f.id = 4110; CHECK(GiverVerdict(f, 1) == Why::Story); }
    { GoodsFacts f = Vial(); f.id = 4311; CHECK(GiverVerdict(f, 1) == Why::Story); }
    { GoodsFacts f = Vial(); f.id = 4312; CHECK(GiverVerdict(f, 1) == Why::Story); }
    { GoodsFacts f = Vial(); f.id = 4999; CHECK(GiverVerdict(f, 1) == Why::Story); }
    { GoodsFacts f = Vial(); f.id = 5000; CHECK(GiverVerdict(f, 1) == Why::None); }
    { GoodsFacts f = Vial(); f.id = 3999; CHECK(GiverVerdict(f, 1) == Why::None); }
    { GoodsFacts f = Vial(); f.id = 800; CHECK(GiverVerdict(f, 1) == Why::Story); }  // gestures
    { GoodsFacts f = Vial(); f.id = 822; CHECK(GiverVerdict(f, 1) == Why::Story); }
    { GoodsFacts f = Vial(); f.id = 823; CHECK(GiverVerdict(f, 1) == Why::None); }
    // unknown param facts: never
    { GoodsFacts f = Vial(); f.goods_type = -1; CHECK(GiverVerdict(f, 1) == Why::Failed); }
    { GoodsFacts f = Vial(); f.max_num = -1; CHECK(GiverVerdict(f, 1) == Why::Failed); }
    { GoodsFacts f = Vial(); f.id = 0; CHECK(GiverVerdict(f, 1) == Why::Failed); }
    // a quick-slot item keeps one
    { GoodsFacts f = Vial(); f.quick = true; CHECK(GiverVerdict(f, 12) == Why::Equipped); CHECK(GiverVerdict(f, 11) == Why::None); }
}

static void RoomRules() {
    CHECK(Room(12, 20, 99) == 8 + 99);
    CHECK(Room(20, 20, 0) == 0);
    CHECK(Room(20, 20, -1) == 0);
    CHECK(Room(25, 20, 5) == 5);  // over the max already: only the box
    int room = -1;
    CHECK(ReceiverVerdict(Vial(18), 2, 0, &room) == Why::None && room == 2);
    CHECK(ReceiverVerdict(Vial(18), 3, 0, &room) == Why::NoRoom && room == 2);
    CHECK(ReceiverVerdict(Vial(18), 3, 1) == Why::None);  // 2 in the inventory, 1 in the box
    CHECK(ReceiverVerdict(Vial(18), 4, 1) == Why::NoRoom);
    // the receiver applies the goods rules to what the giver names
    { GoodsFacts f = Vial(0); f.goods_type = 1; CHECK(ReceiverVerdict(f, 1, 99) == Why::KeyItem); }
    { GoodsFacts f = Vial(0); f.id = 4114; CHECK(ReceiverVerdict(f, 1, 99) == Why::Story); }
    { GoodsFacts f = Vial(0); f.max_num = 1; CHECK(ReceiverVerdict(f, 1, 99) == Why::Unique); }
    CHECK(ReceiverVerdict(Vial(0), 0, 99) == Why::Failed);
    CHECK(ReceiverVerdict(Vial(0), 100, 99) == Why::Failed);
}

static void Moment() {
    GateView v;
    v.enabled = true;
    v.world_up = true;
    CHECK(Gate(v) == Why::None);
    { GateView w = v; w.enabled = false; CHECK(Gate(w) == Why::Off); CHECK(Gate(w, true) == Why::TheirOff); }
    { GateView w = v; w.world_up = false; CHECK(Gate(w) == Why::Loading); }
    { GateView w = v; w.loading = true; CHECK(Gate(w) == Why::Loading); }
    { GateView w = v; w.travelling = true; CHECK(Gate(w) == Why::Travelling); }
    { GateView w = v; w.alive = false; CHECK(Gate(w) == Why::Dead); }
    { GateView w = v; w.spectating = true; CHECK(Gate(w) == Why::Dead); }
    { GateView w = v; w.boss_fight = true; CHECK(Gate(w) == Why::BossFight); }
    { GateView w = v; w.cutscene = true; CHECK(Gate(w) == Why::Cutscene); }
}

static void Wire() {
    Msg m;
    m.op = Op::Offer;
    m.id = 4000000000u;
    m.from = 1;
    m.to = 2;
    m.goods = 1000;
    m.n = 7;
    Msg r;
    CHECK(Decode(Encode(m), &r));
    CHECK(r.op == Op::Offer && r.id == 4000000000u && r.from == 1 && r.to == 2 && r.goods == 1000 && r.n == 7);
    m = Msg();
    m.op = Op::Done;
    m.id = 5;
    m.got = 3;
    m.why = Why::NoRoom;
    CHECK(Decode(Encode(m), &r) && r.op == Op::Done && r.got == 3 && r.why == Why::NoRoom);
    m = Msg();
    m.op = Op::Ready;
    m.id = 5;
    m.room = 9;
    CHECK(Decode(Encode(m), &r) && r.room == 9);
    // refused input
    CHECK(!Decode("", &r));
    CHECK(!Decode("{\"op\":\"nope\",\"id\":1,\"from\":0,\"to\":1}", &r));
    CHECK(!Decode("{\"op\":\"commit\",\"id\":0,\"from\":0,\"to\":1}", &r));
    CHECK(!Decode("{\"op\":\"commit\",\"id\":1,\"from\":9,\"to\":1}", &r));
    CHECK(!Decode("{\"op\":\"commit\",\"id\":1,\"from\":0,\"to\":-1}", &r));
    CHECK(!Decode("{\"op\":\"offer\",\"id\":1,\"from\":0,\"to\":1,\"goods\":1000,\"n\":0}", &r));
    CHECK(!Decode("{\"op\":\"offer\",\"id\":1,\"from\":0,\"to\":1,\"goods\":1000,\"n\":100}", &r));
    CHECK(!Decode("{\"op\":\"offer\",\"id\":1,\"from\":0,\"to\":1,\"goods\":0,\"n\":5}", &r));
    CHECK(!Decode("{\"op\":\"done\",\"id\":1,\"from\":0,\"to\":1,\"got\":-1}", &r));
    CHECK(!Decode(std::string(500, 'x'), &r));
    CHECK(Decode("{\"op\":\"abort\",\"id\":1,\"from\":0,\"to\":1}", &r) && r.op == Op::Abort);
    // an unknown why word is "failed"
    CHECK(Decode("{\"op\":\"refuse\",\"id\":1,\"from\":0,\"to\":1,\"why\":\"zzz\"}", &r) && r.why == Why::Failed);
    for (int i = 0; i < kWhyCount; ++i) CHECK(WhyFromWord(WhyWord(Why(i))) == Why(i));
}

// A whole transfer, step by step, with the two machines wired by hand.
static void Happy() {
    Giver g;
    Receiver r;
    CHECK(g.Begin(7, 1, 1000, 5, 0.0));
    CHECK(!g.Begin(8, 1, 1000, 5, 0.0));  // busy
    CHECK(r.Reserve(0, 7, 1000, 5, 0.1) == Receiver::Result::Ok);
    Step s = g.OnReady(7, 1, 0.2);
    CHECK(s.act == Act::Remove);
    s = g.Removed(5, 0.2);
    CHECK(s.act == Act::SendCommit);
    RStep c = r.OnCommit(0, 7, 0.3);
    CHECK(c.act == RAct::Apply && c.goods == 1000 && c.n == 5);
    RStep d = r.Applied(5, Why::None);
    CHECK(d.act == RAct::SendDone && d.got == 5 && d.why == Why::None);
    s = g.OnDone(7, 1, d.got, d.why);
    CHECK(s.act == Act::Finished && s.refund == 0 && s.got == 5 && s.why == Why::None);
    CHECK(!g.Busy());
    // a repeated COMMIT (replay after a reconnect) gives nothing again, answers the same DONE
    RStep again = r.OnCommit(0, 7, 0.4);
    CHECK(again.act == RAct::SendDone && again.got == 5);
    // and an ABORT after the apply says the truth: applied
    RStep ab = r.OnAbort(0, 7);
    CHECK(ab.act == RAct::SendDone && ab.got == 5);
}

static void Refused() {
    Giver g;
    CHECK(g.Begin(1, 2, 1000, 3, 0.0));
    Step s = g.OnRefuse(1, 2, Why::NoRoom);
    CHECK(s.act == Act::Failed && s.why == Why::NoRoom && s.refund == 0);
    CHECK(!g.Busy());
    // a refusal from the wrong member or id is ignored
    CHECK(g.Begin(2, 2, 1000, 3, 0.0));
    CHECK(g.OnRefuse(2, 3, Why::NoRoom).act == Act::None);
    CHECK(g.OnRefuse(9, 2, Why::NoRoom).act == Act::None);
    CHECK(g.OnReady(2, 3, 0.1).act == Act::None);
    CHECK(g.Busy());
    // no answer: the give fails and nothing was removed
    s = g.Tick(kOfferTimeoutS - 1.0, true);
    CHECK(s.act == Act::None);
    s = g.Tick(kOfferTimeoutS + 1.0, true);
    CHECK(s.act == Act::Failed && s.why == Why::NoAnswer && s.refund == 0);
    // the member leaves before the removal
    CHECK(g.Begin(3, 2, 1000, 3, 100.0));
    CHECK(g.OnMemberGone(1).act == Act::None);
    CHECK(g.OnMemberGone(2).act == Act::Failed);
    // receiver: busy with another transfer, an id seen twice
    Receiver r;
    CHECK(r.Reserve(0, 1, 1000, 1, 0.0) == Receiver::Result::Ok);
    CHECK(r.Reserve(1, 2, 1000, 1, 0.0) == Receiver::Result::Busy);
    r.OnMemberGone(0);
    CHECK(!r.Reserved());
    CHECK(r.Reserve(0, 1, 1000, 1, 1.0) == Receiver::Result::Duplicate);  // dead for good
}

// The removal does not verify: ABORT, the removed ones come back, the receiver gives nothing.
static void RemovalFails() {
    Giver g;
    Receiver r;
    CHECK(g.Begin(11, 1, 1000, 5, 0.0));
    CHECK(r.Reserve(0, 11, 1000, 5, 0.0) == Receiver::Result::Ok);
    CHECK(g.OnReady(11, 1, 0.1).act == Act::Remove);
    Step s = g.Removed(3, 0.1);  // only 3 of 5 left the inventory
    CHECK(s.act == Act::SendAbort && s.refund == 3 && s.why == Why::Failed);
    CHECK(!g.Busy());
    RStep a = r.OnAbort(0, 11);
    CHECK(a.act == RAct::SendDone && a.got == 0);
    // a COMMIT that was already on its way is dead
    RStep c = r.OnCommit(0, 11, 0.2);
    CHECK(c.act == RAct::SendDone && c.got == 0);
    // removed more than asked: all of it goes back
    CHECK(g.Begin(12, 1, 1000, 5, 1.0));
    CHECK(g.OnReady(12, 1, 1.0).act == Act::Remove);
    s = g.Removed(6, 1.0);
    CHECK(s.act == Act::SendAbort && s.refund == 6);
    // removed nothing
    CHECK(g.Begin(13, 1, 1000, 5, 2.0));
    CHECK(g.OnReady(13, 1, 2.0).act == Act::Remove);
    s = g.Removed(0, 2.0);
    CHECK(s.act == Act::SendAbort && s.refund == 0);
}

// The receiver gives less than asked (a full inventory it did not foresee): the giver takes the rest back.
static void ShortDone() {
    Giver g;
    Receiver r;
    CHECK(g.Begin(21, 1, 1000, 5, 0.0));
    CHECK(r.Reserve(0, 21, 1000, 5, 0.0) == Receiver::Result::Ok);
    CHECK(g.OnReady(21, 1, 0.1).act == Act::Remove);
    CHECK(g.Removed(5, 0.1).act == Act::SendCommit);
    CHECK(r.OnCommit(0, 21, 0.2).act == RAct::Apply);
    RStep d = r.Applied(2, Why::NoRoom);
    CHECK(d.got == 2 && d.why == Why::NoRoom);
    Step s = g.OnDone(21, 1, d.got, d.why);
    CHECK(s.act == Act::Finished && s.refund == 3 && s.got == 2 && s.why == Why::NoRoom);
    // a DONE claiming more than n is clamped
    CHECK(g.Begin(22, 1, 1000, 5, 1.0));
    CHECK(g.OnReady(22, 1, 1.0).act == Act::Remove);
    CHECK(g.Removed(5, 1.0).act == Act::SendCommit);
    s = g.OnDone(22, 1, 99, Why::None);
    CHECK(s.got == 5 && s.refund == 0);
}

// Timeout rollback: the COMMIT or the DONE was lost.
static void Timeouts() {
    // (a) COMMIT never arrived (the link died between): no DONE, the giver asks, the receiver never reserved
    // (a new process) -> DONE 0 -> the giver refunds the whole amount through OnDone.
    {
        Giver g;
        Receiver fresh;
        CHECK(g.Begin(31, 1, 1000, 4, 0.0));
        CHECK(g.OnReady(31, 1, 0.0).act == Act::Remove);
        CHECK(g.Removed(4, 0.0).act == Act::SendCommit);
        CHECK(g.Tick(kDoneWaitS - 1, true).act == Act::None);
        Step s = g.Tick(kDoneWaitS + 1, true);
        CHECK(s.act == Act::SendAbort && g.state() == Giver::State::Aborting);
        RStep a = fresh.OnAbort(0, 31);
        CHECK(a.act == RAct::SendDone && a.got == 0);
        s = g.OnDone(31, 1, a.got, a.why);
        CHECK(s.act == Act::Finished && s.refund == 4 && s.got == 0);
        // and the late COMMIT of that transfer gives nothing
        CHECK(fresh.OnCommit(0, 31, 50.0).act == RAct::SendDone);
    }
    // (b) the DONE was lost but the receiver applied: the ABORT learns it, no refund (never a duplicate)
    {
        Giver g;
        Receiver r;
        CHECK(g.Begin(32, 1, 1000, 4, 0.0));
        CHECK(r.Reserve(0, 32, 1000, 4, 0.0) == Receiver::Result::Ok);
        CHECK(g.OnReady(32, 1, 0.0).act == Act::Remove);
        CHECK(g.Removed(4, 0.0).act == Act::SendCommit);
        CHECK(r.OnCommit(0, 32, 0.1).act == RAct::Apply);
        r.Applied(4, Why::None);  // DONE lost
        CHECK(g.Tick(kDoneWaitS + 1, true).act == Act::SendAbort);
        RStep a = r.OnAbort(0, 32);
        Step s = g.OnDone(32, 1, a.got, a.why);
        CHECK(s.act == Act::Finished && s.refund == 0 && s.got == 4);
    }
    // (c) ABORT unanswered with the link up: roll back everything
    {
        Giver g;
        CHECK(g.Begin(33, 1, 1000, 4, 0.0));
        CHECK(g.OnReady(33, 1, 0.0).act == Act::Remove);
        CHECK(g.Removed(4, 0.0).act == Act::SendCommit);
        CHECK(g.Tick(kDoneWaitS + 1, true).act == Act::SendAbort);
        CHECK(g.Tick(kDoneWaitS + 1 + kAbortWaitS - 1, true).act == Act::None);
        Step s = g.Tick(kDoneWaitS + 1 + kAbortWaitS + 1, true);
        CHECK(s.act == Act::RolledBack && s.refund == 4 && s.why == Why::RolledBack);
        CHECK(!g.Busy());
    }
    // (d) the link dies after COMMIT: nothing is decided while the member can come back, rolled back after kLinkDownS
    {
        Giver g;
        CHECK(g.Begin(34, 1, 1000, 4, 0.0));
        CHECK(g.OnReady(34, 1, 0.0).act == Act::Remove);
        CHECK(g.Removed(4, 0.0).act == Act::SendCommit);
        CHECK(g.Tick(2.0, false).act == Act::None);
        CHECK(g.Tick(2.0 + kLinkDownS - 1, false).act == Act::None);
        Step s = g.Tick(2.0 + kLinkDownS + 1, false);
        CHECK(s.act == Act::RolledBack && s.refund == 4);
    }
    // (e) the link comes back in time: the wait starts afresh, then the normal question
    {
        Giver g;
        CHECK(g.Begin(35, 1, 1000, 4, 0.0));
        CHECK(g.OnReady(35, 1, 0.0).act == Act::Remove);
        CHECK(g.Removed(4, 0.0).act == Act::SendCommit);
        CHECK(g.Tick(20.0, false).act == Act::None);
        CHECK(g.Tick(30.0, true).act == Act::None);  // back: fresh wait
        CHECK(g.Tick(30.0 + kDoneWaitS - 1, true).act == Act::None);
        CHECK(g.Tick(30.0 + kDoneWaitS + 1, true).act == Act::SendAbort);
        // the (queued) DONE arrives after all
        Step s = g.OnDone(35, 1, 4, Why::None);
        CHECK(s.act == Act::Finished && s.refund == 0);
    }
    // a DONE nobody asked for is ignored
    {
        Giver g;
        CHECK(g.OnDone(1, 1, 5, Why::None).act == Act::None);
    }
    // receiver: an unanswered reservation expires; a COMMIT after it gives nothing
    {
        Receiver r;
        CHECK(r.Reserve(2, 40, 1000, 3, 0.0) == Receiver::Result::Ok);
        r.Tick(kReserveS - 1);
        CHECK(r.Reserved());
        r.Tick(kReserveS + 1);
        CHECK(!r.Reserved());
        RStep c = r.OnCommit(2, 40, kReserveS + 2);
        CHECK(c.act == RAct::SendDone && c.got == 0);
        // a COMMIT of a reservation that is live but too old is refused as well
        CHECK(r.Reserve(2, 41, 1000, 3, 100.0) == Receiver::Result::Ok);
        c = r.OnCommit(2, 41, 100.0 + kReserveS + 0.5);
        CHECK(c.act == RAct::SendDone && c.got == 0 && !r.Reserved());
    }
    // receiver: a COMMIT from another member than the reserved giver gives nothing
    {
        Receiver r;
        CHECK(r.Reserve(2, 50, 1000, 3, 0.0) == Receiver::Result::Ok);
        RStep c = r.OnCommit(3, 50, 0.1);
        CHECK(c.act == RAct::SendDone && c.got == 0);
        CHECK(r.Reserved());  // the real giver's reservation stays
        CHECK(r.OnCommit(2, 50, 0.2).act == RAct::Apply);
    }
}

static void Texts() {
    CHECK(GaveText("Blood Vial", 3, "Ann") == "Gave Blood Vial x3 to Ann");
    CHECK(GotText("Blood Vial", 3, "Ann") == "Blood Vial x3 from Ann");
    CHECK(GotText("", 1, "Ann") == "Item x1 from Ann");
    CHECK(RefusedText("Blood Vial", "Ann", Why::NoRoom) == "Give Blood Vial to Ann: no room in their inventory and storage");
}

// ---- 0.7.9 gear (weapons and armor) ----

static GearFacts Saw(std::uint32_t id = 5000500) {  // a plain weapon row (a +5 upgrade)
    GearFacts f;
    f.kind = Kind::Weapon;
    f.id = id;
    f.held = true;
    f.free_slots = 40;
    return f;
}

static GearGem OneGem(int slot) {
    GearGem g;
    g.slot = slot;
    g.id = 71000;
    g.data[0] = 1;
    g.data[1] = 4;
    for (int k = 2; k < 8; ++k) g.data[k] = -1;
    g.data[2] = 3210001;
    return g;
}

static void GearRules() {
    // The table is built from the game's params at run time (party_items.cpp EnsureGearTable); the test fills a synthetic one.
    GearTable::Get().Set({5000500, 5000600, 5000000}, {7100, 7200});
    // the table: a normal row is giveable, a shared / quest family or an unknown row is not
    CHECK(GearGiveable(Kind::Weapon, 5000500));
    CHECK(!GearGiveable(Kind::Weapon, 9000000));  // a shared lot's weapon (a shared lot's weapon)
    CHECK(!GearGiveable(Kind::Weapon, 9000700));  // ... at every upgrade level
    CHECK(!GearGiveable(Kind::Weapon, 71000));   // an internal row
    CHECK(!GearGiveable(Kind::Weapon, 0));
    CHECK(!GearGiveable(Kind::Weapon, 99999999));
    CHECK(!GearGiveable(Kind::Armor, 5000500));    // a weapon id is not an armor row
    CHECK(!GearGiveable(Kind::Goods, 1000));
    CHECK(GearGiveable(Kind::Armor, 7100));
    CHECK(!GearGiveable(Kind::Armor, 8100));     // Gascoigne's set (shared lot)
    // the giver
    CHECK(GearGiverVerdict(Saw()) == Why::None);
    { GearFacts f = Saw(); f.held = false; CHECK(GearGiverVerdict(f) == Why::NotHeld); }
    { GearFacts f = Saw(); f.equipped = true; CHECK(GearGiverVerdict(f) == Why::GearEquipped); }
    { GearFacts f = Saw(9000000); CHECK(GearGiverVerdict(f) == Why::GearUnique); }
    { GearFacts f = Saw(9000000); f.equipped = true; CHECK(GearGiverVerdict(f) == Why::GearUnique); }  // the rule first
    { GearFacts f = Saw(); f.kind = Kind::Goods; CHECK(GearGiverVerdict(f) == Why::Failed); }
    // the receiver: the same table on its own data, gems checked, room
    GearGem g[2] = {OneGem(0), OneGem(2)};
    CHECK(GearReceiverVerdict(Saw(), 2, g) == Why::None);
    CHECK(GearReceiverVerdict(Saw(), 0, g) == Why::None);
    { GearFacts f = Saw(9000100); CHECK(GearReceiverVerdict(f, 0, g) == Why::GearUnique); }  // a modified giver
    { GearFacts f = Saw(); f.free_slots = 0; CHECK(GearReceiverVerdict(f, 0, g) == Why::NoRoom); }
    { GearFacts f = Saw(); f.free_slots = -1; CHECK(GearReceiverVerdict(f, 0, g) == Why::Failed); }
    {
        GearFacts f = Saw();
        f.kind = Kind::Armor;
        f.id = 7100;
        CHECK(GearReceiverVerdict(f, 0, g) == Why::None);
        CHECK(GearReceiverVerdict(f, 1, g) == Why::BadGems);
    }
    GearGem dup[2] = {OneGem(0), OneGem(0)};  // the same slot twice
    CHECK(GearReceiverVerdict(Saw(), 2, dup) == Why::BadGems);
    { GearGem b = OneGem(5); CHECK(GearReceiverVerdict(Saw(), 1, &b) == Why::BadGems); }
    { GearGem b = OneGem(0); b.data[0] = 9; CHECK(GearReceiverVerdict(Saw(), 1, &b) == Why::BadGems); }
    { GearGem b = OneGem(0); b.data[1] = 5000; CHECK(GearReceiverVerdict(Saw(), 1, &b) == Why::BadGems); }
    { GearGem b = OneGem(0); b.data[5] = -2; CHECK(GearReceiverVerdict(Saw(), 1, &b) == Why::BadGems); }
    { GearGem b = OneGem(0); b.id = 0; CHECK(GearReceiverVerdict(Saw(), 1, &b) == Why::BadGems); }
    CHECK(GearReceiverVerdict(Saw(), 6, g) == Why::BadGems);  // more gems than slots
    // the slot layout: a gem needs a slot that exists; masks and aux in range
    {
        std::uint32_t mk[kGearSlots] = {1, kNoSlot, 8, kNoSlot, kNoSlot};
        GearGem two[2] = {OneGem(0), OneGem(2)};
        CHECK(GearReceiverVerdict(Saw(), 2, two, mk, 1) == Why::None);
        GearGem three[2] = {OneGem(0), OneGem(1)};  // slot 1 has no shape
        CHECK(GearReceiverVerdict(Saw(), 2, three, mk, 1) == Why::BadGems);
        mk[3] = 0x10000;
        CHECK(GearReceiverVerdict(Saw(), 0, two, mk, 1) == Why::BadGems);
        mk[3] = kNoSlot;
        CHECK(GearReceiverVerdict(Saw(), 0, two, mk, 0x10000) == Why::BadGems);
        mk[3] = 0;
        CHECK(GearReceiverVerdict(Saw(), 0, two, mk, 1) == Why::BadGems);
    }
    // the gear switch is its own consent
    GateView v;
    v.enabled = true;
    v.world_up = true;
    CHECK(Gate(v, false, true) == Why::None);
    v.enabled = false;
    CHECK(Gate(v, false, true) == Why::GearOff);
    CHECK(Gate(v, true, true) == Why::TheirGearOff);
    CHECK(Gate(v, true, false) == Why::TheirOff);
    CHECK(WhyFromWord(WhyWord(Why::TheirGearOff)) == Why::TheirGearOff);
    CHECK(WhyFromWord(WhyWord(Why::BadGems)) == Why::BadGems);
    CHECK(WhyFromWord(WhyWord(Why::GearEquipped)) == Why::GearEquipped);
}

static void GearWire() {
    Msg m;
    m.op = Op::Offer;
    m.id = 77;
    m.from = 1;
    m.to = 0;
    m.goods = 5000500;
    m.n = 1;
    m.kind = Kind::Weapon;
    m.gem_count = 2;
    m.gems[0] = OneGem(0);
    m.gems[1] = OneGem(3);
    m.gems[1].data[1] = 1000;
    m.masks[0] = 1;
    m.masks[3] = 8;
    m.aux = 1;
    Msg r;
    CHECK(Decode(Encode(m), &r));
    CHECK(r.kind == Kind::Weapon && r.gem_count == 2 && r.goods == 5000500 && r.n == 1);
    CHECK(r.gems[0] == m.gems[0] && r.gems[1] == m.gems[1]);
    CHECK(r.masks[0] == 1 && r.masks[1] == kNoSlot && r.masks[3] == 8 && r.aux == 1);
    // no gems, armor, and a goods offer keeps no gear fields
    m.gem_count = 0;
    CHECK(Decode(Encode(m), &r) && r.kind == Kind::Weapon && r.gem_count == 0);
    m.kind = Kind::Armor;
    m.goods = 7100;
    CHECK(Decode(Encode(m), &r) && r.kind == Kind::Armor && r.goods == 7100);
    m = Msg();
    m.op = Op::Offer;
    m.id = 5;
    m.from = 0;
    m.to = 1;
    m.goods = 1000;
    m.n = 5;
    CHECK(Decode(Encode(m), &r) && r.kind == Kind::Goods && r.n == 5);
    // refused input: gear with n != 1, armor with gems, a bad kind, malformed or implausible gem strings
    const std::string head = "{\"op\":\"offer\",\"id\":1,\"from\":0,\"to\":1,";
    CHECK(!Decode(head + "\"goods\":5000500,\"n\":2,\"kind\":\"weapon\",\"gems\":\"\"}", &r));
    CHECK(!Decode(head + "\"goods\":7100,\"n\":1,\"kind\":\"armor\",\"gems\":\"0,71000,1,4,-1,-1,-1,-1,-1,-1\"}", &r));
    CHECK(!Decode(head + "\"goods\":5000500,\"n\":1,\"kind\":\"ring\"}", &r));
    CHECK(!Decode(head + "\"goods\":5000500,\"n\":1,\"kind\":\"weapon\",\"gems\":\"0,71000,1,4\"}", &r));
    CHECK(!Decode(head + "\"goods\":5000500,\"n\":1,\"kind\":\"weapon\",\"gems\":\"9,71000,1,4,-1,-1,-1,-1,-1,-1\"}", &r));
    CHECK(!Decode(head + "\"goods\":5000500,\"n\":1,\"kind\":\"weapon\",\"gems\":\"0,71000,1,4,-1,-1,-1,-1,-1,-1;0,71000,1,4,-1,-1,-1,-1,-1,-1\"}", &r));
    CHECK(Decode(head + "\"goods\":5000500,\"n\":1,\"kind\":\"weapon\",\"gems\":\"0,71000,1,4,-1,-1,-1,-1,-1,-1\",\"masks\":\"1,2147483648,2147483648,2147483648,2147483648,1\"}", &r) && r.gem_count == 1);
    // a gear offer with all five slots stays well inside the body limit
    m = Msg();
    m.op = Op::Offer;
    m.id = 9;
    m.from = 1;
    m.to = 0;
    m.goods = 5000500;
    m.n = 1;
    m.kind = Kind::Weapon;
    m.gem_count = 5;
    for (int i = 0; i < 5; ++i) {
        m.gems[i] = OneGem(i);
        m.gems[i].id = 99999999;
        m.gems[i].data[0] = 3;
        m.gems[i].data[1] = 1000;
        for (int k = 2; k < 8; ++k) m.gems[i].data[k] = 99999999;
        m.masks[i] = 0xffff;
    }
    CHECK(Encode(m).size() < 1000);
    CHECK(Decode(Encode(m), &r) && r.gem_count == 5 && r.gems[4] == m.gems[4]);
}

// The two machines carry the kind through the whole transfer (the receiver applies what it reserved; the giver refunds the same).
static void GearMachines() {
    Giver g;
    CHECK(g.Begin(5, 1, 5000500, 1, 0.0, Kind::Weapon) && g.kind() == Kind::Weapon);
    CHECK(g.OnReady(5, 1, 0.1).act == Act::Remove);
    CHECK(g.Removed(1, 0.2).act == Act::SendCommit);
    const Step d = g.OnDone(5, 1, 0, Why::Failed);  // the receiver could not build it: the giver gets it back
    CHECK(d.act == Act::Finished && d.refund == 1 && d.got == 0);
    Giver g2;
    CHECK(g2.Begin(6, 1, 5000500, 1, 0.0, Kind::Armor));
    CHECK(g2.OnReady(6, 1, 0.1).act == Act::Remove);
    CHECK(g2.Removed(0, 0.2).act == Act::SendAbort);  // the removal did not happen: nothing to refund
    Receiver r;
    CHECK(r.Reserve(1, 9, 5000500, 1, 0.0, Kind::Weapon) == Receiver::Result::Ok);
    const RStep a = r.OnCommit(1, 9, 0.5);
    CHECK(a.act == RAct::Apply && a.kind == Kind::Weapon && a.goods == 5000500 && a.n == 1);
    const RStep done = r.Applied(1, Why::None);
    CHECK(done.act == RAct::SendDone && done.got == 1 && done.kind == Kind::Weapon);
    CHECK(r.OnCommit(1, 9, 0.6).act == RAct::SendDone);  // a replay: the same DONE, never a second weapon
    CHECK(r.OnCommit(1, 9, 0.6).got == 1);
    Receiver r2;
    CHECK(r2.Reserve(1, 10, 7100, 1, 0.0, Kind::Armor) == Receiver::Result::Ok);
    CHECK(r2.OnAbort(1, 10).got == 0);
    CHECK(r2.OnCommit(1, 10, 0.1).got == 0);  // a COMMIT after the ABORT gives nothing
}

int main() {
    GearRules();
    GearWire();
    GearMachines();
    Allow();
    RoomRules();
    Moment();
    Wire();
    Happy();
    Refused();
    RemovalFails();
    ShortDone();
    Timeouts();
    Texts();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("party-give-test: ok");
    return 0;
}
