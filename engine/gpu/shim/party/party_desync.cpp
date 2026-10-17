// SPDX-License-Identifier: GPL-3.0-or-later
// Party desync detector (F5): see party_desync.h.
#include "party_desync.h"

#include "net/json.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

namespace coop::desync {

int WorldBucket(int key) {
    if (key < 0) return 0;
    const int b = static_cast<int>(static_cast<long long>(key) * kBuckets / progress::kPoolBlocks);
    return std::min(b, kBuckets - 1);
}

int FlagBucket(u32 block) { return static_cast<int>((block * 2654435761u) >> 27) & (kBuckets - 1); }

Buckets WorldDigest(const progress::WorldImage& full) {
    return WorldDigestOf([&](int key) -> const progress::GroupBytes* {
        const auto it = full.groups.find(static_cast<u32>(key));
        return it == full.groups.end() ? nullptr : &it->second;
    });
}

Buckets FlagsDigest(const std::map<u32, std::vector<u32>>& blocks) {
    Buckets b;
    b.fill(kFnvBasis);
    for (const auto& [block, bits] : blocks) {
        if (bits.empty()) continue;
        u32& h = b[static_cast<std::size_t>(FlagBucket(block))];
        h = Fnv(h, &block, sizeof block);
        for (u32 bit : bits) h = Fnv(h, &bit, sizeof bit);
    }
    return b;
}

// ---- Enemy census ----

int CensusBucket(u32 entity) { return static_cast<int>((entity * 2654435761u) >> 29) & (kCensusBuckets - 1); }

namespace {
bool EntryLess(const CensusEntry& a, const CensusEntry& b) {
    return a.entity != b.entity ? a.entity < b.entity : a.alive < b.alive;
}
}  // namespace

CensusDigest MakeCensusDigest(u32 map, const std::vector<CensusEntry>& list) {
    CensusDigest d;
    d.has = true;
    d.map = map;
    d.b.fill(kFnvBasis);
    std::vector<CensusEntry> named;
    for (const CensusEntry& e : list) {
        e.alive ? ++d.alive : ++d.dead;
        if (!e.entity) {
            e.alive ? ++d.unnamed_alive : ++d.unnamed_dead;
            continue;
        }
        named.push_back(e);
    }
    std::sort(named.begin(), named.end(), EntryLess);
    for (const CensusEntry& e : named) {
        u32& h = d.b[static_cast<std::size_t>(CensusBucket(e.entity))];
        const u32 v[2] = {e.entity, e.alive ? 1u : 0u};
        h = Fnv(h, v, sizeof v);
    }
    return d;
}

std::vector<int> CensusDiffBuckets(const CensusDigest& host, const CensusDigest& mine, bool* unnamed) {
    std::vector<int> out;
    for (int i = 0; i < kCensusBuckets; ++i)
        if (host.b[static_cast<std::size_t>(i)] != mine.b[static_cast<std::size_t>(i)]) out.push_back(i);
    if (unnamed) *unnamed = host.unnamed_alive != mine.unnamed_alive || host.unnamed_dead != mine.unnamed_dead;
    return out;
}

CensusList CensusListOf(u32 map, const std::vector<CensusEntry>& all, const std::vector<int>& buckets) {
    CensusList l;
    l.map = map;
    l.buckets = buckets;
    std::sort(l.buckets.begin(), l.buckets.end());
    l.buckets.erase(std::unique(l.buckets.begin(), l.buckets.end()), l.buckets.end());
    for (const CensusEntry& e : all) {
        if (!e.entity) {
            e.alive ? ++l.unnamed_alive : ++l.unnamed_dead;
            continue;
        }
        if (std::binary_search(l.buckets.begin(), l.buckets.end(), CensusBucket(e.entity))) l.entries.push_back(e);
    }
    std::sort(l.entries.begin(), l.entries.end(), EntryLess);
    return l;
}

CensusReport CompareCensusList(const CensusList& host, const std::vector<CensusEntry>& mine) {
    CensusReport r;
    const CensusList here = CensusListOf(host.map, mine, host.buckets);
    // Several chrs can share an entity id (rare): compare the alive count per id.
    std::map<u32, std::pair<int, int>> h, m;  // entity -> (alive, dead)
    for (const CensusEntry& e : host.entries) (e.alive ? h[e.entity].first : h[e.entity].second)++;
    for (const CensusEntry& e : here.entries) (e.alive ? m[e.entity].first : m[e.entity].second)++;
    for (const auto& [id, hv] : h) {
        const auto it = m.find(id);
        if (it == m.end()) {
            r.only_host.push_back(id);
            continue;
        }
        if (hv.first < it->second.first) r.dead_on_host.push_back(id);
        if (hv.first > it->second.first) r.alive_on_host.push_back(id);
    }
    for (const auto& [id, mv] : m) {
        (void)mv;
        if (!h.count(id)) r.only_here.push_back(id);
    }
    r.unnamed_alive_delta = here.unnamed_alive - host.unnamed_alive;
    r.unnamed_dead_delta = here.unnamed_dead - host.unnamed_dead;
    return r;
}

std::string DescribeCensusReport(const CensusReport& r) {
    auto ids = [](const std::vector<u32>& v) {
        std::string o;
        for (std::size_t i = 0; i < v.size() && i < 12; ++i) {
            if (i) o += ' ';
            o += std::to_string(v[i]);
        }
        if (v.size() > 12) o += " ...";
        return o;
    };
    std::string out;
    auto add = [&](const char* what, const std::vector<u32>& v) {
        if (v.empty()) return;
        if (!out.empty()) out += "; ";
        out += std::to_string(v.size()) + " " + what + " (" + ids(v) + ")";
    };
    add("dead on the host, alive here", r.dead_on_host);
    add("alive on the host, dead here", r.alive_on_host);
    add("loaded only on the host", r.only_host);
    add("loaded only here", r.only_here);
    if (r.unnamed_alive_delta || r.unnamed_dead_delta) {
        if (!out.empty()) out += "; ";
        out += "enemies without an entity id: alive " + std::to_string(r.unnamed_alive_delta) + ", dead " +
               std::to_string(r.unnamed_dead_delta) + " here vs the host";
    }
    return out.empty() ? std::string("no difference") : out;
}

// ---- FlagModel ----

void FlagModel::Reset(const progress::FlagSnapshot& s) {
    valid_ = true;
    gap_ = false;
    epoch_ = s.epoch;
    seq_ = s.seq;
    blocks_.clear();
    for (const auto& [block, bits] : s.blocks) {
        if (bits.empty()) continue;
        std::vector<u32> v = bits;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        blocks_[block] = std::move(v);
    }
}

void FlagModel::Apply(u64 epoch, const std::vector<progress::FlagChange>& changes) {
    if (!valid_ || epoch != epoch_) return;
    for (const progress::FlagChange& c : changes) {
        if (c.seq <= seq_) continue;
        if (c.seq != seq_ + 1) gap_ = true;
        seq_ = c.seq;
        const u32 block = c.id / progress::kBitsPerBlock, bit = c.id % progress::kBitsPerBlock;
        std::vector<u32>& v = blocks_[block];
        const auto it = std::lower_bound(v.begin(), v.end(), bit);
        const bool has = it != v.end() && *it == bit;
        if (c.value && !has) v.insert(it, bit);
        if (!c.value && has) v.erase(it);
        if (v.empty()) blocks_.erase(block);
    }
}

bool FlagModel::ForgetOne() {
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->second.empty()) continue;
        it->second.erase(it->second.begin());
        if (it->second.empty()) blocks_.erase(it);
        return true;
    }
    return false;
}

