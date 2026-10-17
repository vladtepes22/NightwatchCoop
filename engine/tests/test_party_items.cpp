// SPDX-License-Identifier: GPL-3.0-or-later
// Party items (gpu/shim/party/party_items, phase C3), the parts without the game: the lot / flag /
// ledger tables, the deny rules, award and flag classification, the EVENT "items" JSON round trip,
// the host's send-once ledger and the guest's queue / apply gate with an injected game state.
// Build and run: ninja -C out/gpu party-items-test && out/gpu/party-items-test.exe
#include "party/party_items.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace coop;

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

static void test_tables() {
    CHECK(ItemLotFlagCount() > 1000);
    CHECK(ItemLedgerCount() >= 40);
    // Treasure: flag = 50,000,000 + lot (docs/party/items.md 2.1; probe I2's Hunter Chief Emblem).
    CHECK(ItemLotFlag(2400450) == 52400450);
    CHECK(ItemLotForFlag(52400450) == 2400450);
    CHECK(ItemLotFlag(2300090) == 52300090); // the BB_PARTY_ITEMS_TEST Blood Vial
    // Non-treasure rows: flag != 50M + lot.
    CHECK(ItemLotFlag(2700990) == 52700950);
    CHECK(ItemLotForFlag(52700950) == 2700990);
    CHECK(ItemLotFlag(80000000) == 5000);
    CHECK(ItemLotForFlag(5000) == 80000000);
    // Shared flag (NG variants / chains): the lowest lot.
    CHECK(ItemLotFlag(33000) == 50001700 && ItemLotFlag(33010) == 50001700);
    CHECK(ItemLotForFlag(50001700) == 33000);
    CHECK(ItemLotForFlag(6622) == 10010); // 10010 / 10011 chain
    // Flagless boss lots are not in the flag table.
    CHECK(ItemLotFlag(31000) == kItemNone);
    CHECK(ItemLotFlag(50000001) == kItemNone);
    CHECK(ItemLotForFlag(12345) == kItemNone);
    CHECK(ItemLotForFlag(-1) == kItemNone);
    // Chalice Dungeons are not in the table.
    CHECK(ItemLotFlag(110001000) == kItemNone);
    // Ledger: stable indices (append only), done flag = the awarding event's completion flag.
    CHECK(ItemLedgerIndex(31000) == 25);           // Oedon Tomb Key, Gascoigne
    CHECK(ItemLedgerDoneFlag(25) == 12411800);
    CHECK(ItemLedgerLot(25) == 31000);
    CHECK(ItemLedgerIndex(50000001) == 35);        // Gold Pendant, Cleric Beast
    CHECK(ItemLedgerDoneFlag(35) == 12401800);
    CHECK(ItemLedgerIndex(14000) == 0);
    CHECK(ItemLedgerIndex(22000) >= 0 && ItemLedgerDoneFlag(ItemLedgerIndex(22000)) == 9045); // common 9040 slot 5
    CHECK(ItemLedgerIndex(17010) == kItemNone);    // repeatable (common 9100)
    CHECK(ItemLedgerIndex(43000) == kItemNone);    // repeatable (13501940)
    CHECK(ItemLedgerIndex(2400450) == kItemNone);  // flagged
    for (std::size_t i = 0; i < ItemLedgerCount(); ++i) {
        const int lot = ItemLedgerLot(static_cast<std::int32_t>(i));
        CHECK(lot > 0);
        CHECK(ItemLotFlag(lot) == kItemNone);
        CHECK(!ItemLotDenied(lot, kItemNone));
    }
}

static void test_deny() {
    CHECK(ItemLotDenied(0, kItemNone));
    CHECK(ItemLotDenied(10010, 6622));       // Messenger: Beckoning Bell (per player)
    // The Eye of a Blood-drunk Hunter (DLC entry): its Messenger event never runs on a summoned
    // guest (chr type 0 only), so the host's copy is replayed (quest_audit.md).
    CHECK(!ItemLotDenied(10040, 50000100));
    CHECK(ClassifyAward(10040, true) && ClassifyAward(10040, true)->flag == 50000100);
    CHECK(ItemLotDenied(10050, 6670));
    CHECK(ItemLotDenied(43120, 6676));       // DLC NPC gift with a personal flag
    CHECK(!ItemLotDenied(3401800, 6674));    // Ludwig's reward: exception
    CHECK(!ItemLotDenied(3401850, 6673));    // Laurence's reward: exception
    CHECK(ItemLotDenied(100500, kItemNone)); // rune use
    CHECK(ItemLotDenied(5510, kItemNone));   // PvP
    CHECK(ItemLotDenied(16581, kItemNone));
    CHECK(ItemLotDenied(2100910, kItemNone));
    CHECK(ItemLotDenied(110001000, kItemNone));
    CHECK(ItemLotDenied(2900100, 52900100)); // area 29
    CHECK(ItemLotDenied(43000, kItemNone));
    CHECK(!ItemLotDenied(2400450, 52400450));
    CHECK(!ItemLotDenied(31000, kItemNone));
    CHECK(!ItemLotDenied(80000000, 5000));
}

static void test_classify() {
    // Client lots (hostOnly 0) never: clients get them in vanilla.
    CHECK(!ClassifyAward(2400450, false));
    auto a = ClassifyAward(31000, true);
    CHECK(a && a->lot == 31000 && a->flag == kItemNone && a->ledger == 25 && a->source == ItemSource::Award);
    CHECK(a && a->DoneFlag() == kItemLedgerBase + 25);
    a = ClassifyAward(80000000, true);
    CHECK(a && a->flag == 5000 && a->ledger == kItemNone && a->DoneFlag() == 5000);
    CHECK(!ClassifyAward(10010, true));   // denied
    CHECK(!ClassifyAward(17010, true));   // flagless, not in the ledger (repeatable)
    CHECK(!ClassifyAward(999999, true));  // unknown lot
    a = ClassifyAward(3401800, true);
    CHECK(a && a->flag == 6674);

    std::map<std::int32_t, std::int32_t> captured;
    auto f = ClassifyFlag(52400450, captured);
    CHECK(f && f->lot == 2400450 && f->flag == 52400450 && f->source == ItemSource::Flag);
    f = ClassifyFlag(50001700, captured);
    CHECK(f && f->lot == 33000);
    // D4 per-player treasure: a picked-up map treasure is not forwarded; global flags and awards are.
    CHECK(InstancedTreasureFlag(52410140, true, false));
    CHECK(!InstancedTreasureFlag(52410140, false, false));
    CHECK(!InstancedTreasureFlag(52410140, true, true));
    CHECK(!InstancedTreasureFlag(50001700, true, false));
    CHECK(!InstancedTreasureFlag(12410000, true, false));
    // Progression keys and workshop tools found as map treasure go to everyone (quest_audit.md).
    for (const std::int32_t lot : {2400450, 2800290, 2600550, 2600610, 3200720, 3500800, 2200360, 2411000}) {
        CHECK(SharedKeyTreasureLot(lot));
        CHECK(ItemLotFlag(lot) == 50000000 + lot);
        CHECK(!InstancedTreasureFlag(50000000 + lot, true, false));
        CHECK(!ItemLotDenied(lot, 50000000 + lot));
        const auto k = ClassifyFlag(50000000 + lot, captured);
        CHECK(k && k->lot == lot);
        CHECK(GuestGiftShareable(lot, 50000000 + lot));
    }
    CHECK(!SharedKeyTreasureLot(2410290));            // Saw Hunter Badge: a shop unlock, per player
    CHECK(InstancedTreasureFlag(52410290, true, false));
    CHECK(!SharedKeyTreasureLot(31000));              // Oedon Tomb Key: a boss award (ledger), not treasure
    captured[50001700] = 33010; // the hook saw the NG variant
    f = ClassifyFlag(50001700, captured);
    CHECK(f && f->lot == 33010);
    CHECK(!ClassifyFlag(6622, captured));      // per-player Messenger flag
    CHECK(!ClassifyFlag(12411800, captured));  // not an item flag
    CHECK(!ClassifyFlag(52900100, captured));  // chalice
}

