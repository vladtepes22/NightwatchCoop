// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map page (map_page.h): the current area around the player, its floor following the
// player, zoom with the mouse wheel (and L2 / R2 in the map view). Hunter's Atlas's menu, legend, travel and fog of
// war come later (gpu/shim/map/README.md).
#include "map_page.h"

#include "map_vector.h"
#include "../bbport_settings.h"
#include "../party/game_state.h"
#include "../party/party_balance.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

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

// The map view's inputs (set before g_available, then read only).
std::vector<int> g_keys, g_mouse, g_pad;

// Maps still being made when the game started (the launcher prepares them in the background): the
// present thread looks for them every few seconds and turns the map on when they appear.
using Clock = std::chrono::steady_clock;
struct Pending {
    bool waiting = false;
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkFormat format{};
    std::filesystem::path compiled, parchment;
    Clock::time_point next_check{};
};
Pending g_pending;  // present thread

// The note over the game: "being made" at the start, "ready" when they arrive (present thread).
enum class Note { None, Waiting, Ready };
Note g_note = Note::None;
Clock::time_point g_note_until{};
constexpr auto kNoteTime = std::chrono::seconds(12);

void ReadInputs() {
    std::string keys = "M", pad = "start";  // the defaults, as runtime_pad.c and the launcher's
    const char* config = std::getenv("BB_CONFIG");
    std::ifstream in(config && *config ? config : "bbport.ini");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("bind.map=", 0) == 0) keys = line.substr(9);
        if (line.rfind("pad.map=", 0) == 0) pad = line.substr(8);
    }
    for (const std::string& name : InputNames(keys)) {
        if (SDL_strcasecmp(name.c_str(), "none") == 0) {
            continue;  // unbound (the launcher writes none)
        }
        if (name.size() == 6 && name.rfind("Mouse", 0) == 0 && name[5] >= '1' && name[5] <= '5') {
            g_mouse.push_back(name[5] - '0');
        } else if (const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str()); code != SDL_SCANCODE_UNKNOWN) {
            g_keys.push_back(int(code));
        } else {
            std::printf("Map: unknown key \"%s\" in bind.map\n", name.c_str());
        }
    }
    for (const std::string& name : InputNames(pad)) {
        if (const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str()); b != SDL_GAMEPAD_BUTTON_INVALID) {
            g_pad.push_back(int(b));
        }
    }
}

bool Has(const std::vector<int>& list, int value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

namespace {

// The baked maps are complete once bb-map-bake has renamed its folder into place with the index.
bool Baked(const std::filesystem::path& compiled) {
    std::error_code ec;
    return std::filesystem::is_regular_file(compiled / "atlas2-index.json", ec);
}

bool Enable(const Pending& p) {
    if (!BbMapVector::InitGpu(p.device, p.physical, p.format)) {
        std::printf("Map: off (its GPU pipeline could not be made)\n");
        return false;
    }
    BbMapVector::Configure(p.compiled, p.parchment);
    ReadInputs();
    g_available = true;
    std::printf("Map: on (map button, %zu key(s), overlay menu > Map), maps from %s\n",
                g_keys.size() + g_mouse.size(), p.compiled.string().c_str());
    return true;
}

} // namespace

void Init(VkDevice device, VkPhysicalDevice physical, VkFormat format) {
    const char* dir = std::getenv("BB_MAP_DIR");
    if (!dir || !*dir) {
        return;  // the map is off: nothing baked for it
    }
    Pending p;
    p.device = device;
    p.physical = physical;
    p.format = format;
    p.compiled = std::filesystem::path(dir);
    const char* parchment_env = std::getenv("BB_MAP_PARCHMENT");
    p.parchment = parchment_env && *parchment_env ? std::filesystem::path(parchment_env) : p.compiled / "parchment.bbra";
    if (Baked(p.compiled)) {
        Enable(p);
        return;
    }
    std::printf("Map: waiting for the maps in BB_MAP_DIR %s (being made; on as soon as they are there)\n", dir);
    p.waiting = true;
    g_pending = p;
    g_note = Note::Waiting;
    g_note_until = Clock::now() + kNoteTime;
}

void Poll() {
    if (!g_pending.waiting) {
        return;
    }
    const auto now = Clock::now();
    if (now < g_pending.next_check) {
        return;
    }
    g_pending.next_check = now + std::chrono::seconds(3);
    if (!Baked(g_pending.compiled)) {
        return;
    }
    g_pending.waiting = false;
    if (Enable(g_pending)) {
        g_note = Note::Ready;
        g_note_until = now + kNoteTime;
    }
}

const char* NoteText() {
    if (g_note == Note::None || Clock::now() >= g_note_until) {
        return nullptr;
    }
    return g_note == Note::Waiting
               ? BbSettings::MenuText("Map: still being made. It turns on by itself when ready, no restart needed.",
                                      "Карта: ещё создаётся. Включится сама, перезапуск не нужен.")
               : BbSettings::MenuText("Map ready: hold Options / Start, or press M (Controls in the launcher).",
                                      "Карта готова: удерживайте Options / Start или нажмите M.");
}

bool Available() {
    return g_available.load(std::memory_order_relaxed);
}

bool OpensWithKey(int scancode) {
    return Available() && Has(g_keys, scancode);
}

bool OpensWithMouse(int button) {
    return Available() && Has(g_mouse, button);
}

bool OpensWithPad(int button) {
    return Available() && Has(g_pad, button);
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

void Draw(float scale, bool pad_zoom) {
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
    if (pad_zoom) {
        // Held: about six wheel steps a second.
        const float steps = ImGui::GetIO().DeltaTime * 6.0f;
        if (ImGui::IsKeyDown(ImGuiKey_GamepadR2)) g_zoom = WheelZoom(g_zoom, steps);
        if (ImGui::IsKeyDown(ImGuiKey_GamepadL2)) g_zoom = WheelZoom(g_zoom, -steps);
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
    std::snprintf(line, sizeof line, "%s  floor %d / %d  (%s: zoom)", w.region.c_str(), floor + 1,
                  BbMapVector::Floors(w.region), pad_zoom ? "wheel, L2 / R2" : "wheel");
    draw->AddText(ImVec2(origin.x + 8.0f * scale, origin.y + 6.0f * scale), text, line);
}

} // namespace BbMapPage
