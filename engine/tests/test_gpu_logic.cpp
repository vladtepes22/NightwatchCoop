// SPDX-License-Identifier: GPL-2.0-or-later
// Rules of the deadinside28 0fc2921c ports (gpu/shim/bbport_gpu_logic.h).
// Build and run: cmake --build out/gpu --target gpu-logic-test && out/gpu/gpu-logic-test.exe
#include "bbport_gpu_logic.h"

#include <cassert>
#include <cstdio>

using namespace BbGpuLogic;

int main() {
    // Cube of 6 faces of 0x1000 bytes at 0x10000.
    assert(LayerOfRange(0x10000, 0x1000, 0x10000, 0x6000, 6) == 0);
    assert(LayerOfRange(0x13000, 0x1000, 0x10000, 0x6000, 6) == 3);
    assert(LayerOfRange(0x15000, 0x1000, 0x10000, 0x6000, 6) == 5);
    assert(LayerOfRange(0x16000, 0x1000, 0x10000, 0x6000, 6) == -1); // past the last face
    assert(LayerOfRange(0x10800, 0x1000, 0x10000, 0x6000, 6) == -1); // not on a face boundary
    assert(LayerOfRange(0x10000, 0x6000, 0x10000, 0x6000, 6) == -1); // the whole image
    assert(LayerOfRange(0x10000, 0x1000, 0x10000, 0x1000, 1) == -1);  // one layer: whole-image case
    assert(LayerOfRange(0x0F000, 0x1000, 0x10000, 0x6000, 6) == -1); // before the image
    assert(LayerOfRange(0x10000, 0x1000, 0x10000, 0x6001, 6) == -1); // size not a multiple

    // Occlusion: a begin and an end 8 bytes after it.
    {
        OcclusionIntervals o;
        assert(!o.Counting());
        assert(o.Note(0x1000)); // begin
        assert(o.Counting());
        assert(o.Note(0x1010)); // second begin, nested
        assert(o.Note(0x1008)); // end of the first
        assert(o.Counting());
        assert(o.Note(0x1018)); // end of the second
        assert(!o.Counting());
    }
    {
        // An end with no begin: another layout, counted all the time from here on.
        OcclusionIntervals o;
        assert(!o.Note(0x2008));
        assert(o.Counting() && !o.Enabled());
        assert(o.Note(0x2000) && o.Counting());
    }
    {
        OcclusionIntervals o;
        assert(!o.Note(0x2004)); // odd address
        assert(o.Counting());
        OcclusionIntervals off(false);
        assert(off.Counting() && off.Note(0x2004));
    }

    // CPU marks.
    assert(!CpuMarkStale(10, 8, 5, false, false, false, false)); // recent
    assert(CpuMarkStale(10, 4, 5, false, false, false, false));  // old
    assert(!CpuMarkStale(10, 4, 5, true, false, false, false));  // hot
    assert(!CpuMarkStale(10, 4, 5, false, true, false, false));  // dynamic by the CPU
    assert(CpuMarkStale(10, 4, 5, false, true, true, false));    // dynamic by the GPU, aged
    assert(!CpuMarkStale(10, 4, 5, false, false, false, true));  // watched
    assert(CpuMarkStale(10, 0, 5, false, false, false, false));  // never marked
    std::puts("gpu-logic-test: ok");
    return 0;
}
