// SPDX-License-Identifier: GPL-3.0-or-later
// liftre077 RE aid (BB_TEST_LIFT_WATCH=1): hardware write breakpoints on the lift ride's state (see the .cpp).
#pragma once

#include <cstdint>

namespace coop::liftwatch {
struct Hit {
    std::uint8_t which = 0;     // the debug register (0..3) that fired
    std::uint32_t tid = 0;
    std::uint64_t rip = 0;      // the instruction after the write, guest offset
    std::uint64_t value = 0;    // the watched bytes after the write
    std::uint64_t step = 0;     // BbEngine::FrameRate::Steps() when it fired
    std::uint64_t ret[6] = {};  // return addresses along the rbp chain
    std::uint64_t rsp0 = 0;     // [rsp] when it points into the image
};
/// Watches addr[i] (len[i] 1, 2, 4 or 8 bytes, aligned; 0 addr: unused) for writes in every thread.
void Arm(std::uint64_t image, const std::uint64_t addr[4], const int len[4]);
void SetStep(std::uint64_t step);
bool Pop(Hit* out);
std::uint32_t Dropped();
}  // namespace coop::liftwatch
