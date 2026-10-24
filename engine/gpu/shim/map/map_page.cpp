// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map (map_page.h): the map view (full screen, Hunter's Atlas's frame and controls) and
// the overlay menu's Map tab. The map follows the player; it pans (right stick, drag, arrows), zooms
// (L2 / R2, wheel, - / +), browses the lit lamps of every area (L1 / R1, Q / E), recentres
// (Triangle, C) and toggles height shading (Square, H); its floor follows the player. Icons:
// lit lamps, ladders, gates, NPCs that are there now, the party, the player's facing; optionally the
// pickups still lying there and boss reveal (map_data_rules.h, bbport.ini map_*). The view and
// controls follow Hunter's Atlas source/compact/bbport_map.cpp (MikeTat1988, GPL-2.0-or-later).
// Map travel is deliberately not here.
#include "map_page.h"

#include "bbport_atlas_style.h"
#include "map_art.h"
#include "map_data_rules.h"
#include "map_vector.h"
#include "../bbport_settings.h"
#include "../party/game_state.h"
#include "../party/party_b2.h"
#include "../party/party_balance.h"
#include "../party/party_hud.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace BbMapPage {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace BbMapData;

std::atomic<bool> g_available{false};

// ---- Game thread -> render thread ----

// The player, written by Tick (game thread), read by the drawing (render thread).
struct Where {
    std::string region;
    float x = 0, y = 0, z = 0, yaw = 0;
    bool valid = false;
};
std::mutex g_mu;
Where g_where;                                                        // under g_mu
std::shared_ptr<const std::vector<std::uint32_t>> g_flag_ids;         // under g_mu: what Tick reads
std::shared_ptr<const std::unordered_map<std::uint32_t, int>> g_flags;  // under g_mu: Tick's last read
std::shared_ptr<const std::vector<int>> g_present;                    // under g_mu: NPC entities here now
std::string g_present_region;                                         // under g_mu: the area they are in
std::atomic<std::int64_t> g_drawn_ms{-100000};                        // the map was last drawn (ms)

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ---- Render thread ----

// The generator's data (map_data_rules.h): read on a worker, then read only.
struct Data {
    std::vector<Area> areas;
    std::string error;
};
std::shared_ptr<const Data> g_data;
std::future<std::shared_ptr<const Data>> g_data_job;
fs::path g_data_dir;

// Ladders, gates and NPC sites per area, read when an area is first shown.
std::map<std::string, std::vector<Marker>> g_markers;
std::map<std::string, std::future<std::vector<Marker>>> g_marker_jobs;

// The view.
struct View {
    std::string region;  // the area shown
    bool follow = true;  // centred on the player
    bool preview = false;  // showing a lamp (L1 / R1)
    int lamp = -1;       // index into the lit lamps
    float cx = 0, cz = 0;
    float zoom = kDefaultZoom;  // pixels per unit at 1280 x 720 (the frame's layout scale on top)
    bool shading = true;
    float pu = .5f, pv = .5f;  // the controller pointer, in the canvas
    bool controller = false;   // the last input was a controller's (pointer, button names)
    int since_open = 0;        // frames drawn since it opened
};
View g_view;

// The map view's inputs (set before g_available, then read only).
std::vector<int> g_keys, g_mouse, g_pad;

// Maps still being made when the game started (the launcher prepares them in the background): the
// present thread looks for them every few seconds and turns the map on when they appear.
struct Pending {
    bool waiting = false;
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkFormat format{};
    fs::path compiled, parchment;
    Clock::time_point next_check{};
};
Pending g_pending;  // present thread

// The note over the game: "being made" at the start, "ready" when they arrive (present thread).
enum class Note { None, Waiting, Ready };
Note g_note = Note::None;
Clock::time_point g_note_until{};
constexpr auto kNoteTime = std::chrono::seconds(12);

constexpr int kSettleFrames = 3;  // the map view's first frames take no presses (View::since_open)

const ImU32 kPaper = IM_COL32(223, 214, 187, 255), kMuted = IM_COL32(185, 180, 161, 255),
            kRule = IM_COL32(152, 146, 128, 125);

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

// The baked maps are complete once bb-map-bake has renamed its folder into place with the index.
bool Baked(const fs::path& compiled) {
    std::error_code ec;
    return fs::is_regular_file(compiled / "atlas2-index.json", ec);
}

Json ReadJson(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("map data: " + path.filename().string() + " missing");
    return Json::parse(f);
}

void StartData(const fs::path& dir, const fs::path& art) {
    g_data_dir = dir;
    g_data_job = std::async(std::launch::async, [dir, art]() {
        BbMapArt::Load(art);
        auto data = std::make_shared<Data>();
        try {
            data->areas = ParseAtlas(ReadJson(dir / "atlas.json"));
            ApplyFeatures(ReadJson(dir / "features.json"), data->areas);
            ApplyCatalog(ReadJson(dir / "catalog.json"), data->areas);
        } catch (const std::exception& e) {
            data->error = e.what();
            std::printf("Map: %s (lamps, pickups and place names left out)\n", e.what());
        }
        return std::shared_ptr<const Data>(std::move(data));
    });
}

