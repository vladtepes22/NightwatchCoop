// SPDX-License-Identifier: GPL-2.0-or-later
#include "seamless_overlay.h"
#include "bbport_camera.h"
#include "bbport_frame_state.h"
#include "bbport_settings.h"
#include "party/party_hud.h"
#include "party/party_npctalk.h"
#include "party/party_ping.h"
#include "party/party_status.h"
#include "party/world_warper.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
namespace SeamlessOverlay {
namespace {
float base_scale = 1.0f;
ImFont* game_font = nullptr;
// Party mode notices (join / leave / travel, party_status SetLastEvent): a toast in a bottom corner
// for a few seconds, never taking input. Atomics: Visible() reads them without imgui_mutex.
std::atomic<std::uint64_t> toast_seen{0};  // the party event already shown
std::atomic<std::int64_t> toast_until_ms{0}; // steady clock
std::string toast_text;                      // present thread (imgui_mutex)
constexpr std::int64_t ToastMs = 4000;

std::int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Party HUD (party_hud.h): companions' names and health in the bottom left corner, stacked above
// the performance HUD when it has that corner; the toast goes above the panel. Present thread.
party::hud::Board hud_board;          // the overlay's copy (Read each frame: fixed size, no allocation)
bool perf_hud_banner = false;         // this frame: the game's area-name banner may be up
bool perf_hud_bottom_left = false;    // this frame: the performance HUD's window ...
float perf_hud_top = 0.0f;            // ... and its top edge
float party_hud_top = -1.0f;          // this frame: the panel's top edge (-1: not drawn)
const bool party_tags_on = [] {
    const char* v = std::getenv("BB_PARTY_HUD_TAGS");
    return !(v && v[0] == '0');
}();

bool PartyHudWanted() {
    return party::status::Enabled() && BbSettings::Get().party_hud && party::hud::Shown();
}

/// The overhead display (E2, bbport.ini party_overhead; independent of the panel).
bool PartyTagsWanted() {
    return party_tags_on && party::status::Enabled() && BbSettings::Get().party_name_tags &&
           BbSettings::Get().party_overhead.load() != 0 &&
           party::hud::Shown() && !party::hud::GameDialog();  // not over the game's dimmed dialog box
}

/// I2: "Show party through walls" (bbport.ini party_through_walls; env BB_PARTY_THROUGH_WALLS=0/1
/// over it, for tests).
bool PartyThroughWalls() {
    static const char* const env = std::getenv("BB_PARTY_THROUGH_WALLS");
    return env && *env ? env[0] != '0' : BbSettings::Get().party_through_walls.load();
}

/// I2 pings and the party through walls (party_ping.h): a party with a companion, nothing hiding it.
bool PartyPingsWanted() {
    return party::status::Enabled() && (coop::ping::Enabled() || PartyThroughWalls()) &&
           party::hud::Shown() && !party::hud::GameDialog();
}

// Party alert (party_status SetAlert: the host refused us): a message box in the middle of the
// screen for AlertMs, whatever the notices switch says. Never takes input.
std::atomic<std::uint64_t> alert_seen{0};
std::atomic<std::int64_t> alert_until_ms{0};
std::string alert_text; // present thread (imgui_mutex)
constexpr std::int64_t AlertMs = 15000;

bool AlertWanted() {
    return party::status::Enabled() &&
           (party::status::AlertSeq() != alert_seen.load() || SteadyMs() < alert_until_ms.load());
}

// Shared NPC dialogue (party_npctalk.h): the caption under a companion's conversation.
// The game's full-screen menu (party::hud::GameMenu): the notices and the caption are held - not
// drawn and their time stopped - so they come back for their remaining time once it closes.
// caption_hold: the caption's until_ms it holds for and the ms added to it (present thread).
std::atomic<std::int64_t> caption_hold_key{0}, caption_hold_ms{0};
std::int64_t menu_hold_last_ms = 0;  // present thread: the previous frame's time while held

std::int64_t CaptionUntilMs(std::int64_t until_ms) {
    return until_ms + (caption_hold_key.load() == until_ms ? caption_hold_ms.load() : 0);
}

bool NpcCaptionWanted() {
    return party::status::Enabled() && CaptionUntilMs(coop::npctalk::CurrentCaption().until_ms) > SteadyMs();
}

bool ToastWanted() {
    return party::status::Enabled() && BbSettings::Get().party_toasts &&
           (party::status::EventSeq() != toast_seen.load() || SteadyMs() < toast_until_ms.load());
}

namespace PartyHudDraw {

constexpr float Pi = 3.14159265f;

ImU32 Rgba(float r, float g, float b, float a) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

/// The direction axes on the ground (x, z): the game camera's when it is fresh and stands near
/// the player (the same world space), else the player's facing.
struct Axes {
    float fx = 0, fz = 1, rx = 1, rz = 0;
    bool camera = false; // the camera's: name tags can be projected with it
};

Axes DirectionAxes(const party::hud::Board& b, const BbCamera::State& cam) {
    Axes a;
    if (cam.valid && b.me_have_pos) {
        const auto at = BbCamera::Position(cam);
        const float dx = at[0] - b.me_pos[0], dy = at[1] - b.me_pos[1], dz = at[2] - b.me_pos[2];
        // A third-person camera is a few metres from the player; far off = another space (a
        // cutscene camera, a map with its own origin): not trusted.
        if (dx * dx + dy * dy + dz * dz < 15.0f * 15.0f) {
            const auto& v = cam.view;
            const float sx = cam.proj[0] < 0.0f ? -1.0f : 1.0f; // screen right is view +x times sx
            float fx = v[8], fz = v[10], rx = v[0] * sx, rz = v[2] * sx;
            const float fl = std::sqrt(fx * fx + fz * fz), rl = std::sqrt(rx * rx + rz * rz);
            if (fl > 1e-3f && rl > 1e-3f) {
                a = {fx / fl, fz / fl, rx / rl, rz / rl, true};
                return a;
            }
        }
    }
    a.fx = std::sin(b.me_yaw);
    a.fz = std::cos(b.me_yaw);
    a.rx = std::cos(b.me_yaw);
    a.rz = -std::sin(b.me_yaw);
    return a;
}

/// An arrow (pointing up = straight ahead) turned by `angle` (clockwise = right).
void Arrow(ImDrawList* draw, ImVec2 c, float r, float angle, ImU32 fill, ImU32 edge) {
    const float s = std::sin(angle), co = std::cos(angle);
    const auto at = [&](float x, float y) { // (x right, y up) -> screen
        return ImVec2(c.x + (x * co + y * s) * r, c.y - (-x * s + y * co) * r);
    };
    const ImVec2 tip = at(0.0f, 1.0f), left = at(-0.72f, -0.8f), notch = at(0.0f, -0.35f),
                 right = at(0.72f, -0.8f);
    draw->AddTriangleFilled(tip, left, notch, fill);
    draw->AddTriangleFilled(tip, notch, right, fill);
    draw->AddLine(tip, left, edge, 1.0f);
    draw->AddLine(tip, right, edge, 1.0f);
}

/// A thin bar in the game's style: dark well, gold hairline frame, `fill` up to `frac`.
void Bar(ImDrawList* draw, ImVec2 p0, ImVec2 p1, float frac, ImU32 fill, float alpha) {
    draw->AddRectFilled(p0, p1, Rgba(0.04f, 0.03f, 0.03f, 0.80f * alpha));
    if (frac > 0.0f) {
        const float x = p0.x + (p1.x - p0.x) * std::clamp(frac, 0.0f, 1.0f);
        draw->AddRectFilled(p0, ImVec2(x, p1.y), fill);
    }
    draw->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                  Rgba(0.80f, 0.68f, 0.46f, 0.45f * alpha));
}

const char* StateText(const party::hud::Companion& c) {
    using party::MemberState;
    if (!c.connected) return BbSettings::MenuText("Connection lost", "Связь потеряна");
    if (c.have_hp && !c.alive) return BbSettings::MenuText("Dead", "Мёртв");
    switch (c.state) {
    case MemberState::Title: return BbSettings::MenuText("Title screen", "Главное меню");
    case MemberState::Loading: return BbSettings::MenuText("Loading", "Загрузка");
    case MemberState::Prologue: return BbSettings::MenuText("Prologue", "Пролог");
    case MemberState::Joining: return BbSettings::MenuText("Joining", "Входит");
    case MemberState::Home: return BbSettings::MenuText("Own world", "В своём мире");
    case MemberState::InHostWorld: return BbSettings::MenuText("Host's world", "В мире хоста");
    case MemberState::Dead: return BbSettings::MenuText("Dead", "Мёртв");
    }
    return "";
}

} // namespace PartyHudDraw

/// BB_PARTY_HUD_LOG=1: why the panel shows or not, every 5 s (present thread, before Visible).
void PartyHudDiag() {
    static const bool hud_log = [] {
        const char* v = std::getenv("BB_PARTY_HUD_LOG");
        return v && v[0] && v[0] != '0';
    }();
    if (hud_log) {
        static std::int64_t next_log = 0;
        if (const std::int64_t now = SteadyMs(); now >= next_log) {
            next_log = now + 5000;
            std::printf("Party HUD: overlay: party %d, setting %d, shown %d, menu/loading %d, camera %d, fps %d\n",
                        int(party::status::Enabled()), int(BbSettings::Get().party_hud.load()),
                        int(party::hud::Shown()), int(BbFrameState::IsMenuOrLoading()),
                        int(BbCamera::Latest().valid), int(BbSettings::Get().show_fps.load()));
            // Where each companion's head projects (u, v across the scene; depth in metres).
            const BbCamera::State cam = BbCamera::Latest();
            party::hud::Read(&hud_board);
            for (int i = 0; cam.valid && i < hud_board.count; ++i) {
                const party::hud::Companion& c = hud_board.c[i];
                const float head[3] = {c.pos[0], c.pos[1] + 2.05f, c.pos[2]};
                float u = -1, v = -1, depth = -1;
                const bool in_front = c.have_pos &&
                                      party::hud::Project(cam.view.data(), cam.proj.data(), head, &u, &v, &depth);
                std::printf("Party HUD: overlay: %s %s u %.2f v %.2f depth %.1f\n", c.name,
                            in_front ? "in front" : "behind", u, v, depth);
            }
        }
    }
}

/// Party HUD (party_hud.h): one compact row per companion - name, a thin HP bar (stamina below
/// it), distance and a direction arrow on the same map - in the bottom left corner (above the
/// performance HUD when that has the corner). Companions outside this player's world are greyed.
/// Name tags above companions in view (the game camera, BB_PARTY_HUD_TAGS=0: off). Not
/// interactive; no allocation (the board is a fixed-size copy, texts go to stack buffers).
void PartyHud() {
    using namespace PartyHudDraw;
    party_hud_top = -1.0f;
    const bool panel = PartyHudWanted(), tags = PartyTagsWanted();
    if ((!panel && !tags) || BbFrameState::IsMenuOrLoading()) {
        return;
    }
    party::hud::Read(&hud_board);
    const party::hud::Board& b = hud_board;
    if (!b.active || b.count <= 0) {
        return;
    }
    const BbCamera::State cam = BbCamera::Latest();
    const Axes axes = DirectionAxes(b, cam);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    if (panel) {
    const float pad = 12.0f * s;
    const float bottom = perf_hud_bottom_left ? perf_hud_top - 6.0f * s
                                              : viewport->WorkPos.y + viewport->WorkSize.y - pad;
    const float width = 210.0f * s;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 7.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * s, 2.0f * s));
    ImGui::Begin("##party_hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    char right_text[48];
    for (int i = 0; i < b.count; ++i) {
        const party::hud::Companion& c = b.c[i];
        const bool here = c.same_world && c.connected;
        const float alpha = here ? 1.0f : 0.55f;
        if (i > 0) {
            ImGui::Dummy(ImVec2(0.0f, 5.0f * s));
        }
        // Name (serif, the game's cream; grey outside our world), state / distance on the right.
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::PushFont(game_font, 17.0f);
        float mc[3];
        party::hud::MemberColour(c.slot, mc);  // the same colour as the member's tag (and I2's pings)
        ImGui::TextColored(here ? ImVec4(mc[0], mc[1], mc[2], 1.0f) : ImVec4(0.62f, 0.60f, 0.57f, alpha), "%s",
                           c.name);
        const float name_h = ImGui::GetItemRectSize().y;
        ImGui::PopFont();
        float right_edge = row.x + width;
        if (c.same_map) {
            const float d = party::hud::Distance(b.me_pos, c.pos);
            const float r = 5.5f * s;
            const ImVec2 centre(right_edge - r, row.y + name_h * 0.5f);
            Arrow(draw, centre,
                  r, party::hud::Bearing(b.me_pos, c.pos, axes.fx, axes.fz, axes.rx, axes.rz),
                  Rgba(0.93f, 0.84f, 0.62f, 0.95f * alpha), Rgba(0.10f, 0.07f, 0.04f, 0.9f * alpha));
            right_edge -= 2.0f * r + 5.0f * s;
            std::snprintf(right_text, sizeof right_text, d < 10.0f ? "%.1f m" : "%.0f m", d);
        } else {
            std::snprintf(right_text, sizeof right_text, "%s", here ? "" : StateText(c));
        }
        if (right_text[0]) {
            ImGui::PushFont(game_font, 14.0f);
            const ImVec2 ts = ImGui::CalcTextSize(right_text);
            draw->AddText(ImVec2(right_edge - ts.x, row.y + (name_h - ts.y) * 0.5f),
                          Rgba(0.80f, 0.76f, 0.68f, 0.9f * alpha), right_text);
            ImGui::PopFont();
        }
        // HP (the game's dark red), stamina (green) under it.
        const ImVec2 bar0 = ImGui::GetCursorScreenPos();
        const float hp_h = 4.0f * s, st_h = 2.0f * s;
        const float hp_frac = c.have_hp && c.max_hp > 0 ? float(c.hp) / float(c.max_hp) : 0.0f;
        Bar(draw, bar0, ImVec2(bar0.x + width, bar0.y + hp_h), hp_frac,
            here ? Rgba(0.62f, 0.07f, 0.06f, 1.0f) : Rgba(0.42f, 0.22f, 0.20f, alpha), alpha);
        float used = hp_h;
        if (c.have_stamina && c.max_stamina > 0) {
            const float y = bar0.y + hp_h + 2.0f * s;
            Bar(draw, ImVec2(bar0.x, y), ImVec2(bar0.x + width, y + st_h), float(c.stamina) / float(c.max_stamina),
                here ? Rgba(0.30f, 0.55f, 0.26f, 0.95f) : Rgba(0.30f, 0.40f, 0.28f, alpha), alpha);
            used += 2.0f * s + st_h;
        }
        ImGui::Dummy(ImVec2(width, used + 1.0f * s));
    }
    party_hud_top = ImGui::GetWindowPos().y;
    ImGui::End();
    ImGui::PopStyleVar(3);
    }

