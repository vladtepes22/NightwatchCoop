// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_name_entry.h"

#include "bbport_game_menu.h"
#include "bbport_menu_widgets.h"
#include "input/input.h"
#include "../bbgpu.h"
#include "party/coop_hooks.h"
#include "party/game_state.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" int runtime_file_mount(const char*, const char*);

namespace BbNameEntry {
namespace {

using coop::u64;
using u32 = std::uint32_t;
using BbMenuWidgets::Wrapper;
using BbOsk::PadKey;

// The Contract screen (chrmaketop.gfx's ChrMakeTop; 0x1b8dbb0 makes it, 0x2210 bytes): 0x1b8e6b0(screen, ?, dt) runs
// each frame (its vtable slot at 0x539bbc8). It reads the character's name with 0x1b96490(out string) and shows
// NoInputName ("Entry Incomplete", its wrapper at screen + 0x21b0) while the name is empty. The movie's root wrapper is
// at screen + 0x100; the name row's text is ItemList/Status/CharacterName/Value_0. While typed in, the generated
// movie's NameField shows over the row (a dark field in a gold frame and its own Text) and the row's text hides.
constexpr u64 kUpdate = 0x1b8e6b0, kNameOf = 0x1b96490;
constexpr u64 kRoot = 0x100, kNoInputName = 0x21b0;
constexpr const char* kNameField = "ItemList/Status/CharacterName/Value_0";
constexpr const char* kTypingField = "ItemList/Status/NameField";
constexpr const char* kTypingText = "ItemList/Status/NameField/Text";
// The name getter's string: its text at +0 when set, else at +0x10 (inline below 8 units of capacity, else a
// pointer there); capacity at +0x28; a heap text is freed by the allocator at +0x30 (its vtable +0x70), as the
// screen's update frees it.
constexpr u64 kStrPtr = 0, kStrBuf = 0x10, kStrCap = 0x28, kStrAlloc = 0x30;
constexpr u32 kEnterNameText = 0x6225b;  // "Enter Name" (menu names, 0xc8): the keyboard's title
constexpr char kGuestMovie[] = "/app0/dvdroot_ps4/menu/chrmaketop.gfx";
// The value column's own look (chrmaketop.gfx's value text: centred, 24, #a0a08c, bold) and the cursor in the
// search bar's gold; a hidden cursor keeps its width (the field's dark), so the centred name does not move.
constexpr char16_t kValueOpen[] =
    u"<p align=\"center\"><font size=\"24\" color=\"#a0a08c\" letterSpacing=\"0.000000\" kerning=\"1\"><b>";
constexpr char16_t kValueClose[] = u"</b></font></p>";
constexpr char16_t kCursorOn[] = u"<font color=\"#a0895f\">|</font>";
constexpr char16_t kCursorOff[] = u"<font color=\"#181716\">|</font>";
constexpr std::size_t kDefaultLimit = 16;

using UpdateFn = void(BB_COOP_SYSV*)(u64, u64, float);
UpdateFn g_update_original = nullptr;
bool g_installed = false;
bool g_movie = false;  // the generated movie (NameField, NameKeyboard) is mounted

std::atomic<long long> g_seen_ms{0};       // the Contract screen's last frame
std::atomic<u32> g_ime_length{0};          // the last ImeDialog's most characters
std::atomic<bool> g_typing{false};         // the name's quiet text box is open (any thread)
std::atomic<bool> g_keyboard_wanted{false};// a controller types (any thread)
std::atomic<bool> g_shift{false};          // the next letter in upper case (any thread)
std::atomic<bool> g_shift_pressed{false};  // Shift pressed (its sound: not the case changing by itself)
// The keyboard's cursor (any thread): its view (<< 8: 0 the A-Z grid, 1 QWERTY; kept for the session) and its key.
std::atomic<int> g_key{0};

// Menu thread: what the screen shows.
std::u16string g_before;        // the name as it was when the typing started (a cancel puts it back)
bool g_keyboard_shown = false;
int g_view_shown = -1, g_key_shown = -1, g_shift_shown = -1, g_prompts_mode = 0;
std::string g_typed_before;     // the typed text last frame (an emptied name turns Shift back on)

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
const BbOsk::Layout& Keys() { return BbOsk::NameLayout(); }

std::u16string Utf16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        u32 cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra) cp = c & (0x3f >> extra);
        ++i;
        while (extra-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80)
            cp = cp << 6 | (static_cast<unsigned char>(s[i++]) & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xd800 + (cp >> 10));
            out += char16_t(0xdc00 + (cp & 0x3ff));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}
