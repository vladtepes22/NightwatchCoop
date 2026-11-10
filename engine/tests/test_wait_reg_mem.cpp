// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone CPU checks of WaitRegMemCanPass (video_core/amdgpu/wait_reg_mem_logic.h) against a
// search of every dword under small masks. The case that hung a console build: Equal 0xdd under mask 1.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "gpu/shadps4/video_core/amdgpu/wait_reg_mem_logic.h"

using AmdGpu::WaitRegMemCanPass;

static bool Pass(std::uint32_t fn, std::uint32_t v, std::uint32_t ref, std::uint32_t mask) {
    const std::uint32_t x = v & mask;
    switch (fn) {
    case 0: return true;
    case 1: return x < ref;
    case 2: return x <= ref;
    case 3: return x == ref;
    case 4: return x != ref;
    case 5: return x >= ref;
    case 6: return x > ref;
    default: return false;
    }
}

int main() {
    assert(!WaitRegMemCanPass(3, 0xdd, 1));   // the bbhost case
    assert(WaitRegMemCanPass(3, 0xdd, 0xffffffffu));
    assert(WaitRegMemCanPass(0, 5, 0));
    assert(!WaitRegMemCanPass(7, 0, 0xffffffffu));
    // Exhaustive: masks and refs below 64, every value below 256 (the high bits only repeat the mask's).
    for (std::uint32_t fn = 0; fn < 8; ++fn)
        for (std::uint32_t mask = 0; mask < 64; ++mask)
            for (std::uint32_t ref = 0; ref < 128; ++ref) {
                bool any = false;
                for (std::uint32_t v = 0; v < 256 && !any; ++v) any = Pass(fn, v, ref, mask);
                assert(WaitRegMemCanPass(fn, ref, mask) == any);
            }
    // Full-width masks: the unsigned edges.
    assert(!WaitRegMemCanPass(6, 0xffffffffu, 0xffffffffu));
    assert(WaitRegMemCanPass(5, 0xffffffffu, 0xffffffffu));
    assert(!WaitRegMemCanPass(1, 0, 0xffffffffu));
    assert(WaitRegMemCanPass(4, 1, 0));
    assert(!WaitRegMemCanPass(4, 0, 0));
    std::puts("wait-reg-mem-test ok");
    return 0;
}
