// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: Lantern 4889a8a8b vk_effect_shaders.cpp, the motion blur quality part (graphics_motion_blur_quality=1:
// up to 16 taps per side along the motion instead of the game's 2, same depth and motion tests).
#include "video_core/renderer_vulkan/vk_effect_shaders.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <optional>
#include <vector>

#include "bbport_settings.h"
#include "video_core/host_shaders/native_motion_blur_frag.h"
#include "video_core/renderer_vulkan/vk_pom_patch.h"

namespace Vulkan::EffectShaders {

namespace {

bool Fits(const Shader::Info& info, const Shader::Profile& profile, const Shader::Backend::Bindings& start,
          size_t buffers, size_t images, size_t samplers) {
    return info.sw_stage == Shader::SwStage::Fragment && start.unified == 0 && start.buffer == 0 &&
           profile.uniform_dword_layout && !profile.uniform_exact_base && info.buffers.size() == buffers &&
           info.images.size() == images && info.samplers.size() == samplers &&
           std::ranges::all_of(info.buffers, [](const auto& buffer) { return buffer.is_uniform; });
}

void Declined(const char* name, const Shader::Info& info, const Shader::Profile& profile,
              const Shader::Backend::Bindings& start) {
    static std::atomic<u32> printed{0};
    if (printed.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::printf("%s: left off, shader %08x does not have the expected layout here (start %u, "
                    "%zu buffers, %zu images, %zu samplers, dword uniforms %d)\n",
                    name, u32(info.pgm_hash), start.unified, info.buffers.size(), info.images.size(),
                    info.samplers.size(), int(profile.uniform_dword_layout));
    }
}

} // namespace

std::span<const u32> Replacement(const Shader::Info& info, const Shader::Profile& profile,
                                 const Shader::Backend::Bindings& start) {
    if (info.hw_stage != Shader::HwStage::Fragment) {
        return {};
    }
    if (info.pgm_hash == MotionBlurHash && BbSettings::Get().startup_motion_blur_quality == 1) {
        if (Fits(info, profile, start, 1, 2, 2)) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true, std::memory_order_relaxed)) {
                std::printf("Motion blur quality: high (up to 16 taps per side)\n");
            }
            return NATIVE_MOTION_BLUR_FRAG;
        }
        Declined("Motion blur quality", info, profile, start);
    }
    return {};
}

namespace {

bool SayFirst(std::atomic<u32>& count, u32 cap) {
    return count.fetch_add(1, std::memory_order_relaxed) < cap;
}

/// Deeper parallax surfaces (graphics effect_pom_cavity): Pom::PatchPom changes the SPIR-V of three of the game's
/// parallax materials, and only when the words are exactly the ones it was written for. Anything else is left
/// alone, and says so (capped) in the log.
std::optional<std::vector<u32>> PatchedPom(const Shader::Info& info, std::span<const u32> spv) {
    static std::atomic<u32> in_use{0}, no_cavity{0}, other_words{0}, other_shape{0};
    std::vector<u32> patched(spv.begin(), spv.end());
    switch (Pom::PatchPom(info.pgm_hash, true, patched)) {
    case Pom::Outcome::Patched:
        if (SayFirst(in_use, 6)) {
            std::printf("Deeper parallax surfaces: in use on shader %08x\n", u32(info.pgm_hash));
        }
        return patched;
    case Pom::Outcome::PatchedNoCavity:
        if (SayFirst(no_cavity, 3)) {
            std::printf("Deeper parallax surfaces: shader %08x deepened, but its cavity shading did not fit and "
                        "is left out\n",
                        u32(info.pgm_hash));
        }
        return patched;
    case Pom::Outcome::WrongModule:
        // Expected for the other permutations of these shaders (the one that also writes motion vectors is
        // larger); a message for every hash seen, a few in all.
        if (SayFirst(other_words, 6)) {
            std::printf("Deeper parallax surfaces: shader %08x (%zu words, checksum %08x) is not the module this "
                        "was written for; the game's own version of it stays\n",
                        u32(info.pgm_hash), spv.size(), Pom::Checksum(patched));
        }
        return std::nullopt;
    case Pom::Outcome::NoMatch:
        if (SayFirst(other_shape, 3)) {
            std::printf("Deeper parallax surfaces: shader %08x has the expected checksum but not the expected "
                        "shape; the game's own version of it stays\n",
                        u32(info.pgm_hash));
        }
        return std::nullopt;
    case Pom::Outcome::NotTarget:
        break;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::vector<u32>> Patched(const Shader::Info& info, std::span<const u32> spv) {
    if (info.hw_stage != Shader::HwStage::Fragment) {
        return std::nullopt;
    }
    // Off: nothing runs but this test (the row is read once at start: BbSettings::Get().startup_effects).
    if (BbSettings::Get().startup_effects[BbSettings::EffectPomCavity] && Pom::Targets(info.pgm_hash)) {
        return PatchedPom(info, spv);
    }
    return std::nullopt;
}

} // namespace Vulkan::EffectShaders