std::string Utf8(const std::u16string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        u32 c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xc0 | c >> 6), out += char(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += char(0xe0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3f)), out += char(0x80 | (c & 0x3f));
        } else {
            out += char(0xf0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3f)), out += char(0x80 | (c >> 6 & 0x3f));
            out += char(0x80 | (c & 0x3f));
        }
    }
    return out;
}
std::u16string Escaped(const std::u16string& s) {
    std::u16string out;
    for (const char16_t c : s) {
        if (c == u'&') out += u"&amp;";
        else if (c == u'<') out += u"&lt;";
        else if (c == u'>') out += u"&gt;";
        else out += c;
    }
    return out;
}

// The character's name as the game has it (the name getter, as the screen's update calls it).
std::u16string GameName() {
    alignas(16) std::uint8_t str[0x80]{};
    const u64 s = reinterpret_cast<u64>(str);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kNameOf))(s);
    const u64 cap = *reinterpret_cast<const u64*>(s + kStrCap);
    u64 text = *reinterpret_cast<const u64*>(s + kStrPtr);
    if (!text) text = cap >= 8 ? *reinterpret_cast<const u64*>(s + kStrBuf) : s + kStrBuf;
    std::u16string name;
    for (const char16_t* c = reinterpret_cast<const char16_t*>(text); c && *c && name.size() < 64; ++c) name += *c;
    if (cap >= 8) {
        const u64 alloc = *reinterpret_cast<const u64*>(s + kStrAlloc);
        const u64 free_fn = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(alloc) + 0x70);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(free_fn)(alloc, *reinterpret_cast<const u64*>(s + kStrBuf));
    }
    return name;
}

void ShowNoInputName(u64 screen, bool visible) {
    const u64 wrapper = screen + kNoInputName;
    const u64 vtable = *reinterpret_cast<const u64*>(wrapper);
    if (!vtable) return;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(*reinterpret_cast<const u64*>(vtable + 8))(wrapper, visible ? 1 : 0);
}
// The name row's text (written each frame while typing: the screen may write its own). cursor: -1 none, 0 hidden
// (its width kept), 1 shown.
void ShowName(u64 screen, const std::u16string& name, int cursor) {
    const std::u16string html = kValueOpen + Escaped(name) + (cursor > 0 ? kCursorOn : cursor == 0 ? kCursorOff : u"") +
                                kValueClose;
    BbMenuWidgets::SetFieldText(screen + kRoot, g_movie && cursor >= 0 ? kTypingText : kNameField, html.c_str());
}
// The row while typed in (NameField over it, the row's text hidden; its ornament stays under the field) or as the
// game shows it.
bool g_field_shown = false;
void ShowTypingField(u64 screen, bool typing) {
    if (!g_movie || typing == g_field_shown) return;
    g_field_shown = typing;
    const u64 root = screen + kRoot;
    BbMenuWidgets::ShowPart(root, kTypingField, typing);
    BbMenuWidgets::ShowPart(root, kNameField, !typing);
}

