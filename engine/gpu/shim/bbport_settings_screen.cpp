// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch: the in-game Settings screen's content (bbport_settings_screen.h).
#include "bbport_settings_screen.h"

#include "bbport_menu_widgets.h"
#include "input/input.h"
#include "input/pad.h"

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

// ---- the rows (step 2a: the layout and the cursor; values are the screen's own until they are wired) ----
enum class Kind { Head, Toggle, Slider, Pick, Command, Defaults };
struct Row {
    Kind kind;
    const char16_t* name;
    const char16_t* help;
    int value = 0;                              // Toggle 0/1, Slider 0..10, Pick an index
    std::vector<const char16_t*> choices = {};  // Pick
};
using Rows = std::vector<Row>;

Rows DisplayRows() {
    return {
        {Kind::Head, u"Graphics", nullptr},
        {Kind::Toggle, u"Anti-Aliasing", u"The game's own anti-aliasing pass.", 1},
        {Kind::Toggle, u"Ambient Occlusion", u"Screen-space ambient occlusion.", 1},
        {Kind::Slider, u"AO Strength", u"How dark corners and contact get. 5 is the game's own.", 5},
        {Kind::Slider, u"Shadow Distance", u"How far sun and moon shadows reach. 0 is the game's own.", 0},
        {Kind::Pick, u"Output Resolution", u"Existing upscaler output setting.", 3,
         {u"1280x720", u"1600x900", u"1920x1080", u"2560x1440", u"3840x2160"}},
        {Kind::Slider, u"Fog", u"How thick the haze over distant streets is. 10 is the game's own.", 10},
        {Kind::Head, u"Effects", nullptr},
        {Kind::Toggle, u"Motion Blur", u"The blur on fast camera and character movement.", 0},
        {Kind::Toggle, u"Depth of Field", u"Blurs distant scenery. Off keeps far views sharp.", 0},
        {Kind::Toggle, u"Chromatic Aberration", u"The colour fringing at the edges of the frame.", 0},
        {Kind::Toggle, u"Vignette", u"The darkened frame edges some areas use.", 1},
        {Kind::Slider, u"Bloom", u"The glow around bright light. 10 is the game's own.", 10},
        {Kind::Head, u"Colour", nullptr},
        {Kind::Toggle, u"Picture Tuning", u"Applies the picture settings live. Off: the game's own picture.", 0},
        {Kind::Slider, u"Brightness (Legacy)", u"Adjust the brightness level.", 5},
        {Kind::Slider, u"Brightness (New)", u"How bright the picture is. 5 is the game's own.", 5},
        {Kind::Slider, u"Contrast", u"The difference between dark and bright. 5 is the game's own.", 5},
        {Kind::Slider, u"Saturation", u"Colour. 5 is the game's own; 0 is black and white.", 5},
        {Kind::Slider, u"Hue", u"Shifts every colour. 5 is the game's own.", 5},
        {Kind::Head, u"HUD", nullptr},
        {Kind::Toggle, u"Subtitles", u"Turn subtitles ON/OFF.", 1},
        {Kind::Toggle, u"Display HUD", u"Turn HUD ON/OFF.", 1},
        {Kind::Toggle, u"Auto-Hide HUD", u"Hides the HUD until something happens.", 0},
        {Kind::Defaults, u"Defaults", u"Every setting on this tab back to its default."},
    };
}
Rows SoundRows() {
    return {
        {Kind::Head, u"Sound", nullptr},
        {Kind::Slider, u"Music", u"Adjust music volume.", 3},
        {Kind::Slider, u"Sound Effects", u"Adjust sound effects volume.", 3},
        {Kind::Slider, u"Voice", u"Adjust voice volume.", 3},
        {Kind::Defaults, u"Defaults", u"Every setting on this tab back to its default."},
    };
}
Rows EnhancementRows() {
    return {
        {Kind::Head, u"Enhancements", nullptr},
        {Kind::Toggle, u"Hunter's Dream Mirror", u"Requires a restart. The Dream's mirror opens the appearance editor.", 0},
        {Kind::Toggle, u"Rebirth at the Altar", u"Requires a restart. The Altar of Despair resets level and attributes.", 0},
        {Kind::Toggle, u"PC Camera Controls", u"Requires a restart. Adds the Camera section to Controls.", 0},
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
};
std::mutex g_mu;
State g_state;

Rows RowsOf(int tab) {
    switch (tab) {
    case 0: return DisplayRows();
    case 1: return SoundRows();
    case 2: return EnhancementRows();
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

void Opened() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_state = State{};
}

bool Update(std::uint64_t root, int tab) {
    BbInput::HoldTabRemap();  // L1 / R1 switch the tabs here, the d-pad's Left / Right are the rows'
    std::lock_guard<std::mutex> lk(g_mu);
    State& s = g_state;
    bool help_changed = false;
    if (tab != s.tab) {
        s.tab = tab;
        s.rows = RowsOf(tab);
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
    using namespace PadBits;
    const PadState pad = BbInput::LastPad();
    std::uint32_t dirs = pad.buttons & (kUp | kDown | kLeft | kRight);
    if (pad.ly < 64) dirs |= kUp;  // the left stick, as the game's menus take it
    if (pad.ly > 192) dirs |= kDown;
    if (pad.lx < 64) dirs |= kLeft;
    if (pad.lx > 192) dirs |= kRight;
    const std::uint32_t go = Pressed(s, dirs);
    if (!s.lines.empty()) {
        const int was = s.cursor;
        if (go & kUp) Move(s, -1);
        else if (go & kDown) Move(s, 1);
        else if (go & kLeft) Change(s, -1);
        else if (go & kRight) Change(s, 1);
        help_changed = help_changed || s.cursor != was;
    }
    for (int i = 0; i < kSlots; ++i) {
        const int at = s.top + i;
        const Line* line = at < int(s.lines.size()) ? &s.lines[std::size_t(at)] : nullptr;
        DrawSlot(root, i, line, line ? RowOf(s, at) : nullptr, at == s.cursor, s);
    }
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
