// SPDX-License-Identifier: GPL-3.0-or-later
// Party desync detector (gpu/shim/party/party_desync, F5): buckets, digests of the host-world
// mirror and of the host's candidate flags, the guest's flag model (snapshot + changes, seq gaps),
// the checker's decisions and rate limits, the resync round trip (lost events -> mismatch ->
// partial world image / snapshot -> in sync again), JSON round trips and hostile bodies, and the
// game-side queues (GuestOnDigest / GuestPopResync / HostQueueResync, BB_PARTY_DESYNC_TEST).
// Build and run: cmake --build out/gpu --target party-desync-test && out/gpu/party-desync-test.exe
#include "party/party_desync.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace coop;
using namespace coop::progress;
using namespace coop::desync;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

namespace {

std::vector<int> Differ(const Buckets& a, const Buckets& b) {
    std::vector<int> d;
    for (int i = 0; i < kBuckets; ++i)
        if (a[static_cast<std::size_t>(i)] != b[static_cast<std::size_t>(i)]) d.push_back(i);
    return d;
}

void TestBuckets() {
    std::set<int> used;
    for (int key : WorldMirrorKeys()) {
        const int b = WorldBucket(key);
        CHECK(b >= 0 && b < kBuckets);
        used.insert(b);
    }
    CHECK(used.size() >= kBuckets / 2);  // the mirror keys spread over the buckets
    std::set<int> fb;
    for (u32 block : CandidateBlocks()) {
        const int b = FlagBucket(block);
        CHECK(b >= 0 && b < kBuckets);
        fb.insert(b);
    }
    CHECK(fb.size() >= kBuckets / 2);
    // An absent group digests like a zero one; a set byte changes exactly its bucket.
    WorldImage a, z;
    a.full = z.full = true;
    const int key = WorldMirrorKeys()[WorldMirrorKeys().size() / 2];
    z.groups[static_cast<u32>(key)] = GroupBytes{};
    CHECK(WorldDigest(a) == WorldDigest(z));
    a.groups[static_cast<u32>(key)][7] = 0x10;
    const std::vector<int> d = Differ(WorldDigest(a), WorldDigest(z));
    CHECK(d.size() == 1 && d[0] == WorldBucket(key));
}

// A host with a fake set of candidate blocks; its tracker's stream feeds the guest's model.
struct FakeHost {
    std::mt19937 rng{1234};
    std::map<u32, std::array<u8, kBlockBytes>> game;
    HostTracker tracker{0xabc};
    FakeHost() {
        for (u32 b : CandidateBlocks()) game[b] = {};
        std::vector<FlagChange> sink;
        for (auto& [b, data] : game) tracker.ObserveBlock(b, data.data(), &sink);
        tracker.EndScan();
    }
    std::vector<FlagChange> Mutate(int n) {
        std::vector<FlagChange> out;
        const std::vector<u32>& blocks = CandidateBlocks();
        for (int i = 0; i < n; ++i) {
            const u32 b = blocks[rng() % blocks.size()];
            const auto* mask = CandidateMask(b);
            u32 bit = rng() % kBitsPerBlock;
            for (int tries = 0; tries < 5000 && !GetBit(mask->data(), bit); ++tries) bit = rng() % kBitsPerBlock;
            if (!GetBit(mask->data(), bit)) continue;
            PutBit(game[b].data(), bit, !GetBit(game[b].data(), bit));
            tracker.ObserveBlock(b, game[b].data(), &out);
        }
        return out;
    }
    Digest MakeDigest(const WorldImage* world = nullptr) const {
        Digest d;
        d.epoch = tracker.epoch();
        const FlagSnapshot s = tracker.Snapshot(0);
        d.seq = s.seq;
        d.has_flags = true;
        d.flags = FlagsDigest(s.blocks);
        if (world) {
            d.has_world = true;
            d.world = WorldDigest(*world);
        }
        return d;
    }
};

void TestFlagModel() {
    FakeHost host;
    host.Mutate(300);  // progress before the guest joins
    FlagModel guest;
    CHECK(!guest.valid());
    guest.Reset(host.tracker.Snapshot(0));
    CHECK(guest.valid() && guest.seq() == host.tracker.seq() && guest.epoch() == 0xabc);
    CHECK(guest.Digest() == FlagsDigest(host.tracker.Snapshot(0).blocks));
    CHECK(guest.SetBits() > 0);
    for (int round = 0; round < 50; ++round) {
        const std::vector<FlagChange> batch = host.Mutate(1 + static_cast<int>(host.rng() % 20));
        guest.Apply(0xabc, batch);
        guest.Apply(0xabc, batch);  // replayed after a reconnect: ignored by seq
        guest.Apply(0x999, batch);  // another session's: ignored
    }
    CHECK(!guest.gap());
    CHECK(guest.seq() == host.tracker.seq());
    CHECK(guest.Digest() == FlagsDigest(host.tracker.Snapshot(0).blocks));

    // The checker over that stream: a lost batch is found (gap + differing buckets), asked once,
    // fixed by a snapshot, then "resolved".
    Checker ck;
    Mine mine;
    mine.model = &guest;
    double now = 100;
    CHECK(ck.OnDigest(host.MakeDigest(), mine, now).verdict == Checker::Verdict::Match);
    const std::vector<FlagChange> lost = host.Mutate(5);
    guest.Apply(0xabc, host.Mutate(5));
    CHECK(guest.gap());
    Checker::Result r = ck.OnDigest(host.MakeDigest(), mine, now += 15);
    CHECK(r.verdict == Checker::Verdict::Asked);
    CHECK(r.ask.gap && !r.ask.flags.empty() && r.ask.world.empty());
    // The buckets the guest asks for are those of the lost batch's flags.
    std::set<int> lost_buckets;
    for (const FlagChange& c : lost) lost_buckets.insert(FlagBucket(c.id / kBitsPerBlock));
    for (int b : r.ask.flags) CHECK(lost_buckets.count(b) == 1);
    guest.Reset(host.tracker.Snapshot(0));  // the host's answer
    r = ck.OnDigest(host.MakeDigest(), mine, now += 15);
    CHECK(r.verdict == Checker::Verdict::Resolved);
    CHECK(ck.counters().resolved == 1 && ck.counters().asked == 1);
    CHECK(ck.OnDigest(host.MakeDigest(), mine, now += 15).verdict == Checker::Verdict::Match);

    // A model behind the digest (changes the guest never got, nothing after them): a gap.
    (void)host.Mutate(3);
    r = ck.OnDigest(host.MakeDigest(), mine, now += 15);
    CHECK(r.verdict == Checker::Verdict::Asked && r.ask.gap);
    guest.Reset(host.tracker.Snapshot(0));
    CHECK(ck.OnDigest(host.MakeDigest(), mine, now += 15).verdict == Checker::Verdict::Resolved);

    // Another session's digest, no model, no world: nothing to compare.
    Digest other = host.MakeDigest();
    other.epoch = 0x777;
    CHECK(ck.OnDigest(other, mine, now += 15).verdict == Checker::Verdict::Skipped);
    Checker fresh;
    Mine none;
    CHECK(fresh.OnDigest(host.MakeDigest(), none, 0).verdict == Checker::Verdict::Skipped);
}

// The host-world mirror: partial images in order keep the guest's image equal; a lost one is
// found by bucket and the partial resync of those buckets alone fixes it.
void TestWorld() {
    std::mt19937 rng(99);
    std::vector<u8> pool(kPoolBlocks * kBlockBytes, 0);
    const std::vector<int>& keys = WorldMirrorKeys();
    auto poke = [&](int n) {
        for (int i = 0; i < n; ++i) {
            const int key = keys[rng() % keys.size()];
            const GroupBytes& mask = WorldMirrorMask(key);
            u32 byte = rng() % kBlockBytes;
            for (int t = 0; t < 200 && !mask[byte]; ++t) byte = rng() % kBlockBytes;
            pool[static_cast<std::size_t>(key) * kBlockBytes + byte] ^= static_cast<u8>(1u << (rng() % 8));
        }
    };
    poke(400);
    HostWorldTracker host;
    WorldImage sink;
    host.Observe(pool.data(), &sink);  // baseline
    GuestWorldMirror guest;
    guest.Receive(host.Full(5));
    auto mine = [&] { return WorldDigestOf([&](int key) { return &guest.Group(key); }); };
    CHECK(mine() == WorldDigest(host.Full(5)));
    for (int round = 0; round < 30; ++round) {
        poke(1 + static_cast<int>(rng() % 10));
        WorldImage part;
        host.Observe(pool.data(), &part);
        part.epoch = 5;
        guest.Receive(part);
    }
    CHECK(mine() == WorldDigest(host.Full(5)));
    // A lost partial image.
    poke(3);
    WorldImage lost;
    host.Observe(pool.data(), &lost);
    CHECK(!lost.groups.empty());
    std::set<int> lost_buckets;
    for (const auto& [key, g] : lost.groups) lost_buckets.insert(WorldBucket(static_cast<int>(key)));
    const WorldImage full = host.Full(5);
    const std::vector<int> d = Differ(WorldDigest(full), mine());
    CHECK(!d.empty());
    CHECK(std::set<int>(d.begin(), d.end()) == lost_buckets);
    // The resync: only those buckets' groups, zero groups included.
    const WorldImage fix = WorldBuckets(full, d);
    CHECK(!fix.full && !fix.groups.empty() && fix.groups.size() < keys.size());
    for (const auto& [key, g] : fix.groups) CHECK(std::count(d.begin(), d.end(), WorldBucket(static_cast<int>(key))) == 1);
    guest.Receive(fix);
    CHECK(mine() == WorldDigest(full));
    // A group the host cleared to zero (absent from its full image) is cleared on the guest too.
    const int key = static_cast<int>(lost.groups.begin()->first);
    std::fill(pool.begin() + key * kBlockBytes, pool.begin() + (key + 1) * kBlockBytes, 0);
    WorldImage cleared;
    host.Observe(pool.data(), &cleared);  // lost as well
    const WorldImage full2 = host.Full(5);
    CHECK(full2.groups.count(static_cast<u32>(key)) == 0);
    guest.Receive(WorldBuckets(full2, Differ(WorldDigest(full2), mine())));
    CHECK(mine() == WorldDigest(full2));
    // The test aid corrupts the image: the digest differs in one bucket.
    const int bad = guest.TestCorrupt();
    CHECK(bad >= 0);
    const std::vector<int> d2 = Differ(WorldDigest(full2), mine());
    CHECK(d2.size() == 1 && d2[0] == WorldBucket(bad));

    // The checker on world only.
    Checker ck;
    Mine m;
    m.has_world = true;
    m.world = mine();
    Digest hd;
    hd.epoch = 5;
    hd.has_world = true;
    hd.world = WorldDigest(full2);
    Checker::Result r = ck.OnDigest(hd, m, 10);
    CHECK(r.verdict == Checker::Verdict::Asked && r.ask.world == d2 && r.ask.flags.empty() && !r.ask.gap);
    guest.Receive(WorldBuckets(full2, r.ask.world));
    m.world = mine();
    CHECK(ck.OnDigest(hd, m, 25).verdict == Checker::Verdict::Resolved);
}

void TestCheckerLimits() {
    Digest hd;
    hd.epoch = 1;
    hd.has_world = true;
    hd.world.fill(1);
    Mine m;
    m.has_world = true;
    m.world.fill(2);
    Checker ck;
    double t = 0;
    CHECK(ck.OnDigest(hd, m, t).verdict == Checker::Verdict::Asked);
    CHECK(ck.OnDigest(hd, m, t + 1).verdict == Checker::Verdict::Mismatch);  // within kAskGapS
    CHECK(ck.OnDigest(hd, m, t += 15).verdict == Checker::Verdict::Asked);
    CHECK(ck.OnDigest(hd, m, t += 15).verdict == Checker::Verdict::Asked);
    CHECK(ck.strikes() == kMaxStrikes);
    CHECK(ck.OnDigest(hd, m, t += 15).verdict == Checker::Verdict::Persistent);  // logged once
    CHECK(ck.OnDigest(hd, m, t += 15).verdict == Checker::Verdict::Mismatch);
    CHECK(ck.counters().asked == kMaxStrikes && ck.counters().persistent == 1);
    // After kStrikeResetS it tries again.
    CHECK(ck.OnDigest(hd, m, t + kStrikeResetS).verdict == Checker::Verdict::Asked);
    // A match clears everything.
    m.world.fill(1);
    CHECK(ck.OnDigest(hd, m, t + kStrikeResetS + 15).verdict == Checker::Verdict::Resolved);
    m.world.fill(2);
    CHECK(ck.OnDigest(hd, m, t + kStrikeResetS + 30).verdict == Checker::Verdict::Asked);

    ServeLimit sl;
    CHECK(sl.Take(1, 0) && !sl.Take(1, kServeGapS - 1) && sl.Take(2, 1) && sl.Take(1, kServeGapS + 0.5));
}

void TestJson() {
    Digest d;
    d.epoch = 0xfedcba9876543210ull;
    d.seq = 123456789012ull;
    d.has_world = true;
    for (int i = 0; i < kBuckets; ++i) d.world[static_cast<std::size_t>(i)] = 0xffffffffu - static_cast<u32>(i);
    Digest back;
    CHECK(DigestFromJson(DigestToJson(d), &back) && back == d);
    d.has_world = false;
    d.world = {};  // not sent
    d.has_flags = true;
    d.flags[3] = 7;
    CHECK(DigestFromJson(DigestToJson(d), &back) && back == d);
    Resync r;
    r.epoch = 42;
    r.world = {0, 5, 31};
    r.flags = {2};
    r.gap = true;
    Resync rb;
    CHECK(ResyncFromJson(ResyncToJson(r), &rb) && rb == r);
    std::string err;
    for (const char* bad :
         {"", "[]", "{}", R"({"v":2,"epoch":"0x1","seq":"0x1"})", R"({"v":1,"epoch":"0x1","seq":"0x1","w":[1,2]})",
          R"({"v":1,"epoch":"0x1","seq":"0x1","w":"x"})", R"({"v":1,"seq":"0x1"})",
          R"({"v":1,"epoch":"0x1","seq":"0x1","f":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1]})",
          R"({"v":1,"epoch":"0x1","seq":"0x1","f":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1.5]})",
          R"({"v":1,"epoch":"0x1","seq":"0x1","f":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4294967296]})"})
        CHECK(!DigestFromJson(bad, &back, &err));
    for (const char* bad : {R"({"v":1,"epoch":"0x1","w":[32]})", R"({"v":1,"epoch":"0x1","w":[-1]})",
                            R"({"v":1,"epoch":"0x1","f":[0.5]})", R"({"v":1,"epoch":"0x1","w":{}})", R"({"v":1})"})
        CHECK(!ResyncFromJson(bad, &rb, &err));
    std::string many = R"({"v":1,"epoch":"0x1","w":[)";
    for (int i = 0; i < 40; ++i) many += (i ? "," : "") + std::to_string(i % 32);
    CHECK(!ResyncFromJson(many + "]}", &rb, &err));  // more entries than buckets
    CHECK(ResyncFromJson(R"({"v":1,"epoch":"0x1","w":[3,3,1]})", &rb, &err) && rb.world == std::vector<int>({1, 3}));
    CHECK(!DigestFromJson(std::string(70000, ' ') + "{}", &back, &err));
}

void TestGameQueues() {
#ifdef _WIN32
    _putenv("BB_PARTY_DESYNC_TEST=2");
    _putenv("BB_PARTY_DESYNC_S=3");
#else
    setenv("BB_PARTY_DESYNC_TEST", "2", 1);
    setenv("BB_PARTY_DESYNC_S", "3", 1);
#endif
    CHECK(PeriodFromEnv() == 3.0 && TestAfterFromEnv() == 2);
    FakeHost host;
    host.Mutate(50);
    GuestReset();
    GuestOnSnapshot(host.tracker.Snapshot(0));
    GuestOnChanges(0xabc, host.Mutate(10));
    Checker::Result res;
    bool corrupt = false;
    Buckets none{};
    std::string body;
    CHECK(GuestOnDigest(DigestToJson(host.MakeDigest()), false, none, &res, &corrupt));
    CHECK(res.verdict == Checker::Verdict::Match && !corrupt);
    CHECK(GuestOnDigest(DigestToJson(host.MakeDigest()), false, none, &res, &corrupt));
    CHECK(res.verdict == Checker::Verdict::Match && corrupt);  // the 2nd match: the test aid fires
    CHECK(!GuestPopResync(&body));
    CHECK(GuestOnDigest(DigestToJson(host.MakeDigest()), false, none, &res, &corrupt));
    CHECK(res.verdict == Checker::Verdict::Asked && !corrupt);
    CHECK(GuestPopResync(&body) && !GuestPopResync(&body));
    // The host side takes the body.
    CHECK(HostQueueResync(1, body));
    CHECK(HostQueueResync(1, body));  // a second one from the same member replaces the first
    CHECK(!HostQueueResync(2, "{\"v\":1}"));
    std::vector<std::pair<int, Resync>> q = HostPopResyncs();
    CHECK(q.size() == 1 && q[0].first == 1 && !q[0].second.flags.empty() && q[0].second.epoch == 0xabc);
    CHECK(HostPopResyncs().empty());
    GuestOnSnapshot(host.tracker.Snapshot(0));  // the host's answer
    CHECK(GuestOnDigest(DigestToJson(host.MakeDigest()), false, none, &res, &corrupt));
    CHECK(res.verdict == Checker::Verdict::Resolved);
    CHECK(!GuestOnDigest("garbage", false, none, &res, &corrupt));
#ifdef _WIN32
    _putenv("BB_PARTY_DESYNC_S=0");
#else
    setenv("BB_PARTY_DESYNC_S", "0", 1);
#endif
    CHECK(PeriodFromEnv() == 0);
}

}  // namespace

