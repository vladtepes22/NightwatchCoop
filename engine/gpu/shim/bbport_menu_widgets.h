// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch: the game's menu pieces that our own screens use (the Chalice Glyph Browser, the Contract screen's name
// entry): a movie part through its display object wrapper, a text field's text, the menu's sounds and the button
// prompt row. 1.09's addresses; the prompt hooks are byte-checked when installed.
#pragma once

#include <cstdint>
#include <string>

namespace BbMenuWidgets {

using u64 = std::uint64_t;

/// A display object wrapper from the game's child fetch, 0x1adb220(out, parent wrapper, path): its vtable's +8 sets
/// its visibility; its value at +0x30 (interface +0x40, type +0x48, data +0x50; type & 0x8f == 10 a display object,
/// & 0x40 counted, released by the interface's +0x18).
struct Wrapper {
    alignas(16) std::uint8_t bytes[0x100]{};
    u64 at() { return reinterpret_cast<u64>(bytes); }
    /// The part at `path` ("A/B/C") under the wrapper at `parent`: whether it is a display object.
    bool Fetch(u64 parent, const char* path);
    std::uint32_t Type() { return *reinterpret_cast<const std::uint32_t*>(at() + 0x48); }
    void SetVisible(bool visible);
    Wrapper() = default;
    Wrapper(const Wrapper&) = delete;
    Wrapper& operator=(const Wrapper&) = delete;
    ~Wrapper();
};

/// A text field's text (HTML) through its wrapper object (as the game sets its help line and button prompts).
void SetValueText(u64 wrapper, const char16_t* text);
/// The text of the field at `path` under the wrapper at `root`.
void SetFieldText(u64 root, const char* path, const char16_t* text);
/// The part at `path` under the wrapper at `root` shown or hidden; whether it was found.
bool ShowPart(u64 root, const char* path, bool visible);
/// A sprite part goes to `frame` (1-based) and stops there (as the game's 0x1ad9ab0: the interface's +0x158).
void GotoFrame(u64 wrapper, int frame);
bool GotoPartFrame(u64 root, const char* path, int frame);

/// The menu's sounds, as the game's own menus set them (one sound a frame: one of higher priority wins).
void CursorMoveSound();  ///< the cursor's move
void DecideSound();      ///< a menu's decision (the typed text confirmed)
void EnterSound();       ///< the top menu entering a menu (typing starts, the keyboard shown)
void CancelSound();      ///< a menu's cancel (the keyboard hidden)
void SwitchSound();      ///< Display's switch (the keyboard's other view)
void TypeSound();        ///< a slider's step (a character typed or deleted: not the cursor's move)

/// One button prompt in the key guide's own markup: `image` a key guide image (KG_OK, KG_Cancel ...; the engine's
/// key prompts draw it for the device in use), then the full-width colon and `label`.
std::u16string Prompt(const char16_t* image, const char16_t* label);
/// The prompts' separator (an ideographic space, as the game's own row).
constexpr char16_t kPromptGap[] = u"\x3000";

/// A screen's help line and button prompt fields (an item / command menu screen: 0x1ad4c20's).
constexpr u64 kHelpField = 0x1f8, kPromptsField = 0x2a8;
/// Called after the game sets a screen's help line (0x1ad6210) or redraws its prompts (0x1ad6450): a screen of ours
/// writes its own over the game's there.
using PromptsHook = void (*)(u64 screen);
/// Hooks the two (once; later calls only report it) and adds `hook`; false when the game's code is not 1.09's.
bool AddPromptsHook(PromptsHook hook);
/// The game redraws `screen`'s prompts (the hooks then run).
void RefreshPrompts(u64 screen);

}  // namespace BbMenuWidgets
