// SPDX-License-Identifier: GPL-3.0-or-later
// Party gestures (gpu/shim/party/party_gestures), the parts without the game: the gesture goods
// ids and their teaching flags, the EVENT "gestures" JSON, the owed store on disk and the
// own-world / summoned decision.
// Build and run: ninja -C out/gpu party-gestures-test && out/gpu/party-gestures-test.exe
#include "party/party_gestures.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace coop::gestures;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)

static void test_ids() {
    CHECK(IsGestureGoods(800) && IsGestureGoods(822));
    CHECK(!IsGestureGoods(799) && !IsGestureGoods(823) && !IsGestureGoods(4114) && !IsGestureGoods(0));
    // The talk scripts' teaching flags (ESD scan): 6740 + index, League Oath 6770, none for the
    // defaults and the item-lot gestures.
    CHECK(TeachFlag(805) == 6745 && TeachFlag(804) == 6744 && TeachFlag(820) == 6760 && TeachFlag(822) == 6770);
    CHECK(TeachFlag(800) == -1 && TeachFlag(803) == -1 && TeachFlag(808) == -1 && TeachFlag(812) == -1 &&
          TeachFlag(821) == -1 && TeachFlag(4114) == -1);
    for (std::uint32_t g = 800; g <= 822; ++g) {
        const std::int32_t f = TeachFlag(g);
        CHECK(f == -1 || (f >= 6000 && f <= 8999));  // personal block: kept in the own pool when summoned
    }
    CHECK(std::string(GestureName(800)) == "Point Forward" && std::string(GestureName(822)) == "League Oath");
    CHECK(std::string(GestureName(4114)) == "?");
}

static void test_json() {
    std::vector<std::uint32_t> out;
    CHECK(FromJsonText(ToJsonText({805, 822}), &out) && out == (std::vector<std::uint32_t>{805, 822}));
    // Anything but gesture goods is dropped, duplicates and fractions too.
    CHECK(FromJsonText("{\"goods\":[805,805,4114,-1,799,823,810.5,\"x\",810]}", &out) &&
          out == (std::vector<std::uint32_t>{805, 810}));
    std::string err;
    CHECK(!FromJsonText("[805]", &out, &err) && !err.empty());
    CHECK(!FromJsonText("{\"goods\":805}", &out));
    std::string big = "{\"goods\":[";
    for (int i = 0; i < 65; ++i) big += (i ? "," : "") + std::to_string(800 + i % 23);
    CHECK(!FromJsonText(big + "]}", &out));
    CHECK(FromJsonText("{\"goods\":[]}", &out) && out.empty());
}

static void test_entries() {
    // GestureGameData entries: display index << 16 | param id (gesture index + 1) << 1 | learned.
    // The test save (userdata0000 +0x19120) learned entries 0..3, 8, 9, 12, 13, 21.
    bool on = false;
    CHECK(EntryGoods((0u << 16) | (1u << 1) | 1u, &on) == 800 && on);
    CHECK(EntryGoods((5u << 16) | (6u << 1), &on) == 805 && !on);
    CHECK(EntryGoods((22u << 16) | (23u << 1) | 1u, &on) == 822 && on);
    CHECK(EntryGoods(0xffff0000u) == 0 && EntryGoods(0x0003fffeu) == 0 && EntryGoods(0x0003ffffu) == 0);
    CHECK(EntryGoods(0x00170030u) == 0);  // param id 24: past the table (the save's entries 23..29)
    CHECK(EntryGoods(0) == 0);            // param id 0: the table's empty row
}

static void test_store() {
    Store s;
    CHECK(s.Add(0, 805) && !s.Add(0, 805) && s.Add(1, 805, true) && !s.Add(0, 4114));
    s.Find(0, 805)->summoned = true;
    const Store back = Store::Parse(s.Dump());
    CHECK(back.owed.size() == 2);
    CHECK(back.owed[0].slot == 0 && back.owed[0].goods == 805 && back.owed[0].summoned);
    CHECK(back.owed[1].slot == 1 && back.owed[1].goods == 805 && back.owed[1].summoned);
    // A torn or foreign line is skipped; the rest stays (the first of a duplicate wins).
    const Store torn = Store::Parse("owed 0 80 0\nowed 2 4114 0\nowed 3 806 0\nnonsense\nowed 3\nowed 3 806 1\n"
                                    "known 2 801\nowed 500 807 0\nowed -1 808 1\n");
    CHECK(torn.owed.size() == 2 && torn.owed[0].goods == 806 && !torn.owed[0].summoned);
    CHECK(torn.owed[1].slot == -1 && torn.owed[1].goods == 808 && torn.owed[1].summoned);
    s.Drop(0, 805);
    CHECK(s.owed.size() == 1 && s.owed[0].slot == 1);
    CHECK(Store::Parse("").owed.empty() && Store().Dump().empty());
}

static void test_newly_owed() {
    Store s;
    s.Add(0, 802);
    s.Add(1, 806);
    const std::vector<std::uint32_t> peer = {800, 802, 805, 805, 806, 4114};
    // 800 learned, 802 owed already; 806 is owed to another save slot only.
    CHECK(NewlyOwed(peer, {800}, s, 0) == (std::vector<std::uint32_t>{805, 806}));
    CHECK(NewlyOwed(peer, {}, s, 1) == (std::vector<std::uint32_t>{800, 802, 805}));
    CHECK(NewlyOwed({}, {}, s, 0).empty());
}

static void test_decide() {
    // Own world: the bit is the save's.
    CHECK(Decide(true, 1, false) == Step::Have);
    CHECK(Decide(true, 1, true) == Step::Have);    // learned while summoned, kept
    CHECK(Decide(true, 0, false) == Step::Learn);
    CHECK(Decide(true, 0, true) == Step::Learn);   // learned while summoned, lost: again
    CHECK(Decide(true, -1, false) == Step::Wait);
    // Summoned: learned at once (the character data is the guest's own there).
    CHECK(Decide(false, 0, false) == Step::Learn);
    CHECK(Decide(false, 1, false) == Step::Have);
    CHECK(Decide(false, -1, false) == Step::Wait);
    // Learned while summoned: owed until the own world reads it.
    CHECK(Decide(false, 1, true) == Step::Wait);
    CHECK(Decide(false, 0, true) == Step::Learn);  // lost again (a restart before the save): again
}

int main() {
    test_ids();
    test_json();
    test_entries();
    test_store();
    test_newly_owed();
    test_decide();
    std::printf("party-gestures-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