    // Name tags above the companions in view (only with the game camera in the player's space):
    // the overhead display (E2): name, plus level / health bar / deaths (party_overhead).
    if (!tags || !axes.camera) {
        return;
    }
    const int overhead = BbSettings::Get().party_overhead.load();
    const ImVec2 display = viewport->Size;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < b.count; ++i) {
        const party::hud::Companion& c = b.c[i];
        // Only a live chr here, alive, on our map, from a fresh board (a dead / re-spawned / removed
        // member or a stalled game tick: no tag; user report "health bars floating in midair").
        const char* why = "";
        const bool show = party::hud::TagVisible(c, party::hud::NowMs() - b.published_ms, &why);
        {
            static char last_why[party::kMaxVitals][24];
            const int slot = c.slot >= 0 && c.slot < party::kMaxVitals ? c.slot : 0;
            if (std::strncmp(last_why[slot], why, sizeof last_why[slot]) != 0) {
                std::snprintf(last_why[slot], sizeof last_why[slot], "%s", why);
                if (why[0]) std::printf("Party HUD: tag %s hidden: %s\n", c.name, why);
                else std::printf("Party HUD: tag %s shown\n", c.name);
            }
        }
        if (!show) {
            continue;
        }
        // The chr's position now (its physics position, read this frame), not the board's: the
        // game tick publishes every 100 ms, so a tag placed from the board stepped at 10 Hz
        // behind a moving companion (user report 2026-10-14). The board's is the fallback.
        float pos[3] = {c.pos[0], c.pos[1], c.pos[2]};
        const bool live = coop::PartyHudChrPos(c.chr, c.pos, pos);
        const float d = party::hud::Distance(b.me_pos, pos);
        if (d > 80.0f) {
            continue;
        }
        const float head[3] = {pos[0], pos[1] + party::hud::kTagBarHeightM, pos[2]};
        // The reusable world-space marker projection (party_hud.h WorldToScreen; I2 pings reuse it).
        const party::hud::ScreenPoint sp =
            party::hud::WorldToScreen(cam.view.data(), cam.proj.data(), head, display.x, display.y, 0.02f);
        const float u = sp.x / display.x, v = sp.y / display.y;
        if (!sp.visible || v < 0.04f || v > 0.96f) {
            continue;
        }
        // Clear of the game's own HUD (bars and item slots top left, echoes bottom right).
        if ((u < 0.34f && v < 0.26f) || (u > 0.80f && v > 0.86f)) {
            continue;
        }
        {   // Test aids: BB_PARTY_HUD_TAGLOG=1 logs the tag's distance / camera depth / anchor once a
            // second (plate measurements), BB_PARTY_HUD_TAGLOG=frame every frame with the chr position
            // used and the camera's age; BB_PARTY_HUD_TAG_HIDE=1 draws no tag (the game's plate alone).
            static const char* const taglog_env = std::getenv("BB_PARTY_HUD_TAGLOG");
            static const bool taglog = taglog_env != nullptr, every_frame = taglog && taglog_env[0] == 'f';
            static std::int64_t last_log = 0;
            if (taglog && (every_frame || party::hud::NowMs() - last_log >= 1000)) {
                last_log = party::hud::NowMs();
                std::printf("Party HUD: tagdiag %s d=%.2f depth=%.2f x=%.1f y=%.1f display=%.0fx%.0f t=%lld "
                            "chr=(%.3f %.3f %.3f) %s board=(%.3f %.3f %.3f) cam_age=%lld\n",
                            c.name, d, sp.depth, sp.x, sp.y, display.x, display.y,
                            static_cast<long long>(last_log), pos[0], pos[1], pos[2], live ? "live" : "board",
                            c.pos[0], c.pos[1], c.pos[2], static_cast<long long>(BbCamera::NowMs() - cam.at_ms));
            }
            static const bool hide = std::getenv("BB_PARTY_HUD_TAG_HIDE") != nullptr;
            if (hide) {
                continue;
            }
        }
        const float alpha = std::clamp((80.0f - d) / 30.0f, 0.0f, 1.0f) * 0.95f;
        // Name, with the level ("Lv 42") or the deaths ("3 deaths, rot 2") after it.
        char label[96];
        const bool ru = BbSettings::MenuText("en", "ru")[0] == 'r';
        if (overhead == 2 && c.level > 0) {
            if (ru) std::snprintf(label, sizeof label, "%s  ур. %d", c.name, c.level);
            else std::snprintf(label, sizeof label, "%s  Lv %d", c.name, c.level);
        } else if (overhead == 4 && c.rot > 0) {
            if (ru) std::snprintf(label, sizeof label, "%s  смертей %d, гниль %d", c.name, c.deaths, c.rot);
            else std::snprintf(label, sizeof label, "%s  deaths %d, rot %d", c.name, c.deaths, c.rot);
        } else if (overhead == 5 && c.ping_ms > 0) {
            std::snprintf(label, sizeof label, ru ? "%s  %u мс" : "%s  %u ms", c.name, c.ping_ms);
        } else if (overhead == 4) {
            if (ru) std::snprintf(label, sizeof label, "%s  смертей %d", c.name, c.deaths);
            else std::snprintf(label, sizeof label, "%s  deaths %d", c.name, c.deaths);
        } else {
            std::snprintf(label, sizeof label, "%s", c.name);
        }
        // Laid out as the game's phantom plate (party_hud.h LayoutPlate: measured live; the game's
        // own plate is off in party mode, party_hud_game.cpp): the bar's top on the anchor with
        // light end caps, the name left-aligned above it, Baskerville in the plate's blue with a
        // dark outline and shadow. Bigger near, the game's size from 10 m camera depth on.
        static const float shift = [] {  // test: BB_PARTY_HUD_TAG_SHIFT=F moves ours F of the width right
            const char* e = std::getenv("BB_PARTY_HUD_TAG_SHIFT");
            return e ? static_cast<float>(std::atof(e)) : 0.0f;
        }();
        party::hud::PlateTunePoll();  // test: BB_PARTY_HUD_TUNE_FILE live parameters
        const party::hud::PlateParams& pp = party::hud::CurrentPlateParams();
        const party::hud::PlateLayout pl =
            party::hud::LayoutPlate(sp.x + shift * display.x, sp.y, display.x, display.y, sp.depth, pp);
        const bool with_bar = overhead == 3 && c.have_hp && c.max_hp > 0;
        // pl.font is the em size (as the plate was measured); ImGui sizes a font by its line
        // height (ascent - descent), smaller per em: convert through the cap height of 'H' (live
        // 2026-10-12: without this our name came out at 0.6 of the game's).
        static float size_per_em = 0.0f;
        if (size_per_em <= 0.0f) {
            ImGui::PushFont(game_font, 100.0f);
            const ImFontGlyph* g = ImGui::GetFontBaked()->FindGlyph('H');
            const float cap = g ? g->Y1 - g->Y0 : 0.0f;  // cap height in px at ImGui size 100
            ImGui::PopFont();
            size_per_em = cap > 1.0f ? 100.0f * party::hud::kPlateCapToFont / cap : 1.0f;
            std::printf("Party HUD: plate font size per em %.3f\n", size_per_em);
        }
        ImGui::PushFont(game_font, pl.font * size_per_em);
        const float ascent = ImGui::GetFontBaked()->Ascent;
        const ImVec2 at(std::round(pl.name_x), std::round(pl.name_baseline - ascent));
        const ImU32 dark = Rgba(0.02f, 0.03f, 0.03f, 0.55f * alpha);
        const float o = pl.outline;
        for (const ImVec2 d4 : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o)}) {
            bg->AddText(ImVec2(at.x + d4.x, at.y + d4.y), dark, label);
        }
        bg->AddText(ImVec2(at.x + o, at.y + o), Rgba(0.02f, 0.03f, 0.03f, 0.9f * alpha), label);  // drop shadow
        bg->AddText(at, Rgba(pp.name_rgb[0], pp.name_rgb[1], pp.name_rgb[2], alpha), label);  // the plate's blue
        ImGui::PopFont();
        if (with_bar) {
            // The plate's thin bar: red over burnt orange, the empty part dark, a dark rim, light end caps.
            const ImVec2 p0(pl.bar_x0, pl.bar_y0), p1(pl.bar_x1, pl.bar_y1);
            const float frac = std::clamp(float(c.hp) / float(c.max_hp), 0.0f, 1.0f);
            if (pp.rim > 0.0f) {
                bg->AddRectFilled(ImVec2(p0.x, p0.y - pp.rim), ImVec2(p1.x, p1.y + pp.rim),
                                  Rgba(0.05f, 0.04f, 0.04f, 0.6f * alpha));
            }
            bg->AddRectFilled(p0, p1, Rgba(0.16f, 0.09f, 0.09f, 0.85f * alpha));
            if (frac > 0.0f) {
                const ImU32 top = Rgba(pp.bar_top[0], pp.bar_top[1], pp.bar_top[2], alpha),
                            bottom = Rgba(pp.bar_bottom[0], pp.bar_bottom[1], pp.bar_bottom[2], alpha);
                bg->AddRectFilledMultiColor(p0, ImVec2(p0.x + (p1.x - p0.x) * frac, p1.y), top, top, bottom, bottom);
            }
            const ImU32 cap = Rgba(pp.cap_rgb[0], pp.cap_rgb[1], pp.cap_rgb[2], alpha),
                        glow = Rgba(0.55f, 0.62f, 0.70f, pp.glow * alpha);
            for (const float cx : {pl.cap_l, pl.cap_r}) {
                if (pp.glow > 0.0f) {
                    bg->AddRectFilled(ImVec2(cx - 1.0f, pl.cap_y0 - 1.0f), ImVec2(cx + pl.cap_w + 1.0f, pl.cap_y1 + 1.0f), glow);
                }
                bg->AddRectFilled(ImVec2(cx, pl.cap_y0), ImVec2(cx + pl.cap_w, pl.cap_y1), cap);
            }
        }
    }
}