std::size_t FlagModel::SetBits() const {
    std::size_t n = 0;
    for (const auto& [block, bits] : blocks_) n += bits.size();
    return n;
}

// ---- JSON ----

namespace {

json::Value BucketsJson(const Buckets& b) {
    json::Value a = json::Value::make_array();
    for (u32 x : b) a.push(static_cast<unsigned>(x));
    return a;
}

bool BucketsFrom(const json::Value& v, Buckets* out) {
    if (v.type != json::Value::Type::Array || v.array.size() != kBuckets) return false;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        const json::Value& x = v.array[i];
        if (x.type != json::Value::Type::Number || !(x.number >= 0) || x.number > 4294967295.0 ||
            x.number != static_cast<double>(static_cast<u32>(x.number)))
            return false;
        (*out)[i] = static_cast<u32>(x.number);
    }
    return true;
}

json::Value IndexJson(const std::vector<int>& v) {
    json::Value a = json::Value::make_array();
    for (int x : v) a.push(x);
    return a;
}

bool IndexFrom(const json::Value* v, std::vector<int>* out) {
    out->clear();
    if (!v) return true;
    if (v->type != json::Value::Type::Array || v->array.size() > kBuckets) return false;
    for (const json::Value& x : v->array) {
        if (x.type != json::Value::Type::Number || !(x.number >= 0) || x.number >= kBuckets ||
            x.number != static_cast<double>(static_cast<int>(x.number)))
            return false;
        out->push_back(static_cast<int>(x.number));
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return true;
}

template <class F>
bool Parse(const std::string& text, std::string* error, F body) {
    json::Value v;
    std::string err;
    if (text.size() > 64 * 1024) {
        if (error) *error = "too long";
        return false;
    }
    if (!json::parse(text, v, err)) {
        if (error) *error = err;
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object || json::u32(v, "v") != 1) throw std::runtime_error("unsupported version");
        if (!body(v)) throw std::runtime_error("bad body");
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace

namespace {

json::Value CensusBucketsJson(const CensusBuckets& b) {
    json::Value a = json::Value::make_array();
    for (u32 x : b) a.push(static_cast<unsigned>(x));
    return a;
}

bool U32From(const json::Value* v, u32* out) {
    if (!v || v->type != json::Value::Type::Number || !(v->number >= 0) || v->number > 4294967295.0 ||
        v->number != static_cast<double>(static_cast<u32>(v->number)))
        return false;
    *out = static_cast<u32>(v->number);
    return true;
}

bool CensusBucketsFrom(const json::Value& v, CensusBuckets* out) {
    if (v.type != json::Value::Type::Array || v.array.size() != kCensusBuckets) return false;
    for (std::size_t i = 0; i < kCensusBuckets; ++i)
        if (!U32From(&v.array[i], &(*out)[i])) return false;
    return true;
}

bool SmallCount(const json::Value* v, int* out) {
    if (!v || v->type != json::Value::Type::Number || !(v->number >= 0) || v->number > kCensusMaxEntries ||
        v->number != static_cast<double>(static_cast<int>(v->number)))
        return false;
    *out = static_cast<int>(v->number);
    return true;
}

bool CensusIndexFrom(const json::Value* v, std::vector<int>* out) {
    out->clear();
    if (!v || v->type != json::Value::Type::Array || v->array.size() > kCensusBuckets) return false;
    for (const json::Value& x : v->array) {
        if (x.type != json::Value::Type::Number || !(x.number >= 0) || x.number >= kCensusBuckets ||
            x.number != static_cast<double>(static_cast<int>(x.number)))
            return false;
        out->push_back(static_cast<int>(x.number));
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return true;
}

}  // namespace

std::string DigestToJson(const Digest& d) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(d.epoch));
    v.set("seq", json::hex(d.seq));
    if (d.has_world) v.set("w", BucketsJson(d.world));
    if (d.has_flags) v.set("f", BucketsJson(d.flags));
    if (d.census.has) {
        json::Value e = json::Value::make_object();
        e.set("m", static_cast<unsigned>(d.census.map));
        e.set("a", d.census.alive);
        e.set("d", d.census.dead);
        e.set("ua", d.census.unnamed_alive);
        e.set("ud", d.census.unnamed_dead);
        e.set("b", CensusBucketsJson(d.census.b));
        v.set("e", e);
    }
    return json::dump(v, 0);
}

std::string CensusAskToJson(u32 map, const std::vector<int>& buckets) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("m", static_cast<unsigned>(map));
    v.set("b", IndexJson(buckets));
    return json::dump(v, 0);
}

