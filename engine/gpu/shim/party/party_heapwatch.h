// SPDX-License-Identifier: GPL-3.0-or-later
// BB_HEAP_WATCH_LOG=1: live bytes per guest call site in one of the game's Dantelion heaps
// (party_heapwatch.cpp). Off by default; a diagnostic for "Transaction failed" (heap out of memory).
#pragma once

namespace coop {

/// Called every director tick (main thread). With BB_HEAP_WATCH_LOG=1 it swaps the watched heap's
/// vtable pointer for a copy whose allocate/free entries count live allocations per call site,
/// then logs the totals and the largest sites every 20 s, and each failed allocation at once.
/// BB_HEAP_WATCH_SLOT=0x<offset> picks the heap slot (default 0x5540438, the network/JSON heap).
void HeapWatchTick();

}  // namespace coop