static void test_json() {
    std::vector<ItemGrant> in(3);
    in[0] = {1700000000123ull, 2400450, 52400450, kItemNone, ItemSource::Flag};
    in[1] = {1700000000124ull, 31000, kItemNone, 25, ItemSource::Award};
    in[2] = {7, 80000000, 5000, kItemNone, ItemSource::Full};
    const std::string text = ItemsToJsonText(in);
    std::vector<ItemGrant> out;
    std::string err;
    CHECK(ItemsFromJsonText(text, &out, &err));
    CHECK(out.size() == 3);
    for (std::size_t i = 0; i < out.size() && i < in.size(); ++i) {
        CHECK(out[i].seq == in[i].seq && out[i].lot == in[i].lot && out[i].flag == in[i].flag &&
              out[i].ledger == in[i].ledger && out[i].source == in[i].source);
    }
    CHECK(ItemsFromJsonText("{\"items\":[]}", &out, &err) && out.empty());
    CHECK(!ItemsFromJsonText("{\"items\":[[1,2,3]]}", &out, &err));
    CHECK(!ItemsFromJsonText("{\"items\":[[1,31000,-1,-1,\"award\"]]}", &out, &err)); // neither flag nor ledger
    CHECK(!ItemsFromJsonText("{\"items\":[[1,31000,-1,1000,\"award\"]]}", &out, &err)); // ledger out of range
    CHECK(!ItemsFromJsonText("{\"items\":[[1,0.5,5,-1,\"flag\"]]}", &out, &err));
    CHECK(!ItemsFromJsonText("[]", &out, &err));
    CHECK(!ItemsFromJsonText("not json", &out, &err));
    CHECK(ItemSourceFromName("mark") == ItemSource::Mark);
    CHECK(ItemSourceFromName("zzz") == ItemSource::Unknown);
}

static void test_host() {
    HostItems h;
    h.SeedSeq(100);
    ItemGrant g{0, 2400450, 52400450, kItemNone, ItemSource::Award};
    CHECK(h.Offer(g));
    CHECK(!h.Offer(g)); // same flag: once
    ItemGrant byflag{0, 2400450, 52400450, kItemNone, ItemSource::Flag};
    CHECK(!h.Offer(byflag)); // the C2 report after the hook
    CHECK(h.Offer({0, 31000, kItemNone, 25, ItemSource::Award}));
    CHECK(!h.Offer({0, 31000, kItemNone, 25, ItemSource::Award}));
    CHECK(!h.Offer({0, 1, kItemNone, kItemNone, ItemSource::Award})); // no key
    ItemGrant out;
    CHECK(h.Pop(&out) && out.lot == 2400450 && out.seq == 101);
    CHECK(h.Pop(&out) && out.lot == 31000 && out.seq == 102);
    CHECK(!h.Pop(&out));
    h.Capture(50001700, 33010);
    CHECK(h.Captured().at(50001700) == 33010);
}

static ItemApplyState Home() {
    ItemApplyState s;
    s.world_up = true;
    s.loading = false;
    s.load_mode = 0;
    s.own_world = 1;
    s.session_role = 0;
    s.game_data = true;
    return s;
}

static void test_gate() {
    CHECK(ItemApplyAllowedNow(Home()));
    ItemApplyState s = Home();
    s.load_mode = 1; // summoned: the overlay
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.own_world = 0;
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.loading = true;
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.session_role = 6; // client
    CHECK(!ItemApplyAllowedNow(s));
    s.session_role = 4; // joining
    CHECK(!ItemApplyAllowedNow(s));
    s.session_role = 3; // hosting in the own world
    CHECK(ItemApplyAllowedNow(s));
    s = Home();
    s.game_data = false;
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.world_up = false;
    CHECK(!ItemApplyAllowedNow(s));
}

static void test_guest() {
    GuestItems q;
    std::vector<ItemGrant> in = {{1, 2400450, 52400450, kItemNone, ItemSource::Flag},
                                 {2, 31000, kItemNone, 25, ItemSource::Award}};
    CHECK(q.Offer(in) == 2);
    CHECK(q.Offer(in) == 0); // already queued
    CHECK(q.Pending() == 2);
    double t = 100.0;
    ItemApplyState away = Home();
    away.load_mode = 1;
    // Summoned: nothing, however long.
    for (int i = 0; i < 500; ++i) {
        CHECK(!q.Step(away, t));
        t += 0.016;
    }
    // A mark that arrives while summoned goes first.
    CHECK(q.Offer({{0, 43710, 50002560, kItemNone, ItemSource::Mark}}) == 1);
    // Home: the gate must hold kStableTicks frames first.
    int got_at = -1;
    std::optional<ItemGrant> g;
    for (int i = 0; i < GuestItems::kStableTicks + 5; ++i) {
        g = q.Step(Home(), t);
        t += 0.016;
        if (g) {
            got_at = i;
            break;
        }
    }
    CHECK(got_at == GuestItems::kStableTicks - 1);
    CHECK(g && g->source == ItemSource::Mark && g->lot == 43710);
    q.Done(g->Key());
    // The next (a real award) right after a mark (no popup spacing for marks).
    g = q.Step(Home(), t);
    CHECK(g && g->lot == 2400450);
    q.Done(g->Key());
    q.Awarded(t); // it opened a popup
    // Then spacing.
    t += 0.016;
    CHECK(!q.Step(Home(), t));
    t += GuestItems::kSpacing;
    g = q.Step(Home(), t);
    CHECK(g && g->lot == 31000);
    q.Done(g->Key());
    CHECK(q.Pending() == 0);
    // Done keys are not taken again (a resent full list).
    CHECK(q.Offer(in) == 0);
    CHECK(q.IsDone(52400450));
    CHECK(q.IsDone(std::int64_t(kItemLedgerBase) + 25));
    // A loading screen resets the stability counter.
    CHECK(q.Offer({{3, 80000000, 5000, kItemNone, ItemSource::Flag}}) == 1);
    ItemApplyState loading = Home();
    loading.loading = true;
    CHECK(!q.Step(loading, t + 10));
    CHECK(!q.Step(Home(), t + 10)); // stable count restarted at 1
}