bool Enable(const Pending& p) {
    if (!BbMapVector::InitGpu(p.device, p.physical, p.format)) {
        std::printf("Map: off (its GPU pipeline could not be made)\n");
        return false;
    }
    BbMapVector::Configure(p.compiled, p.parchment);
    ReadInputs();
    // The generator's own data sits beside the baked maps (the launcher's <maps>\atlas\maps and
    // <maps>\baked, Core/MapPrep.cs); the art beside the parchment (the package's map-generator).
    const char* data_env = std::getenv("BB_MAP_DATA");
    StartData(data_env && *data_env ? fs::path(data_env) : p.compiled.parent_path() / "atlas" / "maps",
              p.parchment.parent_path());
    g_available = true;
    std::printf("Map: on (map button, %zu key(s), overlay menu > Map), maps from %s\n",
                g_keys.size() + g_mouse.size(), p.compiled.string().c_str());
    return true;
}

// ---- Drawing helpers ----

void PumpData() {
    if (g_data_job.valid() && g_data_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_data = g_data_job.get();
        auto ids = std::make_shared<const std::vector<std::uint32_t>>(CollectFlags(g_data->areas));
        std::printf("Map: %zu areas, %zu event flags watched%s%s\n", g_data->areas.size(), ids->size(),
                    g_data->error.empty() ? "" : "; ", g_data->error.c_str());
        std::lock_guard<std::mutex> lk(g_mu);
        g_flag_ids = std::move(ids);
    }
    for (auto it = g_marker_jobs.begin(); it != g_marker_jobs.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        try {
            g_markers[it->first] = it->second.get();
        } catch (const std::exception& e) {
            g_markers[it->first] = {};
            std::printf("Map: %s markers: %s\n", it->first.c_str(), e.what());
        }
        it = g_marker_jobs.erase(it);
    }
}

const std::vector<Marker>& MarkersOf(const std::string& region) {
    static const std::vector<Marker> none;
    if (const auto it = g_markers.find(region); it != g_markers.end()) return it->second;
    if (!g_marker_jobs.count(region) && !g_data_dir.empty()) {
        const fs::path path = g_data_dir / (region + ".json");
        g_marker_jobs[region] = std::async(std::launch::async, [path]() { return ParseMarkers(ReadJson(path)); });
    }
    return none;
}

// The event flags of Tick's last read, as a lookup (-1 unknown).
FlagFn Flags() {
    std::shared_ptr<const std::unordered_map<std::uint32_t, int>> flags;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        flags = g_flags;
    }
    return [flags](std::uint32_t id) {
        if (!flags) return -1;
        const auto it = flags->find(id);
        return it == flags->end() ? -1 : it->second;
    };
}

// A lit lamp in the browsing order (areas in the generator's order, then their lamps).
struct LampRef {
    const Area* area;
    const Lamp* lamp;
    bool available;
};

std::vector<LampRef> LitLamps(const FlagFn& flag) {
    std::vector<LampRef> out;
    if (!g_data) return out;
    for (const auto& a : g_data->areas)
        for (const auto& l : a.lamps)
            if (const LampState s = StateOf(l, flag); s.discovered) out.push_back({&a, &l, s.available});
    return out;
}

// Centred text fitted into `width` (shrinks down to `min_size`).
void Centered(ImDrawList* draw, const std::string& text, float x_center, float top, float size, float min_size,
              float width, ImU32 colour) {
    ImFont* font = ImGui::GetFont();
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    if (extent.x > width) {
        size = std::max(min_size, size * width / extent.x);
        extent = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    }
    draw->AddText(font, size, ImVec2(x_center - std::min(extent.x, width) * .5f, top), colour, text.c_str(), nullptr,
                  extent.x > width ? width : 0.0f);
}

// A diamond ornament on the frame's rules.
void Ornament(ImDrawList* draw, ImVec2 p, float r, ImU32 colour, float scale) {
    const float d = r * scale;
    draw->AddQuad(ImVec2(p.x - d, p.y), ImVec2(p.x, p.y - d * .55f), ImVec2(p.x + d, p.y), ImVec2(p.x, p.y + d * .55f), colour,
                  .7f * scale);
}

// Our own symbol colour: neutral, so no member's list colour (party_hud MemberColour) is ours.
const ImU32 kSelfBase = IM_COL32(236, 236, 230, 255);

ImU32 WithAlpha(ImU32 colour, int alpha) { return (colour & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT); }

// A member's colour as the bottom-left list shows it; grey once dead.
ImU32 MemberBase(const party::hud::Companion& c) {
    if (!c.alive) return IM_COL32(120, 120, 120, 255);
    float rgb[3];
    party::hud::MemberColour(c.slot, rgb);
    return IM_COL32(int(rgb[0] * 255 + .5f), int(rgb[1] * 255 + .5f), int(rgb[2] * 255 + .5f), 255);
}

