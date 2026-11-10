// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: Lantern 4889a8a8b vk_effect_shaders.cpp, the motion blur quality part (graphics_motion_blur_quality=1:
// up to 16 taps per side along the motion instead of the game's 2, same depth and motion tests).
#include "video_core/renderer_vulkan/vk_effect_shaders.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "bbport_settings.h"
#include "video_core/host_shaders/native_motion_blur_frag.h"

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

} // namespace Vulkan::EffectShaders
