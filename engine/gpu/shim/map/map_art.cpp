// SPDX-License-Identifier: GPL-2.0-or-later
// The map view's pictures (map_art.h).
#include "map_art.h"

#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace BbMapArt {
namespace {

constexpr int kIconCell = 384, kIconSheet = 1536;  // a 4 x 4 sheet
constexpr const char* kIcons[] = {"lamp", "ladder", "talk", "gate", "player", "compass"};

struct Picture {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;          // read, until made into the texture
    std::unique_ptr<ImTextureData> texture;  // render thread
};
std::mutex g_mu;
Picture g_background, g_icons;  // under g_mu until Ready() has made them

bool Read(const std::filesystem::path& path, Picture& out) {
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "BBRA", 4)) return false;
    const auto u32 = [&](size_t at) { return unsigned(b[at]) | unsigned(b[at + 1]) << 8 | unsigned(b[at + 2]) << 16 | unsigned(b[at + 3]) << 24; };
    const unsigned w = u32(4), h = u32(8);
    if (!w || !h || w > 4096 || h > 4096 || b.size() != 12 + size_t(w) * h * 4) return false;
    out.w = int(w);
    out.h = int(h);
    out.rgba.assign(b.begin() + 12, b.end());
    return true;
}

void Make(Picture& p) {
    if (p.texture || p.rgba.empty()) return;
    auto t = std::make_unique<ImTextureData>();
    t->Create(ImTextureFormat_RGBA32, p.w, p.h);
    t->UseColors = true;
    static int unique = 4100000;
    t->UniqueID = unique++;
    std::memcpy(t->GetPixels(), p.rgba.data(), p.rgba.size());
    ImGui::RegisterUserTexture(t.get());
    p.texture = std::move(t);
    std::vector<unsigned char>().swap(p.rgba);
}

bool Usable(const Picture& p) {
    return p.texture && p.texture->Status == ImTextureStatus_OK;
}

ImVec2 Rotate(ImVec2 p, ImVec2 c, float cs, float sn) {
    const float x = p.x - c.x, y = p.y - c.y;
    return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
}

}  // namespace

void Load(const std::filesystem::path& dir) {
    Picture background, icons;
    const bool b = Read(dir / "background.bbra", background);
    const bool i = Read(dir / "icons.bbra", icons) && icons.w == kIconSheet && icons.h == kIconSheet;
    if (!b || !i) std::printf("Map: %s%s missing or unreadable in %s (drawn without)\n", b ? "" : "background.bbra ",
                              i ? "" : "icons.bbra", dir.string().c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    if (b) g_background = std::move(background);
    if (i) g_icons = std::move(icons);
}

bool Ready() {
    std::lock_guard<std::mutex> lk(g_mu);
    Make(g_background);
    Make(g_icons);
    return Usable(g_icons);
}

void DrawBackground(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    if (Usable(g_background)) draw->AddImage(g_background.texture->GetTexRef(), min, max);
}

void DrawBackgroundFade(ImDrawList* draw, ImVec2 min, ImVec2 max, ImVec2 map_min, ImVec2 map_max, float fade) {
    if (!Usable(g_background) || max.x <= min.x || max.y <= min.y || map_max.x <= map_min.x || map_max.y <= map_min.y ||
        !std::isfinite(fade) || fade <= 0)
        return;
    const float w = std::min({fade, (map_max.x - map_min.x) * .5f, (map_max.y - map_min.y) * .5f});
    const ImVec2 outer[4] = {{map_min.x, map_min.y}, {map_max.x, map_min.y}, {map_max.x, map_max.y}, {map_min.x, map_max.y}};
    const ImVec2 inner[4] = {{map_min.x + w, map_min.y + w}, {map_max.x - w, map_min.y + w},
                             {map_max.x - w, map_max.y - w}, {map_min.x + w, map_max.y - w}};
    // Four trapezoids with the frame's own UVs: opaque at the map's edge, clear inside.
    draw->PushTexture(g_background.texture->GetTexRef());
    for (int side = 0; side < 4; ++side) {
        const int next = (side + 1) % 4;
        const ImVec2 corners[4] = {outer[side], outer[next], inner[next], inner[side]};
        draw->PrimReserve(6, 4);
        const ImDrawIdx base = ImDrawIdx(draw->_VtxCurrentIdx);
        for (int index : {0, 1, 2, 0, 2, 3}) draw->PrimWriteIdx(ImDrawIdx(base + index));
        for (int c = 0; c < 4; ++c) {
            const ImVec2 p = corners[c];
            draw->PrimWriteVtx(p, ImVec2((p.x - min.x) / (max.x - min.x), (p.y - min.y) / (max.y - min.y)),
                               IM_COL32(255, 255, 255, c < 2 ? 255 : 0));
        }
    }
    draw->PopTexture();
}

bool DrawIcon(ImDrawList* draw, const char* name, ImVec2 center, float size, float rotation, ImU32 tint) {
    int index = -1;
    for (int i = 0; i < int(std::size(kIcons)); ++i)
        if (!std::strcmp(name, kIcons[i])) index = i;
    if (index < 0 || !Usable(g_icons) || size <= 0) return false;
    // Sampled a little inside its cell, so the neighbours never bleed into its edges.
    constexpr float inset = 3.0f;
    const int col = index % 4, row = index / 4;
    const ImVec2 uv0((col * kIconCell + inset) / kIconSheet, (row * kIconCell + inset) / kIconSheet);
    const ImVec2 uv1(((col + 1) * kIconCell - inset) / kIconSheet, ((row + 1) * kIconCell - inset) / kIconSheet);
    const float half = size * .5f, cs = std::cos(rotation), sn = std::sin(rotation);
    draw->AddImageQuad(g_icons.texture->GetTexRef(), Rotate(ImVec2(center.x - half, center.y - half), center, cs, sn),
                       Rotate(ImVec2(center.x + half, center.y - half), center, cs, sn),
                       Rotate(ImVec2(center.x + half, center.y + half), center, cs, sn),
                       Rotate(ImVec2(center.x - half, center.y + half), center, cs, sn), uv0, ImVec2(uv1.x, uv0.y), uv1,
                       ImVec2(uv0.x, uv1.y), tint);
    return true;
}

}  // namespace BbMapArt
