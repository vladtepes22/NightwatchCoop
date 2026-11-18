// SPDX-License-Identifier: GPL-2.0-or-later
// Which UI draws filter linearly (renderer_vulkan/ui_smooth.h). Built with BB_UI_SMOOTH_MUTANTS it
// runs the suite once per broken variant of the rules and fails when one is not caught.
#include "gpu/shadps4/video_core/renderer_vulkan/ui_smooth.h"
#include <cstdio>
#include <iterator>

namespace U = Vulkan::UiSmooth;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failed;
#ifndef BB_UI_SMOOTH_MUTANTS
    std::printf("FAIL: %s\n", what);
#else
    (void)what;
#endif
}

// The registers' values (AmdGpu::BlendControl::BlendFactor, AmdGpu::Filter): the rules are templates.
enum class Factor { Zero = 0, One = 1, SrcAlpha = 4, OneMinusSrcAlpha = 5 };
enum class Filter { Point = 0, Bilinear = 1, AnisoPoint = 2, AnisoLinear = 3 };

constexpr std::array<float, 2> kStage{1920.0f, 1080.0f};

bool Near(std::array<float, 2> scale, float x, float y) {
    return std::abs(scale[0] - x) < 0.0005f && std::abs(scale[1] - y) < 0.0005f;
}

// A draw as the rasterizer decides it: a movie draw into a w x h UI image.
bool Draw(U::Mode mode, bool movie, uint32_t w, uint32_t h, std::array<float, 2> ui, bool widescreen,
          bool blend, Factor src, Factor dst) {
    return U::Wanted(mode, movie, U::Scaled(U::StageScale(w, h, ui, widescreen)),
                     U::Additive(blend, src, dst));
}

