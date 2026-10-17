// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: whether the presented frames show the game's 3D scene, or a menu / loading screen.
// Signal: a G-buffer pass (a draw into 5+ color targets with a depth buffer: the deferred
// renderer's geometry pass). Every gameplay frame has one, also under the pause menu (drawn
// over the scene); the title screen, the logos and loading screens have none. The rasterizer
// notes it per draw (stage B, no lock), the presenter counts frames without it. Lock-free.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace BbFrameState {

/// Consecutive presented frames without a G-buffer pass before a frame counts as menu/loading.
inline constexpr std::uint32_t MenuFrames = 10;

inline std::atomic<bool> scene_this_frame{false};
inline std::atomic<bool> scene_active{false};
inline std::atomic<std::uint32_t> frames_without_scene{0};
/// The game's own loading flag (the party director's snapshot, each tick). Some loads draw the
/// scene behind the loading screen (a title join's load after the in-world placement, parfin3
/// s3b): the frame signal alone then kept the party overlay (name tags, the stain marker) on top
/// of the loading screen.
inline std::atomic<bool> game_loading{false};
/// Steady ms until which the overlay stays hidden after a load: the game's flag clears while its
/// loading card is still fading out (s3b: the companion's tag and the stain marker over the card).
inline std::atomic<std::int64_t> loading_hide_until_ms{0};
inline std::int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
/// The party director, every tick: the game's loading flag.
inline void NoteGameLoading(bool loading) {
    const bool was = game_loading.exchange(loading, std::memory_order_relaxed);
    if (was && !loading) {
        loading_hide_until_ms.store(SteadyMs() + 1500, std::memory_order_relaxed);
    }
}

/// Draw recording: a G-buffer pass. Writes once per frame (the first such draw).
inline void NoteScenePass() {
    if (!scene_this_frame.load(std::memory_order_relaxed)) {
        scene_this_frame.store(true, std::memory_order_relaxed);
        scene_active.store(true, std::memory_order_relaxed);
        frames_without_scene.store(0, std::memory_order_relaxed);
    }
}

/// Presenter, once per presented frame.
inline void OnPresent() {
    if (scene_this_frame.exchange(false, std::memory_order_relaxed)) {
        frames_without_scene.store(0, std::memory_order_relaxed);
        scene_active.store(true, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t n = frames_without_scene.load(std::memory_order_relaxed) + 1;
    frames_without_scene.store(n, std::memory_order_relaxed);
    if (n >= MenuFrames) {
        scene_active.store(false, std::memory_order_relaxed);
    }
}

/// The game draws its 3D scene (gameplay, also paused).
inline bool SceneActive() {
    return scene_active.load(std::memory_order_relaxed);
}

/// Menu or loading screen: no 3D scene for MenuFrames presented frames in a row.
inline bool IsMenuOrLoading() {
    if (game_loading.load(std::memory_order_relaxed) ||
        SteadyMs() < loading_hide_until_ms.load(std::memory_order_relaxed)) {
        return true;
    }
    return !SceneActive() &&
           frames_without_scene.load(std::memory_order_relaxed) >= MenuFrames;
}

} // namespace BbFrameState