bool CensusAskFromJson(const std::string& text, u32* map, std::vector<int>* buckets, std::string* error) {
    return Parse(text, error, [&](const json::Value& v) {
        return U32From(v.find("m"), map) && CensusIndexFrom(v.find("b"), buckets);
    });
}

std::string CensusListToJson(const CensusList& l) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("m", static_cast<unsigned>(l.map));
    v.set("b", IndexJson(l.buckets));
    json::Value e = json::Value::make_array();
    for (const CensusEntry& x : l.entries) {
        json::Value pair = json::Value::make_array();
        pair.push(static_cast<unsigned>(x.entity));
        pair.push(x.alive ? 1 : 0);
        e.push(pair);
    }
    v.set("e", e);
    v.set("ua", l.unnamed_alive);
    v.set("ud", l.unnamed_dead);
    return json::dump(v, 0);
}

bool CensusListFromJson(const std::string& text, CensusList* out, std::string* error) {
    return Parse(text, error, [&](const json::Value& v) {
        CensusList l;
        if (!U32From(v.find("m"), &l.map) || !CensusIndexFrom(v.find("b"), &l.buckets) ||
            !SmallCount(v.find("ua"), &l.unnamed_alive) || !SmallCount(v.find("ud"), &l.unnamed_dead))
            return false;
        const json::Value* e = v.find("e");
        if (!e || e->type != json::Value::Type::Array || e->array.size() > kCensusMaxEntries) return false;
        for (const json::Value& x : e->array) {
            if (x.type != json::Value::Type::Array || x.array.size() != 2) return false;
            CensusEntry c;
            u32 alive = 0;
            if (!U32From(&x.array[0], &c.entity) || !U32From(&x.array[1], &alive) || alive > 1 || !c.entity) return false;
            c.alive = alive != 0;
            if (!std::binary_search(l.buckets.begin(), l.buckets.end(), CensusBucket(c.entity))) return false;
            l.entries.push_back(c);
        }
        *out = std::move(l);
        return true;
    });
}