// The Cathedral Ward report: lot 50000010 (ledger 36, event 12411700) gives goods 4114 (Sword
// Hunter Badge, maxNum 1, no storage); a guest whose save already holds it got the blocking
// "inventory or storage max" dialog. Such a lot is now skipped.
static void test_fit() {
    LotSlotState badge;
    badge.category = 4;
    badge.id = 4114;
    badge.num = 1;
    badge.owned = 1;
    badge.max_num = 1;
    badge.max_storage = 0;
    std::string why;
    CHECK(ClassifyLotFit({badge}, &why) == LotFit::Owned);
    CHECK(why.find("4114") != std::string::npos);
    badge.owned = 0;
    CHECK(ClassifyLotFit({badge}) == LotFit::Give);
    // Unknown count: never refused (the old behaviour).
    badge.owned = -1;
    CHECK(ClassifyLotFit({badge}) == LotFit::Give);
    // Consumables: 18 vials + 3 > 20 with storage room -> fits (overflow goes to storage).
    LotSlotState vials;
    vials.category = 4;
    vials.id = 1000;
    vials.num = 3;
    vials.owned = 18;
    vials.max_num = 20;
    vials.max_storage = 2;
    CHECK(ClassifyLotFit({vials}) == LotFit::Give);
    vials.max_storage = 0;
    CHECK(ClassifyLotFit({vials}) == LotFit::Full);
    vials.owned = 17;
    CHECK(ClassifyLotFit({vials}) == LotFit::Give);
    // Weapons have no count limit here; num 0 counts as 1.
    LotSlotState weapon;
    weapon.category = 0;
    weapon.id = 18100000;
    weapon.owned = 5;
    weapon.max_num = 1;
    CHECK(ClassifyLotFit({weapon}) == LotFit::Give);
    LotSlotState key = badge;
    key.id = 4000;
    key.owned = 1;
    key.num = 0;
    // One refused slot refuses the lot; Owned wins over Full.
    vials.owned = 20;
    CHECK(ClassifyLotFit({weapon, vials}) == LotFit::Full);
    CHECK(ClassifyLotFit({vials, key}) == LotFit::Owned);
    CHECK(ClassifyLotFit({}) == LotFit::Give);
    // 0.6.2: a held key item (goodsType 1) is refused whatever its storage max says (the 0.6.1
    // test aid gave lot 31000 with the Oedon Tomb Key held: blocking box).
    LotSlotState tomb = badge;
    tomb.id = 4000;
    tomb.owned = 1;
    tomb.goods_type = 1;
    tomb.max_storage = 1;
    CHECK(ClassifyLotFit({tomb}) == LotFit::Owned);
    tomb.max_num = -1;  // unreadable limits: still held
    CHECK(ClassifyLotFit({tomb}) == LotFit::Owned);
    tomb.owned = 0;
    CHECK(ClassifyLotFit({tomb}) == LotFit::Give);
    // A fight holds every popup back; marks still go.
    GuestItems q;
    CHECK(q.Offer({{1, 31000, kItemNone, 25, ItemSource::Full}}) == 1);
    ItemApplyState fight = Home();
    fight.calm = false;
    double t = 100;
    for (int i = 0; i < GuestItems::kStableTicks + 5; ++i, t += 0.016) {
        CHECK(!q.Step(fight, t));
    }
    CHECK(q.Step(Home(), t).has_value());
}

// Ledger lots a save earned solo (live run, first party session: 29000, 30000, 31000, 50000001,
// 50000010 given again) get their bits before C2 writes any host flag, so the host's full list
// skips them - and a boss the save never beat (its flag later synced by C2) keeps its lot.
static void test_ledger_seed() {
    std::set<std::uint32_t> save = {9066, 9067, 12401800, 12411700, 12411800, 12101800};
    auto is_set = [&save](std::uint32_t id) { return save.count(id) != 0; };
    const std::vector<std::uint32_t> bits = LedgerBitsToSeed(is_set);
    const std::set<std::uint32_t> got(bits.begin(), bits.end());
    CHECK(got.size() == bits.size()); // no duplicates
    for (std::int32_t idx = 0; idx < std::int32_t(ItemLedgerCount()); ++idx) {
        const bool want = save.count(std::uint32_t(ItemLedgerDoneFlag(idx))) != 0;
        CHECK(got.count(kItemLedgerBase + std::uint32_t(idx)) == (want ? 1u : 0u));
    }
    for (std::int32_t lot : {29000, 30000, 31000, 50000001, 50000010}) {
        const std::int32_t idx = ItemLedgerIndex(lot);
        CHECK(idx >= 0);
        CHECK(got.count(kItemLedgerBase + std::uint32_t(idx)) == 1);
        ItemGrant g{1, lot, kItemNone, idx, ItemSource::Full};
        CHECK(g.DoneFlag() == std::int64_t(kItemLedgerBase) + idx);
    }
    // m22's boss (12201800, lot 21002950) was not beaten here: no bit, the replay gives it even
    // after C2 has synced 12201800 into this save.
    CHECK(got.count(kItemLedgerBase + std::uint32_t(ItemLedgerIndex(21002950))) == 0);
    save.insert(kItemLedgerBase + std::uint32_t(ItemLedgerIndex(29000)));
    CHECK(LedgerBitsToSeed(is_set).size() == bits.size() - 1);
    save.insert(kItemLedgerSeeded);
    CHECK(LedgerBitsToSeed(is_set).empty());
    CHECK(ItemLedgerCount() < std::size_t(kItemLedgerSize - 1));
    CHECK(kItemLedgerSeeded == kItemLedgerBase + kItemLedgerSize - 1);
}


static ItemApplyState Summoned() {
    ItemApplyState s;
    s.world_up = true;
    s.load_mode = 1;
    s.own_world = 0;
    s.session_role = 6;
    s.game_data = true;
    return s;
}

static LotRowData Row(std::int32_t lot, std::int32_t flag) {
    LotRowData r;
    r.lot = lot;
    r.flag = flag;
    for (int i = 0; i < 8; ++i) {
        r.category[i] = 0xffffffffu;
    }
    return r;
}

static void Slot(LotRowData* r, int i, std::uint32_t cat, std::int32_t id, int num, int base) {
    r->category[i] = cat;
    r->id[i] = id;
    r->num[i] = static_cast<std::uint8_t>(num);
    r->base[i] = static_cast<std::uint16_t>(base);
}

static void test_summoned_gate() {
    CHECK(ItemSummonedApplyAllowedNow(Summoned()));
    CHECK(ItemApplyModeNow(Summoned()) == ItemApplyMode::Summoned);
    CHECK(ItemApplyModeNow(Home()) == ItemApplyMode::Home);
    ItemApplyState s = Summoned();
    s.load_mode = 0; // flag store not switched yet: neither
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
    s = Summoned();
    s.session_role = 4; // joining
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
    s = Summoned();
    s.loading = true;
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
    s = Summoned();
    s.game_data = false;
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
}

