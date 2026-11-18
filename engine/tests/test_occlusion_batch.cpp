// SPDX-License-Identifier: GPL-3.0-or-later
// BB_OCCLUSION_BATCH (occlusion_batch.h; BBFSS 1451700): reading a batch's query ranges up front, merged,
// gives every block the same sum as reading each block's queries when it is answered (today's path).
// A simulated query ring with wrap-around; batches drawn from the live window the resolver keeps.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/occlusion_batch.h"
#include "gpu/shadps4/video_core/renderer_vulkan/occlusion_model.h"

namespace {
using u64 = std::uint64_t;
constexpr u64 Ring = 64; // small, so batches cross the ring's end often

struct Record {
    bool closed;
    bool complete;
    u64 first_query;
    u64 queries;
};

struct Pool {
    std::vector<u64> value = std::vector<u64>(Ring), available = std::vector<u64>(Ring);
    std::vector<u64> owner = std::vector<u64>(Ring, ~0ull); // the absolute query in each slot
};

// Today's Sum: the block's queries read from the pool when it is answered.
u64 SumNow(const Pool& pool, const Record& r, u64& missing) {
    u64 samples = 0;
    for (u64 q = r.first_query; q < r.first_query + r.queries; ++q) {
        const u64 slot = q % Ring;
        assert(pool.owner[slot] == q);
        missing += pool.available[slot] ? 0 : 1;
        samples += pool.available[slot] ? pool.value[slot] : 1000000;
    }
    return samples;
}

// The batched path: ForEachRange's reads (split at the ring's end, as Read does) into a slot-indexed
// copy, then each block summed from the copy.
struct Batched {
    std::vector<u64> raw = std::vector<u64>(2 * Ring, 0xDEAD);
    std::vector<u64> read_as = std::vector<u64>(Ring, ~0ull);
    int calls = 0;
};
void ReadAll(const Pool& pool, const std::vector<Record>& batch, Batched& b) {
    OcclusionBatch::ForEachRange(
        batch, [](const Record& r) { return r.closed && r.complete; },
        [&](u64 first, u64 count) {
            assert(count <= Ring);
            while (count != 0) {
                const u64 start = first % Ring;
                const u64 run = std::min(count, Ring - start);
                ++b.calls;
                for (u64 i = 0; i < run; ++i) {
                    // a slot is read for one absolute query only within a batch
                    assert(b.read_as[start + i] == ~0ull || b.read_as[start + i] == first + i);
                    b.read_as[start + i] = first + i;
                    b.raw[2 * (start + i)] = pool.value[start + i];
                    b.raw[2 * (start + i) + 1] = pool.available[start + i];
                }
                first += run;
                count -= run;
            }
        });
}
u64 SumBatched(const Batched& b, const Record& r, u64& missing) {
    u64 samples = 0;
    for (u64 q = r.first_query; q < r.first_query + r.queries; ++q) {
        const u64 slot = q % Ring;
        assert(b.read_as[slot] == q); // read, and for this query
        const bool available = b.raw[2 * slot + 1] != 0;
        missing += available ? 0 : 1;
        samples += available ? b.raw[2 * slot] : 1000000;
    }
    return samples;
}
} // namespace