/// Statsaudit finding 1: the guest's own bloodstain in the host's world (party_ping.h StainMark),
/// a blood-red drop through walls with "Blood Echoes N" and the distance (clamped to the screen
/// edge when off screen). Hidden under the game's menus, dialogs, cutscenes and loading screens.
bool PartyStainWanted() {
    party::ping::StainMark m;
    return party::status::Enabled() && party::ping::GetStain(&m) && party::hud::Shown() && !party::hud::GameDialog();
}

void PartyStainMarker() {
    using namespace PartyHudDraw;
    if (!PartyStainWanted() || BbFrameState::IsMenuOrLoading()) {
        return;
    }
    party::ping::StainMark m;
    if (!party::ping::GetStain(&m)) {
        return;
    }
    party::hud::Board& b = hud_board;
    party::hud::Read(&b);
    if (!b.me_have_pos || party::hud::NowMs() - b.published_ms > 1000 || (m.map & 0xffff0000u) != (b.me_map & 0xffff0000u)) {
        return;
    }
    const BbCamera::State cam = BbCamera::Latest();
    if (!DirectionAxes(b, cam).camera) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 display = viewport->Size;
    const float s = base_scale;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float at3[3] = {m.pos[0], m.pos[1] + 0.4f, m.pos[2]};
    const party::ping::EdgePoint e =
        party::ping::ToScreenOrEdge(cam.view.data(), cam.proj.data(), at3, display.x, display.y, 28.0f * s);
    char dist[32], label[96];
    party::ping::FormatDistance(party::hud::Distance(b.me_pos, m.pos), dist, sizeof dist);
    std::snprintf(label, sizeof label, "%s %d  %s", BbSettings::MenuText("Blood Echoes", "Отголоски крови"), m.echoes,
                  dist);
    const ImU32 col = Rgba(0.78f, 0.10f, 0.10f, 0.95f), dark = Rgba(0.02f, 0.02f, 0.02f, 0.8f),
                text = Rgba(0.93f, 0.86f, 0.80f, 0.95f);
    ImGui::PushFont(game_font, 16.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const auto outlined = [&](ImVec2 p, const char* str) {
        for (const ImVec2 d : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1), ImVec2(1, 1)}) {
            bg->AddText(ImVec2(p.x + d.x * s, p.y + d.y * s), dark, str);
        }
        bg->AddText(p, text, str);
    };
    if (e.on_screen) {
        // A drop: a circle with a point on top, the label above it.
        const float r = 7.0f * s;
        const ImVec2 c(e.x, e.y);
        const ImVec2 tip(c.x, c.y - r * 2.2f), l(c.x - r * 0.75f, c.y - r * 0.6f), rr(c.x + r * 0.75f, c.y - r * 0.6f);
        bg->AddCircleFilled(c, r + 1.5f * s, dark, 24);
        bg->AddTriangleFilled(ImVec2(tip.x, tip.y - 1.5f * s), ImVec2(l.x - 1.5f * s, l.y), ImVec2(rr.x + 1.5f * s, rr.y), dark);
        bg->AddCircleFilled(c, r, col, 24);
        bg->AddTriangleFilled(tip, l, rr, col);
        outlined(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(tip.y - ts.y - 4.0f * s)), label);
    } else {
        const float r = 11.0f * s, ca = std::cos(e.angle), sa = std::sin(e.angle);
        const ImVec2 tip(e.x + ca * r, e.y + sa * r);
        const ImVec2 lp(e.x - ca * r * 0.6f - sa * r * 0.8f, e.y - sa * r * 0.6f + ca * r * 0.8f);
        const ImVec2 rp(e.x - ca * r * 0.6f + sa * r * 0.8f, e.y - sa * r * 0.6f - ca * r * 0.8f);
        bg->AddTriangleFilled(tip, lp, rp, col);
        bg->AddTriangle(tip, lp, rp, dark, 1.5f * s);
        float lx = e.x - ca * (r + 6.0f * s) - ts.x * 0.5f - ca * ts.x * 0.5f;
        float ly = e.y - sa * (r + 6.0f * s) - ts.y * 0.5f - sa * ts.y * 0.5f;
        lx = std::clamp(lx, 4.0f * s, display.x - ts.x - 4.0f * s);
        ly = std::clamp(ly, 4.0f * s, display.y - ts.y - 4.0f * s);
        outlined(ImVec2(std::round(lx), std::round(ly)), label);
    }
    ImGui::PopFont();
}

