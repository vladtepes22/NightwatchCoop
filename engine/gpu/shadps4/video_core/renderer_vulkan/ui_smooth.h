// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// bbport (upstream v0.9.43 SmoothUiSampler, without its 4K menus): the game's menus and HUD are
// Scaleform movies whose sprite sheets are placed as non-smoothed bitmaps (SWF fill types 0x42 / 0x43:
// every bitmap fill of fe.gfx, ingametop.gfx, equipment.gfx and common_base.gfx), so they are sampled
// with point filtering: right for the 1920x1080 stage drawn texel for texel. Behind the upscaler the UI is
// drawn at the output's size, and at any size other than the stage's the art is resampled: nearest
// texel then doubles some pixels and not others (1.33x at 1440p), and round sprites come out uneven.
// Such draws filter linearly instead. Pure rules here for tests/test_ui_smooth.cpp; the rasterizer
// applies them (Rasterizer::UiSmoothDraw / SmoothUiSampler).
#include <array>
#include <cmath>
#include <cstdint>

#include "ui_composition.h"

#ifdef BB_UI_SMOOTH_MUTANTS
namespace Vulkan::UiSmooth {
inline int g_mutant = 0;
}
#define BB_UI_SMOOTH_PICK(n, wrong, right) (::Vulkan::UiSmooth::g_mutant == (n) ? (wrong) : (right))
#else
#define BB_UI_SMOOTH_PICK(n, wrong, right) (right)
#endif

namespace Vulkan::UiSmooth {
inline constexpr int kMutants = 6;

/// BB_UI_SMOOTH: unset or anything else = On; 0 = the game's sampling; 2 = additive draws too (a
/// test value: see Additive).
enum class Mode { Off, On, Additive };
constexpr Mode ParseMode(const char* value) {
    return !value || !value[0] ? Mode::On
           : value[0] == '0'  ? Mode::Off
           : value[0] == '2'  ? Mode::Additive
                              : Mode::On;
}

/// How large the 1920x1080 stage's art is drawn in the UI pass's `width` x `height` image, per axis.
/// Two factors multiply (Rasterizer::UpdateUiTransform): the image over the UI viewport (`ui`,
/// UiComposition::UiSize: the stage, the widened viewport, or with Sharp UI the output's own size),
/// kept in shape when Widescreen is on or the viewport is not 16:9; and Scaleform's own scale of the
/// stage inside a larger viewport (Sharp UI). No UI image (width 0): 1.
inline std::array<float, 2> StageScale(uint32_t width, uint32_t height, std::array<float, 2> ui,
                                       bool widescreen) {
    if (!width || !height) {
        return {1.0f, 1.0f};
    }
    const bool fit = widescreen || ui[0] * 1080.0f != ui[1] * 1920.0f;
    const auto image = fit ? BB_UI_SMOOTH_PICK(5, UiComposition::Scale(1, 1, width, height, true, ui),
                                               UiComposition::Fit(width, height, ui).scale)
                           : UiComposition::Scale(1, 1, width, height, true, ui);
    const float stage = BB_UI_SMOOTH_PICK(6, 1.0f, UiComposition::StageScale(ui));
    return {image[0] * stage, image[1] * stage};
}

/// The stage is not drawn texel for texel (a thousandth: under a pixel across the stage).
inline bool Scaled(std::array<float, 2> scale) {
    return BB_UI_SMOOTH_PICK(1, true,
                             std::abs(scale[0] - 1.0f) > 0.001f || std::abs(scale[1] - 1.0f) > 0.001f);
}

/// A blend that adds the texel's colour whatever its alpha (upstream's rule): the transparent gaps
/// of the game's sheets carry colour, and a linear filter reads it past the sprite's edge (upstream
/// saw a faint box around the HUD bars' glows). Weighted by the texel's alpha (SrcAlpha), a gap adds
/// nothing, so that blend is not counted.
template <typename Factor>
constexpr bool Additive(bool blend_enable, Factor src, Factor dst) {
    return BB_UI_SMOOTH_PICK(2, false,
                             blend_enable && dst == Factor::One &&
                                 BB_UI_SMOOTH_PICK(3, true, src != Factor::SrcAlpha));
}

/// Whether a draw's samplers filter linearly: a Scaleform draw (UiComposition::MovieShader) into the
/// UI pass's image, at another size than the stage's, and not an additive one (Mode::Additive: those
/// too).
constexpr bool Wanted(Mode mode, bool movie_shader, bool scaled, bool additive) {
    return mode != Mode::Off && BB_UI_SMOOTH_PICK(4, true, movie_shader) && scaled &&
           (!additive || mode == Mode::Additive);
}

/// The filter of such a draw: point becomes linear, in and out of an anisotropic sampler; nothing
/// else changes (the mip filter and LOD range stay the game's).
template <typename Filter>
constexpr Filter Smooth(Filter filter) {
    return filter == Filter::Point        ? Filter::Bilinear
           : filter == Filter::AnisoPoint ? Filter::AnisoLinear
                                          : filter;
}

} // namespace Vulkan::UiSmooth