// The player's symbol, shared by us and every companion: the map's "player" icon turned to its
// facing (forward = (sin, -cos) on the map) in a dark disc with a halo, tinted `base`.
void PlayerSymbol(ImDrawList* draw, bool art, ImVec2 p, float s, float yaw, ImU32 base) {
    const ImU32 tint = WithAlpha(base, 230), ring = WithAlpha(base, 120), halo = WithAlpha(base, 27);
    draw->AddCircleFilled(p, 18.5f * s, halo, 32);
    draw->AddCircleFilled(p, 16.0f * s, IM_COL32(20, 29, 29, 210), 32);
    draw->AddCircle(p, 17.0f * s, ring, 32, .8f * s);
    if (!(art && BbMapArt::DrawIcon(draw, "player", p, BbAtlasStyle::Layout::player_size * s, yaw, tint))) {
        const ImVec2 d(std::sin(yaw), -std::cos(yaw));
        draw->AddTriangleFilled(ImVec2(p.x + d.x * 11 * s, p.y + d.y * 11 * s), ImVec2(p.x - d.y * 6 * s - d.x * 6 * s, p.y + d.x * 6 * s - d.y * 6 * s),
                                ImVec2(p.x + d.y * 6 * s - d.x * 6 * s, p.y - d.x * 6 * s - d.y * 6 * s), tint);
    }
}

// Input of this frame (render thread; the overlay feeds the map view's pad and keys to ImGui).
float Stick(ImGuiKey negative, ImGuiKey positive) {
    const float v = ImGui::GetKeyData(positive)->AnalogValue - ImGui::GetKeyData(negative)->AnalogValue;
    return std::abs(v) > .18f ? v : 0.0f;
}
bool Pressed(ImGuiKey key) {
    return ImGui::IsKeyPressed(key, false);
}

// One control in the footer: what it does, its button or key.
struct Control {
    const char* label;
    const char* key;
    int action;  // 0 none, 1 lamps, 2 zoom, 3 recentre, 5 shading, 6 back
};

// What a canvas drew: the icons under the pointer get a tooltip.
struct Tip {
    ImVec2 at;
    float radius;
    std::string text;
};

// The map itself in `[origin, origin + size]`: input, floors, icons, tooltips. `pad`: the map
// view's controller and keyboard controls (in the menu tab they navigate the menu). Returns the
// title and subtitle for the frame.
struct CanvasResult {
    std::string title, subtitle, note;
    int floor = -1;
    bool any_lamp = false;
    bool back = false;
};