/// I2 (party_ping.h): every live ping of a member in this player's world on this map, through
/// walls, in the pinger's colour with its name and distance (clamped to the screen edge with an
/// arrow when off screen), and with "Show party through walls" every companion on this map as a
/// small diamond with its distance. Under the game's menus, dialogs, cutscenes and loading screens
/// nothing is drawn (PartyPingsWanted, the caller's GameMenu check). No allocation.
void PartyPings() {
    using namespace PartyHudDraw;
    if (!PartyPingsWanted() || BbFrameState::IsMenuOrLoading()) {
        return;
    }
    party::hud::Board& b = hud_board;
    party::hud::Read(&b);
    if (!b.active || !b.me_have_pos || party::hud::NowMs() - b.published_ms > 1000) {
        return;
    }
    const BbCamera::State cam = BbCamera::Latest();
    if (!DirectionAxes(b, cam).camera) {
        return;  // no game camera in the player's space (a cutscene camera, a load)
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 display = viewport->Size;
    const float s = base_scale;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    char text[64];
    const auto outlined = [&](ImVec2 at, ImU32 colour, float alpha, const char* str) {
        const ImU32 dark = Rgba(0.02f, 0.02f, 0.02f, 0.75f * alpha);
        for (const ImVec2 d : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1), ImVec2(1, 1)}) {
            bg->AddText(ImVec2(at.x + d.x * s, at.y + d.y * s), dark, str);
        }
        bg->AddText(at, colour, str);
    };
    const party::hud::Companion* member_of[party::ping::kMaxSlots] = {};
    for (int i = 0; i < b.count; ++i) {
        if (b.c[i].slot >= 0 && b.c[i].slot < party::ping::kMaxSlots) member_of[b.c[i].slot] = &b.c[i];
    }

    // Companions through walls: a diamond at the chest with the distance under it.
    if (PartyThroughWalls()) {
        ImGui::PushFont(game_font, 14.0f);  // FontScaleMain scales it
        for (int i = 0; i < b.count; ++i) {
            const party::hud::Companion& c = b.c[i];
            const char* why = "";
            if (!party::hud::TagVisible(c, party::hud::NowMs() - b.published_ms, &why)) continue;
            float pos[3] = {c.pos[0], c.pos[1], c.pos[2]};
            coop::PartyHudChrPos(c.chr, c.pos, pos);
            const float d = party::hud::Distance(b.me_pos, pos);
            if (d < 2.0f || d > 400.0f) continue;
            const float chest[3] = {pos[0], pos[1] + 1.0f, pos[2]};
            const party::hud::ScreenPoint sp =
                party::hud::WorldToScreen(cam.view.data(), cam.proj.data(), chest, display.x, display.y, 0.02f);
            if (!sp.visible) continue;
            float rgb[3];
            party::hud::MemberColour(c.slot, rgb);
            const float a = 0.85f, r = 6.0f * s;
            const ImVec2 c0(sp.x, sp.y);
            const ImVec2 q[4] = {ImVec2(c0.x, c0.y - r), ImVec2(c0.x + r, c0.y), ImVec2(c0.x, c0.y + r),
                                 ImVec2(c0.x - r, c0.y)};
            bg->AddQuadFilled(q[0], q[1], q[2], q[3], Rgba(rgb[0], rgb[1], rgb[2], 0.35f * a));
            bg->AddQuad(q[0], q[1], q[2], q[3], Rgba(0.02f, 0.02f, 0.02f, 0.8f * a), 3.0f * s);
            bg->AddQuad(q[0], q[1], q[2], q[3], Rgba(rgb[0], rgb[1], rgb[2], a), 1.5f * s);
            party::ping::FormatDistance(d, text, sizeof text);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            outlined(ImVec2(std::round(c0.x - ts.x * 0.5f), std::round(c0.y + r + 2.0f * s)),
                     Rgba(rgb[0], rgb[1], rgb[2], a), a, text);
        }
        ImGui::PopFont();
    }

    if (!coop::ping::Enabled()) {
        return;
    }
    party::ping::Marker marks[party::ping::kMaxSlots];
    const std::int64_t now = party::hud::NowMs();
    const int n = party::ping::Read(marks, now);
    for (int k = 0; k < n; ++k) {
        const party::ping::Marker& m = marks[k];
        if (m.map != b.me_map) continue;  // another map (or our own ping before a load)
        if (!m.mine) {
            const party::hud::Companion* c = member_of[m.slot];
            if (!c || !c->connected || !c->same_world) continue;  // not in this player's world
        }
        float rgb[3];
        party::hud::MemberColour(m.slot, rgb);
        const std::int64_t left = m.until_ms - now, age = party::ping::kLifeMs - left;
        const float a = party::ping::Alpha(left);
        const ImU32 col = Rgba(rgb[0], rgb[1], rgb[2], a), dark = Rgba(0.02f, 0.02f, 0.02f, 0.8f * a);
        const float inset = 28.0f * s;
        const party::ping::EdgePoint e =
            party::ping::ToScreenOrEdge(cam.view.data(), cam.proj.data(), m.pos, display.x, display.y, inset);
        const float d = party::hud::Distance(b.me_pos, m.pos);
        party::ping::FormatDistance(d, text, sizeof text);
        char label[64];
        std::snprintf(label, sizeof label, "%s  %s", m.name, text);
        ImGui::PushFont(game_font, 16.0f);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        if (e.on_screen) {
            // A pin: a ring with a dot on the spot, a stem up to the label (name, distance).
            const float r = 8.0f * s;
            const ImVec2 at(e.x, e.y);
            if (age < 1200) {  // an attention pulse for the first second
                const float t = float(age) / 1200.0f;
                bg->AddCircle(at, r * (1.0f + 2.5f * t), Rgba(rgb[0], rgb[1], rgb[2], 0.8f * (1.0f - t) * a), 32,
                              2.0f * s);
            }
            bg->AddCircle(at, r, dark, 32, 4.0f * s);
            bg->AddCircle(at, r, col, 32, 2.0f * s);
            bg->AddCircleFilled(at, 2.5f * s, col, 12);
            const float stem = 22.0f * s;
            bg->AddLine(ImVec2(at.x, at.y - r), ImVec2(at.x, at.y - r - stem), dark, 3.0f * s);
            bg->AddLine(ImVec2(at.x, at.y - r), ImVec2(at.x, at.y - r - stem), col, 1.5f * s);
            outlined(ImVec2(std::round(at.x - ts.x * 0.5f), std::round(at.y - r - stem - ts.y - 2.0f * s)), col, a,
                     label);
        } else {
            // Off screen: an arrow on the edge pointing at it, the name and distance beside it.
            const float r = 11.0f * s, ca = std::cos(e.angle), sa = std::sin(e.angle);
            const ImVec2 tip(e.x + ca * r, e.y + sa * r);
            const ImVec2 left_pt(e.x - ca * r * 0.6f - sa * r * 0.8f, e.y - sa * r * 0.6f + ca * r * 0.8f);
            const ImVec2 right_pt(e.x - ca * r * 0.6f + sa * r * 0.8f, e.y - sa * r * 0.6f - ca * r * 0.8f);
            bg->AddTriangleFilled(tip, left_pt, right_pt, col);
            bg->AddTriangle(tip, left_pt, right_pt, dark, 1.5f * s);
            // The label on the screen side of the arrow.
            float lx = e.x - ca * (r + 6.0f * s) - ts.x * 0.5f - ca * ts.x * 0.5f;
            float ly = e.y - sa * (r + 6.0f * s) - ts.y * 0.5f - sa * ts.y * 0.5f;
            lx = std::clamp(lx, 4.0f * s, display.x - ts.x - 4.0f * s);
            ly = std::clamp(ly, 4.0f * s, display.y - ts.y - 4.0f * s);
            outlined(ImVec2(std::round(lx), std::round(ly)), col, a, label);
        }
        ImGui::PopFont();
    }
}

