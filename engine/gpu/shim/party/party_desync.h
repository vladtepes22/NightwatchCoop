// SPDX-License-Identifier: GPL-3.0-or-later
// Party desync detector (F5): periodic digests of the host's synced state, checked by every guest,
// with an automatic resync of the parts that differ.
//
// What is checked: the two host-authoritative streams of C2 (party_progress.h).
//   world  the host-world mirror image (EVENT world_groups): the guest's GuestWorldMirror image
//          must equal the host's HostWorldTracker shadow byte for byte.
//   flags  the host's candidate flags (EVENT flag_snapshot + "flags"): the guest keeps a model of
//          them (FlagModel: the last snapshot plus every change after it, in seq order); it must
//          equal the host's HostTracker shadow at the digest's seq. A seq gap (a change the guest
//          never got) is a desync by itself.
// Both are hashed into kBuckets buckets (world: by group key range; flags: by block), so a
// mismatch names the part that differs and only that is sent again.
//
// Wire (PartyLink EVENTs, reliable and ordered per member, so a digest arrives after every change
// the host sent before it):
//   host -> all   "sync_digest"  {"v":1,"epoch":"<hex>","seq":"<hex>","w":[32 u32],"f":[32 u32]}
//                                ("w" / "f" absent: the host has no baseline of that part yet)
//   guest -> host "sync_resync"  {"v":1,"epoch":"<hex>","w":[bucket...],"f":[bucket...],"gap":0|1}
// The host answers a resync with the world groups of those buckets (EVENT world_groups, partial,
// zero groups included) and, for any flags bucket or a gap, a fresh flag_snapshot (the guest's
// join catch-up path: set-only, idempotent).
//
// Rate limits: the host sends a digest every BB_PARTY_DESYNC_S seconds (default 15, 0 = off) and
// serves at most one resync per member per kServeGapS; a guest asks at most once per kAskGapS and
// gives up after kMaxStrikes resyncs in a row that did not help (logged once as "persistent"), until
// a digest matches again or kStrikeResetS pass.
//
// Test aid: BB_PARTY_DESYNC_TEST=N (guest) corrupts one mirror group of its image and forgets one
// flag of its model right after the N-th matching digest, so the next digest must find, resync and
// resolve both (live check of the whole path).
//
// The pure part (buckets, digests, the model, the checker, JSON) is unit-tested (party-desync-test).
#pragma once

#include "party_progress.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coop::desync {

using progress::u32;
using progress::u64;

constexpr int kBuckets = 32;
using Buckets = std::array<u32, kBuckets>;

constexpr const char* kDigestEvent = "sync_digest";
constexpr const char* kResyncEvent = "sync_resync";

constexpr double kDefaultPeriodS = 15.0;
constexpr double kServeGapS = 10.0;
constexpr double kAskGapS = 5.0;
constexpr int kMaxStrikes = 3;
constexpr double kStrikeResetS = 300.0;

/// The bucket of a mirror group key (contiguous key ranges) / of a flag block (hashed).
int WorldBucket(int key);
int FlagBucket(u32 block);

/// Mirror groups: `group(key)` -> its 125 bytes (nullptr or all-zero: contributes nothing, so an
/// absent group equals a zero one). Only mirror keys count.
template <class GroupFn>
Buckets WorldDigestOf(GroupFn group);
/// A full image (absent groups are zero).
Buckets WorldDigest(const progress::WorldImage& full);
/// A snapshot's blocks (blocks without set bits contribute nothing).
Buckets FlagsDigest(const std::map<u32, std::vector<u32>>& blocks);

/// Guest: the host's candidate flags as the stream says. Not thread-safe by itself.
class FlagModel {
public:
    void Reset(const progress::FlagSnapshot& s);
    /// Changes in order; those of another epoch or at / below the model's seq are ignored. A change
    /// past seq + 1 is a gap: the model is marked (gap()) and stays usable for the rest.
    void Apply(u64 epoch, const std::vector<progress::FlagChange>& changes);
    bool valid() const { return valid_; }
    bool gap() const { return gap_; }
    u64 epoch() const { return epoch_; }
    u64 seq() const { return seq_; }
    Buckets Digest() const { return FlagsDigest(blocks_); }
    /// Test aid: drops one set bit (the first); false when there is none.
    bool ForgetOne();
    std::size_t SetBits() const;

private:
    bool valid_ = false, gap_ = false;
    u64 epoch_ = 0, seq_ = 0;
    std::map<u32, std::vector<u32>> blocks_;  // block -> set bits (ascending)
};

