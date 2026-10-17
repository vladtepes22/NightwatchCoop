// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game camera of the last finished frame, for the overlay (party HUD name tags and
// direction arrows). Published by CameraMotion::CommitFrameCamera (render thread, once a frame,
// from the scene constants: docs/upscaler.md), read by the overlay (present thread). A plain copy
// under a mutex both sides hold for a few dozen bytes.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace BbCamera {

struct State {
    bool valid = false;
    std::array<float, 12> view{}; ///< world to view, 3x4 rows (view z > 0 in front)
    std::array<float, 4> proj{};  ///< x scale, y scale (signed: ndc +y down the screen), z scale, z offset
    std::int64_t at_ms = 0;       ///< steady clock of the publish
};

inline std::mutex mutex;
inline State state;

inline std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void Publish(const std::array<float, 12>& view, const std::array<float, 4>& proj) {
    std::lock_guard<std::mutex> lk(mutex);
    state.valid = proj[0] != 0.0f && proj[1] != 0.0f;
    state.view = view;
    state.proj = proj;
    state.at_ms = NowMs();
}

/// The latest camera; invalid when none was published in the last `max_age_ms`.
inline State Latest(std::int64_t max_age_ms = 250) {
    std::lock_guard<std::mutex> lk(mutex);
    State s = state;
    if (NowMs() - s.at_ms > max_age_ms) s.valid = false;
    return s;
}

/// The camera's world position (-R^T t).
inline std::array<float, 3> Position(const State& s) {
    const auto& v = s.view;
    return {-(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]), -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
            -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11])};
}

// ---- Depth probe (I2 pings, party_ping.h) ----
// The game thread asks (RequestProbe); the render thread, at the frame's display pass, copies the
// scene depth along the screen's centre column into a host buffer and, once the GPU is past it,
// publishes the world points (vk_camera_motion.cpp ProbeDepth). One probe at a time.
constexpr int kProbeSamples = 96;
struct Probe {
    bool valid = false;
    std::uint32_t seq = 0;  ///< the request it answers
    std::array<float, 3> cam{};
    int n = 0;              ///< samples, top of the screen first
    int centre = 0;         ///< the sample at the screen centre
    std::array<std::array<float, 3>, kProbeSamples> pts{};
    std::array<std::uint8_t, kProbeSamples> hit{};  ///< 0: sky / no depth
    std::int64_t at_ms = 0;
};
inline std::atomic<std::uint32_t> probe_requested{0};  ///< last request's seq
inline std::atomic<std::uint32_t> probe_taken{0};      ///< last seq the render thread took
inline Probe probe_result;                             ///< under `mutex`

/// Game thread: a new probe; returns its seq.
inline std::uint32_t RequestProbe() {
    return probe_requested.fetch_add(1, std::memory_order_acq_rel) + 1;
}
/// Render thread: the pending request's seq (0: none). Taken once.
inline std::uint32_t TakeProbeRequest() {
    const std::uint32_t want = probe_requested.load(std::memory_order_acquire);
    std::uint32_t had = probe_taken.load(std::memory_order_acquire);
    if (want == had) return 0;
    return probe_taken.compare_exchange_strong(had, want) ? want : 0;
}
inline void PublishProbe(const Probe& p) {
    std::lock_guard<std::mutex> lk(mutex);
    probe_result = p;
    probe_result.at_ms = NowMs();
}
/// The answer to request `seq` (or a later one); false while none.
inline bool ProbeFor(std::uint32_t seq, Probe* out) {
    std::lock_guard<std::mutex> lk(mutex);
    if (!probe_result.valid || probe_result.seq < seq) return false;
    *out = probe_result;
    return true;
}

} // namespace BbCamera