/// Party notices (join / leave / travel): the latest party_status event for ToastMs in a bottom
/// corner, clear of the game's HUD (bottom left; bottom right while the performance HUD has the
/// bottom left), fading out. Not
/// interactive, never "menu open"; a newer event replaces the one shown.
void PartyToast() {
    if (!party::status::Enabled() || !BbSettings::Get().party_toasts) {
        return;
    }
    const std::int64_t now = SteadyMs();
    if (party::status::EventSeq() != toast_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        toast_seen = b.event_seq;
        toast_text = b.last_event;
        toast_until_ms = toast_text.empty() ? 0 : now + ToastMs;
    }
    const std::int64_t left = toast_until_ms.load() - now;
    if (left <= 0 || toast_text.empty()) {
        return;
    }
    const float alpha = std::clamp(float(left) / 500.0f, 0.0f, 1.0f); // fades over the last 0.5 s
    const auto& s = BbSettings::Get();
    const bool right = false;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    // Bottom left above the party HUD panel when that is drawn (the panel stacks above the
    // performance HUD instead, and the toast then keeps the bottom right).
    const float bottom = !right && party_hud_top > 0.0f ? party_hud_top - 6.0f * base_scale
                                                         : viewport->WorkPos.y + viewport->WorkSize.y - pad;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (right ? viewport->WorkSize.x - pad : pad), bottom),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * base_scale, 8.0f * base_scale));
    ImGui::PushFont(game_font, 19.0f);
    ImGui::Begin("##party_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::TextColored(ImVec4(0.96f, 0.88f, 0.70f, alpha), "%s", toast_text.c_str());
    // The game's gold underline, fading to both ends.
    const float w = ImGui::GetItemRectSize().x, y = ImGui::GetItemRectMax().y + 3.0f * base_scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 gold = ImGui::GetColorU32(ImVec4(0.80f, 0.68f, 0.46f, 0.85f * alpha));
    const ImU32 clear = ImGui::GetColorU32(ImVec4(0.80f, 0.68f, 0.46f, 0.0f));
    const float t = 1.2f * base_scale, mid = at.x + w * 0.5f;
    draw->AddRectFilledMultiColor(ImVec2(at.x, y), ImVec2(mid, y + t), clear, gold, gold, clear);
    draw->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(at.x + w, y + t), gold, clear, clear, gold);
    ImGui::Dummy(ImVec2(0.0f, 4.0f * base_scale));
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