bool DigestFromJson(const std::string& text, Digest* out, std::string* error) {
    return Parse(text, error, [&](const json::Value& v) {
        Digest d;
        d.epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        d.seq = json::as_u64(json::member(v, "seq"), "seq");
        if (const json::Value* w = v.find("w")) {
            if (!BucketsFrom(*w, &d.world)) return false;
            d.has_world = true;
        }
        if (const json::Value* f = v.find("f")) {
            if (!BucketsFrom(*f, &d.flags)) return false;
            d.has_flags = true;
        }
        if (const json::Value* e = v.find("e")) {  // the enemy census (newer hosts)
            if (e->type != json::Value::Type::Object || !U32From(e->find("m"), &d.census.map) ||
                !SmallCount(e->find("a"), &d.census.alive) || !SmallCount(e->find("d"), &d.census.dead) ||
                !SmallCount(e->find("ua"), &d.census.unnamed_alive) ||
                !SmallCount(e->find("ud"), &d.census.unnamed_dead) || !e->find("b") ||
                !CensusBucketsFrom(*e->find("b"), &d.census.b))
                return false;
            d.census.has = true;
        }
        *out = d;
        return true;
    });
}

std::string ResyncToJson(const Resync& r) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(r.epoch));
    v.set("w", IndexJson(r.world));
    v.set("f", IndexJson(r.flags));
    v.set("gap", r.gap ? 1 : 0);
    return json::dump(v, 0);
}

bool ResyncFromJson(const std::string& text, Resync* out, std::string* error) {
    return Parse(text, error, [&](const json::Value& v) {
        Resync r;
        r.epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        if (!IndexFrom(v.find("w"), &r.world) || !IndexFrom(v.find("f"), &r.flags)) return false;
        const json::Value* g = v.find("gap");
        r.gap = g && g->type == json::Value::Type::Number && g->number != 0;
        *out = r;
        return true;
    });
}

progress::WorldImage WorldBuckets(const progress::WorldImage& full, const std::vector<int>& buckets) {
    progress::WorldImage img;
    img.epoch = full.epoch;
    img.full = false;
    bool want[kBuckets] = {};
    for (int b : buckets)
        if (b >= 0 && b < kBuckets) want[b] = true;
    for (int key : progress::WorldMirrorKeys()) {
        if (!want[WorldBucket(key)]) continue;
        const auto it = full.groups.find(static_cast<u32>(key));
        img.groups[static_cast<u32>(key)] = it == full.groups.end() ? progress::GroupBytes{} : it->second;
    }
    return img;
}

// ---- Checker ----

const char* VerdictName(Checker::Verdict v) {
    switch (v) {
    case Checker::Verdict::Skipped: return "skipped";
    case Checker::Verdict::Match: return "match";
    case Checker::Verdict::Resolved: return "resolved";
    case Checker::Verdict::Mismatch: return "mismatch";
    case Checker::Verdict::Asked: return "asked";
    case Checker::Verdict::Persistent: return "persistent";
    }
    return "?";
}