static void test_plan() {
    auto none = [](std::int32_t) { return false; };
    // Treasure: one row, one slot (Hunter Chief Emblem style).
    LotRowData t = Row(2400450, 52400450);
    Slot(&t, 0, 4, 4002, 1, 100);
    LotPlan p = PlanSummonedLot({t}, {}, none);
    CHECK(p.result == LotPlanResult::Give && p.items.size() == 1);
    CHECK(p.items[0] == (GrantItem{4, 4002, 1}));
    CHECK(p.row_flags == std::vector<std::int32_t>{52400450});
    // lotItemNum 0 counts as 1; a count is taken as is.
    t.num[0] = 0;
    CHECK(PlanSummonedLot({t}, {}, none).items[0].count == 1);
    t.num[0] = 3;
    CHECK(PlanSummonedLot({t}, {}, none).items[0].count == 3);
    // A chain (cap + garb) with a second row of its own flag already held: only the first.
    LotRowData a = Row(2110010, 52110010), b = Row(2110011, 52110011);
    Slot(&a, 0, 1, 230000, 1, 100);
    Slot(&b, 0, 1, 231000, 1, 100);
    p = PlanSummonedLot({a, b}, {}, [](std::int32_t f) { return f == 52110011; });
    CHECK(p.result == LotPlanResult::Give && p.items.size() == 1 && p.items[0].id == 230000);
    CHECK(p.row_flags == std::vector<std::int32_t>{52110010});
    p = PlanSummonedLot({a, b}, {}, none);
    CHECK(p.items.size() == 2 && p.row_flags.size() == 2);
    // A blood gem: own world only.
    LotRowData g = Row(17000, 50000200);
    Slot(&g, 0, 8, 10001, 1, 100);
    CHECK(PlanSummonedLot({g}, {}, none).result == LotPlanResult::Unsupported);
    // A random row without the host's list: own world only; with a matching list: that list.
    LotRowData r = Row(22502610, -1);
    Slot(&r, 0, 4, 1000, 2, 50);
    Slot(&r, 1, 4, 900, 5, 50);
    CHECK(PlanSummonedLot({r}, {}, none).result == LotPlanResult::Random);
    CHECK(LotChainRandom({r}) && !LotChainRandom({t}));
    p = PlanSummonedLot({r}, {{4, 900, 5}}, none);
    CHECK(p.result == LotPlanResult::Give && p.items.size() == 1 && p.items[0].id == 900);
    // A host list naming an item the lot cannot give, or a higher count: ignored (rows decide).
    CHECK(PlanSummonedLot({r}, {{4, 4114, 1}}, none).result == LotPlanResult::Random);
    CHECK(PlanSummonedLot({r}, {{4, 900, 6}}, none).result == LotPlanResult::Random);
    CHECK(PlanSummonedLot({t}, {{4, 4002, 1}, {4, 4002, 1}}, none).items.size() == 1); // 2 items, 1 row
    // The host's capture of a deterministic lot is used as is.
    p = PlanSummonedLot({t}, {{4, 4002, 2}}, none);
    CHECK(p.why == "host's item list" && p.items[0].count == 2);
    // An empty deterministic row gives nothing but still counts (its flag is set).
    LotRowData e = Row(5000, 50005000);
    e.base[0] = 100; // id 0: "nothing"
    p = PlanSummonedLot({e}, {}, none);
    CHECK(p.result == LotPlanResult::Give && p.items.empty() && p.row_flags.size() == 1);
    CHECK(PlanSummonedLot({}, {}, none).result == LotPlanResult::Missing);
    // The host's roll: always a slot of the row, the same for the same seed.
    for (std::uint64_t seed = 1; seed < 200; ++seed) {
        const std::vector<GrantItem> rolled = RollLotChain({r}, seed);
        CHECK(rolled.size() == 1 && (rolled[0].id == 1000 || rolled[0].id == 900));
        CHECK(rolled == RollLotChain({r}, seed));
        CHECK(PlanSummonedLot({r}, rolled, none).result == LotPlanResult::Give);
    }
    std::set<std::int32_t> seen;
    for (std::uint64_t seed = 1; seed < 200; ++seed) {
        seen.insert(RollLotChain({r}, seed)[0].id);
    }
    CHECK(seen.size() == 2);
    CHECK(GrantCategorySupported(0) && GrantCategorySupported(4) && !GrantCategorySupported(8) &&
          !GrantCategorySupported(3));
}

static void test_items_json() {
    ItemGrant g{5, 22502610, kItemNone, 30, ItemSource::Award};
    g.items = {{4, 900, 5}, {0, 12080000, 1}};
    std::vector<ItemGrant> out;
    std::string err;
    CHECK(ItemsFromJsonText(ItemsToJsonText({g}), &out, &err));
    CHECK(out.size() == 1 && out[0].items == g.items);
    // Old rows (5 elements) still parse, with no items.
    CHECK(ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag"]]})", &out, &err) && out[0].items.empty());
    // Bad item lists are refused.
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[8,1,1]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,0,1]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,1,0]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,1,256]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,1]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",{"a":1}]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[-4,1,1]]]]})", &out, &err));
    std::string many = R"({"items":[[1,2400450,52400450,-1,"flag",[)";
    for (int i = 0; i < 17; ++i) {
        many += std::string(i ? "," : "") + "[4,1000,1]";
    }
    many += "]]]}";
    CHECK(!ItemsFromJsonText(many, &out, &err));
}

static GrantItem Gem(std::int32_t id) {
    GrantItem it{8, id, 1};
    it.has_gem = true;
    const std::int32_t data[kGemFields] = {0, 7, 100202, 110001, -1, -1, -1, -1};
    for (std::size_t k = 0; k < kGemFields; ++k) {
        it.gem[k] = data[k];
    }
    return it;
}

