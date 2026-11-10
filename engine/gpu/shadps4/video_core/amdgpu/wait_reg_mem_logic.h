// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: whether a PM4 WAIT_REG_MEM can ever be satisfied. Idea from droogie/bbhost 9781692 (GPL-3.0),
// reimplemented here. The packet compares (polled dword & mask) against ref with a function; when no
// dword can pass that compare (for example Equal with a ref that has a bit outside the mask) the
// command processor's wait would never end, so the walk must not wait for it.
#pragma once

#include <cstdint>

namespace AmdGpu {

/// function: 0 always, 1 <, 2 <=, 3 ==, 4 !=, 5 >=, 6 >, 7 reserved (never).
inline bool WaitRegMemCanPass(std::uint32_t function, std::uint32_t ref, std::uint32_t mask) {
    switch (function) {
    case 0:
        return true;
    case 1:
        return ref > 0;           // (v & mask) can be 0
    case 2:
        return true;              // 0 <= ref
    case 3:
        return (ref & ~mask) == 0; // ref must be a submask of mask
    case 4:
        return mask != 0 || ref != 0; // mask 0: v & mask is always 0
    case 5:
        return mask >= ref;       // the largest (v & mask) is mask itself
    case 6:
        return mask > ref;
    default:
        return false;
    }
}

} // namespace AmdGpu