int RunSuite() {
    g_failed = 0;
    using enum Factor;
    const auto normal = [](uint32_t w, uint32_t h, std::array<float, 2> ui, bool widescreen) {
        return Draw(U::Mode::On, true, w, h, ui, widescreen, true, SrcAlpha, OneMinusSrcAlpha);
    };

    // The switch: unset, empty and unknown values are on; 0 is the game's sampling; 2 adds additive.
    Check(U::ParseMode(nullptr) == U::Mode::On && U::ParseMode("") == U::Mode::On, "unset = on");
    Check(U::ParseMode("1") == U::Mode::On && U::ParseMode("x") == U::Mode::On, "1 / unknown = on");
    Check(U::ParseMode("0") == U::Mode::Off, "0 = off");
    Check(U::ParseMode("2") == U::Mode::Additive, "2 = additive too");

    // The stage texel for texel: nothing changes. 1080p; no UI image (the upscaler off or not in
    // its UI phase); Widescreen at 16:10 and 21:9 of 1080 lines (the stage whole and centred).
    Check(Near(U::StageScale(1920, 1080, kStage, true), 1, 1), "1080p scale 1");
    Check(!normal(1920, 1080, kStage, true) && !normal(1920, 1080, kStage, false), "1080p untouched");
    Check(!normal(0, 0, kStage, true), "no UI image: untouched");
    Check(!normal(1920, 1200, {1920.0f, 1200.0f}, true), "1920x1200 widescreen: stage 1:1");
    Check(!normal(2520, 1080, {2520.0f, 1080.0f}, true), "2520x1080 widescreen: stage 1:1");
    Check(!normal(2560, 1080, {2520.0f, 1080.0f}, true), "2560x1080, 2520 viewport fitted: 1:1");

    // Larger and smaller outputs: the stage is resampled.
    Check(Near(U::StageScale(2560, 1440, kStage, true), 1.3333f, 1.3333f), "1440p scale");
    Check(normal(2560, 1440, kStage, true) && normal(2560, 1440, kStage, false), "1440p smoothed");
    Check(Near(U::StageScale(2560, 1600, {1920.0f, 1200.0f}, true), 1.3333f, 1.3333f), "2560x1600 scale");
    Check(normal(2560, 1600, {1920.0f, 1200.0f}, true), "2560x1600 smoothed");
    Check(normal(3840, 2160, kStage, true), "4K smoothed");
    Check(normal(3440, 1440, {2520.0f, 1080.0f}, true), "3440x1440 widescreen smoothed");
    Check(normal(1280, 720, kStage, true), "720p (smaller) smoothed");
    // Widescreen keeps the shape: a 2560x1080 image around the 16:9 stage is 1:1, not 1.33 wide.
    Check(Near(U::StageScale(2560, 1080, kStage, true), 1, 1), "fit: 2560x1080 scale 1");
    Check(!normal(2560, 1080, kStage, true), "fit: 2560x1080 untouched");
    // Without it the 16:9 viewport is stretched over the image.
    Check(Near(U::StageScale(2560, 1080, kStage, false), 1.3333f, 1), "stretch: 2560x1080 scale");
    Check(normal(2560, 1080, kStage, false), "stretch: 2560x1080 smoothed");
    // Sharp UI: the viewport is the output's size (image over viewport 1), Scaleform scales the stage.
    Check(Near(U::StageScale(2560, 1440, {2560.0f, 1440.0f}, false), 1.3333f, 1.3333f), "Sharp UI scale");
    Check(normal(2560, 1440, {2560.0f, 1440.0f}, false), "Sharp UI 1440p smoothed");
    Check(normal(3440, 1440, {3440.0f, 1440.0f}, true), "Sharp UI 3440x1440 smoothed");

    // Only Scaleform draws.
    Check(!Draw(U::Mode::On, false, 2560, 1440, kStage, true, true, SrcAlpha, OneMinusSrcAlpha),
          "not a movie draw: untouched");
    // Blends: opaque and alpha blends are smoothed; adding the colour whatever the alpha is not
    // (the sheets' gaps carry colour), adding it weighted by alpha is.
    Check(Draw(U::Mode::On, true, 2560, 1440, kStage, true, false, One, Zero), "opaque smoothed");
    Check(!Draw(U::Mode::On, true, 2560, 1440, kStage, true, true, One, One), "One/One kept");
    Check(Draw(U::Mode::On, true, 2560, 1440, kStage, true, true, SrcAlpha, One), "SrcAlpha/One smoothed");
    Check(Draw(U::Mode::On, true, 2560, 1440, kStage, true, false, One, One), "blend off: not additive");
    Check(U::Additive(true, Zero, One) && !U::Additive(true, One, OneMinusSrcAlpha), "additive = dst One");
    // The switch.
    Check(!Draw(U::Mode::Off, true, 2560, 1440, kStage, true, true, SrcAlpha, OneMinusSrcAlpha), "0: off");
    Check(Draw(U::Mode::Additive, true, 2560, 1440, kStage, true, true, One, One), "2: additive too");
    Check(!Draw(U::Mode::Additive, true, 1920, 1080, kStage, true, true, One, One), "2: 1080p untouched");
    Check(!Draw(U::Mode::Additive, false, 2560, 1440, kStage, true, true, One, One), "2: movie draws only");

    // The filter: point to linear, nothing else.
    Check(U::Smooth(Filter::Point) == Filter::Bilinear, "point -> bilinear");
    Check(U::Smooth(Filter::AnisoPoint) == Filter::AnisoLinear, "aniso point -> aniso linear");
    Check(U::Smooth(Filter::Bilinear) == Filter::Bilinear &&
              U::Smooth(Filter::AnisoLinear) == Filter::AnisoLinear,
          "linear kept");
    return g_failed;
}
} // namespace

int main() {
#ifdef BB_UI_SMOOTH_MUTANTS
    const char* names[] = {"",
                           "smooths at 1:1 too",
                           "additive draws smoothed",
                           "alpha-weighted add excluded",
                           "non-movie draws smoothed",
                           "Widescreen fit ignored",
                           "Sharp UI stage scale ignored"};
    static_assert(std::size(names) == U::kMutants + 1);
    int survivors = 0;
    for (int m = 0; m <= U::kMutants; ++m) {
        U::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("ui smooth mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("ui smooth mutants: mutant %d (%s) survived\n", m, names[m]);
            ++survivors;
        } else {
            std::printf("ui smooth mutants: mutant %d (%s) caught by %d checks\n", m, names[m], found);
        }
    }
    if (survivors) return 1;
    std::printf("ui smooth mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("ui smooth: %d failures\n", g_failed);
        return 1;
    }
    std::printf("ui smooth: all tests passed\n");
    return 0;
#endif
}