// ---- the controller's keyboard (NameKeyboard) ----
std::string KeyPath(int view, int key, const char* part) {
    return "NameKeyboard/" + Keys().views[std::size_t(view)].name + "/Key_" + std::to_string(key) +
           (part ? std::string("/") + part : "");
}
void ShowKeyboard(u64 screen, bool visible) {
    if (visible == g_keyboard_shown) return;
    g_keyboard_shown = visible;
    const u64 root = screen + kRoot;
    if (visible) {  // its title: the game's own "Enter Name"
        const char16_t* title = BbGameMenu::GameText(0xc8, kEnterNameText);
        const std::u16string html =
            std::u16string(u"<p align=\"left\"><font size=\"22\" color=\"#a0895f\" letterSpacing=\"0.000000\" "
                           u"kerning=\"1\"><b>") +
            Escaped(title && *title ? title : u"Enter Name") + u"</b></font></p>";
        BbMenuWidgets::SetFieldText(root, "NameKeyboard/Title", html.c_str());
        g_view_shown = g_key_shown = g_shift_shown = -1;
    }
    if (!visible && g_view_shown >= 0 && g_key_shown >= 0)  // its highlight goes with it
        BbMenuWidgets::ShowPart(root, KeyPath(g_view_shown, g_key_shown, "Hot").c_str(), false);
    BbMenuWidgets::ShowPart(root, "NameKeyboard", visible);
    if (visible) BbMenuWidgets::EnterSound();
    else if (g_typing.load(std::memory_order_acquire)) BbMenuWidgets::CancelSound();  // hidden by a keyboard key
    std::printf("Name entry: the controller's keyboard %s\n", visible ? "shown" : "hidden");
    std::fflush(stdout);
}
// Each letter's and Shift's label in the view: upper case (Alt) while Shift is on.
void ShowShift(u64 root, int view, bool shift) {
    const std::string& letters = Keys().views[std::size_t(view)].letters;
    for (int k = 0; k < Keys().Letters(); ++k) {
        if (!std::isalpha(static_cast<unsigned char>(letters[std::size_t(k)]))) continue;
        BbMenuWidgets::ShowPart(root, KeyPath(view, k, "Label").c_str(), !shift);
        BbMenuWidgets::ShowPart(root, KeyPath(view, k, "Alt").c_str(), shift);
    }
    const int key = Keys().Wide("Shift");
    BbMenuWidgets::ShowPart(root, KeyPath(view, key, "Label").c_str(), !shift);
    BbMenuWidgets::ShowPart(root, KeyPath(view, key, "Alt").c_str(), shift);
}
void DrawKeyboard(u64 screen) {
    const bool want = g_movie && g_typing.load(std::memory_order_acquire) &&
                      g_keyboard_wanted.load(std::memory_order_acquire);
    ShowKeyboard(screen, want);
    if (!want) return;
    const u64 root = screen + kRoot;
    const int packed = g_key.load(std::memory_order_acquire);
    const int view = packed >> 8 & 1, key = packed & 0xff;
    const int shift = g_shift.load(std::memory_order_acquire) ? 1 : 0;
    if (view != g_view_shown) {
        if (g_key_shown >= 0 && g_view_shown >= 0)
            BbMenuWidgets::ShowPart(root, KeyPath(g_view_shown, g_key_shown, "Hot").c_str(), false);
        for (int i = 0; i < 2; ++i)
            BbMenuWidgets::ShowPart(root, ("NameKeyboard/" + Keys().views[std::size_t(i)].name).c_str(), i == view);
        if (g_view_shown >= 0) {
            BbMenuWidgets::SwitchSound();
            std::printf("Name entry: the controller's keyboard: the %s view\n", view ? "QWERTY" : "A-Z grid");
            std::fflush(stdout);
        }
        g_view_shown = view;
        g_key_shown = -1;
        g_shift_shown = -1;
    }
    if (shift != g_shift_shown) {
        if (g_shift_pressed.exchange(false, std::memory_order_acq_rel) && g_shift_shown >= 0) BbMenuWidgets::SwitchSound();
        ShowShift(root, view, shift != 0);
        g_shift_shown = shift;
    }
    if (key == g_key_shown) return;
    if (g_key_shown >= 0) {
        BbMenuWidgets::ShowPart(root, KeyPath(view, g_key_shown, "Hot").c_str(), false);
        BbMenuWidgets::CursorMoveSound();
    }
    BbMenuWidgets::ShowPart(root, KeyPath(view, key, "Hot").c_str(), true);
    g_key_shown = key;
}

