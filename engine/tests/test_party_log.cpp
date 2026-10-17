// SPDX-License-Identifier: GPL-3.0-or-later
// Party logging levels, areas, tags, rate limits and mark lines (gpu/shim/party/party_log.h).
// Build and run: ninja -C out/gpu party-log-test && out/gpu/party-log-test.exe
#include "party/party_log.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

using namespace coop::plog;

static void TestLevels() {
    CHECK(ParseConfig(nullptr, nullptr, nullptr).level == Level::Normal);
    CHECK(ParseConfig("", "", "").level == Level::Normal);
    CHECK(ParseConfig("normal", nullptr, nullptr).level == Level::Normal);
    CHECK(ParseConfig("0", nullptr, nullptr).level == Level::Normal);
    CHECK(ParseConfig("off", nullptr, nullptr).level == Level::Normal);
    CHECK(ParseConfig("bogus", nullptr, nullptr).level == Level::Normal);
    CHECK(ParseConfig("verbose", nullptr, nullptr).level == Level::Verbose);
    CHECK(ParseConfig(" Verbose\r\n", nullptr, nullptr).level == Level::Verbose);
    CHECK(ParseConfig("1", nullptr, nullptr).level == Level::Verbose);
    CHECK(ParseConfig("TRACE", nullptr, nullptr).level == Level::Trace);
    CHECK(ParseConfig("2", nullptr, nullptr).level == Level::Trace);
    CHECK(std::strcmp(LevelName(Level::Normal), "normal") == 0);
    CHECK(std::strcmp(LevelName(Level::Verbose), "verbose") == 0);
    CHECK(std::strcmp(LevelName(Level::Trace), "trace") == 0);
    // Snapshot period
    CHECK(ParseConfig(nullptr, nullptr, nullptr).snapshot_s == 10);
    CHECK(ParseConfig(nullptr, nullptr, "30").snapshot_s == 30);
    CHECK(ParseConfig(nullptr, nullptr, "0").snapshot_s == 0);
    CHECK(ParseConfig(nullptr, nullptr, "-5").snapshot_s == 10);
    CHECK(ParseConfig(nullptr, nullptr, "x").snapshot_s == 10);
    CHECK(ParseConfig(nullptr, nullptr, "999999").snapshot_s == 3600);
}

static void TestAreas() {
    CHECK(ParseConfig("verbose", nullptr, nullptr).areas == kAllAreas);
    CHECK(ParseConfig("verbose", "all", nullptr).areas == kAllAreas);
    CHECK(ParseConfig("verbose", "net", nullptr).areas == kNet);
    CHECK(ParseConfig("verbose", "net,summon", nullptr).areas == (kNet | kSummon));
    CHECK(ParseConfig("verbose", " Travel ; DEATH ", nullptr).areas == (kTravel | kDeath));
    CHECK(ParseConfig("verbose", "items,flags,dialog", nullptr).areas == (kItems | kFlags | kDialog));
    CHECK(ParseConfig("verbose", "net,typo", nullptr).areas == kNet);
    CHECK(ParseConfig("verbose", "typo", nullptr).areas == kAllAreas);  // a typo alone does not silence
    CHECK(ParseConfig("verbose", "none", nullptr).areas == 0);
    CHECK(AreasText(kAllAreas) == "all");
    CHECK(AreasText(0) == "none");
    CHECK(AreasText(kNet | kDialog) == "net,dialog");
    CHECK(AreaOfEvent("travel") == kTravel);
    CHECK(AreaOfEvent("lamp_request") == kTravel);
    CHECK(AreaOfEvent("b2") == kSummon);
    CHECK(AreaOfEvent("phantom") == kDeath);
    CHECK(AreaOfEvent("items_full") == kItems);
    CHECK(AreaOfEvent("drop_seed") == kItems);
    CHECK(AreaOfEvent("flags") == kFlags);
    CHECK(AreaOfEvent("flag_snapshot") == kFlags);
    CHECK(AreaOfEvent("world_action") == kFlags);
    CHECK(AreaOfEvent("plog") == kNet);
    CHECK(AreaOfEvent("whatever") == kNet);
}

static void TestTagsAndOn() {
    Config c;
    c.level = Level::Normal;
    SetConfigForTest(c);
    CHECK(!Verbose());
    CHECK(Tag().empty());
    CHECK(!On(kNet));
    c.level = Level::Verbose;
    c.areas = kNet | kItems;
    SetConfigForTest(c);
    CHECK(Verbose() && !TraceOn());
    CHECK(On(kNet) && On(kItems) && !On(kTravel));
    CHECK(On(kTravel | kItems));
    CHECK(!On(kNet, Level::Trace));
    SetPeer('G', 1);
    SetPartyClockOffset(true, 5000);
    const std::string tag = Tag();
    CHECK(tag.rfind("{run " + RunId() + " G1 t+", 0) == 0);
    CHECK(tag.find(" pc ") != std::string::npos);
    CHECK(tag.find(" pc -") == std::string::npos);
    CHECK(tag.back() == ' ');
    CHECK(PartyClockMs() >= 5000);
    SetPartyClockOffset(false, 0);
    CHECK(PartyClockMs() == -1);
    CHECK(Tag().find(" pc - ") != std::string::npos);
    SetPeer('-', -1);
    CHECK(Tag().find(" -- t+") != std::string::npos);
    CHECK(RunId().size() == 6);
    // TagLine: after the first ": " of a "Party..." line only.
    CHECK(TagLine("Party B2: guest arrived\n", "{T} ") == "Party B2: {T} guest arrived\n");
    CHECK(TagLine("Party: x: y\n", "{T} ") == "Party: {T} x: y\n");
    CHECK(TagLine("Boot: fine\n", "{T} ") == "Boot: fine\n");
    CHECK(TagLine("=== USER MARK 1 === x\n", "{T} ") == "=== USER MARK 1 === x\n");
    CHECK(TagLine("Party B2: y\n", "") == "Party B2: y\n");
    CHECK(TagLine("Party without colon\n", "{T} ") == "Party without colon\n");
}

static void TestRateLimit() {
    RateLimit r(3, 1000);
    int sup = -1;
    CHECK(r.Allow(0, &sup) && sup == 0);
    CHECK(r.Allow(10));
    CHECK(r.Allow(20));
    CHECK(!r.Allow(30));
    CHECK(!r.Allow(999));
    CHECK(r.Allow(1000, &sup) && sup == 2);  // the next window reports the two dropped
    CHECK(r.Allow(1001, &sup) && sup == 0);
}

static void TestMarkLine() {
    const std::string l = MarkLine(3, "Hunter0 (slot 0)", "F9", 123456, "19:49:49.123");
    CHECK(l == "=== USER MARK 3 === Hunter0 (slot 0), F9; local 19:49:49.123, party clock 123.456 s");
    const std::string m = MarkLine(1, "", "Party tab", -1, "01:02:03.004");
    CHECK(m.rfind("=== USER MARK 1 === this player, Party tab;", 0) == 0);
    CHECK(m.find("unknown (no party link)") != std::string::npos);
}

int main() {
    TestLevels();
    TestAreas();
    TestTagsAndOn();
    TestRateLimit();
    TestMarkLine();
    std::printf("party-log-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
