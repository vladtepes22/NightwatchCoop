// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch: the in-game Settings screen's content (bbport_settings_screen.h).
#include "bbport_settings_screen.h"

#include "bbport_menu_widgets.h"
#include "bbport_settings.h"
#include "engine/camera.h"
#include "engine/option_menu.h"
#include "input/input.h"
#include "input/menu_pointer.h"
#include "input/pad.h"
#include "party/coop_hooks.h"
#include "party/game_state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace BbSettingsScreen {
namespace {

// The book's own tabs keep their ids (0x24..0x27, the chalice picker's) and their name texts (40435..40438); the two
// after them get ids past the glyph browser's Custom tab (0x28) and menu text ids of ours (0xc8, not the game's).
// Frames: the generated inventory.gfx's (tools/engine/menu_assets.cpp SettingsTabPictures: 63..66; the glyph
// screen's grey System icon 61) and the game's weapon hand (4); without that movie, game tab pictures stand in.
constexpr Tab kTabs[] = {
    {0x24, 63, 2, 40435, u"Display"},
    {0x25, 64, 8, 40436, u"Sound"},
    {0x26, 65, 0x3c, 40437, u"Enhancements"},
    {0x27, 4, 4, 40438, u"Controls"},
    {0x29, 66, 0x39, 263193, u"Party"},
    {0x2a, 61, 0x3a, 263194, u"System"},
};
static_assert(std::size(kTabs) >= kBookTabs);

// ---- the rows: a PC row's value is the PC pages' setting (engine/option_menu.cpp, by its name's text id), read when
// the tab is made and saved as it changes; the others are the screen's own until they are wired ----
enum class Kind { Head, Toggle, Slider, Pick, Command, Defaults };
struct Row {
    Kind kind;
    const char16_t* name;
    const char16_t* help;
    int value = 0;                              // Toggle 0/1, Slider 0..10, Pick an index
    std::vector<const char16_t*> choices = {};  // Pick
    bool drop = false;                          // Pick: its choices in a pop-up (Confirm / a click opens it)
    int def = 0;                                // its default (Defaults; set from `value` when the tab is made)
    std::uint32_t pc = 0;                       // a PC row: its text id (BbEngine::Options::PcValue)
    bool auto_hud = false;                      // Auto-Hide HUD (BbSettings auto_hide_hud)
    int game = -1;                              // a game option: its byte in the game's options (GameOption)
    bool picture = false;                       // the details page shows the game's brightness picture
    int stored = -1;                            // the value the setting has (a change is saved once)
};
Row Pc(std::uint32_t id, Row r) {
    r.pc = id;
    return r;
}
// The game's own options: one object, [[0x553b130] + 0x48] (the System pages copy it out with 0x14ef730 and back with
// 0x14ef550 through 0x1bb5040: nothing else is applied or saved there; the save data keeps it). Its bytes (the
// pages' copy has them one lower from Subtitles on): +2 Brightness (0 .. 10), +4 Music, +5 Sound Effects, +6 Voice
// (0 .. 10), +8 Subtitles, +9 Display HUD (1 On, 0 Off: the game's On / Off list, 0x1b2b3b0). Defaults as the game's
// (0x1c8af20): Brightness 5, the volumes 10, Subtitles Off, Display HUD On.
constexpr int kBrightness = 2, kMusic = 4, kSoundEffects = 5, kVoice = 6, kSubtitles = 8, kDisplayHud = 9;
// The game's Controls screen's rows (its rows fn 0x1b211a0, the pages' copy one lower from Subtitles on): +0 Camera
// Sensitivity (0 .. 10), +1 Controller Vibration (0 .. 10), +0xa Camera X-Axis, +0xb Camera Y-Axis (the list
// 0x1b29ee0: Normal (40310) 1, Reversed (40311) 0), +0xc Auto Target Switch, +0xd Camera Auto-Adjust (On 1 / Off 0).
// Defaults (0x1c8af20): sensitivity 5, vibration 10, both axes Normal, both switches On.
constexpr int kCameraSpeed = 0, kVibration = 1, kCameraX = 0xa, kCameraY = 0xb, kAutoTarget = 0xc, kAutoAdjust = 0xd;
constexpr std::uint64_t kGameOptionsOwner = 0x553b130, kGameOptions = 0x48;
std::uint64_t GameOptions() {
    std::uint64_t owner = 0, options = 0;
    return coop::SafeGet(coop::Guest(kGameOptionsOwner), &owner) && owner &&
                   coop::SafeGet(owner + kGameOptions, &options) && options
               ? options
               : 0;
}
Row Game(int byte, Row r) {
    r.game = byte;
    return r;
}
using Rows = std::vector<Row>;

Rows DisplayRows() {
    return {
        {Kind::Head, u"Graphics", nullptr},
        Pc(117001, {Kind::Toggle, u"Anti-Aliasing", u"The game's own anti-aliasing pass.", 1}),
        Pc(117000, {Kind::Toggle, u"Ambient Occlusion", u"Screen-space ambient occlusion.", 1}),
        Pc(117007, {Kind::Slider, u"AO Strength", u"How dark corners and contact get. 5 is the game's own.", 5}),
        Pc(117006, {Kind::Slider, u"Shadow Distance", u"How far sun and moon shadows reach. 0 is the game's own.", 0}),
        // the choices: bbport_settings.h OutputWidths' order (the PC page's 117010.. texts)
        Pc(117004, {Kind::Pick, u"Output Resolution",
                    u"Existing upscaler output setting. Restart required unless Live Resolution is enabled.", 2,
                    {u"1280x720", u"1600x900", u"1920x1080", u"2560x1440", u"3840x2160", u"2560x1080 (21:9)",
                     u"3440x1440 (21:9)", u"3840x1600 (24:10)", u"3840x1080 (32:9)", u"5120x1440 (32:9)",
                     u"5120x2160 (21:9)", u"7680x2160 (32:9)", u"Display (auto)", u"Custom (bbport.ini)"},
                    true}),
        Pc(117008, {Kind::Slider, u"Fog", u"How thick the haze over distant streets is. 10 is the game's own.", 10}),
        {Kind::Head, u"Effects", nullptr},
        Pc(117005, {Kind::Toggle, u"Motion Blur",
                    u"The blur on fast camera and character movement. Requires a restart in this renderer.", 0}),
        Pc(117002, {Kind::Toggle, u"Depth of Field",
                    u"Blurs distant scenery. Off keeps far views sharp. Requires a restart in this renderer.", 0}),
        Pc(117003, {Kind::Toggle, u"Chromatic Aberration",
                    u"The colour fringing at the edges of the frame. Requires a restart in this renderer.", 0}),
        Pc(120001, {Kind::Toggle, u"Vignette", u"The darkened frame edges some areas use.", 1}),
        Pc(120000, {Kind::Slider, u"Bloom", u"The glow around bright light. 10 is the game's own.", 10}),
        {Kind::Head, u"Colour", nullptr},
        Pc(125004, {Kind::Toggle, u"Picture Tuning",
                    u"Applies the picture settings live. Off: the game's own picture. Moving one of them turns it on.",
                    0}),
        [] {
            // the game's Brightness screen's instruction (112010) and picture
            Row r = Game(kBrightness, {Kind::Slider, u"Brightness (Legacy)",
                                       u"Adjust slider until the man is visible and beast invisible.", 5});
            r.picture = true;
            return r;
        }(),
        Pc(125000, {Kind::Slider, u"Brightness (New)", u"How bright the picture is. 5 is the game's own.", 5}),
        Pc(125001, {Kind::Slider, u"Contrast", u"The difference between dark and bright. 5 is the game's own.", 5}),
        Pc(125002, {Kind::Slider, u"Saturation", u"Colour. 5 is the game's own; 0 is black and white.", 5}),
        Pc(125003, {Kind::Slider, u"Hue", u"Shifts every colour. 5 is the game's own.", 5}),
        {Kind::Head, u"HUD", nullptr},
        Game(kSubtitles, {Kind::Toggle, u"Subtitles", u"Turn subtitles ON/OFF.", 0}),
        Game(kDisplayHud, {Kind::Toggle, u"Display HUD", u"Turn HUD ON/OFF.", 1}),
        [] {
            Row r{Kind::Toggle, u"Auto-Hide HUD",
                  u"Bars and slots fade out when idle; damage, fights, lock-on, items and menus bring them back.", 0};
            r.auto_hud = true;
            return r;
        }(),
        {Kind::Defaults, u"Defaults", u"Every setting on this tab back to its default."},
    };
}
// Controls: the game's own controls, the PC pages' PC Controls (118000 .., 116002) and, while the PC camera is on
// (Enhancements' PC Camera Controls, read live; the camera code in), its Camera rows (118004, 121000, 121001).
// "Camera Sensitivity" of PC Controls is named Mouse Sensitivity here (the game's own row has that name).
Rows ControlsRows() {
    Rows rows = {
        {Kind::Head, u"General", nullptr},
        Game(kCameraX, {Kind::Pick, u"Camera X-Axis", u"Change camera x-axis.", 1, {u"Reversed", u"Normal"}}),
        Game(kCameraY, {Kind::Pick, u"Camera Y-Axis", u"Change camera y-axis.", 1, {u"Reversed", u"Normal"}}),
        Game(kCameraSpeed, {Kind::Slider, u"Camera Sensitivity", u"Set camera movement speed.", 5}),
        Game(kAutoTarget, {Kind::Toggle, u"Auto Target Switch", u"Auto lock-on to new target when former target dies.", 1}),
        Game(kAutoAdjust, {Kind::Toggle, u"Camera Auto-Adjust", u"Set whether the camera automatically avoids walls.", 1}),
        Game(kVibration, {Kind::Slider, u"Controller Vibration", u"Set strength of controller vibration.", 10}),
        {Kind::Head, u"PC", nullptr},
        Pc(118000, {Kind::Toggle, u"Mouse Camera", u"The mouse turns the camera outside menus.", 1}),
        Pc(118001, {Kind::Slider, u"Mouse Sensitivity", u"How fast the mouse turns the camera.", 5}),
        Pc(118002, {Kind::Toggle, u"Mouse X-Axis", u"The mouse's horizontal camera direction.", 0}),
        Pc(118003, {Kind::Toggle, u"Mouse Y-Axis", u"The mouse's vertical camera direction.", 0}),
        Pc(116002, {Kind::Toggle, u"Mouse in Menus", u"Move and click to select.", 1}),
        Pc(118005, {Kind::Toggle, u"Camera Auto Rotation",
                    u"Allow the camera to turn automatically while using the mouse.", 0}),
        // the choices: the PC page's (118010 .., 118013 ..), in BbSettings' Prompts / PadIcons order
        Pc(118006, {Kind::Pick, u"Button Prompts",
                    u"The button icons in menus and prompts. Automatic shows keys while the keyboard or mouse is in use.",
                    0, {u"Automatic", u"Keyboard", u"Controller"}}),
        Pc(118007, {Kind::Pick, u"Controller Icons",
                    u"The controller's buttons in menus and prompts. Automatic shows Xbox buttons while an Xbox "
                    u"controller is in use.",
                    0, {u"Automatic", u"PlayStation", u"Xbox"}}),
    };
    if (BbEngine::Camera::Active() && BbSettings::Get().camera_controls) {
        rows.push_back({Kind::Head, u"Camera", nullptr});
        rows.push_back(Pc(118004, {Kind::Slider, u"Field of View",
                                   u"How wide the camera sees. 0 is the game's own; each step widens it 5%.", 0}));
        rows.push_back(Pc(121000, {Kind::Slider, u"Camera Distance",
                                   u"How far behind the character the camera sits. 5 is the game's own.", 5}));
        rows.push_back(Pc(121001, {Kind::Slider, u"Camera Height",
                                   u"How high above the character the camera looks. 5 is the game's own.", 5}));
    }
    rows.push_back({Kind::Defaults, u"Defaults", u"Every setting on this tab back to its default."});
    return rows;
}
Rows SoundRows() {
    return {
        {Kind::Head, u"Sound", nullptr},
        Game(kMusic, {Kind::Slider, u"Music", u"Adjust music volume.", 10}),
        Game(kSoundEffects, {Kind::Slider, u"Sound Effects", u"Adjust sound effects volume.", 10}),
        Game(kVoice, {Kind::Slider, u"Voice", u"Adjust voice volume.", 10}),
        {Kind::Defaults, u"Defaults", u"Every setting on this tab back to its default."},
    };
}
Rows EnhancementRows() {
    return {
        {Kind::Head, u"Enhancements", nullptr},
        Pc(124001, {Kind::Toggle, u"Hunter's Dream Mirror",
                    u"Requires a restart. The Dream's mirror opens the appearance editor (the unused \"Put on Disguise\").", 0}),
        Pc(124002, {Kind::Toggle, u"Rebirth at the Altar",
                    u"Requires a restart. The Altar of Despair resets level and attributes.", 0}),
        Pc(124004, {Kind::Toggle, u"PC Camera Controls", u"Adds the Camera section to Controls: field of view, camera distance and height.", 0}),
        {Kind::Defaults, u"Defaults", u"Every setting on this tab back to its default."},
    };
}
Rows SystemRows() {
    return {
        {Kind::Head, u"System", nullptr},
        {Kind::Command, u"Exit Game", u"Return to the title screen."},
        {Kind::Command, u"Quit to Desktop", u"Close the game."},
    };
}

constexpr int kSlots = 13;  // tools/engine/menu_assets.cpp kSettingsSlots
// The slots' layout and the choices pop-up's (tools/engine/menu_assets.cpp SettingsRowsSprite: the same numbers), in
// the slots' own units (a line kPitch / kScale tall).
constexpr double kLine = 49 / 0.9;
constexpr int kPopupLines = 14, kPopupFrames = 180;
constexpr double kPopupPitch = 38, kPopupPad = 10, kPopupStep = 4;

// What one slot shows, laid out as the Environment screen: a section's heading shares the line of its first row
// (left, above), a divider line separates two sections, Defaults and commands are buttons.
struct Line {
    int row = -1;                     // into State::rows; -1: a divider line
    const char16_t* head = nullptr;   // the section heading over this line
};
struct State {
    int tab = -1;
    Rows rows;
    std::vector<Line> lines;
    int cursor = 0;  // a line with a row
    int top = 0;     // the first line shown
    std::u16string shown[kSlots];  // what each slot shows (redrawn only when it changes)
    int scroll_shown = -2;         // the scroll bar's thumb frame shown (0: hidden; -2: not yet set)
    std::uint32_t held = 0;
    std::chrono::steady_clock::time_point next{};
    // Confirm / Back as last seen (for their presses); at first every button counts as held: the one that opened
    // the screen (the pause menu's Confirm) acts only once let go and pressed again
    std::uint32_t buttons = ~0u;
    // the choices pop-up
    bool popup = false;
    int popup_line = -1;   // the line of the row it belongs to
    int popup_cursor = 0;  // the highlighted choice
    int popup_top = 0;     // the first choice shown
    std::u16string popup_shown = u"\x1";
    bool swallow = false;  // the Confirm / Back that closed it, until let go (the game never sees them)
    // the mouse
    bool pointer_seen = false;
    float pointer_x = 0, pointer_y = 0;
    // a direction moved the cursor: the mouse highlights again once it moved kNavigatedSlack px from here
    bool navigated = false;
    float navigated_x = 0, navigated_y = 0;
    std::chrono::steady_clock::time_point navigated_at{};
};
constexpr float kNavigatedSlack = 24;  // as input/menu_pointer.cpp
constexpr auto kNavigatedQuiet = std::chrono::milliseconds(500);
std::mutex g_mu;
State g_state;

// The settings' values into the rows (when the tab is made, and after a change: Picture Tuning may have followed).
void LoadValues(Rows& rows, bool defaults) {
    for (Row& r : rows) {
        std::int32_t v = 0, d = 0;
        if (r.pc && BbEngine::Options::PcValue(r.pc, &v, &d)) {
            r.value = r.stored = v;
            if (defaults) r.def = d;
        } else if (r.auto_hud) {
            r.value = r.stored = BbSettings::Get().auto_hide_hud ? 1 : 0;
            if (defaults) r.def = 0;
        } else if (r.game >= 0) {
            std::uint8_t b = 0;
            const std::uint64_t options = GameOptions();
            if (!options || !coop::SafeGet(options + std::uint64_t(r.game), &b)) continue;  // not wired: the screen's
            const int most = r.kind == Kind::Toggle ? 1 : r.kind == Kind::Pick ? int(r.choices.size()) - 1 : 10;
            r.value = r.stored = std::clamp(int(b), 0, std::max(0, most));
        }
    }
}
// What the player changed, saved (each row once per change); true when one was.
constexpr std::uint32_t kPictureTuning = 125004;  // a picture row's change turns it on: written after them
bool SaveValues(Rows& rows) {
    bool saved = false;
    for (int k = 0; k < 2 * int(rows.size()); ++k) {  // two passes: Picture Tuning's row in the second
        const int pass = k / int(rows.size());
        Row& r = rows[std::size_t(k % int(rows.size()))];
        if ((r.pc == kPictureTuning) != (pass == 1)) continue;
        if (r.value == r.stored || (!r.pc && !r.auto_hud && r.game < 0)) continue;  // (a row that is not wired)
        if (r.pc) {
            BbEngine::Options::SetPcValue(r.pc, r.value);
        } else if (r.game >= 0) {
            const std::uint64_t options = GameOptions();
            const std::uint8_t b = std::uint8_t(r.value);
            const bool done = options && coop::SafeWrite(options + std::uint64_t(r.game), &b, 1);
            std::printf("Settings screen: the game's option +%d now %d%s\n", r.game, r.value,
                        done ? "" : ": the game's options are not there, not changed");
            std::fflush(stdout);
        } else {
            BbSettings::Get().auto_hide_hud = r.value != 0;
            BbSettings::Save();
            std::printf("Settings screen: Auto-Hide HUD %s; saved to bbport.ini\n", r.value ? "on" : "off");
            std::fflush(stdout);
        }
        r.stored = r.value;
        saved = true;
    }
    return saved;
}

Rows RowsOf(int tab) {
    switch (tab) {
    case 0: return DisplayRows();
    case 1: return SoundRows();
    case 2: return EnhancementRows();
    case 3: return ControlsRows();
    case 5: return SystemRows();
    default: return {};
    }
}
// A heading is a line of its own (over its underline); the list ends with an empty line, so its last row keeps a
// little room from the frame.
std::vector<Line> LinesOf(const Rows& rows) {
    std::vector<Line> lines;
    for (int i = 0; i < int(rows.size()); ++i) {
        const Row& r = rows[std::size_t(i)];
        lines.push_back(r.kind == Kind::Head ? Line{-1, r.name} : Line{i, nullptr});
    }
    if (!lines.empty()) lines.push_back(Line{});
    return lines;
}
const Row* RowOf(const State& s, int line) {
    if (line < 0 || line >= int(s.lines.size()) || s.lines[std::size_t(line)].row < 0) return nullptr;
    return &s.rows[std::size_t(s.lines[std::size_t(line)].row)];
}

std::u16string Escaped(const char16_t* s) {
    std::u16string out;
    for (; s && *s; ++s) {
        if (*s == u'&') out += u"&amp;";
        else if (*s == u'<') out += u"&lt;";
        else out += *s;
    }
    return out;
}
std::u16string Html(const char16_t* align, int size, const char16_t* color, const std::u16string& text) {
    std::u16string px;
    for (char c : std::to_string(size)) px += char16_t(c);
    return std::u16string(u"<p align=\"") + align + u"\"><font size=\"" + px + u"\" color=\"" + color +
           u"\" letterSpacing=\"0.000000\" kerning=\"1\">" + text + u"</font></p>";
}
std::u16string Number(int v) {
    std::u16string s;
    for (char c : std::to_string(v)) s += char16_t(c);
    return s;
}
std::u16string ValueText(const Row& r) {
    switch (r.kind) {
    case Kind::Toggle: return r.value ? u"On" : u"Off";
    case Kind::Pick: return r.value >= 0 && r.value < int(r.choices.size()) ? Escaped(r.choices[std::size_t(r.value)]) : u"";
    default: return u"";
    }
}

std::string SlotPath(int slot, const char* part) { return "ItemList/SettingsRows/Slot_" + std::to_string(slot) + "/" + part; }

void DrawSlot(std::uint64_t root, int slot, const Line* line, const Row* r, bool selected, State& s) {
    std::u16string key;  // what the slot shows, as one key (the parts are redrawn only when it changes)
    if (line) {
        key = line->head ? std::u16string(line->head) : u"";
        key += r ? std::u16string(1, char16_t('0' + int(r->kind))) + (selected ? u"*" : u"-") + r->name + u"|" +
                       ValueText(*r) + u"|" + Number(r->value)
                 : u"line";
    }
    if (key == s.shown[slot]) return;
    s.shown[slot] = key;
    using BbMenuWidgets::SetFieldText;
    const auto show = [&](const char* part, bool v) { BbMenuWidgets::ShowPart(root, SlotPath(slot, part).c_str(), v); };
    const bool slider = r && r->kind == Kind::Slider, combo = r && (r->kind == Kind::Toggle || r->kind == Kind::Pick);
    const bool button = r && r->kind == Kind::Defaults, command = r && r->kind == Kind::Command;
    const bool named = r && !button && !command;  // commands: a button centred on the list
    const char16_t* color = selected ? u"#ffffff" : u"#cccccc";
    if (line && line->head) SetFieldText(root, SlotPath(slot, "Head").c_str(), Html(u"left", 26, u"#b49762", Escaped(line->head)).c_str());
    if (named) SetFieldText(root, SlotPath(slot, "Name").c_str(), Html(u"right", 24, color, Escaped(r->name)).c_str());
    if (combo)
        SetFieldText(root, SlotPath(slot, "Combo/Item_0_0/Text_0").c_str(), Html(u"center", 24, color, ValueText(*r)).c_str());
    if (slider) {
        SetFieldText(root, SlotPath(slot, "Slider/Text_0").c_str(), Html(u"center", 23, u"#cccccc", Number(r->value)).c_str());
        BbMenuWidgets::GotoPartFrame(root, SlotPath(slot, "Slider/Bar").c_str(), 1 + r->value * 99 / 10);
    }
    if (button) SetFieldText(root, SlotPath(slot, "Button/Text_0").c_str(), Html(u"center", 24, color, Escaped(r->name)).c_str());
    if (command) SetFieldText(root, SlotPath(slot, "Command/Text_0").c_str(), Html(u"center", 24, color, Escaped(r->name)).c_str());
    show("Head", line && line->head);
    show("Name", named);
    show("NameLine", named);
    show("Cursor", selected && named);
    show("Combo", combo);
    show("Combo/Cursor", selected && combo);
    show("Slider", slider);
    show("Slider/Cursor", selected && slider);
    show("Slider/LeftArrow", selected && slider);
    show("Slider/RightArrow", selected && slider);
    show("Button", button);
    show("Button/Cursor", selected && button);
    show("Command", command);
    show("Command/Cursor", selected && command);
    show("HeadLine", line && line->head);
}

int FirstSelectable(const State& s) {
    for (int i = 0; i < int(s.lines.size()); ++i)
        if (RowOf(s, i)) return i;
    return 0;
}
void Scroll(State& s) {
    int want = s.top;
    // going up: the cursor's heading (the line above it) in view too
    const int above = s.cursor > 0 && !RowOf(s, s.cursor - 1) && s.lines[std::size_t(s.cursor - 1)].head ? s.cursor - 1 : s.cursor;
    if (above < want) want = above;
    // going down: the line after the cursor in view too (the end's empty line, or what follows)
    const int below = std::min(s.cursor + 1, int(s.lines.size()) - 1);
    if (below >= want + kSlots) want = below - kSlots + 1;
    s.top = std::max(0, std::min(want, std::max(0, int(s.lines.size()) - kSlots)));
}
void Move(State& s, int dir) {
    for (int i = s.cursor + dir; i >= 0 && i < int(s.lines.size()); i += dir) {
        if (!RowOf(s, i)) continue;
        s.cursor = i;
        Scroll(s);
        BbMenuWidgets::CursorMoveSound();
        return;
    }
}
void Change(State& s, int dir) {
    if (s.cursor < 0 || s.cursor >= int(s.lines.size()) || s.lines[std::size_t(s.cursor)].row < 0) return;
    Row& r = s.rows[std::size_t(s.lines[std::size_t(s.cursor)].row)];
    const int before = r.value;
    switch (r.kind) {
    case Kind::Toggle: r.value = r.value ? 0 : 1; break;
    case Kind::Slider: r.value = std::clamp(r.value + dir, 0, 10); break;
    case Kind::Pick:
        if (!r.choices.empty()) r.value = (r.value + dir + int(r.choices.size())) % int(r.choices.size());
        break;
    default: return;
    }
    if (r.value != before) BbMenuWidgets::TypeSound();
}
Row* CurrentRow(State& s) {
    if (s.cursor < 0 || s.cursor >= int(s.lines.size()) || s.lines[std::size_t(s.cursor)].row < 0) return nullptr;
    return &s.rows[std::size_t(s.lines[std::size_t(s.cursor)].row)];
}

// ---- the choices pop-up ----
int PopupShown(const Row& r) { return std::min(int(r.choices.size()), kPopupLines); }
void PopupScroll(State& s, const Row& r) {
    const int shown = PopupShown(r);
    s.popup_top = std::clamp(s.popup_top, std::max(0, s.popup_cursor - shown + 1), s.popup_cursor);
    s.popup_top = std::clamp(s.popup_top, 0, std::max(0, int(r.choices.size()) - shown));
}
void OpenPopup(State& s) {
    Row* r = CurrentRow(s);
    if (!r || r->choices.empty()) return;
    s.popup = true;
    s.popup_line = s.cursor;
    s.popup_cursor = std::clamp(r->value, 0, int(r->choices.size()) - 1);
    s.popup_top = s.popup_cursor - PopupShown(*r) / 2;
    PopupScroll(s, *r);
    s.popup_shown = u"\x1";
    BbMenuWidgets::EnterSound();
    std::printf("Settings screen: the %s pop-up is open\n", "choices");
    std::fflush(stdout);
}
void ClosePopup(State& s, bool pick) {
    Row* r = CurrentRow(s);
    if (pick && r) {
        r->value = s.popup_cursor;
        BbMenuWidgets::DecideSound();
    } else {
        BbMenuWidgets::CancelSound();
    }
    s.popup = false;
    s.swallow = true;
    s.popup_shown = u"\x1";
    s.shown[std::size_t(std::clamp(s.popup_line - s.top, 0, kSlots - 1))] = u"\x1";  // its row redrawn
    std::printf("Settings screen: the choices pop-up closed (%s)\n", pick ? "a choice" : "cancelled");
    std::fflush(stdout);
}
// Under its row when it fits in the list, else over it, else as low as fits.
int PopupFrame(const State& s, const Row& r) {
    const double row = (s.popup_line - s.top) * kLine;
    const double high = 2 * kPopupPad + PopupShown(r) * kPopupPitch, list = kSlots * kLine;
    double top = row + 50;
    if (top + high > list) top = row + 4 - high >= 0 ? row + 4 - high : std::max(0.0, list - high);
    return 1 + std::clamp(int(std::lround(top / kPopupStep)), 0, kPopupFrames - 1);
}
std::string PopupPath(int item, const char* part) {
    return "ItemList/SettingsRows/Popup/Box/Item_" + std::to_string(item) + "/" + part;
}
void DrawPopup(std::uint64_t root, State& s) {
    const Row* r = RowOf(s, s.popup_line);
    std::u16string key;
    if (s.popup && r) {
        key = Number(s.popup_line - s.top) + u"|" + Number(s.popup_top) + u"|" + Number(s.popup_cursor) + u"|" +
              r->name;
    }
    if (key == s.popup_shown) return;
    const bool was_open = !s.popup_shown.empty() && s.popup_shown != u"\x1";
    s.popup_shown = key;
    BbMenuWidgets::ShowPart(root, "ItemList/SettingsRows/Popup", !key.empty());
    if (key.empty()) return;
    (void)was_open;
    const int shown = PopupShown(*r);
    BbMenuWidgets::GotoPartFrame(root, "ItemList/SettingsRows/Popup", PopupFrame(s, *r));
    BbMenuWidgets::GotoPartFrame(root, "ItemList/SettingsRows/Popup/Box/Frame", shown);
    for (int k = 0; k < kPopupLines; ++k) {
        const int choice = s.popup_top + k;
        const bool on = k < shown && choice < int(r->choices.size());
        BbMenuWidgets::ShowPart(root, ("ItemList/SettingsRows/Popup/Box/Item_" + std::to_string(k)).c_str(), on);
        if (!on) continue;
        const bool lit = choice == s.popup_cursor;
        const char16_t* color = lit ? u"#ffffff" : choice == r->value ? u"#b49762" : u"#cccccc";
        BbMenuWidgets::SetFieldText(root, PopupPath(k, "Text/Text_0").c_str(),
                                    Html(u"center", 22, color, Escaped(r->choices[std::size_t(choice)])).c_str());
        BbMenuWidgets::ShowPart(root, PopupPath(k, "Cursor").c_str(), lit);
    }
}

// Every row of the tab back to its default.
void Defaults(State& s) {
    for (Row& r : s.rows) {
        r.value = r.def;
        r.stored = -1;  // every one written (Picture Tuning too, after the picture rows that turn it on)
    }
    for (auto& k : s.shown) k = u"\x1";
    BbMenuWidgets::DecideSound();
    std::printf("Settings screen: tab %d back to its defaults\n", s.tab);
    std::fflush(stdout);
}
// Confirm (or a click) on the selected row: a toggle switches, a list steps on or opens its pop-up, Defaults resets
// the tab; a command is not wired yet.
void Activate(State& s) {
    Row* r = CurrentRow(s);
    if (!r) return;
    switch (r->kind) {
    case Kind::Toggle: Change(s, 1); break;
    case Kind::Pick:
        if (r->drop) OpenPopup(s);
        else Change(s, 1);
        break;
    case Kind::Defaults: Defaults(s); break;
    case Kind::Command:
        BbMenuWidgets::DecideSound();
        std::printf("Settings screen: %s (not wired yet)\n", r->name == std::u16string(u"Exit Game") ? "Exit Game" : "Quit to Desktop");
        std::fflush(stdout);
        break;
    default: break;
    }
}

// A press, then held: again after 0.5 s, then 20 a second (as the on-screen keyboard).
std::uint32_t Pressed(State& s, std::uint32_t now_held) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    const std::uint32_t fresh = now_held & ~s.held;
    s.held = now_held;
    if (fresh) {
        s.next = now + milliseconds(500);
        return fresh;
    }
    if (now_held && now >= s.next) {
        s.next = now + milliseconds(50);
        return now_held;
    }
    return 0;
}

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("BB_SETTINGS_SCREEN");
        return v && *v == '1';
    }();
    return on;
}

