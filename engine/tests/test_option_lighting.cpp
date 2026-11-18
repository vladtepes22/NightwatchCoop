// SPDX-License-Identifier: GPL-2.0-or-later
// The optional lighting rows of the game's Options screen (gpu/shim/engine/option_lighting.h): what a row
// writes is what bbport.ini keeps. The table's own rules are static_asserts in the header.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "gpu/shim/engine/option_lighting.h"

using namespace BbEngine::OptionLighting;

static std::string Text(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
// The step option_menu.cpp shows for a value (its Read).
static int StepOf(const Row& row, float value) {
    return int(std::clamp(std::lround((value - row.base) / row.step), 0l, long(kSteps)));
}

int main() {
    const auto temporary = std::filesystem::temp_directory_path() /
        ("bbport-option-lighting-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string filename = temporary.string();
    const char* path = filename.c_str();
#ifdef _WIN32
    _putenv_s("BB_CONFIG", path);
#else
    setenv("BB_CONFIG", path, 1);
#endif
    auto& d = BbSettings::Get();

    // Defaults: the page's Defaults row puts every row where a new bbport.ini has it.
    int toggles = 0;
    for (const Row& row : kRows) {
        if (row.kind == Kind::Toggle) {
            toggles += Shown(row);
            assert(row.def == (d.effects[row.effect] ? 1 : 0) && row.def == 0);
            assert(row.restart == BbSettings::EffectChangeNeedsRestart(row.effect, true, false));
        } else if (row.kind == Kind::Slider) {
            assert(std::fabs(At(row, row.def) - (d.*row.amount).load()) < 1e-6f);
            assert(StepOf(row, (d.*row.amount).load()) == row.def);
        } else {
            assert((d.*row.number).load() == row.def);
        }
    }
    int available = 0, shown = 0;
    for (int e = BbSettings::EffectFirstLighting; e < BbSettings::EffectCount; ++e) available += BbSettings::EffectAvailable(e);
    for (const Row& row : kRows) shown += Shown(row);
    assert(toggles == available); // every lighting effect of this build has its row, and no other has one

    // Every step of every slider, and every choice, is kept by Save and read back by Load as the same
    // step: none is outside what the settings accept, none is lost to the file's rounding.
    for (const Row& row : kRows) {
        if (!Shown(row)) continue;
        if (row.kind == Kind::Slider) {
            for (int step = 0; step <= kSteps; ++step) {
                (d.*row.amount) = At(row, step);
                BbSettings::Save();
                (d.*row.amount) = -99.0f;
                BbSettings::Load();
                const float kept = (d.*row.amount).load();
                if (StepOf(row, kept) != step || std::fabs(kept - At(row, step)) > row.step * 0.26f) {
                    std::printf("FAIL: row %u step %d: wrote %.4f, kept %.4f\n", row.id, step, At(row, step), kept);
                    return 1;
                }
            }
            (d.*row.amount) = At(row, row.def);
        } else if (row.kind == Kind::Pick) {
            for (int choice = 0; choice < row.choices; ++choice) {
                (d.*row.number) = choice;
                BbSettings::Save();
                (d.*row.number) = -99;
                BbSettings::Load();
                assert((d.*row.number).load() == choice);
            }
            (d.*row.number) = row.def;
        } else {
            d.effects[row.effect] = true;
            BbSettings::Save();
            assert(Text(path).find(std::string(BbSettings::Effects[row.effect].key) + "=1\n") != std::string::npos);
            d.effects[row.effect] = false;
            BbSettings::Load();
            assert(d.effects[row.effect]);
            d.effects[row.effect] = false;
        }
        BbSettings::Save();
    }
    const std::string saved = Text(path);
    for (const char* key : {"graphics_ao_quality=0\n", "graphics_ssr_intensity=1.00\n", "graphics_gi_strength=6.00\n",
                            "graphics_volumetric_intensity=1.00\n", "graphics_point_fog_intensity=1.00\n",
                            "graphics_point_fog_falloff=0\n", "graphics_point_fog_glow_size=0.080\n", "effect_gtao=0\n",
                            "graphics_smooth_normals=0\n",
                            "effect_point_shadows=0\n"})
        assert(saved.find(key) != std::string::npos);
    std::filesystem::remove(temporary);
    std::printf("PASS: option lighting: %d rows (%d effects) on pages of %d, %d and %d; defaults; every step and choice "
                "kept by bbport.ini\n",
                shown, toggles, ShownRows(Lighting), ShownRows(Shadows), ShownRows(Light));
}
