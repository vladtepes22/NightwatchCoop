// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: Lantern 4889a8a8b vk_effect_shaders.cpp, the motion blur quality part (graphics_motion_blur_quality=1:
// up to 16 taps per side along the motion instead of the game's 2, same depth and motion tests).
#include "video_core/renderer_vulkan/vk_effect_shaders.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <optional>
#include <vector>

#include "bbport_settings.h"
#include "video_core/host_shaders/gtao_frag.h"
#include "video_core/host_shaders/gtao_hq_frag.h"
#include "video_core/host_shaders/native_motion_blur_frag.h"
#include "video_core/host_shaders/ssr_frag.h"
#include "video_core/renderer_vulkan/vk_ssr_composite_patch.h"
#include "video_core/renderer_vulkan/vk_pom_patch.h"

namespace Vulkan::EffectShaders {

namespace {

std::atomic<bool> gtao_active{false};

// bbport (upstream v0.9.43): Improved reflections (effect_modern_ssr). Upstream also accepts a second
// composite (its shader lifter's output); we have no lifter, so only the recompiler's is matched.
constexpr u64 WaterHash = 0xe3021062;

std::atomic<bool> ssr_active{false}, composite_active{false};

bool Wanted(int effect) {
    return BbSettings::Get().startup_effects[effect];
}

/// FNV-1a over the words: the composite and water patches match one exact module.
u32 Checksum(std::span<const u32> spv) {
    u32 checksum = 2166136261u;
    for (const u32 word : spv) {
        checksum = (checksum ^ word) * 16777619u;
    }
    return checksum;
}

/// The water shader's reflection offset (%1307) is halved: its OpFMul writes a new id that a second
/// OpFMul by the module's own 0.5 constant turns into %1307.
bool PatchWater(std::vector<u32>& spv) {
    constexpr size_t Words = 13751;
    constexpr u32 OriginalChecksum = 0xf7a3a6ee;
    constexpr u32 OffsetResultId = 1307;
    constexpr u32 OpConstant = 43, OpFMul = 133;
    if (spv.size() != Words || Checksum(spv) != OriginalChecksum) {
        return false;
    }
    size_t fmul = 0;
    for (size_t i = 5; i < spv.size(); i += spv[i] >> 16) {
        if ((spv[i] & 0xffff) == OpFMul && spv[i + 2] == OffsetResultId) {
            fmul = i;
            break;
        }
    }
    if (!fmul) {
        return false;
    }
    const u32 type_id = spv[fmul + 1];
    u32 half_id = 0;
    for (size_t i = 5; i < spv.size() && !half_id; i += spv[i] >> 16) {
        if ((spv[i] & 0xffff) == OpConstant && spv[i + 1] == type_id &&
            spv[i + 3] == std::bit_cast<u32>(0.5f)) {
            half_id = spv[i + 2];
        }
    }
    if (!half_id) {
        return false;
    }
    const u32 scaled_id = spv[3]++;
    spv[fmul + 2] = scaled_id;
    const std::array<u32, 5> scale{(5u << 16) | OpFMul, type_id, OffsetResultId, scaled_id, half_id};
    spv.insert(spv.begin() + fmul + 5, scale.begin(), scale.end());
    return true;
}

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
    if (info.pgm_hash == SsrHash && Wanted(BbSettings::EffectModernSsr)) {
        if (Fits(info, profile, start, 1, 3, 3)) {
            if (!ssr_active.exchange(true, std::memory_order_relaxed)) {
                std::printf("Improved reflections: in use\n");
            }
            return SSR_FRAG;
        }
        Declined("Improved reflections", info, profile, start);
        return {};
    }
    if (info.pgm_hash == SsaoHash && BbSettings::Get().startup_effects[BbSettings::EffectGtao]) {
        if (Fits(info, profile, start, 3, 3, 3)) {
            const bool high = BbSettings::Get().startup_ao_quality == 1;
            if (!gtao_active.exchange(true, std::memory_order_relaxed)) {
                std::printf(high ? "GTAO: in use in place of the game's SSAO shader (High: three "
                                   "directions, spatial denoise, full internal resolution)\n"
                                 : "GTAO: in use in place of the game's SSAO shader\n");
            }
            return high ? std::span<const u32>(GTAO_HQ_FRAG) : std::span<const u32>(GTAO_FRAG);
        }
        Declined("GTAO", info, profile, start);
    } else if (info.pgm_hash == MotionBlurHash && BbSettings::Get().startup_motion_blur_quality == 1) {
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
    // Improved reflections: off, these are two compares. Each patch takes one exact module (word
    // count and checksum) and otherwise leaves the game's shader, once saying so in the log.
    if (info.pgm_hash == SsrCompositeHash && Wanted(BbSettings::EffectModernSsr)) {
        const u32 checksum = Checksum(spv);
        if (spv.size() != kSsrCompositeOriginalWords || checksum != kSsrCompositeOriginalChecksum) {
            static std::atomic<bool> printed{false};
            if (!printed.exchange(true)) {
                std::printf("Improved reflections: the composite shader differs on this GPU's "
                            "profile (%zu words, %08x); the game's composite stays\n",
                            spv.size(), checksum);
            }
            return std::nullopt;
        }
        if (!composite_active.exchange(true, std::memory_order_relaxed)) {
            std::printf("Improved reflections: composite patched (full-size trace, reflection strength)\n");
        }
        return std::vector<u32>(kSsrCompositeSpv.begin(), kSsrCompositeSpv.end());
    }
    if (info.pgm_hash == WaterHash && Wanted(BbSettings::EffectModernSsr)) {
        std::vector<u32> patched(spv.begin(), spv.end());
        if (PatchWater(patched)) {
            return patched;
        }
        static std::atomic<bool> printed{false};
        if (!printed.exchange(true)) {
            std::printf("Improved reflections: the water shader differs on this GPU's profile (%zu words, "
                        "%08x); the game's water stays\n",
                        spv.size(), Checksum(spv));
        }
        return std::nullopt;
    }
    // Off: nothing runs but this test (the row is read once at start: BbSettings::Get().startup_effects).
    if (BbSettings::Get().startup_effects[BbSettings::EffectPomCavity] && Pom::Targets(info.pgm_hash)) {
        return PatchedPom(info, spv);
    }
    return std::nullopt;
}

bool GtaoActive() {
    return gtao_active.load(std::memory_order_relaxed);
}

bool SsrActive() {
    return ssr_active.load(std::memory_order_relaxed);
}

bool SsrCompositeActive() {
    return composite_active.load(std::memory_order_relaxed);
}

} // namespace Vulkan::EffectShaders