/// The world warper's progress (world_warper.h): a small panel at the top middle, the warper's
/// own lines (phase and count, the area, what it does and the new pipelines). Not interactive.
void WarperPanel() {
    if (!coop::warper::Active()) {
        return;
    }
    const std::string text = coop::warper::StatusText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 10.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * base_scale, 6.0f * base_scale));
    ImGui::PushFont(game_font, 17.0f);
    ImGui::Begin("##world_warper", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    std::size_t at = 0;
    bool first = true;
    while (at <= text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;
        ImGui::TextColored(first ? ImVec4(0.96f, 0.88f, 0.70f, 1.0f) : ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "%s",
                           line.c_str());
        first = false;
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

/// The party alert: a bordered box in the upper middle (clear of the game's own dialogs), its
/// first line as the title, then the text; fades out over its last second.
void PartyAlert() {
    if (!party::status::Enabled()) {
        return;
    }
    const std::int64_t now = SteadyMs();
    if (party::status::AlertSeq() != alert_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        alert_seen = b.alert_seq;
        alert_text = b.alert;
        alert_until_ms = alert_text.empty() ? 0 : now + AlertMs;
    }
    const std::int64_t left = alert_until_ms.load() - now;
    if (left <= 0 || alert_text.empty()) {
        return;
    }
    const float alpha = std::clamp(float(left) / 1000.0f, 0.0f, 1.0f);
    const std::size_t nl = alert_text.find('\n');
    const std::string title = alert_text.substr(0, nl);
    const std::string body = nl == std::string::npos ? std::string() : alert_text.substr(nl + 1);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x - 32.0f * base_scale, 760.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.22f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * base_scale, 16.0f * base_scale));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.68f, 0.46f, 0.85f * alpha));
    ImGui::Begin("##party_alert", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(game_font, 22.0f);
    ImGui::TextColored(ImVec4(0.96f, 0.88f, 0.70f, alpha), "%s", title.c_str());
    ImGui::PopFont();
    if (!body.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f * base_scale));
        ImGui::PushFont(game_font, 18.0f);
        ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.88f, alpha), "%s", body.c_str());
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