// Seamless research #3: the enemy census in the digest.
void TestCensus() {
    // A Central Yharnam-like census: named enemies (map entity ids) and some without an id.
    std::vector<CensusEntry> host;
    for (u32 i = 0; i < 60; ++i) host.push_back({2410000u + i * 7u, i % 9 != 0});
    for (int i = 0; i < 5; ++i) host.push_back({0, i != 0});
    std::vector<CensusEntry> guest(host.rbegin(), host.rend());  // another chr order
    const CensusDigest hd = MakeCensusDigest(0x18010000u, host);
    const CensusDigest gd = MakeCensusDigest(0x18010000u, guest);
    CHECK(hd.has && hd == gd);  // order does not matter
    CHECK(hd.alive + hd.dead == 65 && hd.unnamed_alive == 4 && hd.unnamed_dead == 1);
    bool unnamed = true;
    CHECK(CensusDiffBuckets(hd, gd, &unnamed).empty() && !unnamed);
    // A kill on one side changes exactly the bucket of that entity.
    std::vector<CensusEntry> killed = guest;
    u32 victim = 0;
    for (CensusEntry& e : killed)
        if (e.entity && e.alive) {
            e.alive = false;
            victim = e.entity;
            break;
        }
    const CensusDigest kd = MakeCensusDigest(0x18010000u, killed);
    const std::vector<int> diff = CensusDiffBuckets(hd, kd, &unnamed);
    CHECK(diff.size() == 1 && diff[0] == CensusBucket(victim) && !unnamed);
    // The host's list of that bucket names it: alive on the host, dead here.
    const CensusList list = CensusListOf(0x18010000u, host, diff);
    CHECK(!list.entries.empty() && list.unnamed_alive == 4 && list.unnamed_dead == 1);
    for (const CensusEntry& e : list.entries) CHECK(CensusBucket(e.entity) == diff[0]);
    CensusReport r = CompareCensusList(list, killed);
    CHECK(r.alive_on_host.size() == 1 && r.alive_on_host[0] == victim && r.dead_on_host.empty() && r.only_host.empty() &&
          r.only_here.empty() && !r.unnamed_alive_delta);
    CHECK(DescribeCensusReport(r).find(std::to_string(victim)) != std::string::npos);
    CHECK(CompareCensusList(list, guest).empty());  // the kill reached this game: no difference
    CHECK(DescribeCensusReport(CompareCensusList(list, guest)) == "no difference");
    // An enemy without an id killed on one side: only the counts tell.
    std::vector<CensusEntry> u = guest;
    for (CensusEntry& e : u)
        if (!e.entity && e.alive) {
            e.alive = false;
            break;
        }
    CHECK(CensusDiffBuckets(hd, MakeCensusDigest(0x18010000u, u), &unnamed).empty() && unnamed);
    const CensusReport ur = CompareCensusList(CensusListOf(0x18010000u, host, {}), u);
    CHECK(ur.unnamed_alive_delta == -1 && ur.unnamed_dead_delta == 1 && !ur.empty());
    // Loaded on one side only.
    std::vector<CensusEntry> extra = guest;
    extra.push_back({2419999u, true});
    const std::vector<int> d2 = CensusDiffBuckets(hd, MakeCensusDigest(0x18010000u, extra), nullptr);
    CHECK(d2.size() == 1);
    CHECK(CompareCensusList(CensusListOf(0x18010000u, host, d2), extra).only_here.size() == 1);

    // JSON: the digest with a census round-trips; an older digest without "e" still parses.
    Digest d;
    d.epoch = 7;
    d.seq = 9;
    d.has_flags = true;
    d.flags.fill(3);
    d.census = hd;
    Digest back;
    std::string err;
    CHECK(DigestFromJson(DigestToJson(d), &back, &err) && back == d);
    Digest old = d;
    old.census = CensusDigest{};
    CHECK(DigestFromJson(DigestToJson(old), &back, &err) && back == old && !back.census.has);
    CHECK(!DigestFromJson(R"({"v":1,"epoch":"1","seq":"1","e":{"m":1,"a":1,"d":0,"ua":0,"ud":0,"b":[1,2]}})", &back, &err));
    CHECK(!DigestFromJson(R"({"v":1,"epoch":"1","seq":"1","e":{"m":1,"a":-1,"d":0,"ua":0,"ud":0,"b":[1,2,3,4,5,6,7,8]}})",
                          &back, &err));
    // The ask and the list.
    u32 m = 0;
    std::vector<int> b;
    CHECK(CensusAskFromJson(CensusAskToJson(0x18010000u, {3, 1, 3}), &m, &b, &err) && m == 0x18010000u &&
          b == std::vector<int>({1, 3}));
    CHECK(!CensusAskFromJson(R"({"v":1,"m":1,"b":[8]})", &m, &b, &err));
    CensusList lb;
    CHECK(CensusListFromJson(CensusListToJson(list), &lb, &err) && lb.map == list.map && lb.buckets == list.buckets &&
          lb.entries == list.entries && lb.unnamed_alive == 4 && lb.unnamed_dead == 1);
    // A list entry outside the asked buckets, an id 0 or an alive value other than 0/1 is refused.
    const int other = (diff[0] + 1) % kCensusBuckets;
    u32 outside = 1;
    while (CensusBucket(outside) != other) ++outside;
    CHECK(!CensusListFromJson(R"({"v":1,"m":1,"b":[)" + std::to_string(diff[0]) + R"(],"e":[[)" +
                                  std::to_string(outside) + R"(,1]],"ua":0,"ud":0})",
                              &lb, &err));
    CHECK(!CensusListFromJson(R"({"v":1,"m":1,"b":[0],"e":[[0,1]],"ua":0,"ud":0})", &lb, &err));
    CHECK(!CensusListFromJson(R"({"v":1,"m":1,"b":[0],"e":[],"ua":-2,"ud":0})", &lb, &err));

    // The game-side queues.
    CHECK(GuestOnCensusList(CensusListToJson(list), &err));
    CensusList popped;
    CHECK(GuestPopCensusList(&popped) && popped.entries == list.entries && !GuestPopCensusList(&popped));
    CHECK(HostQueueCensusAsk(1, CensusAskToJson(5, {2}), &err) && HostQueueCensusAsk(1, CensusAskToJson(6, {4}), &err));
    const auto asks = HostPopCensusAsks();
    CHECK(asks.size() == 1 && asks[0].first == 1 && asks[0].second.first == 6u && asks[0].second.second == std::vector<int>({4}));
    CHECK(!HostQueueCensusAsk(2, "{}", &err));
    std::string body = DigestToJson(d);
    Checker::Result res;
    bool corrupt = false;
    Buckets w{};
    CHECK(GuestOnDigest(body, false, w, &res, &corrupt, &err));
    CensusDigest cd;
    CHECK(GuestPopCensusDigest(&cd) && cd == hd && !GuestPopCensusDigest(&cd));
}

int main() {
    TestBuckets();
    TestFlagModel();
    TestWorld();
    TestCheckerLimits();
    TestJson();
    TestGameQueues();
    TestCensus();
    std::printf("party-desync-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
