// SPDX-License-Identifier: GPL-3.0-or-later
// Shared NPC dialogue (gpu/shim/party/party_npctalk.h): event JSON, speaker names, range,
// distance decision, dedupe of lines the listener's game played itself, talk menu choices.
#include "party/party_npctalk.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace coop::npctalk;

static void TestJson() {
    TalkEvent e;
    e.kind = Kind::Line;
    e.npc = 2410782;
    e.talk = 1900002;
    e.msg = 1900002;
    e.voice = 1900002;
    e.map = 0x18010000;
    e.has_pos = true;
    e.pos[0] = -12.345f;
    e.pos[1] = 3.0f;
    e.pos[2] = 88.0f;
    e.who = "Hunter1";
    e.slot = 1;
    const std::string t = ToJsonText(e);
    TalkEvent b;
    std::string err;
    assert(FromJsonText(t, &b, &err));
    assert(b.kind == Kind::Line && b.npc == e.npc && b.talk == e.talk && b.msg == e.msg && b.voice == e.voice);
    assert(b.map == e.map && b.has_pos && b.pos[0] < -12.33f && b.pos[0] > -12.36f && b.who == "Hunter1" && b.slot == 1);

    TalkEvent g;
    g.kind = Kind::Gift;
    g.npc = 2410782;
    g.lot = 10050;
    g.item_cat = 4;
    g.item_id = 4312;
    assert(FromJsonText(ToJsonText(g), &b, &err) && b.kind == Kind::Gift && b.lot == 10050 && b.item_cat == 4 &&
           b.item_id == 4312 && b.slot == -1 && b.who.empty());

    // Refused: unknown kind, no NPC, bad names, bad positions, junk.
    assert(!FromJsonText("{\"k\":\"shout\",\"npc\":1}", &b, &err));
    assert(!FromJsonText("{\"k\":\"line\",\"npc\":0}", &b, &err));
    assert(!FromJsonText("{\"k\":\"line\",\"npc\":5,\"who\":\"a b\"}", &b, &err));
    assert(!FromJsonText("{\"k\":\"line\",\"npc\":5,\"who\":\"ThisNameIsWayTooLong\"}", &b, &err));
    assert(!FromJsonText("{\"k\":\"line\",\"npc\":5,\"pos\":[1,2]}", &b, &err));
    assert(!FromJsonText("{\"k\":\"line\",\"npc\":5,\"pos\":[1,2,1e9]}", &b, &err));
    assert(!FromJsonText("{\"k\":\"line\",\"npc\":-5}", &b, &err));
    assert(!FromJsonText("{\"k\":\"line\",\"npc\":5,\"voice\":1000000000}", &b, &err));
    assert(!FromJsonText("{\"k\":\"gift\",\"npc\":5,\"cat\":99}", &b, &err));
    assert(!FromJsonText("[1,2]", &b, &err) && !FromJsonText("nope", &b, &err));
    assert(FromJsonText("{\"k\":\"end\",\"npc\":5}", &b, &err) && b.kind == Kind::End);
}

static void TestNames() {
    assert(std::string(NpcNameForTalk(1900002)) == "Gilbert");
    assert(std::string(NpcNameForTalk(1300100)) == "Iosefka");
    assert(std::string(NpcNameForTalk(1400000)) == "Iosefka"); // the clinic door's "Oh, well, hello..."
    assert(std::string(NpcNameForTalk(400000)) == "the Doll");
    assert(std::string(NpcNameForTalk(500002)) == "Gehrman");
    assert(NpcNameForTalk(4000000) == nullptr && NpcNameForTalk(0) == nullptr);
    assert(CaptionFor("Hunter1", 1900002) == "Hunter1 talking to Gilbert");
    assert(CaptionFor("", 99) == "A hunter talking to someone");
}

static void TestRange() {
    assert(RangeFromEnv(nullptr, 25) == 25 && RangeFromEnv("", 25) == 25 && RangeFromEnv("40", 25) == 40);
    assert(RangeFromEnv("0", 25) == 0 && RangeFromEnv("x", 25) == 25 && RangeFromEnv("-3", 25) == 25);
    assert(RangeFromEnv("9999", 25) == 500 && RangeFromEnv(nullptr, 900) == 500);
    assert(EnabledFromEnv(nullptr, true) && !EnabledFromEnv("0", true) && EnabledFromEnv("1", false));
    assert(!EnabledFromEnv(nullptr, false) && EnabledFromEnv("yes", true));
    for (int p = 0; p <= 10; ++p) assert(RangePosition(RangeAtPosition(p)) == p);
    assert(RangePosition(25) == 4 && RangePosition(0) == 10 && RangePosition(27) == 4 && RangePosition(500) == 9);
    assert(RangeAtPosition(-1) == 5 && RangeAtPosition(99) == 0);
    assert(DescribeRange(25) == "within 25 m of the NPC" && DescribeRange(0) == "anywhere on the same map");
}

static void TestLineSeconds() {
    assert(LineSeconds(0) == 2.5 && LineSeconds(500) == 10.0);
    const double s = LineSeconds(34); // "Then, if you find any survivors..."
    assert(s > 4.0 && s < 4.5);
    // dlast D8: a listener keeps the line until the talker's next line / end; the limit only
    // covers a lost end (the talker's own 30 s line limit + 2 s), never below the estimate.
    assert(RemoteLineLimit(34) == 32.0 && RemoteLineLimit(0) == 32.0 && RemoteLineLimit(500) == 32.0);
    assert(RemoteLineLimit(34) > LineSeconds(34) + 1.1);
}

