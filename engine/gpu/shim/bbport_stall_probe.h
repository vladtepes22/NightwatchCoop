// SPDX-License-Identifier: GPL-3.0-or-later
// Stall probe (BB_STALL_PROBE=1 or =<ms>, off by default): for every main-loop frame longer than
// the threshold (250 ms by default) one "Stall probe:" line saying where the time went: the main
// thread's own work vs. waiting in the frame end for the game's render thread, and where each of
// those threads was (sampled every ms while the frame runs long). Rules and the frame-end / render
// thread background: bbport_stall_probe_rules.h.
#pragma once

#include <atomic>
#include <cstdint>

namespace BbStallProbe {

/// What the main thread runs now, for the samples (set by our own per-frame code).
enum Zone : int { kZoneGame = 0, kZonePartyTick = 1, kZoneCount };
extern std::atomic<int> g_zone;
extern bool g_on;

/// Marks a section of the main thread as ours (CoopTick). One relaxed store each way.
class ZoneScope {
public:
    explicit ZoneScope(Zone z) : prev_(g_zone.exchange(z, std::memory_order_relaxed)) {}
    ~ZoneScope() { g_zone.store(prev_, std::memory_order_relaxed); }
    ZoneScope(const ZoneScope&) = delete;
    ZoneScope& operator=(const ZoneScope&) = delete;

private:
    int prev_;
};

/// bbgpu_patch_image, after coop::HooksInit: reads BB_STALL_PROBE, installs the two call-site hooks
/// (frame end 0x1d97853, render-thread done 0x21b6a68) and starts the sampler. Nothing when off.
void Install();

/// The frame end's quiet point (research/reseam081.md 3.2): `fn(prev_exit_ns, entry_ns, exit_ns)` runs on the
/// main thread right after the GX frame end returned (the render thread idle, the next frame not handed
/// yet), every frame, probe on or off. Installs the frame-end call-site hook when it is not in yet. False:
/// the hook could not be installed (code differs); nothing runs then. One callback (party_seam.cpp).
using FrameEndFn = void (*)(std::uint64_t prev_exit_ns, std::uint64_t entry_ns, std::uint64_t exit_ns);
bool SetFrameEndCallback(FrameEndFn fn);

} // namespace BbStallProbe
