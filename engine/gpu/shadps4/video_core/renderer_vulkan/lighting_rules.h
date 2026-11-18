// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: which optional lighting rows make the renderer run its lighting hooks at all.
#pragma once

#include "bbport_settings.h"

namespace Vulkan::LightingRules {

/// True when a lighting row that is in this build is on. A row that is not in the build yet
/// (BbSettings::EffectAvailable) never counts, whatever its flag says.
constexpr bool AnyWanted(const bool (&on)[BbSettings::EffectCount]) {
    for (int e = BbSettings::EffectFirstLighting; e < BbSettings::EffectCount; ++e) {
        // The deeper-parallax row patches shaders at compile time and has no pass: it starts no hook.
        // Improved reflections is built in at start; its hooks run while its SSR replacement is in
        // (EffectShaders::SsrActive, Rasterizer::EffectsOnFrameEnd), not on the row's flag.
        if (on[e] && BbSettings::EffectAvailable(e) && e != BbSettings::EffectPomCavity &&
            e != BbSettings::EffectModernSsr) {
            return true;
        }
    }
    return false;
}

} // namespace Vulkan::LightingRules