/// While the game's full-screen menu is up the shown notice, alert and caption keep their remaining
/// time: their end moves on by each held frame (a notice that arrives under the menu waits too).
void HoldPartyNoticesInGameMenu() {
    const std::int64_t now = SteadyMs();
    if (!party::hud::GameMenu()) {
        menu_hold_last_ms = 0;
        return;
    }
    const std::int64_t dt = menu_hold_last_ms ? std::clamp<std::int64_t>(now - menu_hold_last_ms, 0, 1000) : 0;
    menu_hold_last_ms = now;
    // A notice / alert that came in under the menu starts its time when the menu closes.
    if (party::status::EventSeq() != toast_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        toast_seen = b.event_seq;
        toast_text = b.last_event;
        toast_until_ms = toast_text.empty() ? 0 : now + ToastMs;
    } else if (toast_until_ms.load() > now) {
        toast_until_ms += dt;
    }
    if (party::status::AlertSeq() != alert_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        alert_seen = b.alert_seq;
        alert_text = b.alert;
        alert_until_ms = alert_text.empty() ? 0 : now + AlertMs;
    } else if (alert_until_ms.load() > now) {
        alert_until_ms += dt;
    }
    const std::int64_t cu = coop::npctalk::CurrentCaption().until_ms;
    if (caption_hold_key.load() != cu) {
        caption_hold_key = cu;
        caption_hold_ms = 0;
    }
    if (CaptionUntilMs(cu) > now) caption_hold_ms += dt;
}

