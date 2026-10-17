// SPDX-License-Identifier: GPL-3.0-or-later
// D4b shared enemy drop roll (gpu/shim/party/party_droproll), the parts without the game: the
// per-enemy seed, the MT19937 seeding the game RNG gets, the EVENT "drop_seed" JSON, the switches and
// the game's lot row pick with each player's own item discovery.
// Build and run: cmake --build out/gpu --target party-droproll-test && out/gpu/party-droproll-test.exe
#include "party/party_droproll.h"

#include <cstdio>
#include <set>
#include <string>

using namespace coop::droproll;

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

static DropKey Enemy(std::int32_t entity, std::uint32_t handle = 0x14005) {
    DropKey k;
    k.map = 0x18010000; // m24_01
    k.entity = entity;
    k.entity2 = entity;
    k.handle = handle;
    k.lot = 240000000 + entity % 1000;
    return k;
}

// The host and every guest compute the same seed for the same kill.
static void test_seed_shared() {
    const std::uint64_t salt = 0x1234567890abcdefull;
    CHECK(DropRollSeed(salt, 3, Enemy(2410700)) == DropRollSeed(salt, 3, Enemy(2410700)));
    // An enemy with entity ids: the chr table slot does not matter (it may differ between games).
    CHECK(DropRollSeed(salt, 3, Enemy(2410700, 0x14005)) == DropRollSeed(salt, 3, Enemy(2410700, 0x18009)));
    // The census key (+0x3B8) differs between games: it never changes the seed.
    DropKey other_key = Enemy(2410700);
    other_key.entity2 = 77;
    CHECK(DropRollSeed(salt, 3, Enemy(2410700)) == DropRollSeed(salt, 3, other_key));
    // Without an entity id the slot is all there is.
    CHECK(DropRollSeed(salt, 3, Enemy(0, 0x14005)) != DropRollSeed(salt, 3, Enemy(0, 0x14006)));
    CHECK(DropRollSeed(salt, 3, Enemy(0, 0x14005)) == DropRollSeed(salt, 3, Enemy(0, 0xff814005)));
}

// Another enemy, another respawn (generation) or another host run rolls anew.
static void test_seed_varies() {
    const std::uint64_t salt = 0x1234567890abcdefull;
    std::set<std::uint32_t> seen;
    for (std::uint32_t gen = 1; gen <= 50; ++gen) {
        for (std::int32_t e = 2410700; e < 2410720; ++e) {
            seen.insert(DropRollSeed(salt, gen, Enemy(e)));
        }
    }
    CHECK(seen.size() == 1000); // no collisions in a realistic set
    CHECK(DropRollSeed(salt, 1, Enemy(2410700)) != DropRollSeed(salt + 1, 1, Enemy(2410700)));
    DropKey other_map = Enemy(2410700);
    other_map.map = 0x18000000;
    CHECK(DropRollSeed(salt, 1, Enemy(2410700)) != DropRollSeed(salt, 1, other_map));
    DropKey other_lot = Enemy(2410700);
    other_lot.lot += 1;
    CHECK(DropRollSeed(salt, 1, Enemy(2410700)) != DropRollSeed(salt, 1, other_lot));
}

// The seeding is the reference MT19937 (the game's RNG at GSM +0xA78 refills with 0x181E5C0 and
// tempers like the reference).
static void test_mt() {
    CHECK(Mt19937First(5489) == 3499211612u); // the reference generator's first output
    std::uint32_t st[624];
    SeedMt19937(st, 5489);
    CHECK(st[0] == 5489 && st[1] == 1301868182u);
    CHECK(Mt19937First(1) == 1791095845u);
}

static void test_json() {
    DropSeed s;
    s.salt = 0xfedcba9876543210ull;
    s.gen = 7;
    s.discovery = 110.5f;
    DropSeed back;
    std::string err;
    CHECK(DropSeedFromJson(DropSeedToJson(s), &back, &err));
    CHECK(back == s);
    s.discovery = 0;
    CHECK(DropSeedFromJson(DropSeedToJson(s), &back, &err) && back == s);
    // Refused: salt 0, discovery out of range, garbage.
    DropSeed bad = s;
    bad.salt = 0;
    CHECK(!DropSeedFromJson(DropSeedToJson(bad), &back, &err));
    bad = s;
    bad.discovery = 5000.0f;
    CHECK(!DropSeedFromJson(DropSeedToJson(bad), &back, &err));
    bad.discovery = 0.5f;
    CHECK(!DropSeedFromJson(DropSeedToJson(bad), &back, &err));
    CHECK(!DropSeedFromJson("{\"salt\":\"0x1\",\"gen\":-1}", &back, &err));
    CHECK(!DropSeedFromJson("[1,2]", &back, &err));
    CHECK(!DropSeedFromJson("not json", &back, &err));
}

// Item discovery per player: the same seed (the same draw) for two players, each with its own
// discovery. Row shapes from 1.09 ItemLotParam enemy lots.
static LotSlot Slot(std::uint16_t base, bool luck) {
    LotSlot s;
    s.base = base;
    s.luck = luck;
    return s;
}