// ---- the button prompts while typing (the help line stays the game's: "Add name to Contract Ledger.") ----
u64 g_prompts_screen = 0;
int PromptsMode() {
    if (!g_typing.load(std::memory_order_acquire)) return 0;
    return g_movie && g_keyboard_wanted.load(std::memory_order_acquire) ? 2 : 1;
}
void NamePrompts(u64 screen) {
    if (screen != g_prompts_screen || !g_prompts_mode) return;
    u64 prompts = 0;
    if (!coop::SafeGet(screen + BbMenuWidgets::kPromptsField, &prompts)) return;
    using BbMenuWidgets::kPromptGap;
    using BbMenuWidgets::Prompt;
    const std::u16string row =
        g_prompts_mode == 2
            ? Prompt(u"KG_L_UDLR", u"Move") + kPromptGap + Prompt(u"KG_OK", u"Type") + kPromptGap +
                  Prompt(u"KG_R_L", u"Delete") + kPromptGap + Prompt(u"KG_Cancel", u"Cancel")
            : Prompt(u"KG_OK", u"Done") + kPromptGap + Prompt(u"KG_Cancel", u"Cancel") + kPromptGap +
                  Prompt(u"KG_R_L", u"Delete");
    BbMenuWidgets::SetValueText(prompts, row.c_str());
}
void ShowPrompts(u64 screen) {
    const int mode = PromptsMode();
    if (mode == g_prompts_mode && screen == g_prompts_screen) return;
    g_prompts_mode = mode;
    g_prompts_screen = screen;
    BbMenuWidgets::RefreshPrompts(screen);  // the game's, then ours over them while typing
    std::printf("Name entry: the button prompts: %s\n",
                mode == 2 ? "the controller's keyboard" : mode == 1 ? "typing on the keyboard" : "the screen's");
    std::fflush(stdout);
}

// Each frame of the Contract screen, after the game's own.
BB_COOP_SYSV void Update(u64 screen, u64 arg, float dt) {
    g_update_original(screen, arg, dt);
    g_seen_ms.store(NowMs(), std::memory_order_release);
    if (!g_typing.load(std::memory_order_acquire)) {
        if (g_keyboard_shown || g_prompts_mode || g_field_shown) {  // a typing that ended without a last frame here
            ShowTypingField(screen, false);
            ShowKeyboard(screen, false);
            ShowPrompts(screen);
        }
        return;
    }
    char text[256] = {};
    const int state = bbgpu_text_input_poll(text, sizeof text);
    if (state != 0) g_keyboard_wanted.store(false, std::memory_order_release);
    const std::string typed = text;
    if (typed.empty() && !g_typed_before.empty()) g_shift.store(true, std::memory_order_release);  // a new first letter
    if (state == 0 && typed.size() != g_typed_before.size()) BbMenuWidgets::TypeSound();  // typed or deleted (any way)
    g_typed_before = typed;
    DrawKeyboard(screen);
    if (state == 0) {
        ShowNoInputName(screen, false);
        ShowTypingField(screen, true);
        const bool blink = NowMs() / 500 % 2 == 0;
        ShowName(screen, Utf16(typed), blink ? 1 : 0);
        ShowPrompts(screen);
        return;
    }
    // The typing ended: Done (1) gives the game the name (it keeps it from now on), a cancel (2) keeps the one before.
    g_typing.store(false, std::memory_order_release);
    const std::u16string name = state == 1 ? Utf16(typed) : g_before;
    ShowTypingField(screen, false);
    ShowName(screen, name, -1);
    ShowNoInputName(screen, name.empty());
    if (state == 1) BbMenuWidgets::DecideSound();
    else BbMenuWidgets::CancelSound();
    ShowKeyboard(screen, false);
    ShowPrompts(screen);
    std::printf("Name entry: %s\n", state == 1 ? "the name entered" : "cancelled, the name as it was");
    std::fflush(stdout);
}

bool MountMovie() {
    const char* assets = std::getenv("BB_PC_MENU_ASSETS");
    if (!assets || !*assets) return false;
    std::error_code ec;
    const std::filesystem::path movie = std::filesystem::path(assets) / "dvdroot_ps4/menu/chrmaketop.gfx";
    if (!std::filesystem::is_regular_file(movie, ec)) return false;
    return runtime_file_mount(kGuestMovie, movie.string().c_str()) == 0;
}

}  // namespace