/// A companion's NPC conversation (party_npctalk.h): "Hunter talking to Gilbert" and the talker's
/// choice / gift (or the line itself when the game's own subtitle could not be used) in the game's
/// Baskerville, centred above the game's subtitles; fades out over its last second. No input.
void PartyNpcCaption() {
    if (!party::status::Enabled()) {
        return;
    }
    const coop::npctalk::Caption c = coop::npctalk::CurrentCaption();
    const std::int64_t left = CaptionUntilMs(c.until_ms) - SteadyMs();
    if (left <= 0 || (c.header.empty() && c.line.empty())) {
        return;
    }
    const float alpha = std::clamp(float(left) / 1000.0f, 0.0f, 1.0f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x - 32.0f * base_scale, 900.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.78f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(width, viewport->WorkSize.y * 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##party_npc_caption", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto centred = [&](const std::string& text, float size, ImVec4 colour) {
        ImGui::PushFont(game_font, size);
        const float wrap = width - 24.0f * base_scale;
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
        const float avail = std::max(ImGui::GetContentRegionAvail().x, sz.x);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - sz.x) * 0.5f));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // A soft shadow like the game's subtitles (no box).
        draw->AddText(nullptr, 0.0f, ImVec2(at.x + 1.5f * base_scale, at.y + 1.5f * base_scale),
                      ImGui::GetColorU32(ImVec4(0, 0, 0, 0.85f * alpha)), text.c_str(), nullptr, wrap);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextColored(colour, "%s", text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    };
    if (!c.header.empty()) {
        centred("[" + c.header + "]", 17.0f, ImVec4(0.80f, 0.68f, 0.46f, 0.95f * alpha));
    }
    if (!c.line.empty()) {
        centred(c.line, 21.0f, ImVec4(0.93f, 0.92f, 0.88f, alpha));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}


}
bool Visible() {
    return ToastWanted() || AlertWanted() || NpcCaptionWanted() || PartyHudWanted() ||
        PartyTagsWanted() || PartyPingsWanted() || PartyStainWanted() || coop::warper::Active();
}
void Draw(float scale) {
    base_scale = scale;
    PartyHudDiag();
    HoldPartyNoticesInGameMenu();
    if (!party::hud::GameMenu()) {
        PartyHud(); PartyPings(); PartyStainMarker(); PartyToast(); PartyAlert(); PartyNpcCaption();
    }
    WarperPanel();
}
}