static void test_gems() {
    auto none = [](std::int32_t) { return false; };
    // Blood gem treasure (lot 20000: slot 100202): the host's generated gem (slot id + offset).
    LotRowData g = Row(20000, 50000400);
    Slot(&g, 0, 8, 100202, 1, 100);
    CHECK(LotChainHasGem({g}));
    CHECK(PlanSummonedLot({g}, {}, none).result == LotPlanResult::Unsupported); // no gem data
    LotPlan p = PlanSummonedLot({g}, {Gem(100203)}, none);
    CHECK(p.result == LotPlanResult::Give && p.why == "host's item list" && p.items.size() == 1);
    CHECK(p.items[0].has_gem && p.items[0].gem[2] == 100202 && p.row_flags == std::vector<std::int32_t>{50000400});
    CHECK(PlanSummonedLot({g}, {Gem(100202)}, none).result == LotPlanResult::Give);
    // Outside the slot's range, below it, a count of 2 or no data: not the host's list.
    CHECK(PlanSummonedLot({g}, {Gem(100202 + kMaxGemIdOffset + 1)}, none).result == LotPlanResult::Unsupported);
    CHECK(PlanSummonedLot({g}, {Gem(100201)}, none).result == LotPlanResult::Unsupported);
    GrantItem two = Gem(100203);
    two.count = 2;
    CHECK(PlanSummonedLot({g}, {two}, none).result == LotPlanResult::Unsupported);
    GrantItem bare{8, 100203, 1};
    CHECK(PlanSummonedLot({g}, {bare}, none).result == LotPlanResult::Unsupported);
    // A gem cannot stand for a goods slot or the other way round.
    LotRowData t = Row(2400450, 52400450);
    Slot(&t, 0, 4, 4002, 1, 100);
    CHECK(!LotChainHasGem({t}));
    CHECK(PlanSummonedLot({t}, {Gem(4002)}, none).why != "host's item list");
    CHECK(PlanSummonedLot({g}, {{4, 100202, 1}}, none).result == LotPlanResult::Unsupported);
    // Gem + goods chain: both from the host's list.
    LotRowData g2 = Row(20001, 50000401);
    Slot(&g2, 0, 4, 1000, 2, 100);
    p = PlanSummonedLot({g, g2}, {Gem(100203), {4, 1000, 2}}, none);
    CHECK(p.result == LotPlanResult::Give && p.items.size() == 2 && p.row_flags.size() == 2);
    // The plausibility rules.
    CHECK(GrantItemSupported(Gem(100203)) && !GrantItemSupported(bare) && GrantItemSupported({4, 1000, 1}));
    GrantItem bad = Gem(100203);
    bad.gem[0] = 4;
    CHECK(!GrantItemSupported(bad));
    bad = Gem(100203);
    bad.gem[1] = 1001;
    CHECK(!GrantItemSupported(bad));
    bad = Gem(100203);
    bad.gem[5] = -2;
    CHECK(!GrantItemSupported(bad));
    GrantItem goods_with_gem = Gem(1000);
    goods_with_gem.category = 4;
    CHECK(!GrantItemSupported(goods_with_gem));
    // JSON: [8, id, 1, [8 fields]] round trip; no data / bad data refused.
    ItemGrant row{9, 20000, 50000400, kItemNone, ItemSource::Award};
    row.items = {Gem(100203), {4, 1000, 2}};
    std::vector<ItemGrant> out;
    std::string err;
    CHECK(ItemsFromJsonText(ItemsToJsonText({row}), &out, &err));
    CHECK(out.size() == 1 && out[0].items == row.items && out[0].items[0].gem[3] == 110001);
    CHECK(ItemsFromJsonText(R"({"items":[[1,20000,50000400,-1,"award",[[8,100203,1,[0,7,1,2,-1,-1,-1,-1]]]]]})",
                            &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,20000,50000400,-1,"award",[[8,100203,1]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,20000,50000400,-1,"award",[[8,100203,2,[0,7,1,2,-1,-1,-1,-1]]]]]})",
                             &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,20000,50000400,-1,"award",[[8,100203,1,[0,7,1,2,-1,-1,-1]]]]]})",
                             &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,20000,50000400,-1,"award",[[8,100203,1,[9,7,1,2,-1,-1,-1,-1]]]]]})",
                             &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,20000,50000400,-1,"award",[[4,1000,1,[0,7,1,2,-1,-1,-1,-1]]]]]})",
                             &out, &err));
    // The host never rolls a gem chain (no instance data for a roll): the guest waits for home.
    LotRowData rg = Row(64000, -1);
    Slot(&rg, 0, 8, 50000, 1, 200);
    Slot(&rg, 1, 4, 1000, 1, 800);
    CHECK(LotChainRandom({rg}) && LotChainHasGem({rg}));
}

static void test_guest_summoned() {
    GuestItems q;
    std::vector<ItemGrant> in = {{1, 2400450, 52400450, kItemNone, ItemSource::Flag},
                                 {2, 17000, 50000200, kItemNone, ItemSource::Flag},
                                 {3, 31000, kItemNone, 25, ItemSource::Award}};
    CHECK(q.Offer(in) == 3);
    double t = 100.0;
    std::optional<ItemGrant> g;
    int got_at = -1;
    for (int i = 0; i < GuestItems::kStableTicks + 5 && !g; ++i) {
        g = q.Step(Summoned(), t);
        t += 0.016;
        got_at = i;
    }
    // Summoned: the same 90-frame settle, then in order.
    CHECK(got_at == GuestItems::kStableTicks - 1);
    CHECK(g && g->lot == 2400450 && q.LastMode() == ItemApplyMode::Summoned);
    q.Done(g->Key());
    q.Awarded(t);
    // The spacing / calm rules hold while summoned too.
    CHECK(!q.Step(Summoned(), t + 1));
    ItemApplyState fight = Summoned();
    fight.calm = false;
    CHECK(!q.Step(fight, t + GuestItems::kSpacing + 1));
    t += GuestItems::kSpacing + 2;
    g = q.Step(Summoned(), t);
    CHECK(g && g->lot == 17000);
    // A blood gem lot: deferred to the own world; the next one is not blocked by it.
    q.Defer(*g);
    CHECK(q.HomeOnly() == 1 && q.Pending() == 2);
    g = q.Step(Summoned(), t + 0.016);
    CHECK(g && g->lot == 31000);
    q.Done(g->Key());
    // Nothing else while summoned (the deferred one waits), however long.
    for (int i = 0; i < 300; ++i) {
        t += 0.1;
        CHECK(!q.Step(Summoned(), t));
    }
    // A mark arriving while summoned goes first and is applied at once (own pool).
    CHECK(q.Offer({{0, 43710, 50002560, kItemNone, ItemSource::Mark}}) == 1);
    g = q.Step(Summoned(), t);
    CHECK(g && g->source == ItemSource::Mark);
    q.Done(g->Key());
    // Home: the deferred lot comes (after the gate settles in the new mode).
    g.reset();
    for (int i = 0; i < GuestItems::kStableTicks + 5 && !g; ++i) {
        t += 0.016;
        g = q.Step(Home(), t);
    }
    CHECK(g && g->lot == 17000 && q.LastMode() == ItemApplyMode::Home);
    q.Done(g->Key());
    CHECK(q.Pending() == 0 && q.HomeOnly() == 0);
    // Done keys stay done (a resent full list after a rejoin).
    CHECK(q.Offer(in) == 0);
}

// D5: an NPC's gift to a guest goes up to the host (GuestGiftShareable, EVENT gift_up, rate limit).
static void test_gift_up() {
    CHECK(ItemLotFlag(17000) == 50000200);
    CHECK(GuestGiftShareable(17000, 50000200));        // a global gift lot with its own flag
    CHECK(!GuestGiftShareable(17000, 50000205));       // flag of another lot
    CHECK(!GuestGiftShareable(17000, kItemNone));      // flagless: the host could not dedupe
    CHECK(!GuestGiftShareable(31000, kItemNone));      // ledger lot (host-only award)
    CHECK(!GuestGiftShareable(10010, 6622));           // Messenger gift: per player
    CHECK(GuestGiftShareable(10040, 50000100));        // the Eye: replayed to everyone (quest_audit.md)
    CHECK(!GuestGiftShareable(2410290, 52410290));     // map treasure: instanced per player (D4)
    CHECK(GuestGiftShareable(2400450, 52400450));      // ... except a progression key (Hunter Chief Emblem)
    CHECK(!GuestGiftShareable(2300090, 52300090));
    CHECK(!GuestGiftShareable(123456789, 50000200));   // unknown lot

    const std::string body = GiftUpToJsonText(17000, 50000200);
    std::int32_t lot = 0, flag = 0;
    std::string err;
    CHECK(GiftUpFromJsonText(body, &lot, &flag, &err) && lot == 17000 && flag == 50000200);
    CHECK(!GiftUpFromJsonText(GiftUpToJsonText(2410290, 52410290), &lot, &flag, &err));
    CHECK(!GiftUpFromJsonText("{\"lot\":17000}", &lot, &flag, &err));
    CHECK(!GiftUpFromJsonText("{\"lot\":17000.5,\"flag\":50000200}", &lot, &flag, &err));
    CHECK(!GiftUpFromJsonText("{\"lot\":1e300,\"flag\":50000200}", &lot, &flag, &err));
    CHECK(!GiftUpFromJsonText("[17000,50000200]", &lot, &flag, &err));
    CHECK(!GiftUpFromJsonText("not json", &lot, &flag, &err));

    GiftRateLimit r;
    for (int i = 0; i < GiftRateLimit::kPerWindow; ++i) {
        CHECK(r.Take(1, 10.0 + i));
    }
    CHECK(!r.Take(1, 20.0));                           // fifth within 60 s
    CHECK(r.Take(2, 20.0));                            // other slots count apart
    CHECK(r.Take(1, 10.0 + GiftRateLimit::kWindow));   // a new window
    CHECK(!r.Take(-1, 0.0) && !r.Take(8, 0.0));
}


