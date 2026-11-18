// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: ray traced lighting is not in this build. The other lighting passes (contact shadows,
// indirect lighting, point light shadows) ask this class whether rays are available; here the
// answer is always no, so each of them takes its screen-space path. The interface is the one
// those passes use upstream, so the real class can replace this file without touching them.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class RtShadow {
public:
    [[nodiscard]] bool Capturing() const noexcept {
        return false;
    }
    [[nodiscard]] bool Wanted() const noexcept {
        return false;
    }
    [[nodiscard]] bool Complete() const noexcept {
        return false;
    }
    [[nodiscard]] bool KeepMap() const noexcept {
        return false;
    }
    [[nodiscard]] u32 LampRayMask() const noexcept {
        return 0xffu;
    }
    [[nodiscard]] bool RigidWanted() const noexcept {
        return false;
    }

    bool PrepareScene() noexcept {
        return false;
    }
    [[nodiscard]] vk::AccelerationStructureKHR Scene() const noexcept {
        return {};
    }

    struct Inputs {
        vk::Buffer constants;
        vk::DeviceSize constants_offset = 0;
        vk::DeviceSize constants_range = 0;
        vk::Buffer uniforms;
        vk::DeviceSize uniforms_offset = 0;
        vk::DeviceSize uniforms_range = 0;
        vk::ImageView depth;
        vk::ImageView target;
        u32 constants_offset_word = 0;
        u32 frame_index = 0;
        u32 width = 0;
        u32 height = 0;
    };
    bool Record(const Inputs&) noexcept {
        return false;
    }
    void RecordBuilds() noexcept {}
    void OnFrameEnd() noexcept {}
};

} // namespace Vulkan