CanvasResult Canvas(ImDrawList* draw, ImVec2 origin, ImVec2 size, float s, bool pad, int footer_action, float footer_side) {
    CanvasResult r;
    View& v = g_view;
    ImGuiIO& io = ImGui::GetIO();
    const auto& settings = BbSettings::Get();
    Where w;
    std::shared_ptr<const std::vector<int>> present;
    std::string present_region;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        w = g_where;
        present = g_present;
        present_region = g_present_region;
    }
    const FlagFn flag = Flags();
    const bool fog = settings.map_boss_reveal;
    const auto lamps = LitLamps(flag);
    r.any_lamp = !lamps.empty();

    // The controls.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##map_canvas", size);
    const bool hovered = ImGui::IsItemHovered();
    const float dt = std::clamp(io.DeltaTime, 0.0f, .1f);
    float zoom_steps = 0;
    int lamp_step = 0;
    bool recentre = false;
    if (hovered) zoom_steps += io.MouseWheel;
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float z = v.zoom * s;
        v.cx -= io.MouseDelta.x / z;
        v.cz += io.MouseDelta.y / z;
        v.follow = false;
    }
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) v.controller = false;
    // Its first frames take no presses: a key or button from before it opened (the one that opened
    // it, one still queued) is not one of its controls.
    const bool settled = ++v.since_open > kSettleFrames;
    if (pad && settled) {
        const float lx = Stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight),
                    ly = Stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown),
                    rx = Stick(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight),
                    ry = Stick(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown);
        float kx = float(ImGui::IsKeyDown(ImGuiKey_RightArrow)) - float(ImGui::IsKeyDown(ImGuiKey_LeftArrow));
        float ky = float(ImGui::IsKeyDown(ImGuiKey_DownArrow)) - float(ImGui::IsKeyDown(ImGuiKey_UpArrow));
        if (lx || ly || rx || ry) v.controller = true;
        v.pu = std::clamp(v.pu + lx * 500 * dt / std::max(1.0f, size.x), 0.0f, 1.0f);
        v.pv = std::clamp(v.pv + ly * 500 * dt / std::max(1.0f, size.y), 0.0f, 1.0f);
        const float pan_x = rx + kx, pan_y = ry + ky;
        if (pan_x || pan_y) {
            const float z = v.zoom * s;
            v.cx += pan_x * 350 * s * dt / z;
            v.cz -= pan_y * 350 * s * dt / z;
            v.follow = false;
        }
        // Held triggers: about six wheel steps a second.
        zoom_steps += (ImGui::GetKeyData(ImGuiKey_GamepadR2)->AnalogValue - ImGui::GetKeyData(ImGuiKey_GamepadL2)->AnalogValue) *
                      dt * 6.0f;
        if (Pressed(ImGuiKey_Equal) || Pressed(ImGuiKey_KeypadAdd)) zoom_steps += 1;
        if (Pressed(ImGuiKey_Minus) || Pressed(ImGuiKey_KeypadSubtract)) zoom_steps -= 1;
        if (Pressed(ImGuiKey_GamepadL1) || Pressed(ImGuiKey_Q)) lamp_step = -1;
        if (Pressed(ImGuiKey_GamepadR1) || Pressed(ImGuiKey_E)) lamp_step = 1;
        if (Pressed(ImGuiKey_GamepadFaceUp) || Pressed(ImGuiKey_C)) recentre = true;
        if (Pressed(ImGuiKey_GamepadFaceLeft) || Pressed(ImGuiKey_H)) v.shading = !v.shading;
        for (ImGuiKey k : {ImGuiKey_GamepadL1, ImGuiKey_GamepadR1, ImGuiKey_GamepadFaceUp, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadL2, ImGuiKey_GamepadR2})
            if (ImGui::IsKeyDown(k)) v.controller = true;
        for (ImGuiKey k : {ImGuiKey_Q, ImGuiKey_E, ImGuiKey_H, ImGuiKey_C, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow})
            if (ImGui::IsKeyDown(k)) v.controller = false;
    }
    // The footer's buttons (mouse): left / right half for the two-way ones.
    switch (footer_action) {
    case 1: lamp_step = footer_side > 0 ? 1 : -1; break;
    case 2: zoom_steps += footer_side > 0 ? 1 : -1; break;
    case 3: recentre = true; break;
    case 5: v.shading = !v.shading; break;
    case 6: r.back = true; break;
    default: break;
    }
    if (zoom_steps != 0) v.zoom = WheelZoom(v.zoom, zoom_steps);

    if (recentre) {
        v.follow = true;
        v.preview = false;
        v.lamp = -1;
        BbMapVector::SetManualFloor(-1);
    }
    if (lamp_step && !lamps.empty()) {
        v.lamp = CycleLamp(int(lamps.size()), v.lamp, lamp_step);
        const LampRef& l = lamps[v.lamp];
        std::printf("Map: showing lamp %s (%s, %s, frame %d after opening)\n", l.lamp->name.c_str(), l.area->id.c_str(),
                    footer_action == 1 ? "footer" : ImGui::IsKeyDown(ImGuiKey_GamepadL1) || ImGui::IsKeyDown(ImGuiKey_GamepadR1) ? "L1 / R1" : "Q / E",
                    v.since_open);
        v.preview = true;
        v.follow = false;
        v.region = l.area->id;
        v.cx = l.lamp->x;
        v.cz = l.lamp->z;
        v.pu = v.pv = .5f;
        BbMapVector::SetManualFloor(-1);
    }
    if (v.preview && (v.lamp < 0 || v.lamp >= int(lamps.size()))) v.preview = false;  // a lamp went out (new save)
    if (!v.preview) {
        if (v.region != w.region) {
            v.region = w.region;
            v.follow = true;
            BbMapVector::SetManualFloor(-1);
        }
        if (v.follow && w.valid) {
            v.cx = w.x;
            v.cz = w.z;
        }
    }

    const Area* area = g_data ? Find(g_data->areas, v.region) : nullptr;
    const bool here = !v.preview && w.valid && w.region == v.region;
    if (v.region.empty()) {
        r.note = w.valid || !w.region.empty() ? BbSettings::MenuText("No map here", "Здесь нет карты")
                                              : BbSettings::MenuText("Waiting for the world", "Ожидание мира");
        if (r.any_lamp) r.title = BbSettings::MenuText("Lamps: L1 / R1 or Q / E", "Фонари: L1 / R1 или Q / E");
        return r;
    }
    if (!BbMapVector::Load(v.region)) {
        const std::string error = BbMapVector::Error();
        r.note = error.empty() ? BbSettings::MenuText("Loading the map...", "Загрузка карты...") : error;
        return r;
    }

    // The floor: the player's, the lamp's, else the largest.
    int floor;
    if (here) {
        floor = BbMapVector::Select(v.region, w.x, w.y, w.z, true);
    } else if (v.preview) {
        const Lamp* l = lamps[v.lamp].lamp;
        floor = BbMapVector::Select(v.region, l->x, l->y, l->z, true, false);
    } else {
        floor = BbMapVector::Primary(v.region);
    }
    r.floor = floor;
    float anchor = 0, low = 0, high = 0;
    const bool heights = BbMapVector::FloorHeights(v.region, floor, &anchor, &low, &high);
    const auto near_floor = [&](float y, float margin) { return !heights || (y >= low - margin && y <= high + margin) || std::abs(y - anchor) <= margin; };

    const float z = v.zoom * s;
    const ImVec2 center(origin.x + size.x * .5f, origin.y + size.y * .5f);
    const auto project = [&](float x, float zz) { return ImVec2(center.x + (x - v.cx) * z, center.y - (zz - v.cz) * z); };
    const auto on_canvas = [&](ImVec2 p, float m) {
        return p.x >= origin.x - m && p.y >= origin.y - m && p.x <= origin.x + size.x + m && p.y <= origin.y + size.y + m;
    };

    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    std::vector<bool> shown;
    if (fog && area) shown = RegionVisibility(*area, flag);
    if (fog && area && std::none_of(shown.begin(), shown.end(), [](bool b) { return b; }))
        r.note = BbSettings::MenuText("Revealed when this area's boss is defeated", "Откроется после победы над боссом этой области");
    BbMapVector::Draw(draw, v.region, floor, center, v.cx, v.cz, z, 1.0f, v.shading, s, fog && area ? &shown : nullptr);

    std::vector<Tip> tips;
    const bool art = BbMapArt::Ready();
    const auto icon = [&](const char* name, ImVec2 p, float px, ImU32 tint = IM_COL32_WHITE, float rotation = 0) {
        if (!(art && BbMapArt::DrawIcon(draw, name, p, px * s, rotation, tint))) {
            draw->AddCircleFilled(p, px * s * .22f, tint == IM_COL32_WHITE ? kPaper : tint);  // without the icon sheet
            draw->AddCircle(p, px * s * .22f, IM_COL32(20, 12, 10, 255), 0, 1.5f * s);
        }
    };
    if (area) {
        // Ladders, gates near this floor (shown where the map is); NPC sites only in the player's area
        // and only while their NPC is there, alive (the game's loaded characters).
        const NpcMode npc_mode = settings.map_npcs == 1 ? NpcMode::NewDialogue : settings.map_npcs == 2 ? NpcMode::Hidden : NpcMode::All;
        for (const auto& m : MarkersOf(v.region)) {
            if (!near_floor(m.y, 6) || !Revealed(*area, fog, m.x, m.y, m.z, flag)) continue;
            if (m.kind == "npc" && (present_region != v.region || !present ||
                                    !std::binary_search(present->begin(), present->end(), m.entity) ||
                                    !NpcShown(*area, npc_mode, m.entity, flag)))
                continue;
            const ImVec2 p = project(m.x, m.z);
            if (!on_canvas(p, 22 * s)) continue;
            icon(m.kind == "ladder" ? "ladder" : m.kind == "gate" ? "gate" : "talk", p, 32);
            tips.push_back({p, 18 * s,
                            m.kind == "gate" ? m.name + "\n" + BbSettings::MenuText("Gate or door (may need opening)", "Ворота или дверь (может быть закрыта)")
                                             : m.name});
        }
        // Pickups still lying there.
        if (settings.map_pickups) {
            const unsigned kinds = (settings.map_pickups_healing ? kHealing : 0) | (settings.map_pickups_insight ? kInsight : 0) |
                                   (settings.map_pickups_equipment ? kEquipment : 0) | (settings.map_pickups_coldblood ? kEchoes : 0);
            for (const auto& p : Pickups(*area, kinds, fog, flag)) {
                if (!near_floor(p.y, 3)) continue;
                const ImVec2 at = project(p.x, p.z);
                if (!on_canvas(at, 0)) continue;
                const ImU32 c = p.category == "healing" ? IM_COL32(230, 81, 76, 255)
                                : p.category == "insight" ? IM_COL32(189, 121, 234, 255)
                                : p.category == "equipment" ? IM_COL32(93, 172, 242, 255)
                                                            : IM_COL32(235, 193, 79, 255);
                draw->AddCircleFilled(at, 4.1f * s, IM_COL32(20, 22, 24, 255), 12);
                draw->AddCircleFilled(at, 3.0f * s, c, 12);
                tips.push_back({at, 9 * s, p.label});
            }
        }
    }
    // Lit lamps on every floor (Tomb of Oedon lies under Central Yharnam).
    for (int i = 0; i < int(lamps.size()); ++i) {
        const LampRef& l = lamps[i];
        if (l.area->id != v.region) continue;
        const ImVec2 p = project(l.lamp->x, l.lamp->z);
        if (!on_canvas(p, 22 * s)) continue;
        if (v.preview && v.lamp == i) draw->AddCircle(p, 20 * s, IM_COL32(233, 206, 143, 220), 32, 1.5f * s);
        icon("lamp", p, 32, l.available ? IM_COL32_WHITE : IM_COL32(100, 100, 100, 125));
        tips.push_back({p, 20 * s, l.lamp->name});
    }
    // The party: companions in this world on this map.
    party::hud::Board board;
    party::hud::Read(&board);
    if (board.active) {
        for (int i = 0; i < board.count; ++i) {
            const auto& c = board.c[i];
            if (!c.same_world || !c.have_pos || Region(c.map_id) != v.region) continue;
            const ImVec2 p = project(c.pos[0], c.pos[2]);
            if (!on_canvas(p, 0)) continue;
            const ImU32 base = MemberBase(c);
            PlayerSymbol(draw, art, p, s, c.have_yaw ? c.yaw : 0.0f, base);
            draw->AddText(ImGui::GetFont(), 14 * s, ImVec2(p.x + 22 * s, p.y - 8 * s), base, c.name);
            tips.push_back({p, 18 * s, c.name});
        }
    }
    // The player, facing their way (the character's yaw: forward = (sin, 0, cos)).
    if (here) PlayerSymbol(draw, art, project(w.x, w.z), s, w.yaw, kSelfBase);
    // The controller's pointer.
    const ImVec2 pointer = pad && v.controller ? ImVec2(origin.x + size.x * v.pu, origin.y + size.y * v.pv) : io.MousePos;
    if (pad && v.controller) {
        draw->AddCircle(pointer, 8 * s, kPaper, 24, 1.0f * s);
        draw->AddLine(ImVec2(pointer.x - 13 * s, pointer.y), ImVec2(pointer.x + 13 * s, pointer.y), kPaper, .7f * s);
        draw->AddLine(ImVec2(pointer.x, pointer.y - 13 * s), ImVec2(pointer.x, pointer.y + 13 * s), kPaper, .7f * s);
    }
    draw->PopClipRect();
    // A tooltip for the icon nearest the pointer.
    if ((pad && v.controller) || hovered) {
        const Tip* best = nullptr;
        float best_d = 1e9f;
        for (const auto& t : tips) {
            const float d = std::hypot(pointer.x - t.at.x, pointer.y - t.at.y);
            if (d < t.radius && d < best_d) {
                best = &t;
                best_d = d;
            }
        }
        if (best) {
            if (pad && v.controller) ImGui::SetNextWindowPos(ImVec2(pointer.x + 22 * s, pointer.y + 18 * s));
            ImGui::SetTooltip("%s", best->text.c_str());
        }
    }

    // The title: the lamp being shown, else where the player stands, else the area.
    if (area) {
        if (v.preview) {
            r.title = area->name;
            r.subtitle = lamps[v.lamp].lamp->name;
        } else if (here) {
            // Place names cost a walk over many triangles: four times a second is enough.
            static std::string title, subtitle, region;
            static std::int64_t at = 0;
            if (NowMs() - at > 250 || region != v.region) {
                title = Title(*area, w.x, w.y, w.z);
                subtitle = Subtitle(*area, w.x, w.y, w.z);
                region = v.region;
                at = NowMs();
            }
            r.title = title;
            r.subtitle = subtitle;
        } else {
            r.title = area->name;
        }
    } else {
        r.title = v.region;
    }
    return r;
}

}  // namespace

