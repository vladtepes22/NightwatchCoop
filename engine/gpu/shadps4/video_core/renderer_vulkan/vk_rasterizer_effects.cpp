// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: where the optional lighting passes hook into the frame. Every hook returns at once
// unless a lighting effect is on, so a player with all of them off pays one branch per call.
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/lighting_rules.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Vulkan {

void Rasterizer::EffectsOnFrameEnd() {
    const auto& s = BbSettings::Get();
    bool on[BbSettings::EffectCount];
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        on[e] = s.effects[e].load(std::memory_order_relaxed);
    }
    effects_wanted = LightingRules::AnyWanted(on);
}

void Rasterizer::EffectsBeforeTargets(const GraphicsPipeline*) {
    if (!effects_wanted) {
        return;
    }
}

void Rasterizer::EffectsAfterTargets(const GraphicsPipeline*) {
    if (!effects_wanted) {
        return;
    }
}

void Rasterizer::EffectsBlendedScene() {
    if (!effects_wanted) {
        return;
    }
}

void Rasterizer::EffectsBeforeUpscaler(u64) {
    if (!effects_wanted) {
        return;
    }
}

void Rasterizer::EffectsBeforeResources(const GraphicsPipeline*) {
    if (!effects_wanted) {
        return;
    }
}

} // namespace Vulkan