std::span<const Tab> Tabs() { return kTabs; }

const char16_t* Text(std::uint32_t category, std::uint32_t id) {
    if (category == 0xc8) {
        if (id == 263100) return u"Settings";  // the book's title
        for (const Tab& t : kTabs)
            if (t.text == id) return t.name;
        return nullptr;
    }
    if (category == 0xc9 && id == 263000) {  // the help line: the selected row's help
        std::lock_guard<std::mutex> lk(g_mu);
        const Row* r = RowOf(g_state, g_state.cursor);
        return r && r->help ? r->help : u"";
    }
    return nullptr;
}

void Selected(const char16_t** name, const char16_t** help, bool* picture) {
    std::lock_guard<std::mutex> lk(g_mu);
    const Row* r = RowOf(g_state, g_state.cursor);
    *name = r ? r->name : nullptr;
    *help = r ? r->help : nullptr;
    if (picture) *picture = r && r->picture;
}

void Opened() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_state = State{};
}

bool Update(std::uint64_t root, int tab, const Pointer& pointer) {
    std::lock_guard<std::mutex> lk(g_mu);
    State& s = g_state;
    using namespace PadBits;
    const PadState pad = BbInput::LastPad();
    const std::uint32_t confirm = menu_confirm_button(), back = menu_back_button();
    if (s.swallow && !(pad.buttons & (confirm | back))) s.swallow = false;
    // L1 / R1 switch the tabs here, the d-pad's Left / Right (and the left stick) are the rows'; the wheel scrolls
    // the view; the pop-up (and the button that closed it, until let go) takes every button
    BbInput::ScreenInput how;
    how.own_rows = true;
    how.all = s.popup || s.swallow;
    BbInput::HoldScreenInput(how);
    const int wheel = BbInput::TakeWheelLines();
    const std::uint32_t fresh_buttons = pad.buttons & ~s.buttons;
    s.buttons = pad.buttons;
    bool help_changed = false;
    if (s.tab < 0 && pointer.take_click) pointer.take_click();  // opening: the pause menu's click is not ours
    if (tab != s.tab) {
        s.tab = tab;
        s.rows = RowsOf(tab);
        for (Row& r : s.rows) r.def = r.value;
        LoadValues(s.rows, true);
        s.popup = false;
        s.popup_shown = u"\x1";
        s.lines = LinesOf(s.rows);
        s.cursor = FirstSelectable(s);
        s.top = 0;
        Scroll(s);
        for (auto& k : s.shown) k = u"\x1";  // redraw every slot
        s.scroll_shown = -2;
        help_changed = true;
        std::printf("Settings screen: tab %d, %zu rows on %zu lines\n", tab, s.rows.size(), s.lines.size());
        std::fflush(stdout);
    }
    const std::uint32_t dirs = pad.buttons & (kUp | kDown | kLeft | kRight);  // the left stick's too (input.cpp)
    const std::uint32_t go = Pressed(s, dirs);
    // the mouse: moved, it highlights what it is on; a click there acts on it
    bool moved = pointer.on && s.pointer_seen && (pointer.x != s.pointer_x || pointer.y != s.pointer_y);
    if (dirs) {
        s.navigated = true;
        s.navigated_x = pointer.x, s.navigated_y = pointer.y;
        s.navigated_at = std::chrono::steady_clock::now();
    } else if (s.navigated) {
        const float dx = pointer.x - s.navigated_x, dy = pointer.y - s.navigated_y;
        if (std::chrono::steady_clock::now() - s.navigated_at > kNavigatedQuiet &&
            dx * dx + dy * dy > kNavigatedSlack * kNavigatedSlack)
            s.navigated = false;
        else
            moved = false;
    }
    s.pointer_seen = pointer.on;
    s.pointer_x = pointer.x, s.pointer_y = pointer.y;
    const auto on = [&](const std::string& path, float* fx = nullptr) {
        return pointer.on && pointer.at && pointer.at(root, path.c_str(), pointer.x, pointer.y, fx);
    };
    const auto click = [&] { return pointer.take_click && pointer.take_click(); };
    if (s.popup) {
        const Row* r = RowOf(s, s.popup_line);
        if (!r) {
            s.popup = false;
        } else {
            const int count = int(r->choices.size()), shown = PopupShown(*r);
            int over = -1;  // the choice the mouse is on
            for (int k = 0; k < shown && pointer.on; ++k)
                if (s.popup_top + k < count && on(PopupPath(k, "Hit"))) over = s.popup_top + k;
            if (moved && over >= 0 && over != s.popup_cursor) {
                s.popup_cursor = over;
                BbMenuWidgets::CursorMoveSound();
            }
            if (pointer.on && click()) {
                if (over >= 0) {
                    s.popup_cursor = over;
                    ClosePopup(s, true);
                } else if (!on("ItemList/SettingsRows/Popup/Box/Frame")) {
                    ClosePopup(s, false);  // a click off the pop-up
                }
            } else if (fresh_buttons & confirm) {
                ClosePopup(s, true);
            } else if (fresh_buttons & back) {
                ClosePopup(s, false);
            } else if (go & (kUp | kDown)) {
                const int was = s.popup_cursor;
                s.popup_cursor = std::clamp(s.popup_cursor + (go & kUp ? -1 : 1), 0, count - 1);
                PopupScroll(s, *r);
                if (s.popup_cursor != was) BbMenuWidgets::CursorMoveSound();
            }
        }
    } else if (!s.lines.empty()) {
        const int was = s.cursor;
        // the wheel: the view only, the cursor stays (out of view, the next key or button brings it back)
        if (wheel) {  // the cursor's move sound for each scroll that moved the view, as the game's lists
            const int top = std::clamp(s.top + wheel, 0, std::max(0, int(s.lines.size()) - kSlots));
            if (top != s.top) BbMenuWidgets::CursorMoveSound();
            s.top = top;
        }
        if ((go || (fresh_buttons & confirm)) && (s.cursor < s.top || s.cursor >= s.top + kSlots)) Scroll(s);
        int over = -1, over_slot = -1;  // the row line the mouse is on
        for (int i = 0; i < kSlots && pointer.on; ++i) {
            const int at = s.top + i;
            if (RowOf(s, at) && on(SlotPath(i, "Hit"))) over = at, over_slot = i;
        }
        if (moved && over >= 0 && over != s.cursor) {
            s.cursor = over;  // no scrolling: the line is in view, and the list stays under the mouse
            BbMenuWidgets::CursorMoveSound();
        }
        if (over >= 0 && click()) {
            if (over != s.cursor) {
                s.cursor = over;
                BbMenuWidgets::CursorMoveSound();
            }
            Row* r = CurrentRow(s);
            float fx = 0;
            if (r && r->kind == Kind::Slider) {
                if (on(SlotPath(over_slot, "Slider/LeftArrow"))) Change(s, -1);
                else if (on(SlotPath(over_slot, "Slider/RightArrow"))) Change(s, 1);
                else if (on(SlotPath(over_slot, "BarHit"), &fx)) {
                    const int v = std::clamp(int(std::lround(fx * 10)), 0, 10);
                    if (v != r->value) {
                        r->value = v;
                        BbMenuWidgets::TypeSound();
                    }
                }
            } else if (r && r->kind == Kind::Pick && !r->drop && on(SlotPath(over_slot, "Combo"), &fx)) {
                Change(s, fx < 0.5f ? -1 : 1);
            } else {
                Activate(s);
            }
        } else if (fresh_buttons & confirm) {
            Activate(s);
        } else if (go & kUp) Move(s, -1);
        else if (go & kDown) Move(s, 1);
        else if (go & kLeft) Change(s, -1);
        else if (go & kRight) Change(s, 1);
        help_changed = help_changed || s.cursor != was;
    }
    if (SaveValues(s.rows)) LoadValues(s.rows, false);
    for (int i = 0; i < kSlots; ++i) {
        const int at = s.top + i;
        const Line* line = at < int(s.lines.size()) ? &s.lines[std::size_t(at)] : nullptr;
        DrawSlot(root, i, line, line ? RowOf(s, at) : nullptr, at == s.cursor, s);
    }
    DrawPopup(root, s);
    // the scroll bar: shown while the tab has more lines than slots, its thumb at how far the list is scrolled
    const int more = int(s.lines.size()) - kSlots;
    // the thumb: frame 1 + size * 21 + position (tools/engine/menu_assets.cpp Thumb): size from the share of the
    // list in view (10 .. 90 %), position in twentieths of the scroll
    const int share = more > 0 ? std::clamp(int(std::lround(10.0 * kSlots / double(s.lines.size()))) - 1, 0, 8) : 0;
    const int thumb = more > 0 ? 1 + share * 21 + int(std::lround(20.0 * s.top / more)) : 0;
    if (thumb != s.scroll_shown) {
        BbMenuWidgets::ShowPart(root, "ItemList/SettingsScroll", thumb > 0);
        if (thumb > 0) BbMenuWidgets::GotoPartFrame(root, "ItemList/SettingsScroll/Thumb", thumb);
        s.scroll_shown = thumb;
    }
    return help_changed;
}

} // namespace BbSettingsScreen
