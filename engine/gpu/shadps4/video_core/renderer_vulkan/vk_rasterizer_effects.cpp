// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: where the optional lighting passes hook into the frame. Every hook returns at once
// unless a lighting effect is on, so a player with all of them off pays one branch per call.
#include <bit>

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
    ssgi->OnFrameEnd();
    if (!on[BbSettings::EffectGi] && !rt_shadow->Wanted()) {
        ssgi->Release(); // the row was switched off: give the images back
    }
}

void Rasterizer::EffectsBeforeTargets(const GraphicsPipeline*) {
    if (!effects_wanted) {
        return;
    }
}

void Rasterizer::EffectsAfterTargets(const GraphicsPipeline* pipeline) {
    if (!effects_wanted) {
        return;
    }
    const auto& key = pipeline->GetGraphicsKey();
    if (std::popcount(key.mrt_mask) >= 5 && db_desc.first) {
        ssgi->OnGBufferDraw(db_desc.first, cb_descs[2].first, cb_descs[0].first);
    }
}

void Rasterizer::EffectsBlendedScene() {
    if (!effects_wanted) {
        return;
    }
    ssgi->OnOpaqueScene(cb_descs[0].first, db_desc.first, upscaler->Jitter());
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