// ---- Enemy census (seamless research #3): the loaded map's enemies on every game ----
// The host's digest also carries a census digest of its loaded map: every regular enemy (team 23,
// HP read: party_balance LoadedEnemyList) keyed by its map entity id (chr +0x1F0, the same on every
// game; the census key +0x3B8 is not), hashed into kCensusBuckets buckets by entity, plus the alive /
// dead counts of the enemies without an entity id (no key that is the same on every game). A guest
// in the same map compares it with its own census; a difference that is still there 3 s later (a
// kill in flight reaches the other game within a second) asks the host for those buckets' entries
// ("census_ask") and logs exactly which enemies differ ("census_list"). Report only: the game's own
// enemy sync owns the enemies; nothing is changed. Wire, inside sync_digest:
//   "e": {"m": map, "a": alive, "d": dead, "ua": unnamed alive, "ud": unnamed dead, "b": [8 u32]}
//   guest -> host "census_ask"  {"v":1,"m":map,"b":[bucket...]}
//   host -> guest "census_list" {"v":1,"m":map,"b":[bucket...],"e":[[entity,alive]...],"ua":n,"ud":n}
constexpr int kCensusBuckets = 8;
constexpr const char* kCensusAskEvent = "census_ask";
constexpr const char* kCensusListEvent = "census_list";
constexpr double kCensusConfirmS = 3.0;  // a difference must still be there this much later
constexpr int kCensusMaxEntries = 4096;
using CensusBuckets = std::array<u32, kCensusBuckets>;

struct CensusEntry {
    u32 entity = 0;  ///< map entity id (chr +0x1F0); 0 = none
    bool alive = false;
    bool operator==(const CensusEntry& o) const { return entity == o.entity && alive == o.alive; }
};
struct CensusDigest {
    bool has = false;
    u32 map = 0;  ///< the loaded map (0xAABBCCDD)
    int alive = 0, dead = 0, unnamed_alive = 0, unnamed_dead = 0;
    CensusBuckets b{};
    bool operator==(const CensusDigest& o) const {
        return has == o.has && map == o.map && alive == o.alive && dead == o.dead && unnamed_alive == o.unnamed_alive &&
               unnamed_dead == o.unnamed_dead && b == o.b;
    }
};
int CensusBucket(u32 entity);
CensusDigest MakeCensusDigest(u32 map, const std::vector<CensusEntry>& list);
/// The buckets that differ; *unnamed: the counts of the enemies without an entity id differ.
std::vector<int> CensusDiffBuckets(const CensusDigest& host, const CensusDigest& mine, bool* unnamed);

struct CensusList {
    u32 map = 0;
    std::vector<int> buckets;          // ascending
    std::vector<CensusEntry> entries;  // the named entries of those buckets, by entity
    int unnamed_alive = 0, unnamed_dead = 0;
};
CensusList CensusListOf(u32 map, const std::vector<CensusEntry>& all, const std::vector<int>& buckets);
struct CensusReport {
    std::vector<u32> dead_on_host;   ///< dead on the host, alive here
    std::vector<u32> alive_on_host;  ///< alive on the host, dead here
    std::vector<u32> only_host;      ///< loaded on the host, not here
    std::vector<u32> only_here;      ///< loaded here, not on the host
    int unnamed_alive_delta = 0;     ///< here minus host
    int unnamed_dead_delta = 0;
    bool empty() const {
        return dead_on_host.empty() && alive_on_host.empty() && only_host.empty() && only_here.empty() &&
               !unnamed_alive_delta && !unnamed_dead_delta;
    }
};
/// The host's entries of `host.buckets` against this game's census (the same buckets).
CensusReport CompareCensusList(const CensusList& host, const std::vector<CensusEntry>& mine);
std::string DescribeCensusReport(const CensusReport& r);

std::string CensusAskToJson(u32 map, const std::vector<int>& buckets);
bool CensusAskFromJson(const std::string& text, u32* map, std::vector<int>* buckets, std::string* error = nullptr);
std::string CensusListToJson(const CensusList& l);
bool CensusListFromJson(const std::string& text, CensusList* out, std::string* error = nullptr);

struct Digest {
    u64 epoch = 0, seq = 0;
    bool has_world = false, has_flags = false;
    Buckets world{}, flags{};
    CensusDigest census;  ///< the host's loaded map's enemies (absent: an older host or no world)
    bool operator==(const Digest& o) const {
        return epoch == o.epoch && seq == o.seq && has_world == o.has_world && has_flags == o.has_flags &&
               world == o.world && flags == o.flags && census == o.census;
    }
};
std::string DigestToJson(const Digest& d);
bool DigestFromJson(const std::string& text, Digest* out, std::string* error = nullptr);

struct Resync {
    u64 epoch = 0;
    std::vector<int> world, flags;  // bucket indices, ascending, unique
    bool gap = false;
    bool empty() const { return world.empty() && flags.empty() && !gap; }
    bool operator==(const Resync& o) const {
        return epoch == o.epoch && world == o.world && flags == o.flags && gap == o.gap;
    }
};
std::string ResyncToJson(const Resync& r);
bool ResyncFromJson(const std::string& text, Resync* out, std::string* error = nullptr);