namespace {
std::string List(const std::vector<int>& v) {
    std::string s;
    for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x);
    return s.empty() ? "-" : s;
}
}  // namespace

Checker::Result Checker::OnDigest(const Digest& host, const Mine& mine, double now_s) {
    ++n_.digests;
    Result r;
    Resync diff;
    diff.epoch = host.epoch;
    bool compared = false;
    if (host.has_world && mine.has_world) {
        compared = true;
        for (std::size_t i = 0; i < kBuckets; ++i)
            if (host.world[i] != mine.world[i]) diff.world.push_back(static_cast<int>(i));
    }
    const FlagModel* m = mine.model;
    if (host.has_flags && m && m->valid() && m->epoch() == host.epoch) {
        if (m->seq() == host.seq) {
            compared = true;
            const Buckets mf = m->Digest();
            for (std::size_t i = 0; i < kBuckets; ++i)
                if (host.flags[i] != mf[i]) diff.flags.push_back(static_cast<int>(i));
            diff.gap = m->gap() && !diff.flags.empty();
        } else if (m->seq() < host.seq) {
            // The digest comes after every change the host sent before it (one ordered stream):
            // a model behind it missed changes.
            compared = true;
            diff.gap = true;
        }
        // Ahead (seq past the host's): the host's tracker restarted within an epoch; its next
        // snapshot resets the model.
    }
    if (!compared) {
        ++n_.skipped;
        r.verdict = Verdict::Skipped;
        r.detail = !mine.has_world && !(m && m->valid()) ? "nothing received yet" : "not comparable yet";
        return r;
    }
    if (diff.empty()) {
        ++n_.matched;
        r.verdict = open_ ? Verdict::Resolved : Verdict::Match;
        if (open_) {
            ++n_.resolved;
            r.detail = "in sync again after " + std::to_string(strikes_) + " resync(s)";
        }
        open_ = false;
        strikes_ = 0;
        persistent_logged_ = false;
        return r;
    }
    ++n_.mismatched;
    r.detail = "world buckets " + List(diff.world) + ", flag buckets " + List(diff.flags) + (diff.gap ? ", seq gap" : "") +
               " (host seq " + std::to_string(host.seq) + ", mine " +
               std::to_string(m && m->valid() ? m->seq() : 0) + ")";
    if (strikes_ >= kMaxStrikes && now_s - first_strike_ >= kStrikeResetS) strikes_ = 0;  // try again
    if (strikes_ >= kMaxStrikes) {
        r.verdict = persistent_logged_ ? Verdict::Mismatch : Verdict::Persistent;
        if (!persistent_logged_) ++n_.persistent;
        persistent_logged_ = true;
        return r;
    }
    if (now_s - last_ask_ < kAskGapS) {
        r.verdict = Verdict::Mismatch;
        return r;
    }
    if (strikes_ == 0) first_strike_ = now_s;
    ++strikes_;
    ++n_.asked;
    open_ = true;
    last_ask_ = now_s;
    r.verdict = Verdict::Asked;
    r.ask = diff;
    return r;
}

bool ServeLimit::Take(int slot, double now_s) {
    auto it = last_.find(slot);
    if (it != last_.end() && now_s - it->second < kServeGapS) return false;
    last_[slot] = now_s;
    return true;
}

// ---- the game side ----

double PeriodFromEnv() {
    const char* v = std::getenv("BB_PARTY_DESYNC_S");
    if (!v || !*v) return kDefaultPeriodS;
    const double s = std::atof(v);
    return s <= 0 ? 0 : std::max(1.0, s);
}

int TestAfterFromEnv() {
    const char* v = std::getenv("BB_PARTY_DESYNC_TEST");
    return v && *v ? std::max(0, std::atoi(v)) : 0;
}

namespace {

struct GuestState {
    std::mutex mu;
    FlagModel model;
    Checker checker;
    std::string pending;
    CensusDigest census;  // the last digest's census, not taken yet
    bool census_new = false;
    CensusList census_list;
    bool census_list_new = false;
    int test_after = -1;  // -1: not read yet
    u64 matches = 0;
    bool test_fired = false;
};
GuestState& GS() {
    static GuestState s;
    return s;
}

struct HostState {
    std::mutex mu;
    std::vector<std::pair<int, Resync>> queue;
    std::vector<std::pair<int, std::pair<u32, std::vector<int>>>> census_asks;
};
HostState& HS() {
    static HostState s;
    return s;
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

void GuestOnChanges(u64 epoch, const std::vector<progress::FlagChange>& changes) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    g.model.Apply(epoch, changes);
}

void GuestOnSnapshot(const progress::FlagSnapshot& s) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    g.model.Reset(s);
}