// Playtest 1 #15 / #12: the item-get popup waits while the player is dead, a cutscene plays or
// the boss-defeat banner runs; marks (no popup) still go.
static void test_popup_hold() {
    ItemApplyState s = Summoned();
    CHECK(ItemPopupHold(s) == nullptr);
    ItemApplyState dead = s;
    dead.alive = false;
    CHECK(std::string(ItemPopupHold(dead)) == "dead");
    ItemApplyState remo = s;
    remo.cutscene = true;
    CHECK(std::string(ItemPopupHold(remo)) == "cutscene");
    ItemApplyState boss = s;
    boss.boss_quiet = false;
    CHECK(std::string(ItemPopupHold(boss)) == "boss defeat banner");
    ItemApplyState fight = s;
    fight.calm = false;
    CHECK(std::string(ItemPopupHold(fight)) == "in a fight");

    GuestItems q;
    // The boss reward (Sword Hunter Badge) arrives in the same second as the boss defeat.
    CHECK(q.Offer({{1, 50000010, kItemNone, 36, ItemSource::Award}}) == 1);
    double t = 100;
    std::optional<ItemGrant> g;
    for (int i = 0; i < GuestItems::kStableTicks + 5; ++i, t += 0.016) {
        g = q.Step(boss, t);
        CHECK(!g);
    }
    CHECK(q.LastHold() && std::string(q.LastHold()) == "boss defeat banner");
    for (int i = 0; i < 10; ++i, t += 0.016) {
        CHECK(!q.Step(dead, t)); // the player died meanwhile: still held
    }
    CHECK(std::string(q.LastHold()) == "dead");
    for (int i = 0; i < 10; ++i, t += 0.016) {
        CHECK(!q.Step(remo, t));
    }
    // A load in between resets the settle; the grant keeps its place and comes once all is quiet.
    g = q.Step(s, t);
    CHECK(g && g->lot == 50000010);
    CHECK(q.LastHold() == nullptr);
    // A mark goes even while dead (no popup).
    GuestItems m;
    ItemGrant mk;
    mk.lot = 2400450;
    mk.flag = 52400450;
    mk.source = ItemSource::Mark;
    CHECK(m.Offer({mk}) == 1);
    std::optional<ItemGrant> got;
    for (int i = 0; i < GuestItems::kStableTicks + 2 && !got; ++i, t += 0.016) {
        got = m.Step(dead, t);
    }
    CHECK(got && got->source == ItemSource::Mark);
}

// The item-get box watchdog: left over a load, a dead player, a confirm check that stays held.
static void test_itemget_watch() {
    const char* why = nullptr;
    // Normal: the box is up, the player confirms (entries 0) - nothing to clear, however long.
    {
        ItemGetWatch w;
        for (double t = 0; t < 60; t += 0.5) {
            CHECK(!w.Step(t, 1, true, true, false, &why));
        }
        CHECK(!w.Step(60, 0, true, true, false, &why));
    }
    // A load while the box is up: cleared right after the load (not during it).
    {
        ItemGetWatch w;
        CHECK(!w.Step(0, 1, true, true, false, &why));
        CHECK(!w.Step(1, 1, false, true, true, &why));
        CHECK(!w.Step(5, 1, false, true, true, &why));
        CHECK(w.Step(6, 1, true, true, false, &why));
        CHECK(std::string(why) == "left over from before a load");
        // State reset: a new box after that starts fresh.
        CHECK(!w.Step(7, 1, true, true, false, &why));
    }
    // The host died while a box was up on the guest (#12): the guest dies too / gets sent home.
    {
        ItemGetWatch w;
        CHECK(!w.Step(0, 2, true, true, false, &why));
        CHECK(!w.Step(1, 2, true, false, false, &why));
        CHECK(!w.Step(1 + ItemGetWatch::kDeadFor - 0.1, 2, true, false, false, &why));
        CHECK(w.Step(1 + ItemGetWatch::kDeadFor, 2, true, false, false, &why));
        CHECK(std::string(why) == "the player is dead");
    }
    // A short death (revived in place) does not clear.
    {
        ItemGetWatch w;
        CHECK(!w.Step(0, 1, true, false, false, &why));
        CHECK(!w.Step(1, 1, true, true, false, &why));
        CHECK(!w.Step(10, 1, true, true, false, &why));
    }
    // The confirm check held (the frozen badge box, #15): cleared after kHeldFor s; a moment of
    // "armed" restarts the count.
    {
        ItemGetWatch w;
        CHECK(!w.Step(0, 1, false, true, false, &why));
        CHECK(!w.Step(10, 1, false, true, false, &why));
        CHECK(!w.Step(11, 1, true, true, false, &why));
        CHECK(!w.Step(12, 1, false, true, false, &why));
        CHECK(!w.Step(12 + ItemGetWatch::kHeldFor - 1, 1, false, true, false, &why));
        CHECK(w.Step(12 + ItemGetWatch::kHeldFor, 1, false, true, false, &why));
        CHECK(std::string(why) == "its confirm check stays held");
    }
}