int main() {
    std::mt19937_64 rng(12345);
    u64 released = 0, next = 0;
    Pool pool;
    int batches = 0, records = 0, calls = 0, wanted = 0;
    for (int round = 0; round < 20000; ++round) {
        // queries taken since the last batch: never more than the ring holds past `released`
        const u64 take = rng() % (Ring - (next - released) + 1);
        for (u64 q = next; q < next + take; ++q) {
            pool.owner[q % Ring] = q;
            pool.value[q % Ring] = rng() % 50;
            pool.available[q % Ring] = rng() % 8 != 0;
        }
        next += take;
        // a batch of records over the live window [released, next): in order, out of order, nested,
        // overlapping, empty, incomplete and unpaired ones
        std::vector<Record> batch;
        const int n = int(rng() % 7);
        for (int i = 0; i < n && next > released; ++i) {
            const u64 first = released + rng() % (next - released);
            const u64 queries = rng() % 4 == 0 ? 0 : 1 + rng() % (next - first);
            batch.push_back({rng() % 5 != 0, rng() % 6 != 0, first, std::min(queries, next - first)});
        }
        Batched b;
        ReadAll(pool, batch, b);
        int here = 0;
        for (const Record& r : batch) {
            if (!r.closed || !r.complete) continue;
            ++here;
            u64 missing_now = 0, missing_batched = 0;
            const u64 now = SumNow(pool, r, missing_now);
            const u64 batched = SumBatched(b, r, missing_batched);
            if (now != batched || missing_now != missing_batched) {
                std::printf("FAIL: round %d: block [%llu, +%llu): sum %llu vs %llu\n", round,
                            (unsigned long long)r.first_query, (unsigned long long)r.queries,
                            (unsigned long long)now, (unsigned long long)batched);
                return 1;
            }
        }
        assert(b.calls <= 2 * here); // at most one read per block (two where a range wraps)
        calls += b.calls;
        wanted += here;
        records += int(batch.size());
        ++batches;
        // the resolver releases up to some point the batch no longer needs
        if (next > released) released += rng() % (next - released + 1);
    }
    std::printf("occlusion batch: merged reads cover every block: %d batches, %d records (%d read), %d result "
                "reads for %d blocks\n",
                batches, records, wanted, calls, wanted);

    // Part 2: the resolver as vk_occlusion.cpp runs it, blocks opened and closed through the real
    // OcclusionModel::OpenBlocks (Begin, End, Expire, FirstNeeded as Dump calls them). Today's path reads
    // each closed block when it is answered and resets the pool below its needed_from right after; the
    // batched path reads every block first and resets once, below the largest needed_from. A slot reset
    // before it is read reads unavailable, so equal sums show today's resets never reach a later block.
    {
        struct Dumped {
            bool closed, complete;
            u64 first_query, queries, needed_from;
        };
        OcclusionModel::OpenBlocks open;
        u64 next_query = 0, issued = 0, frame = 0, reset_below = 0;
        Pool live; // values and availability of the queries the GPU has run
        int compared = 0;
        for (int tick = 0; tick < 20000; ++tick) {
            std::vector<Dumped> batch;
            const int events = int(rng() % 12);
            for (int e = 0; e < events; ++e) {
                frame += rng() % 3 == 0 ? 1 : 0;
                open.Expire(frame, 2);
                // draws between dumps take queries while a block wants them; the ring never laps
                const u64 draws = rng() % 6;
                for (u64 d = 0; d < draws; ++d) {
                    ++issued;
                    if (next_query - reset_below >= Ring || rng() % 25 == 0) continue; // pool full or no query
                    live.owner[next_query % Ring] = next_query;
                    live.value[next_query % Ring] = rng() % 50;
                    live.available[next_query % Ring] = 1; // the tick has run on the GPU
                    ++next_query;
                }
                // mostly a few blocks; now and then one of many more than OpenBlocks holds (Begin drops the oldest)
                const u64 block = rng() % 10 == 0 ? rng() % 200 : rng() % 12;
                Dumped r{};
                if (rng() % 2 == 0) {
                    open.Begin(block, issued, next_query, frame);
                } else if (const auto c = open.End(block, issued, next_query)) {
                    r = {true, c->complete, c->first_query, c->queries, 0};
                }
                r.needed_from = open.FirstNeeded(next_query);
                batch.push_back(r);
            }
            // today's path
            Pool today = live;
            std::vector<u64> sums_today;
            u64 reset_today = reset_below;
            for (const Dumped& r : batch) {
                if (r.closed && r.complete) {
                    u64 missing = 0;
                    sums_today.push_back(SumNow(today, {true, true, r.first_query, r.queries}, missing) +
                                         missing * 7);
                }
                for (; reset_today < r.needed_from; ++reset_today) today.available[reset_today % Ring] = 0;
            }
            // the batched path
            std::vector<Record> as_records;
            for (const Dumped& r : batch) as_records.push_back({r.closed, r.complete, r.first_query, r.queries});
            Batched b;
            ReadAll(live, as_records, b);
            std::vector<u64> sums_batched;
            u64 release_below = 0;
            for (const Dumped& r : batch) {
                if (r.closed && r.complete) {
                    u64 missing = 0;
                    sums_batched.push_back(SumBatched(b, {true, true, r.first_query, r.queries}, missing) +
                                           missing * 7);
                }
                release_below = std::max(release_below, r.needed_from);
            }
            u64 reset_batched = reset_below;
            for (; reset_batched < release_below; ++reset_batched) live.available[reset_batched % Ring] = 0;
            if (sums_today != sums_batched || reset_today != reset_batched) {
                std::printf("FAIL: tick %d: today's and the batched sums or resets differ\n", tick);
                return 1;
            }
            reset_below = reset_batched;
            compared += int(sums_today.size());
        }
        assert(compared > 5000);
        std::printf("occlusion batch: %d closed blocks through OpenBlocks: same sums and same resets as today's "
                    "path\n",
                    compared);
    }
    std::printf("PASS: occlusion batch\n");
    return 0;
}