void Install() {
    if (std::getenv("BB_NAME_ENTRY_OFF")) {
        std::printf("Name entry: off (BB_NAME_ENTRY_OFF): the name is typed in the text window\n");
        return;
    }
    static const std::uint8_t kNameOfCode[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                               0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00};
    g_installed = coop::Matches(kNameOf, kNameOfCode, sizeof kNameOfCode) &&
                  coop::ReplacePrologue(kUpdate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                                        reinterpret_cast<const void*>(&Update), reinterpret_cast<void**>(&g_update_original),
                                        "name entry: the Contract screen") &&
                  BbMenuWidgets::AddPromptsHook(&NamePrompts);
    g_movie = g_installed && MountMovie();
    std::printf("Name entry: %s%s\n", g_installed ? "on" : "off (unexpected code): the name is typed in the text window",
                g_installed ? (g_movie ? ", with its typing field and the controller's keyboard"
                                                : ", no typing field or keyboard (no generated chrmaketop.gfx)")
                            : "");
    std::fflush(stdout);
}

void NoteImeLength(std::uint32_t max_length) { g_ime_length.store(max_length, std::memory_order_release); }

bool Begin(std::string* initial, std::size_t* limit) {
    if (!g_installed || NowMs() - g_seen_ms.load(std::memory_order_acquire) > 300) return false;
    g_before = GameName();
    if (initial) *initial = Utf8(g_before);
    const u32 length = g_ime_length.load(std::memory_order_acquire);
    if (limit) *limit = length ? length : kDefaultLimit;
    g_typed_before = Utf8(g_before);
    g_shift.store(g_before.empty(), std::memory_order_release);
    g_shift_pressed.store(false, std::memory_order_release);
    g_key.store(g_key.load(std::memory_order_acquire) & ~0xff, std::memory_order_release);  // its view kept
    g_keyboard_wanted.store(host_input_device() == InputDevice::Pad, std::memory_order_release);
    g_typing.store(true, std::memory_order_release);
    BbMenuWidgets::EnterSound();
    std::printf("Name entry: typing the name (%u characters at most, %s)\n", unsigned(limit ? *limit : 0),
                host_input_device() == InputDevice::Pad ? "controller" : "keyboard");
    std::fflush(stdout);
    return true;
}

bool Typing() { return g_typing.load(std::memory_order_acquire); }

int KeyboardPress(PadKey key) {
    if (key == PadKey::kNone) return 0;
    if (!g_keyboard_wanted.exchange(true, std::memory_order_acq_rel)) {  // the first press shows the keyboard
        g_key.store(g_key.load(std::memory_order_acquire) & ~0xff, std::memory_order_release);
        if (key != PadKey::kDelete && key != PadKey::kDone && key != PadKey::kCancel) return 0;
    }
    const BbOsk::Layout& l = Keys();
    const int packed = g_key.load(std::memory_order_acquire);
    const int view = packed >> 8 & 1, k = packed & 0xff;
    switch (key) {
    case PadKey::kUp:
    case PadKey::kDown:
    case PadKey::kLeft:
    case PadKey::kRight: g_key.store(view << 8 | BbOsk::Moved(l, view, k, key), std::memory_order_release); return 0;
    case PadKey::kToggle: g_key.store((1 - view) << 8 | BbOsk::InView(l, view, 1 - view, k), std::memory_order_release); return 0;
    case PadKey::kPress:
        if (k < l.Letters()) {
            char c = l.views[std::size_t(view)].letters[std::size_t(k)];
            if (g_shift.exchange(false, std::memory_order_acq_rel)) c = char(std::toupper(static_cast<unsigned char>(c)));
            return c;
        }
        if (k == l.Wide("Shift")) {
            g_shift.store(!g_shift.load(std::memory_order_acquire), std::memory_order_release);
            g_shift_pressed.store(true, std::memory_order_release);
            return 0;
        }
        if (k == l.Wide("Space")) return ' ';
        return k == l.Wide("Delete") ? BbOsk::kTypedDelete : BbOsk::kTypedDone;
    case PadKey::kDelete: return BbOsk::kTypedDelete;
    case PadKey::kDone: return BbOsk::kTypedDone;
    case PadKey::kCancel: return BbOsk::kTypedCancel;
    default: return 0;
    }
}

void KeyboardKeyTyped() { g_keyboard_wanted.store(false, std::memory_order_release); }

}  // namespace BbNameEntry
