// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the game's own fragment shaders swapped for our versions (Lantern 4889a8a8b, vk_effect_shaders;
// only the motion blur quality part so far, the lighting effects come in 0.3.1).
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Vulkan::EffectShaders {

inline constexpr u64 SsaoHash = 0xcbbca5dc;
inline constexpr u64 SsrHash = 0x6c2323c2;
inline constexpr u64 SsrCompositeHash = 0x3fe9e964;
inline constexpr u64 MotionBlurHash = 0x29e06868;

/// SPIR-V to compile instead of the game's shader `info`, or empty for the game's own. `start` = the
/// bindings before this stage (the replacement keeps the game's layout, so only stage-first fits).
std::span<const u32> Replacement(const Shader::Info& info, const Shader::Profile& profile,
                                 const Shader::Backend::Bindings& start);

/// A changed copy of the recompiled SPIR-V of fragment shader `info`, or nullopt to keep `spv`. `spv` is never
/// changed, and the shader cache keeps the unchanged words: the caller applies this again when it loads them.
std::optional<std::vector<u32>> Patched(const Shader::Info& info, std::span<const u32> spv);

} // namespace Vulkan::EffectShaders