// #16: a set ledger bit with none of the lot's unique items held is a wrong skip.
static void test_ledger_suspicious() {
    LotSlotState key;
    key.category = 4;
    key.id = 4000; // Oedon Tomb Key
    key.num = 1;
    key.owned = 0;
    key.max_num = 1;
    key.max_storage = 0;
    CHECK(LedgerSkipSuspicious({key}));
    LotSlotState held = key;
    held.owned = 1;
    CHECK(!LedgerSkipSuspicious({held}));
    LotSlotState unknown = key;
    unknown.owned = -1;
    CHECK(!LedgerSkipSuspicious({unknown}));
    // par_c6: goods given while summoned, given again in the own world when missing.
    CHECK(SummonedRegiveCount(1, 0, 1) == 1);   // the key never reached the save
    CHECK(SummonedRegiveCount(1, 1, 1) == 0);   // kept
    CHECK(SummonedRegiveCount(8, 3, 8) == 5);   // 5 of 8 missing
    CHECK(SummonedRegiveCount(8, 0, 3) == 3);   // never more than was given
    CHECK(SummonedRegiveCount(-1, 0, 1) == 0);  // unknown counts: nothing
    CHECK(SummonedRegiveCount(1, -1, 1) == 0);
    LotSlotState vial = key;
    vial.max_num = 20;
    vial.max_storage = 600;
    CHECK(!LedgerSkipSuspicious({vial}));
    LotSlotState weapon = key;
    weapon.category = 0;
    CHECK(!LedgerSkipSuspicious({weapon}));
    CHECK(!LedgerSkipSuspicious({key, held}));
    CHECK(!LedgerSkipSuspicious({}));
    // verify_c6: key items (goodsType 1) read 0 in a summoned guest's inventory: checked at home.
    LotSlotState door_key = key;
    door_key.goods_type = 1;
    CHECK(LotHasKeyItem({door_key}));
    LotSlotState badge = key;
    badge.id = 4001;
    badge.goods_type = 0;
    CHECK(!LotHasKeyItem({badge}));
    CHECK(LotHasKeyItem({badge, door_key}));
    CHECK(!LotHasKeyItem({key}));  // goodsType unknown
    LotSlotState weapon_type1 = weapon;
    weapon_type1.goods_type = 1;
    CHECK(!LotHasKeyItem({weapon_type1}));  // goods only
    CHECK(!LotHasKeyItem({}));
    // 0.6.2 (regression run 0.6.1): never a key item / unique goods while summoned - lot 50000001
    // (goods 4001, goodsType 0, maxNum 1) read 0 summoned, was given, the game showed its box.
    CHECK(KeyGoods(door_key));
    CHECK(KeyGoods(badge));       // unique goods, goodsType 0
    CHECK(KeyGoods(key));         // unique goods, goodsType unknown
    CHECK(!KeyGoods(vial));       // stackable with storage
    CHECK(!KeyGoods(weapon));
    CHECK(!KeyGoods(weapon_type1));
    LotSlotState stack_key = vial;
    stack_key.goods_type = 1;     // a goodsType 1 item with a count (still a key item)
    CHECK(KeyGoods(stack_key));
    CHECK(!SummonedGiveAllowed({badge}));
    CHECK(!SummonedGiveAllowed({vial, door_key}));
    CHECK(SummonedGiveAllowed({vial}));
    CHECK(SummonedGiveAllowed({weapon}));
    CHECK(SummonedGiveAllowed({}));
    // Own world: a key item is given only when it reads 0 (held or unknown: never).
    CHECK(KeyGiveAllowedHeld(door_key, 0));
    CHECK(!KeyGiveAllowedHeld(door_key, 1));
    CHECK(!KeyGiveAllowedHeld(door_key, -1));
    CHECK(KeyGiveAllowedHeld(vial, 5));
    CHECK(KeyGiveAllowedHeld(weapon, 1));
}

// Playtest 1 save audit: the guest's save (slot 1) held the ledger bits of the Oedon Tomb Key
// (31000, ledger 25) and the Sword Hunter Badge (50000010, ledger 36) and both boss flags, but
// neither item. The own-world catch-up picks exactly those from the save alone.
static void test_key_catchup() {
    const std::set<std::uint32_t> boss = {12411700, 12411800, 12401800, 12101800};
    auto is_boss = [&boss](std::uint32_t id) { return boss.count(id) != 0; };
    std::set<std::uint32_t> save = {kItemLedgerBase + 25, kItemLedgerBase + 36, 12411700, 12411800, 9067,
                                    kItemLedgerBase + 24, kItemLedgerSeeded};
    auto own = [&save](std::uint32_t id) { return save.count(id) != 0; };
    CHECK(ItemLedgerLot(25) == 31000 && ItemLedgerLot(36) == 50000010);
    std::vector<std::int32_t> lots = KeyCatchupLots(own, is_boss);
    CHECK(lots.size() == 2);
    CHECK(lots.size() == 2 && lots[0] == 31000 && lots[1] == 50000010);
    // Tiny Music Box (ledger 24, common ev 9040 slot 27, done 9067): not a boss reward, never caught up.
    CHECK(std::find(lots.begin(), lots.end(), 30000) == lots.end());
    // A ledger bit without the boss flag in this save, or the boss flag without the bit (a boss
    // C2 synced, its reward never given): nothing (the join's full list handles the latter).
    save.erase(12411800);
    lots = KeyCatchupLots(own, is_boss);
    CHECK(lots.size() == 1 && lots[0] == 50000010);
    save.insert(12411800);
    save.erase(kItemLedgerBase + 36);
    lots = KeyCatchupLots(own, is_boss);
    CHECK(lots.size() == 1 && lots[0] == 31000);
    CHECK(KeyCatchupLots([](std::uint32_t) { return false; }, is_boss).empty());
    // A shared key treasure whose flag this save has (Blood Gem Workshop Tool, 52411000): after the
    // boss rewards; an unshared treasure flag (52410120) is never caught up.
    CHECK(SharedKeyTreasureLot(2411000) && ItemLotFlag(2411000) == 52411000);
    save.insert(52411000);
    save.insert(52410120);
    lots = KeyCatchupLots(own, is_boss);
    CHECK(lots.size() == 2 && lots[0] == 31000 && lots[1] == 2411000);
    // A lot is given only when every slot is a missing unique good and one of them is a key item.
    LotSlotState key;
    key.category = 4;
    key.id = 4000;
    key.num = 1;
    key.owned = 0;
    key.max_num = 1;
    key.max_storage = 0;
    key.goods_type = 1;
    CHECK(KeyCatchupNeeded({key}));
    LotSlotState held = key;
    held.owned = 1;
    CHECK(!KeyCatchupNeeded({held}));
    LotSlotState unknown = key;
    unknown.owned = -1;
    CHECK(!KeyCatchupNeeded({unknown}));
    // 0.6.2: unique goods with goodsType 0 (Amelia's 4001, badges) are caught up too: a summoned
    // guest no longer gives them (their held count reads 0 there).
    LotSlotState pendant = key;
    pendant.id = 4001;
    pendant.goods_type = 0;
    CHECK(KeyCatchupNeeded({pendant}));
    pendant.owned = 1;
    CHECK(!KeyCatchupNeeded({pendant}));
    LotSlotState vial_slot = key;
    vial_slot.goods_type = 0;
    vial_slot.max_num = 20;
    vial_slot.max_storage = 600;
    CHECK(!KeyCatchupNeeded({vial_slot}));
    CHECK(!KeyCatchupNeeded({}));
    // party_summoned_gives.txt entries belong to the character (save slot) they were given to.
    CHECK(SummonedGiveForSlot(1, 1));
    CHECK(!SummonedGiveForSlot(1, 0));
    CHECK(SummonedGiveForSlot(-1, 0));  // an older entry without a slot
    CHECK(SummonedGiveForSlot(1, -1));  // the slot cannot be read: as before
}