bool GuestOnDigest(const std::string& body, bool has_world, const Buckets& world, Checker::Result* res,
                   bool* test_corrupt, std::string* error) {
    Digest d;
    if (!DigestFromJson(body, &d, error)) return false;
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    Mine mine;
    mine.has_world = has_world;
    mine.world = world;
    mine.model = &g.model;
    *res = g.checker.OnDigest(d, mine, Now());
    if (d.census.has) {
        g.census = d.census;
        g.census_new = true;
    }
    if (res->verdict == Checker::Verdict::Asked) g.pending = ResyncToJson(res->ask);
    *test_corrupt = false;
    if (g.test_after < 0) g.test_after = TestAfterFromEnv();
    if (res->verdict == Checker::Verdict::Match && g.test_after > 0 && !g.test_fired &&
        ++g.matches >= static_cast<u64>(g.test_after)) {
        g.test_fired = true;
        g.model.ForgetOne();
        *test_corrupt = true;
    }
    return true;
}

bool GuestPopResync(std::string* body) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    if (g.pending.empty()) return false;
    body->swap(g.pending);
    g.pending.clear();
    return true;
}

void GuestReset() {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    g.model = FlagModel{};
    g.checker = Checker{};
    g.pending.clear();
    g.census_new = g.census_list_new = false;
}

bool GuestPopCensusDigest(CensusDigest* out) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.census_new) return false;
    g.census_new = false;
    *out = g.census;
    return true;
}

bool GuestOnCensusList(const std::string& body, std::string* error) {
    CensusList l;
    if (!CensusListFromJson(body, &l, error)) return false;
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    g.census_list = std::move(l);
    g.census_list_new = true;
    return true;
}

bool GuestPopCensusList(CensusList* out) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.census_list_new) return false;
    g.census_list_new = false;
    *out = std::move(g.census_list);
    return true;
}

bool HostQueueCensusAsk(int slot, const std::string& body, std::string* error) {
    u32 map = 0;
    std::vector<int> buckets;
    if (!CensusAskFromJson(body, &map, &buckets, error)) return false;
    HostState& h = HS();
    std::lock_guard<std::mutex> lk(h.mu);
    h.census_asks.erase(std::remove_if(h.census_asks.begin(), h.census_asks.end(),
                                       [&](const auto& e) { return e.first == slot; }),
                        h.census_asks.end());
    if (h.census_asks.size() >= 16) {
        if (error) *error = "too many census requests queued";
        return false;
    }
    h.census_asks.push_back({slot, {map, std::move(buckets)}});
    return true;
}

std::vector<std::pair<int, std::pair<u32, std::vector<int>>>> HostPopCensusAsks() {
    HostState& h = HS();
    std::lock_guard<std::mutex> lk(h.mu);
    std::vector<std::pair<int, std::pair<u32, std::vector<int>>>> out;
    out.swap(h.census_asks);
    return out;
}

int CensusTestAfterFromEnv() {
    const char* v = std::getenv("BB_PARTY_CENSUS_TEST");
    return v && *v ? std::max(0, std::atoi(v)) : 0;
}

bool HostQueueResync(int slot, const std::string& body, std::string* error) {
    Resync r;
    if (!ResyncFromJson(body, &r, error)) return false;
    HostState& h = HS();
    std::lock_guard<std::mutex> lk(h.mu);
    // One pending request per member: a newer one replaces it (the main thread serves them).
    h.queue.erase(std::remove_if(h.queue.begin(), h.queue.end(), [&](const auto& e) { return e.first == slot; }),
                  h.queue.end());
    if (h.queue.size() >= 16) {
        if (error) *error = "too many resync requests queued";
        return false;
    }
    h.queue.emplace_back(slot, std::move(r));
    return true;
}

std::vector<std::pair<int, Resync>> HostPopResyncs() {
    HostState& h = HS();
    std::lock_guard<std::mutex> lk(h.mu);
    std::vector<std::pair<int, Resync>> out;
    out.swap(h.queue);
    return out;
}

}  // namespace coop::desync
