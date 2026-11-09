// SPDX-License-Identifier: GPL-3.0-or-later
// H5 look sync rules (gpu/shim/party/look_sync_rules.h): finding the appearance block, the hash, the settle logic.
// Build and run: cmake --build out/gpu --target look-sync-test && out/gpu/look-sync-test.exe
#include "party/look_sync_rules.h"

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <vector>

using namespace coop::looksync;

int main() {
    // FindFace: the magic at an aligned offset with room after it; not at the very end; not unaligned.
    std::vector<std::uint8_t> rec(0x1000, 0);
    assert(FindFace(rec.data(), rec.size()) == -1);
    std::copy_n("FACE", 4, rec.begin() + 0x340);
    assert(FindFace(rec.data(), rec.size()) == 0x340);
    std::vector<std::uint8_t> tail(0x400, 0);
    std::copy_n("FACE", 4, tail.begin() + 0x400 - 0x100);  // fewer than kFaceSize bytes behind it
    assert(FindFace(tail.data(), tail.size()) == -1);
    std::vector<std::uint8_t> odd(0x400, 0);
    std::copy_n("FACE", 4, odd.begin() + 0x101);  // unaligned
    assert(FindFace(odd.data(), odd.size()) == -1);
    assert(FindFace(nullptr, 100) == -1 && FindFace(rec.data(), 8) == -1);

    // HashBlock: equal bytes equal hash, one byte differs.
    std::vector<std::uint8_t> a(kFaceSize, 7), b = a;
    assert(HashBlock(a.data(), a.size()) == HashBlock(b.data(), b.size()));
    b[100] ^= 1;
    assert(HashBlock(a.data(), a.size()) != HashBlock(b.data(), b.size()));

    // Detector: the baseline is silent; a change fires once after it settles; a flicker that returns does not fire.
    Detector d;
    assert(!d.Update(true, 1, 0.0));   // baseline
    assert(!d.Update(true, 1, 5.0));
    assert(!d.Update(true, 2, 6.0));   // changed, pending
    assert(d.pending());
    assert(!d.Update(true, 2, 6.5));
    assert(d.Update(true, 2, 7.6));    // settled: fires
    assert(!d.Update(true, 2, 9.0));   // once
    assert(!d.Update(true, 3, 10.0));  // editor still moving: new pending
    assert(!d.Update(true, 4, 10.8));  // moved again: the clock restarts
    assert(!d.Update(true, 4, 12.0));  // 1.2 s only
    assert(d.Update(true, 4, 12.4));
    assert(!d.Update(true, 5, 20.0));
    assert(!d.Update(true, 4, 20.5));  // back to the sent look: nothing to send
    assert(!d.pending());
    assert(!d.Update(true, 4, 30.0));
    // An unreadable record (a load) resets: the next valid hash is a new baseline, not a change.
    assert(!d.Update(false, 0, 31.0));
    assert(!d.Update(true, 9, 32.0));
    assert(!d.Update(true, 9, 40.0));
    std::puts("look-sync-test: OK");
    return 0;
}