// The Old Hunter Bell and the other Messenger gifts: per player (never replayed), and a guest's
// own grant of one is never accepted from a peer.
// P2 (quest_audit.md): the repeatable NPC gifts of common 9100 are the guest's own grants.
static void test_repeat_gift() {
    for (std::int32_t lot : {28000, 32020, 24060, 17010}) {
        CHECK(RepeatGiftLotId(lot));
        CHECK(ItemLotDenied(lot, ItemLotFlag(lot)));  // never replayed from the host
        CHECK(ItemLotFlag(lot) < 0 && ItemLedgerIndex(lot) < 0);
    }
    // Iosefka's Blood Vial is also her once-only first gift (common 9040 slot 6, ledger 5); the
    // repeatable grant has its own key, so a ledger bit set for the first gift does not stop it.
    CHECK(RepeatGiftLotId(23000) && ItemLedgerIndex(23000) == 5);
    {
        const std::optional<ItemGrant> v = MakeRepeatGrant(23000, 7);
        CHECK(v && v->ledger == kItemNone && v->DoneFlag() < 0 && v->Key() == kRepeatKeyBase + 7);
    }
    CHECK(!RepeatGiftLotId(10050) && !RepeatGiftLotId(31000) && !RepeatGiftLotId(17000));
    CHECK(!MakeRepeatGrant(31000, 1));
    CHECK(std::string(ItemSourceName(ItemSource::Repeat)) == "repeat");
    CHECK(ItemSourceFromName("repeat") == ItemSource::Repeat);
    const std::optional<ItemGrant> a = MakeRepeatGrant(28000, 1), b = MakeRepeatGrant(28000, 2);
    CHECK(a && b && a->source == ItemSource::Repeat && a->lot == 28000 && a->DoneFlag() < 0);
    CHECK(a->Key() == kRepeatKeyBase + 1 && b->Key() == kRepeatKeyBase + 2);
    CHECK(!ItemGrantFromPeerAllowed(*a));  // never taken from a peer
    // Each talk gives it again: two grants of the same lot are two queue entries.
    GuestItems q;
    CHECK(q.Offer({*a}) == 1);
    CHECK(q.Offer({*a}) == 0);
    CHECK(q.Offer({*b}) == 1);
    // While summoned: a consumable unique (Blood of Arianna: maxNum 1, goodsType 0) may be given;
    // a key item (Queenly Flesh: goodsType 1) waits for the own world.
    LotSlotState blood;
    blood.category = 4;
    blood.id = 701;
    blood.max_num = 1;
    blood.max_storage = 0;
    blood.goods_type = 0;
    LotSlotState flesh = blood;
    flesh.id = 4305;
    flesh.goods_type = 1;
    CHECK(!SummonedGiveAllowed({blood}) && RepeatSummonedGiveAllowed({blood}));
    CHECK(!RepeatSummonedGiveAllowed({flesh}) && !RepeatSummonedGiveAllowed({blood, flesh}));
    LotSlotState elixir = blood;
    elixir.id = 1120;
    elixir.max_num = 10;
    elixir.max_storage = 600;
    CHECK(SummonedGiveAllowed({elixir}) && RepeatSummonedGiveAllowed({elixir}));
    // A peer row without a flag or ledger index (what a repeat grant would be) is refused.
    std::vector<ItemGrant> out;
    std::string err;
    CHECK(!ItemsFromJsonText(R"({"items":[[1,28000,-1,-1,"repeat"]]})", &out, &err));
}

// G1: the host's Chalice Dungeon awards (boss ritual materials) for the guests in the dungeon.
static void test_chalice_award() {
    CHECK(ChaliceAwardLot(110001000) && ChaliceAwardLot(131302160) && ChaliceAwardLot(2900010));
    CHECK(!ChaliceAwardLot(31000) && !ChaliceAwardLot(2410000) && !ChaliceAwardLot(0));
    GrantItem blood;  // Ritual Blood (2) x2
    blood.category = 4;
    blood.id = 7001;
    blood.count = 2;
    CHECK(!MakeChaliceGrant(110001000, {}, 1));       // the host's items are required
    CHECK(!MakeChaliceGrant(31000, {blood}, 1));      // not a dungeon lot
    const std::optional<ItemGrant> a = MakeChaliceGrant(110001000, {blood}, 1), b = MakeChaliceGrant(110001000, {blood}, 2);
    CHECK(a && b && a->source == ItemSource::Chalice && a->DoneFlag() < 0 && a->flag == kItemNone);
    CHECK(a->Key() == kChaliceKeyBase + 1 && b->Key() == kChaliceKeyBase + 2 && a->Key() > kRepeatKeyBase);
    // Two kills of the same layer boss (repeatable per dungeon): two host grants.
    HostItems h;
    CHECK(h.Offer(*a) && h.Offer(*b) && !h.Offer(*a));
    // The wire: the row carries the items, no flag; a peer row without items or with a flag is refused.
    std::vector<ItemGrant> out;
    std::string err;
    CHECK(ItemsFromJsonText(ItemsToJsonText({*a}), &out, &err) && out.size() == 1 &&
          out[0].source == ItemSource::Chalice && out[0].items.size() == 1 && out[0].items[0].id == 7001);
    CHECK(ItemGrantFromPeerAllowed(out[0]));
    CHECK(out[0].Key() < 0);  // the guest gives it its own key (RequestGuestItems)
    CHECK(!ItemsFromJsonText(R"({"items":[[1,110001000,-1,-1,"chalice"]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,110001000,52900100,-1,"chalice",[[4,7001,2]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,31000,-1,-1,"chalice",[[4,7001,2]]]]})", &out, &err));
    // Only a summoned guest in a Chalice Dungeon takes it.
    CHECK(ChaliceGrantAccepted(true, 0x1d0a5a00));
    CHECK(!ChaliceGrantAccepted(false, 0x1d0a5a00));  // at home (its own dungeon or the Dream)
    CHECK(!ChaliceGrantAccepted(true, 0x15000000));   // summoned elsewhere
    CHECK(std::string(ItemSourceName(ItemSource::Chalice)) == "chalice");
}

static void test_messenger_own() {
    CHECK(MessengerGiftLot(10050) && MessengerGiftLot(10010) && MessengerGiftLot(10000));
    CHECK(!MessengerGiftLot(10011) && !MessengerGiftLot(31000));
    CHECK(ItemLotDenied(10050, ItemLotFlag(10050)));
    CHECK(std::string(ItemSourceName(ItemSource::Own)) == "own");
    CHECK(ItemSourceFromName("own") == ItemSource::Own);
    ItemGrant own;
    own.lot = 10050;
    own.flag = ItemLotFlag(10050);
    own.source = ItemSource::Own;
    CHECK(own.flag > 0);
    CHECK(!ItemGrantFromPeerAllowed(own));
    // The guest's queue takes it like any grant (popup gate, spacing).
    GuestItems q;
    CHECK(q.Offer({own}) == 1);
    CHECK(q.Offer({own}) == 0);
    // A re-give (done flag set, the bell not held) plans with the row flags ignored: its own row
    // flag is the done flag, which would otherwise make the plan give nothing (live run 2).
    LotRowData bell;
    bell.lot = 10050;
    bell.flag = own.flag;
    Slot(&bell, 0, 4, 4312, 1, 100);
    const LotPlan skipped = PlanSummonedLot({bell}, {}, [](std::int32_t) { return true; });
    CHECK(skipped.result == LotPlanResult::Give && skipped.items.empty());
    const LotPlan regive = PlanSummonedLot({bell}, {}, [](std::int32_t) { return false; });
    CHECK(regive.result == LotPlanResult::Give && regive.items.size() == 1 && regive.items[0].id == 4312);
}

int main() {
    test_tables();
    test_fit();
    test_ledger_seed();
    test_deny();
    test_classify();
    test_json();
    test_host();
    test_gate();
    test_guest();
    test_summoned_gate();
    test_plan();
    test_items_json();
    test_gems();
    test_guest_summoned();
    test_gift_up();
    test_popup_hold();
    test_itemget_watch();
    test_ledger_suspicious();
    test_messenger_own();
    test_repeat_gift();
    test_chalice_award();
    test_key_catchup();
    std::printf("party-items-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
