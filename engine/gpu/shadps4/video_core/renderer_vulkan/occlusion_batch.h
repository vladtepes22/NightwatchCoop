// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

/// BB_OCCLUSION_BATCH (default on; BBFSS 1451700 "Batch and instrument occlusion query resolution"): the
/// resolver reads the query results of a whole batch of closed blocks before it answers them, one
/// vkGetQueryPoolResults per run of adjacent or overlapping query ranges instead of one per block, into a
/// buffer indexed by ring slot (vk_occlusion.cpp). Pure, so tests/test_occlusion_batch.cpp can check that
/// every query a block sums is read and that the sums equal the per-block reads.
namespace OcclusionBatch {

/// Calls read(first, count) for the merged ranges of the records `wanted` picks (absolute query
/// numbers; a range may cross the ring's end, the caller splits it). A record merges into the open
/// range when it starts at or before that range's end; a record wholly before it widens the range over
/// the gap (those queries are read and not used). All of one batch lie in the live window, less than the
/// ring apart, so a range never covers a slot twice.
template <class Records, class Wanted, class Read>
void ForEachRange(const Records& records, Wanted&& wanted, Read&& read) {
    std::uint64_t first = 0, end = 0;
    for (const auto& record : records) {
        if (!wanted(record) || record.queries == 0) {
            continue;
        }
        const std::uint64_t record_end = record.first_query + record.queries;
        if (first != end && record.first_query <= end) {
            first = std::min(first, record.first_query);
            end = std::max(end, record_end);
        } else {
            if (first != end) {
                read(first, end - first);
            }
            first = record.first_query;
            end = record_end;
        }
    }
    if (first != end) {
        read(first, end - first);
    }
}

} // namespace OcclusionBatch