static void TestDecide() {
    assert(Decide(true, true, true, 10.0f, 25) == Reach::Share);
    assert(Decide(true, true, true, 25.0f, 25) == Reach::Share);
    assert(Decide(true, true, true, 25.5f, 25) == Reach::FarNotice);
    assert(Decide(true, true, true, 300.0f, 0) == Reach::Share);   // whole map
    assert(Decide(true, true, true, -1.0f, 25) == Reach::Share);   // distance unknown
    assert(Decide(true, true, false, 1.0f, 25) == Reach::Ignore);  // another map
    assert(Decide(true, false, true, 1.0f, 25) == Reach::Ignore);  // another world
    assert(Decide(false, true, true, 1.0f, 25) == Reach::Ignore);  // off
}

static void TestRecent() {
    RecentLines r;
    r.Note(2410782, 1900002, 100.0);
    assert(r.Recent(2410782, 1900002, 101.0) && r.Recent(2410782, 1900002, 120.0));
    assert(!r.Recent(2410782, 1900002, 120.5));
    assert(!r.Recent(2410782, 1900003, 101.0) && !r.Recent(2410700, 1900002, 101.0));
    for (int i = 0; i < 100; ++i) r.Note(1, std::uint32_t(i), 200.0);
    assert(r.Recent(1, 99, 200.0) && !r.Recent(1, 0, 200.0)); // bounded
}

static void TestChoices() {
    ChoiceTracker t;
    std::uint32_t msg = 0, npc = 0;
    const std::uint32_t kShow = 10, kTalk = 1;
    // Clear, three rows, the menu, then the branch's first command: row 1 chosen.
    assert(!t.OnCommand(ChoiceTracker::kClear, 7, -1, -1, 0, &msg, &npc));
    assert(!t.OnCommand(ChoiceTracker::kAdd, 7, 0, 15000100, 0, &msg, &npc));
    assert(!t.OnCommand(ChoiceTracker::kAdd, 7, 1, 15000110, 0, &msg, &npc));
    assert(!t.OnCommand(ChoiceTracker::kAdd, 7, 2, 15000005, 0, &msg, &npc));
    assert(t.Rows() == 3);
    assert(!t.OnCommand(kShow, 7, -1, -1, 0, &msg, &npc));
    assert(t.OnCommand(kTalk, 7, -1, -1, 1, &msg, &npc) && msg == 15000110 && npc == 7);
    // Only once.
    assert(!t.OnCommand(kTalk, 7, -1, -1, 1, &msg, &npc));
    // A cancelled menu (result not a row) says nothing; another NPC's commands neither.
    assert(!t.OnCommand(ChoiceTracker::kClear, 7, -1, -1, 0, &msg, &npc));
    assert(!t.OnCommand(ChoiceTracker::kAdd, 7, 0, 15000100, 0, &msg, &npc));
    assert(!t.OnCommand(kShow, 7, -1, -1, 0, &msg, &npc));
    assert(!t.OnCommand(kTalk, 7, -1, -1, -1, &msg, &npc));
    assert(!t.OnCommand(ChoiceTracker::kClear, 7, -1, -1, 0, &msg, &npc));
    assert(!t.OnCommand(ChoiceTracker::kAdd, 7, 0, 15000100, 0, &msg, &npc));
    assert(!t.OnCommand(kShow, 8, -1, -1, 0, &msg, &npc)); // another NPC: not the menu
    assert(!t.OnCommand(kShow, 7, -1, -1, 0, &msg, &npc));
    // The next menu's Clear right after the choice still reads it.
    assert(t.OnCommand(ChoiceTracker::kClear, 7, -1, -1, 0, &msg, &npc) && msg == 15000100);
    assert(t.Rows() == 0);
    // Live (the Doll, 2026-10-14): Clear, 4 rows, the menu (0xA), then the ESD's wait command 0x44
    // every frame with the result unchanged (-1) until the row is decided, then the branch.
    ChoiceTracker d;
    assert(!d.OnCommand(ChoiceTracker::kClear, 210304, -1, -1, -1, &msg, &npc));
    for (int i = 0; i < 4; ++i) assert(!d.OnCommand(ChoiceTracker::kAdd, 210304, i, 15000100 + i, -1, &msg, &npc));
    assert(!d.OnCommand(kShow, 210304, -1, -1, -1, &msg, &npc));
    for (int i = 0; i < 40; ++i) assert(!d.OnCommand(0x44, 210304, -1, -1, -1, &msg, &npc));
    assert(!d.OnCommand(0x44, 210693, -1, -1, 1, &msg, &npc)); // another NPC's script in between
    assert(d.OnCommand(0, 210304, -1, -1, 1, &msg, &npc) && msg == 15000101 && npc == 210304);
    // The same row again (the result stays 1): read when the next menu is cleared.
    for (int i = 0; i < 4; ++i) assert(!d.OnCommand(ChoiceTracker::kAdd, 210304, i, 15000100 + i, 1, &msg, &npc));
    assert(!d.OnCommand(kShow, 210304, -1, -1, 1, &msg, &npc));
    for (int i = 0; i < 10; ++i) assert(!d.OnCommand(0x44, 210304, -1, -1, 1, &msg, &npc));
    assert(d.OnCommand(ChoiceTracker::kClear, 210304, -1, -1, 1, &msg, &npc) && msg == 15000101);
}

static void TestRate() {
    EventRate r;
    for (int i = 0; i < EventRate::kPerWindow; ++i) assert(r.Take(1, 5.0));
    assert(!r.Take(1, 6.0) && r.Take(2, 6.0));
    assert(r.Take(1, 15.5));
    assert(!r.Take(-1, 1.0) && !r.Take(8, 1.0));
}

int main() {
    TestJson();
    TestNames();
    TestRange();
    TestDecide();
    TestLineSeconds();
    TestRecent();
    TestChoices();
    TestRate();
    std::printf("party-npctalk-test: all passed\n");
    return 0;
}