void Init(VkDevice device, VkPhysicalDevice physical, VkFormat format) {
    const char* dir = std::getenv("BB_MAP_DIR");
    if (!dir || !*dir) {
        return;  // the map is off: nothing baked for it
    }
    Pending p;
    p.device = device;
    p.physical = physical;
    p.format = format;
    p.compiled = fs::path(dir);
    const char* parchment_env = std::getenv("BB_MAP_PARCHMENT");
    p.parchment = parchment_env && *parchment_env ? fs::path(parchment_env) : p.compiled / "parchment.bbra";
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
    const bool world = s.world_up && !s.loading && s.player;
    if (world) {
        float p[3];
        w.region = Region(s.map_id);
        if (!w.region.empty() && coop::balance::ChrPosition(s.player, p)) {
            w.x = p[0];
            w.y = p[1];
            w.z = p[2];
            w.yaw = coop::b2::OwnYaw();
            w.valid = true;
        }
    }
    // Each area change once in the log (what the map was given; map id, position).
    static std::string logged = "-";
    if (world && w.region != logged) {
        logged = w.region;
        std::printf("Map: player in map %08x -> %s%s (%.1f, %.1f, %.1f)\n", s.map_id, w.region.empty() ? "no map" : w.region.c_str(),
                    w.valid ? "" : ", no position", double(w.x), double(w.y), double(w.z));
    }
    // The event flags the map's rules read, four times a second while the map is drawn.
    static std::int64_t last_read = 0;
    const std::int64_t now = NowMs();
    std::shared_ptr<const std::vector<std::uint32_t>> ids;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ids = g_flag_ids;
    }
    std::shared_ptr<const std::unordered_map<std::uint32_t, int>> flags;
    std::shared_ptr<const std::vector<int>> present;
    bool publish = false;
    if (ids && now - g_drawn_ms.load(std::memory_order_relaxed) < 1000 && now - last_read >= 250) {
        last_read = now;
        publish = true;
        auto read = std::make_shared<std::unordered_map<std::uint32_t, int>>();
        if (world) {
            read->reserve(ids->size());
            for (const std::uint32_t id : *ids) {
                bool on = false;
                (*read)[id] = coop::ReadEventFlag(id, &on) ? int(on) : -1;
            }
        }
        flags = std::move(read);
        present = std::make_shared<const std::vector<int>>(world ? coop::balance::PresentEntities() : std::vector<int>{});
        static const bool npc_log = [] { const char* e = std::getenv("BB_MAP_NPC_LOG"); return e && *e == '1'; }();
        static std::int64_t npc_logged = 0;
        if (npc_log && world && now - npc_logged > 10000) {
            npc_logged = now;
            coop::balance::LogPresentChrs();
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (publish) {
        g_flags = std::move(flags);
        g_present = std::move(present);
        g_present_region = w.region;
    }
    g_where = std::move(w);
}

void Draw(float scale) {
    if (!Available()) {
        return;
    }
    PumpData();
    g_drawn_ms = NowMs();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 120.0f * scale);
    size.y = std::max(size.y, 120.0f * scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 map_origin(origin.x, origin.y + line);
    const CanvasResult r = Canvas(draw, map_origin, ImVec2(size.x, size.y - line), scale, false, 0, 0);
    std::string head = r.title;
    if (!r.subtitle.empty()) head += "  -  " + r.subtitle;
    draw->AddText(origin, kPaper, head.c_str());
    if (!r.note.empty()) {
        const ImVec2 e = ImGui::CalcTextSize(r.note.c_str());
        draw->AddText(ImVec2(map_origin.x + (size.x - e.x) * .5f, map_origin.y + (size.y - line - e.y) * .5f), kPaper, r.note.c_str());
    }
}

void Opened() {
    ImGui::GetIO().ClearInputKeys();
    View& v = g_view;
    v.since_open = 0;
    v.follow = true;
    v.preview = false;
    v.lamp = -1;
    v.pu = v.pv = .5f;
    BbMapVector::SetManualFloor(-1);
}

bool DrawView() {
    if (!Available()) {
        return false;
    }
    PumpData();
    g_drawn_ms = NowMs();
    ImGuiIO& io = ImGui::GetIO();
    View& v = g_view;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##mapview", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    const ImVec2 screen = ImGui::GetWindowPos(), full = ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool art = BbMapArt::Ready();
    draw->AddRectFilled(screen, ImVec2(screen.x + full.x, screen.y + full.y), IM_COL32(24, 24, 21, 255));
    if (art) BbMapArt::DrawBackground(draw, screen, ImVec2(screen.x + full.x, screen.y + full.y));
    const BbAtlasStyle::Layout layout(full.x, full.y, screen.x, screen.y);
    using L = BbAtlasStyle::Layout;
    const float s = layout.scale;
    const auto at = [&](float x, float y) {
        const auto p = layout.At(x, y);
        return ImVec2(p.x, p.z);
    };
    // The frame: a thin border with curled corners (decoration only).
    draw->AddRect(at(10, 8), at(1270, 710), IM_COL32(156, 148, 126, 95), 0, 0, .8f * s);
    for (float x : {10.f, 1270.f})
        for (float y : {8.f, 710.f}) {
            const float sx = x < 640 ? 1.f : -1.f, sy = y < 360 ? 1.f : -1.f;
            draw->AddBezierCubic(at(x, y + sy * 42), at(x + sx * 3, y + sy * 4), at(x + sx * 24, y + sy * 26), at(x + sx * 42, y), kRule, .7f * s);
            Ornament(draw, at(x + sx * 16, y + sy * 15), 4, kRule, s);
        }

    // The footer's controls (decided before the canvas so a click there acts this frame).
    const bool controller = v.controller;
    std::vector<Control> controls;
    if (!LitLamps(Flags()).empty())  // only once one is lit
        controls.push_back({BbSettings::MenuText("Lamps", "Фонари"), controller ? "L1/R1" : "Q/E", 1});
    if (controller) controls.push_back({BbSettings::MenuText("Pointer", "Указатель"), "L3", 0});
    controls.push_back({BbSettings::MenuText("Pan", "Сдвиг"), controller ? "R3" : BbSettings::MenuText("Drag", "Тянуть"), 0});
    controls.push_back({BbSettings::MenuText("Zoom", "Масштаб"), controller ? "L2/R2" : "-/+", 2});
    controls.push_back({BbSettings::MenuText("Recentre", "К себе"), controller ? "Triangle" : "C", 3});
    controls.push_back({BbSettings::MenuText("Shading", "Высоты"), controller ? "Square" : "H", 5});
    controls.push_back({BbSettings::MenuText("Back", "Назад"), controller ? "Circle" : "Esc", 6});
    int footer_action = 0;
    float footer_side = 0;
    ImFont* font = ImGui::GetFont();
    const auto slot = [&](int i) { return layout.FooterSlot(i, int(controls.size())); };
    for (int i = 0; i < int(controls.size()); ++i) {
        const auto box = slot(i);
        ImGui::SetCursorScreenPos(ImVec2(box.x + 3 * s, box.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("footer", ImVec2(std::max(1.0f, box.w - 6 * s), box.h)) && controls[i].action) {
            footer_action = controls[i].action;
            footer_side = io.MousePos.x > box.x + box.w * .5f ? 1.f : -1.f;
        }
        if (ImGui::IsItemHovered()) draw->AddLine(ImVec2(box.x + 8 * s, box.y + box.h), ImVec2(box.x + box.w - 8 * s, box.y + box.h), kRule, .8f * s);
        ImGui::PopID();
    }

    // The map.
    const auto canvas = layout.Canvas();
    const ImVec2 origin(canvas.x, canvas.y), size(canvas.w, canvas.h);
    const CanvasResult r = Canvas(draw, origin, size, s, true, footer_action, footer_side);
    if (art) BbMapArt::DrawBackgroundFade(draw, screen, ImVec2(screen.x + full.x, screen.y + full.y), origin,
                                          ImVec2(origin.x + size.x, origin.y + size.y), L::edge_fade * s);
    if (!r.note.empty()) Centered(draw, r.note, origin.x + size.x * .5f, origin.y + size.y * .5f - 10 * s, 20 * s, 14 * s, size.x, kMuted);

    // Title and subtitle.
    Centered(draw, r.title.empty() ? std::string(BbSettings::MenuText("Map", "Карта")) : r.title, at(640, 0).x,
             at(0, L::title_top).y, L::title_size * .8f * s, L::title_min * .8f * s, L::title_width * s, kPaper);
    draw->AddLine(at(320, L::header_rule), at(960, L::header_rule), kRule, .8f * s);
    for (float x : {336.f, 640.f, 944.f}) Ornament(draw, at(x, L::header_rule), x == 640 ? 5.f : 3.f, kRule, s);
    if (!r.subtitle.empty())
        Centered(draw, r.subtitle, at(640, 0).x, at(0, L::subtitle_top).y, L::subtitle_size * .8f * s, L::subtitle_min * .8f * s,
                 L::subtitle_width * s, kMuted);
    // The side rules, the compass.
    for (float x : {230.f, 1065.f}) {
        draw->AddLine(at(x, 123), at(x, 643), IM_COL32(154, 144, 119, 76), .65f * s);
        Ornament(draw, at(x, 123), 3, IM_COL32(154, 144, 119, 95), s);
        Ornament(draw, at(x, 643), 3, IM_COL32(154, 144, 119, 95), s);
    }
    if (art) BbMapArt::DrawIcon(draw, "compass", at(L::compass_x, L::compass_y), L::compass_size * s, 0, IM_COL32(174, 166, 143, 170));
    // The key, bottom right: floors, heights, the pickups' colours.
    if (r.floor >= 0) {
        constexpr float x = L::key_x;
        float bottom = L::key_y;
        const char* labels[] = {BbSettings::MenuText("Floor", "Этаж"), BbSettings::MenuText("Other levels", "Другие уровни"),
                                BbSettings::MenuText("Interior", "Помещения")};
        const ImU32 colours[] = {IM_COL32(207, 204, 196, 255), IM_COL32(70, 70, 63, 255), IM_COL32(53, 48, 39, 255)};
        draw->AddLine(at(x, bottom - 8), at(x + L::key_w, bottom - 8), kRule, .6f * s);
        for (int i = 0; i < 3; ++i) {
            const float row = bottom + i * 18;
            draw->AddRectFilled(at(x, row + 2), at(x + 13, row + 13), colours[i]);
            draw->AddRect(at(x, row + 2), at(x + 13, row + 13), kRule, 0, 0, .6f * s);
            draw->AddText(font, 13 * s, at(x + 20, row - 1), kMuted, labels[i]);
        }
        bottom -= 20;
        if (v.shading) {
            const float top = bottom - 86;
            draw->AddRectFilledMultiColor(at(x, top), at(x + 13, bottom), IM_COL32(225, 220, 202, 195), IM_COL32(225, 220, 202, 195),
                                          IM_COL32(52, 55, 51, 195), IM_COL32(52, 55, 51, 195));
            draw->AddRect(at(x, top), at(x + 13, bottom), kRule, 0, 0, .8f * s);
            draw->AddText(font, 13 * s, at(x + 20, top - 2), kMuted, BbSettings::MenuText("High", "Выше"));
            draw->AddText(font, 13 * s, at(x + 20, bottom - 15), kMuted, BbSettings::MenuText("Low", "Ниже"));
            bottom = top - 24;
        }
        const auto& st = BbSettings::Get();
        if (st.map_pickups) {
            const struct {
                bool on;
                const char* label;
                ImU32 colour;
            } keys[] = {{st.map_pickups_healing, BbSettings::MenuText("Blood Vials", "Флаконы крови"), IM_COL32(230, 81, 76, 255)},
                        {st.map_pickups_insight, BbSettings::MenuText("Insight", "Озарение"), IM_COL32(189, 121, 234, 255)},
                        {st.map_pickups_equipment, BbSettings::MenuText("Equipment", "Снаряжение"), IM_COL32(93, 172, 242, 255)},
                        {st.map_pickups_coldblood, BbSettings::MenuText("Coldblood", "Отголоски"), IM_COL32(235, 193, 79, 255)}};
            int count = 0;
            for (const auto& k : keys) count += k.on;
            if (count) {
                float row = bottom - 18 * count;
                draw->AddText(font, 12 * s, at(x, row - 20), kMuted, BbSettings::MenuText("Pickups", "Предметы"));
                for (const auto& k : keys) {
                    if (!k.on) continue;
                    draw->AddCircleFilled(at(x + 6, row + 7), 3 * s, k.colour, 12);
                    draw->AddText(font, 13 * s, at(x + 20, row - 1), kPaper, k.label);
                    row += 18;
                }
            }
        }
    }
    if (g_data && !g_data->error.empty()) draw->AddText(font, 12 * s, at(175, 634), kMuted, g_data->error.c_str());

    // The footer.
    draw->AddLine(at(152, L::footer_rule), at(1116, L::footer_rule), kRule, .8f * s);
    Ornament(draw, at(157, 659), 3, kRule, s);
    Ornament(draw, at(1111, 659), 3, kRule, s);
    for (int i = 0; i < int(controls.size()); ++i) {
        const auto& c = controls[i];
        const auto box = slot(i);
        const float label_size = 16 * s, key_size = 12 * s;
        const ImVec2 text = font->CalcTextSizeA(label_size, FLT_MAX, 0, c.label), key = font->CalcTextSizeA(key_size, FLT_MAX, 0, c.key);
        const float glyph = key.x + 10 * s;
        const float px = box.x + (box.w - glyph - 6 * s - text.x) * .5f;
        if (i) draw->AddLine(ImVec2(box.x, box.y + 5 * s), ImVec2(box.x, box.y + 28 * s), kRule, .7f * s);
        draw->AddRect(ImVec2(px, box.y + 5 * s), ImVec2(px + glyph, box.y + 28 * s), kRule, 2 * s, 0, .7f * s);
        draw->AddText(font, key_size, ImVec2(px + 5 * s, box.y + 9 * s), kMuted, c.key);
        draw->AddText(font, label_size, ImVec2(px + glyph + 6 * s, box.y + 6 * s), kPaper, c.label);
    }
    ImGui::End();
    return !r.back;
}

}  // namespace BbMapPage