static void test_discovery_per_player() {
    // "Nothing" 950 (no luck) then one rare item 50 (luck): the common enemy drop shape.
    const LotSlot rare[8] = {Slot(950, false), Slot(50, true)};
    // Blood Vial 90 (no luck) or Quicksilver Bullet 10 (luck): lot 9906550.
    const LotSlot vial_or_bullet[8] = {Slot(90, false), Slot(10, true)};
    // Nothing, then two luck items: the boundary between the two moves with discovery.
    const LotSlot two_luck[8] = {Slot(800, false), Slot(150, true), Slot(50, true)};
    // Nothing between two luck items: the one-luck-slot rule still holds for each.
    const LotSlot split[8] = {Slot(40, true), Slot(900, false), Slot(60, false)};

    // Weights: the luck slot x discovery (x100 points), the others not.
    std::uint32_t w[8];
    CHECK(LotRowWeights(rare, 0.0f, 1.0f, w) == 100000 && w[0] == 95000 && w[1] == 5000);
    CHECK(LotRowWeights(rare, 0.0f, 2.1f, w) == 95000 + 10500 && w[1] == 10500);
    LotSlot taken[8] = {Slot(950, false), Slot(50, true)};
    taken[1].taken = true;
    CHECK(LotRowWeights(taken, 0.0f, 2.1f, w) == 95000 && w[1] == 0);
    const LotSlot empty[8] = {};
    CHECK(LotRowPick(empty, 0.0f, 1.0f, 12345) == -1);

    // One shared roll per kill: the same seed gives both games the same draw.
    const std::uint64_t salt = 0x1234567890abcdefull;
    int low_items = 0, high_items = 0, high_only = 0, rare_lost = 0, nothing_instead = 0, other_luck = 0;
    int vial_to_bullet = 0, bullet_lost = 0;
    for (std::int32_t e = 2410000; e < 2410000 + 20000; ++e) {
        const std::uint32_t draw = Mt19937First(DropRollSeed(salt, 1, Enemy(e)));
        CHECK(draw == Mt19937First(DropRollSeed(salt, 1, Enemy(e))));
        // Arcane < 8 (1.0) vs Arcane 50 (2.1), 1.7 vs 2.1 + an Eye-rune-like bonus, and equal.
        const float pairs[][2] = {{1.0f, 2.1f}, {1.7f, 2.4f}, {1.0f, 1.0f}};
        for (const auto& p : pairs) {
            const int lo = LotRowPick(rare, 0.0f, p[0], draw), hi = LotRowPick(rare, 0.0f, p[1], draw);
            if (p[0] == p[1]) {
                CHECK(lo == hi);  // equal discovery: equal result (the 0.6.1 behaviour)
                continue;
            }
            low_items += lo == 1;
            high_items += hi == 1;
            high_only += lo == 0 && hi == 1;
            rare_lost += lo == 1 && hi != 1;
            const int vlo = LotRowPick(vial_or_bullet, 0.0f, p[0], draw);
            const int vhi = LotRowPick(vial_or_bullet, 0.0f, p[1], draw);
            bullet_lost += vlo == 1 && vhi != 1;
            vial_to_bullet += vlo == 0 && vhi == 1;
            const int tlo = LotRowPick(two_luck, 0.0f, p[0], draw), thi = LotRowPick(two_luck, 0.0f, p[1], draw);
            nothing_instead += tlo > 0 && thi == 0;
            other_luck += tlo > 0 && thi > 0 && tlo != thi;
            const int slo = LotRowPick(split, 0.0f, p[0], draw), shi = LotRowPick(split, 0.0f, p[1], draw);
            rare_lost += slo == 0 && shi != 0;
        }
    }
    // Higher discovery: the rare item whenever the lower one got it, plus sometimes when not.
    CHECK(rare_lost == 0);
    CHECK(high_only > 0 && high_items == low_items + high_only);
    CHECK(high_items > low_items);
    // Rough rates: 5% at 1.0, ~10% at 2.1 (20000 kills x 2 pairs).
    CHECK(low_items > 2400 && low_items < 3600 && high_items > 4000 && high_items < 5600);
    // Discovery trades the common item for the rare one, never the other way round.
    CHECK(bullet_lost == 0 && vial_to_bullet > 0);
    // Two luck items: never "nothing" instead of an item, but the same draw can give the
    // higher-discovery player the other luck item (the boundary between them moves).
    CHECK(nothing_instead == 0);
    CHECK(other_luck > 0);
}

static void test_discovery_switch() {
    CHECK(!HostDiscoveryFromEnv(nullptr));
    CHECK(!HostDiscoveryFromEnv(""));
    CHECK(!HostDiscoveryFromEnv("own"));
    CHECK(HostDiscoveryFromEnv("host"));
}

// BB_PARTY_SHARED_DROP_ROLL: on by default, 0 = the old own roll per player.
static void test_switch() {
    CHECK(SharedRollFromEnv(nullptr));
    CHECK(SharedRollFromEnv(""));
    CHECK(SharedRollFromEnv("1"));
    CHECK(!SharedRollFromEnv("0"));
}

int main() {
    test_seed_shared();
    test_seed_varies();
    test_mt();
    test_json();
    test_switch();
    test_discovery_per_player();
    test_discovery_switch();
    std::printf("party-droproll-test: %d checks, %d failures\n", g_checks, g_failures);
    // pard3: a kill before the guest's world is up rolls nothing on the guest.
    CHECK(coop::droproll::DropRollBeforeArrival(false, false, 0x18000000u));
    CHECK(coop::droproll::DropRollBeforeArrival(true, true, 0x18000000u));
    CHECK(coop::droproll::DropRollBeforeArrival(true, false, 0xffffffffu));
    CHECK(!coop::droproll::DropRollBeforeArrival(true, false, 0x18000000u));
    return g_failures ? 1 : 0;
}
