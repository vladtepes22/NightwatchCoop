// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../bbport_settings.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>

/// The optional lighting rows (bbport_settings.h, Effects[10..]) on the game's own Options screen: three
/// System pages, each row an entry of kRows in the order shown. This table is the one place the rows are
/// listed: option_menu.cpp builds the pages from it, tools/engine/menu_assets.cpp sizes each page's
/// sprite from it, and tests/test_menu_assets.cpp checks pc_option_messages.tsv has its captions.
///
/// A page holds at most eight rows (the option movie's sprite; menu_assets.cpp). A row whose effect is
/// not in the build (BbSettings::EffectAvailable) is never made, and neither are its controls.
/// To add a row: one entry here (ids go up by one inside a page) and its line in the .tsv.
namespace BbEngine::OptionLighting {
enum Page { Lighting, Shadows, Light, PageCount };
struct PageInfo {
    const char* section;      ///< the page's name in the option movie
    std::uint32_t system_row; ///< the System list's row that opens it (caption, line help)
    std::uint32_t title;      ///< the page's title: its block + 20, rows from the block's first id
};
/// 110018 and the 128000 block are left free (upstream's PC Looks).
inline constexpr PageInfo kPages[PageCount] = {
    {"PCLighting", 110016, 126020}, // ambient occlusion, reflections, surfaces
    {"PCShadows", 110017, 127020},  // contact, soft sun and lamp shadows
    {"PCLightFog", 110019, 129020}, // indirect lighting, volumetric sunlight, fog around lamps
};
inline constexpr int kMostRows = 8;    ///< rows a page's sprite can hold
inline constexpr int kMostChoices = 2; ///< choices of the widest Pick below
inline constexpr int kSteps = 10;      ///< a slider's last step (the game's sliders go 0-10)

enum class Kind { Toggle, Slider, Pick };
using Amount = std::atomic<float> BbSettings::Values::*;
using Number = std::atomic<int> BbSettings::Values::*;
struct Row {
    Page page;
    std::uint32_t id; ///< caption (group 200) and line help (201)
    Kind kind;
    int effect; ///< Toggle: the Effects[] row it switches; a control: the row it belongs to
    Amount amount = nullptr; ///< Slider: the value is base + step * (0-10)
    float base = 0, step = 0;
    Number number = nullptr; ///< Pick: the value is the choice's index
    std::uint32_t first_choice = 0; ///< Pick: the first choice's caption, the next ones follow
    int choices = 0;
    int def = 0;          ///< what the page's Defaults row sets: on/off, the step, the choice
    bool restart = false; ///< the line help says "Requires a restart."
};
constexpr Row Toggle(Page page, std::uint32_t id, int effect) {
    return {page, id, Kind::Toggle, effect, nullptr, 0, 0, nullptr, 0, 0, BbSettings::Effects[effect].default_on ? 1 : 0,
            BbSettings::EffectChangeNeedsRestart(effect, true, false)};
}
constexpr Row Slider(Page page, std::uint32_t id, int of, Amount amount, float base, float step, int def) {
    return {page, id, Kind::Slider, of, amount, base, step, nullptr, 0, 0, def, false};
}
constexpr Row Pick(Page page, std::uint32_t id, int of, Number number, std::uint32_t first_choice, int choices, int def,
                   bool restart) {
    return {page, id, Kind::Pick, of, nullptr, 0, 0, number, first_choice, choices, def, restart};
}

inline constexpr Row kRows[] = {
    // PC Lighting. Ambient occlusion
    Toggle(Lighting, 126000, BbSettings::EffectGtao),
    Pick(Lighting, 126001, BbSettings::EffectGtao, &BbSettings::Values::graphics_ao_quality, 126010, 2, 0, true),
    // Reflections
    Toggle(Lighting, 126002, BbSettings::EffectModernSsr),
    Slider(Lighting, 126003, BbSettings::EffectModernSsr, &BbSettings::Values::graphics_ssr_intensity, 0, 0.2f, 5),
    // Surfaces
    Toggle(Lighting, 126004, BbSettings::EffectPomCavity),
    // Normals for ambient occlusion and indirect lighting (graphics_smooth_normals; after a restart)
    Pick(Lighting, 126005, BbSettings::EffectGtao, &BbSettings::Values::graphics_smooth_normals, 126012, 2, 0, true),

    // PC Shadows
    Toggle(Shadows, 127000, BbSettings::EffectContactShadows),
    Toggle(Shadows, 127001, BbSettings::EffectPcss),
    Toggle(Shadows, 127002, BbSettings::EffectPointShadows),
    Toggle(Shadows, 127003, BbSettings::EffectRayTracing),

    // PC Light and Fog
    Toggle(Light, 129000, BbSettings::EffectGi),
    Slider(Light, 129001, BbSettings::EffectGi, &BbSettings::Values::graphics_gi_strength, 0, 1.2f, 5),
    Toggle(Light, 129002, BbSettings::EffectVolumetricLight),
    Slider(Light, 129003, BbSettings::EffectVolumetricLight, &BbSettings::Values::graphics_volumetric_intensity, 0, 0.2f, 5),
    Toggle(Light, 129004, BbSettings::EffectPointFog),
    Slider(Light, 129005, BbSettings::EffectPointFog, &BbSettings::Values::graphics_point_fog_intensity, 0, 0.2f, 5),
    Pick(Light, 129006, BbSettings::EffectPointFog, &BbSettings::Values::graphics_point_fog_falloff, 129010, 2,
         BbSettings::PointFogFalloffClassic, false),
    Slider(Light, 129007, BbSettings::EffectPointFog, &BbSettings::Values::graphics_point_fog_glow_size,
           BbSettings::PointFogGlowSizeMin, 0.01f, 5),
};

/// The row is made: its effect is in this build.
constexpr bool Shown(const Row& row) { return BbSettings::EffectAvailable(row.effect); }
/// Rows the page shows, which is what its sprite is built for.
constexpr int ShownRows(Page page) {
    int rows = 0;
    for (const Row& row : kRows) rows += row.page == page && Shown(row);
    return rows;
}
/// A slider's value at a step, as option_menu.cpp writes it.
constexpr float At(const Row& row, int step) { return row.base + float(step) * row.step; }

namespace Checks {
constexpr bool PagesFit() {
    for (int page = 0; page < PageCount; ++page) {
        const int rows = ShownRows(Page(page));
        if (rows < 1 || rows > kMostRows) return false;
    }
    return true;
}
constexpr bool EveryEffectOnce() {
    for (int e = BbSettings::EffectFirstLighting; e < BbSettings::EffectCount; ++e) {
        int toggles = 0;
        for (const Row& row : kRows) toggles += row.kind == Kind::Toggle && row.effect == e;
        if (toggles > 1 || (BbSettings::EffectAvailable(e) && toggles != 1)) return false;
    }
    return true;
}
constexpr bool IdsInOrder() {
    for (int page = 0; page < PageCount; ++page) {
        const std::uint32_t block = kPages[page].title - 20;
        std::uint32_t next = block;
        for (const Row& row : kRows) {
            if (row.page != page) continue;
            if (row.id != next++ || row.id >= block + 10) return false;
            if (row.kind == Kind::Pick && (row.first_choice < block + 10 || row.first_choice + row.choices > block + 20))
                return false;
        }
    }
    return true;
}
constexpr bool ControlsFollowTheirRow() {
    for (std::size_t r = 0; r < std::size(kRows); ++r) {
        const Row& row = kRows[r];
        if (row.effect < BbSettings::EffectFirstLighting || row.effect >= BbSettings::EffectCount) return false;
        if (row.kind == Kind::Toggle) continue;
        bool after = false;
        for (std::size_t k = 0; k < r; ++k)
            after |= kRows[k].kind == Kind::Toggle && kRows[k].effect == row.effect && kRows[k].page == row.page;
        if (!after) return false;
        if (row.kind == Kind::Slider && (!row.amount || !(row.step > 0) || row.def < 0 || row.def > kSteps)) return false;
        if (row.kind == Kind::Pick && (!row.number || row.choices < 2 || row.choices > kMostChoices || row.def < 0 ||
                                       row.def >= row.choices))
            return false;
    }
    return true;
}
} // namespace Checks
static_assert(Checks::PagesFit(), "a lighting page needs 1 to 8 rows: move rows to another page");
static_assert(Checks::EveryEffectOnce(), "every lighting effect in the build has exactly one row in kRows");
static_assert(Checks::IdsInOrder(), "a page's rows are numbered from its block's first id; choices from block + 10");
static_assert(Checks::ControlsFollowTheirRow(), "a slider or choice comes after its effect's row, on the same page");
} // namespace BbEngine::OptionLighting