/// Host: the groups of `buckets` from the full image, as a partial image (zero groups included,
/// so the guest's wrong bytes are overwritten).
progress::WorldImage WorldBuckets(const progress::WorldImage& full, const std::vector<int>& buckets);

/// Guest: what the guest holds, for one digest from the host.
struct Mine {
    bool has_world = false;  // an image arrived
    Buckets world{};
    const FlagModel* model = nullptr;
};

/// Guest: the decision per digest (pure; the clock is passed in).
class Checker {
public:
    enum class Verdict { Skipped, Match, Resolved, Mismatch, Asked, Persistent };
    struct Result {
        Verdict verdict = Verdict::Skipped;
        Resync ask;          // to send when verdict == Asked
        std::string detail;  // for the log
    };
    Result OnDigest(const Digest& host, const Mine& mine, double now_s);
    struct Counters {
        u64 digests = 0, matched = 0, mismatched = 0, asked = 0, resolved = 0, persistent = 0, skipped = 0;
    };
    const Counters& counters() const { return n_; }
    int strikes() const { return strikes_; }

private:
    Counters n_;
    int strikes_ = 0;           // resyncs asked since the last match
    bool open_ = false;         // a mismatch is open (asked, not resolved yet)
    bool persistent_logged_ = false;
    double last_ask_ = -1e18, first_strike_ = 0;
};
const char* VerdictName(Checker::Verdict v);

/// Host: at most one resync per member per kServeGapS.
class ServeLimit {
public:
    bool Take(int slot, double now_s);

private:
    std::map<int, double> last_;
};

// ---- the game side (party_progress_bridge.cpp / party_runtime.cpp) ----

/// BB_PARTY_DESYNC_S (default 15; 0 = off).
double PeriodFromEnv();
/// BB_PARTY_DESYNC_TEST (0 = off).
int TestAfterFromEnv();

/// Guest, the link's callback thread: a "flags" / "flag_snapshot" event the progress layer took.
void GuestOnChanges(u64 epoch, const std::vector<progress::FlagChange>& changes);
void GuestOnSnapshot(const progress::FlagSnapshot& s);
/// Guest, the link's callback thread: EVENT sync_digest. `world` = the guest's mirror digest
/// (has_world false without an image). A resync to send is queued for GuestPopResync. *test_corrupt:
/// the BB_PARTY_DESYNC_TEST aid fired (the model already forgot a flag; the caller corrupts one
/// mirror group).
bool GuestOnDigest(const std::string& body, bool has_world, const Buckets& world, Checker::Result* res,
                   bool* test_corrupt, std::string* error = nullptr);
/// Guest, main thread: the resync body to send to the host (false: none).
bool GuestPopResync(std::string* body);
/// Guest: a new host / session: the model and the checker start over.
void GuestReset();

/// Guest, the link's callback thread: the census part of the last digest (taken by the main thread).
bool GuestPopCensusDigest(CensusDigest* out);
/// Guest, the link's callback thread: EVENT census_list (validated, kept for the main thread).
bool GuestOnCensusList(const std::string& body, std::string* error = nullptr);
bool GuestPopCensusList(CensusList* out);
/// Host, the link's callback thread: EVENT census_ask from `slot` (validated, queued; one per member).
bool HostQueueCensusAsk(int slot, const std::string& body, std::string* error = nullptr);
std::vector<std::pair<int, std::pair<u32, std::vector<int>>>> HostPopCensusAsks();
/// BB_PARTY_CENSUS_TEST (guest, 0 = off): after the N-th matching census, this game's census
/// reports one living named enemy as dead (a fake kill on one side) until the census list has
/// named it, so the next digest must find it and the list must name it (live check of the path).
int CensusTestAfterFromEnv();

/// Host, the link's callback thread: EVENT sync_resync from `slot` (validated, queued).
bool HostQueueResync(int slot, const std::string& body, std::string* error = nullptr);
/// Host, main thread: the queued requests (slot, request).
std::vector<std::pair<int, Resync>> HostPopResyncs();

// FNV-1a, 32 bit.
inline u32 Fnv(u32 h, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
constexpr u32 kFnvBasis = 2166136261u;

template <class GroupFn>
Buckets WorldDigestOf(GroupFn group) {
    Buckets b;
    b.fill(kFnvBasis);
    for (int key : progress::WorldMirrorKeys()) {
        const progress::GroupBytes* g = group(key);
        if (!g) continue;
        bool any = false;
        for (std::uint8_t x : *g) any = any || x;
        if (!any) continue;
        u32& h = b[WorldBucket(key)];
        const u32 k = static_cast<u32>(key);
        h = Fnv(Fnv(h, &k, sizeof k), g->data(), g->size());
    }
    return b;
}

}  // namespace coop::desync
