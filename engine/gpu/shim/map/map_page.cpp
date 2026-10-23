// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map page (map_page.h). Step 4b of the map: the current area around the player, its
// floor following the player, mouse-wheel zoom. Hunter's Atlas's menu, legend, travel and fog of
// war come later (gpu/shim/map/README.md).
#include "map_page.h"

#include "map_vector.h"
#include "../party/game_state.h"
#include "../party/party_balance.h"

#include "imgui.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace BbMapPage {
namespace {

std::atomic<bool> g_available{false};

// The player, written by Tick (game thread), read by Draw (render thread).
struct Where {
    std::string region;
    float x = 0, y = 0, z = 0;
    bool valid = false;
};
std::mutex g_mu;
Where g_where;  // under g_mu

float g_zoom = kDefaultZoom;  // render thread

} // namespace

void Init(VkDevice device, VkPhysicalDevice physical, VkFormat format) {
    const char* dir = std::getenv("BB_MAP_DIR");
    if (!dir || !*dir) {
        return;  // the map is off: nothing baked for it
    }
    const std::filesystem::path compiled(dir);
    const char* parchment_env = std::getenv("BB_MAP_PARCHMENT");
    const std::filesystem::path parchment =
        parchment_env && *parchment_env ? std::filesystem::path(parchment_env) : compiled / "parchment.bbra";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(compiled / "atlas2-index.json", ec)) {
        std::printf("Map: off (no atlas2-index.json in BB_MAP_DIR %s)\n", dir);
        return;
    }
    if (!BbMapVector::InitGpu(device, physical, format)) {
        std::printf("Map: off (its GPU pipeline could not be made)\n");
        return;
    }
    BbMapVector::Configure(compiled, parchment);
    g_available = true;
    std::printf("Map: on (overlay menu > Map), maps from %s\n", dir);
}

bool Available() {
    return g_available.load(std::memory_order_relaxed);
}

void Tick() {
    if (!Available()) {
        return;
    }
    const coop::GameSnapshot s = coop::ReadGameState(false);
    Where w;
    if (s.world_up && !s.loading && s.player) {
        float p[3];
        w.region = Region(s.map_id);
        if (!w.region.empty() && coop::balance::ChrPosition(s.player, p)) {
            w.x = p[0];
            w.y = p[1];
            w.z = p[2];
            w.valid = true;
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_where = std::move(w);
}

void Draw(float scale) {
    if (!Available()) {
        return;
    }
    Where w;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        w = g_where;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 120.0f * scale);
    size.y = std::max(size.y, 120.0f * scale);
    ImGui::InvisibleButton("##map_canvas", size);
    if (ImGui::IsItemHovered()) {
        g_zoom = WheelZoom(g_zoom, ImGui::GetIO().MouseWheel);
    }
    const ImU32 text = IM_COL32(223, 214, 187, 255);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
    const auto note = [&](const char* line) {
        const ImVec2 extent = ImGui::CalcTextSize(line);
        draw->AddText(ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), text, line);
    };
    if (!w.valid) {
        note(w.region.empty() ? "No map here" : "Waiting for the world");
        return;
    }
    if (!BbMapVector::Load(w.region)) {
        const std::string error = BbMapVector::Error();
        note(error.empty() ? "Loading the map..." : error.c_str());
        return;
    }
    const int floor = BbMapVector::Select(w.region, w.x, w.y, w.z, true);
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    BbMapVector::Draw(draw, w.region, floor, center, w.x, w.z, g_zoom, 1.0f, true, scale, nullptr);
    // The player: always the centre (the view follows them).
    draw->AddCircleFilled(center, 6.0f * scale, IM_COL32(190, 30, 45, 255));
    draw->AddCircle(center, 6.0f * scale, IM_COL32(20, 12, 10, 255), 0, 2.0f * scale);
    draw->PopClipRect();
    char line[96];
    std::snprintf(line, sizeof line, "%s  floor %d / %d  (wheel: zoom)", w.region.c_str(), floor + 1,
                  BbMapVector::Floors(w.region));
    draw->AddText(ImVec2(origin.x + 8.0f * scale, origin.y + 6.0f * scale), text, line);
}

} // namespace BbMapPage
