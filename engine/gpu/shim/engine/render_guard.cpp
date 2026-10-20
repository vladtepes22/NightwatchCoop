// SPDX-License-Identifier: GPL-3.0-or-later
// The render-target bind (0x25c2100: context, depth target, color target; true when bound) asks
// each target for its size through the target's vtable (+0x48 width, +0x50 height). The common
// render-target class answers from its surface at +0x40 with no check (0x259f870: movzx eax,
// word [rax+0x44]), so a target whose surface is gone faults reading 0x44: reported on Ryzen 5000
// with an RTX 2070 Super, in battle (bound from 0x2663140, the three-target pass setup). Such a
// target is treated as none: the bind returns false, as it does for a missing color target, and
// the game draws that pass into whatever is bound. Binary Ninja addresses (image base 0x400000).
#include "render_guard.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <atomic>
#include <span>

namespace BbEngine::RenderGuard {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))

constexpr std::uint64_t kBindTarget = 0x25c2100;
// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x168
const std::uint8_t kBindPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                      0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00, 0x00};
constexpr std::uint64_t kSurfaceWidth = 0x259f870;
constexpr std::size_t kWidthSlot = 0x48, kSurface = 0x40;

std::uint64_t g_width = 0;
std::atomic<int> g_skipped{0};

template<typename T> bool Read(std::uint64_t at, T* out) {
    return at && host_read_safe(reinterpret_cast<void*>(at), out, sizeof(T));
}

bool Surfaceless(std::uint64_t target) {
    std::uint64_t vtable = 0, width = 0, surface = 0;
    if (!target || !Read(target, &vtable) || !Read(vtable + kWidthSlot, &width) || width != g_width) return false;
    return Read(target + kSurface, &surface) && !surface;
}

// saved[3] rdx (color target), saved[4] rsi (depth target); nonzero skips the bind, which returns 0.
GUEST_ABI std::int64_t BindTarget(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t depth = saved[4], color = saved[3];
    if (!color || (!Surfaceless(depth) && !Surfaceless(color))) return 0;
    const int n = g_skipped.fetch_add(1);
    if (n < 4 || (n & (n + 1)) == 0)
        host_log("Render guard: a pass's %s target has no surface; pass skipped (%d so far)",
                 Surfaceless(color) ? "color" : "depth", n + 1);
    return 1;
}
} // namespace

bool Install(std::uint8_t* image, std::size_t size) {
    Hook hook;
    if (!Prepare(hook, image, size, kBindTarget - 0x400000, kBindPrologue, BindTarget)) {
        host_log("Render guard off: the render-target bind differs from 1.09");
        return false;
    }
    g_width = reinterpret_cast<std::uint64_t>(image) + (kSurfaceWidth - 0x400000);
    Commit(std::span{&hook, 1});
    return true;
}
} // namespace BbEngine::RenderGuard
