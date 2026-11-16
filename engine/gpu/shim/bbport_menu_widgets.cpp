// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_menu_widgets.h"

#include "party/coop_hooks.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace BbMenuWidgets {
namespace {

using u32 = std::uint32_t;
constexpr u64 kChildFetch = 0x1adb220;
using FetchFn = u64(BB_COOP_SYSV*)(u64 out, u64 parent, const char* path);

// The menu's next sound: {value, priority} at 0x557fc60, its label at 0x557fc68; a sound is set only over one of no
// higher priority. The cursor's move: {0, 0} and the label at 0x49344e4 ("cursor move" in Japanese); a decision, as a
// menu deciding: {0x64, 1} and the label at 0x493451b ("decide", as 0x1b26e05 sets it); the top menu entering one
// of its menus: {0x7da, 9} and the label at 0x4933506 ("top menu category select", as 0x1af47b9 sets it); "cancel"
// (0xc8, priority 1: 0x1b26b5f), "display switch" (0x3fc, priority 5: Display's action, 0x1ba678c) and "slider
// adjust" (0x190, priority 0: a slider's step, 0x1b30c59).
constexpr u64 kMenuSound = 0x557fc60, kMenuSoundLabel = 0x557fc68, kCursorMoveLabel = 0x49344e4;
constexpr u64 kDecideLabel = 0x493451b, kEnterMenuLabel = 0x4933506;
constexpr u64 kCancelLabel = 0x493450b, kSwitchLabel = 0x49340d5, kSliderLabel = 0x493387b;
void MenuSound(std::int32_t value, std::int32_t priority, u64 label_at) {
    auto* sound = reinterpret_cast<std::int32_t*>(coop::Guest(kMenuSound));
    if (sound[1] > priority) return;
    sound[0] = value;
    sound[1] = priority;
    const u64 label = coop::Guest(label_at);
    std::memcpy(reinterpret_cast<void*>(coop::Guest(kMenuSoundLabel)), &label, sizeof label);
}

// The help line and the button prompts: the game sets the help line with 0x1ad6210(screen, text) and redraws the
// prompts with 0x1ad6450(screen) (the "<font color='#A5A58C'>%s(U+FF1A)%s</font>" runs, U+3000 between, into the
// field at screen + 0x2a8).
constexpr u64 kSetHelp = 0x1ad6210, kRefreshPrompts = 0x1ad6450;
using SetHelpFn = void(BB_COOP_SYSV*)(u64, u64);
using RefreshPromptsFn = void(BB_COOP_SYSV*)(u64);
SetHelpFn g_set_help_original = nullptr;
RefreshPromptsFn g_refresh_prompts_original = nullptr;
std::mutex g_hooks_mu;
std::vector<PromptsHook> g_hooks;
bool g_installed = false, g_install_ok = false;
void RunHooks(u64 screen) {
    std::vector<PromptsHook> hooks;
    {
        std::lock_guard<std::mutex> lk(g_hooks_mu);
        hooks = g_hooks;
    }
    for (const PromptsHook hook : hooks) hook(screen);
}
BB_COOP_SYSV void SetHelp(u64 screen, u64 text) {
    g_set_help_original(screen, text);
    RunHooks(screen);
}
BB_COOP_SYSV void Refresh(u64 screen) {
    g_refresh_prompts_original(screen);
    RunHooks(screen);
}

}  // namespace

bool Wrapper::Fetch(u64 parent, const char* path) {
    reinterpret_cast<FetchFn>(coop::Guest(kChildFetch))(at(), parent, path);
    return (Type() & 0x8f) == 10;
}
void Wrapper::SetVisible(bool visible) {
    const u64 vtable = *reinterpret_cast<const u64*>(at());
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(*reinterpret_cast<const u64*>(vtable + 8))(at(), visible ? 1 : 0);
}
Wrapper::~Wrapper() {
    if (!(Type() & 0x40)) return;
    const u64 iface = *reinterpret_cast<const u64*>(at() + 0x40);
    const u64 release = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x18);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64)>(release)(iface, at() + 0x30,
                                                                   *reinterpret_cast<const u64*>(at() + 0x50));
}

void SetValueText(u64 wrapper, const char16_t* text) {
    if (!wrapper || !text) return;
    alignas(16) std::uint8_t value[0x40]{};
    const u64 v = reinterpret_cast<u64>(value);
    const u64 vtable = *reinterpret_cast<const u64*>(wrapper);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(*reinterpret_cast<const u64*>(vtable))(v, wrapper);
    const u64 iface = *reinterpret_cast<const u64*>(v + 0x18);
    const u32 type = *reinterpret_cast<const u32*>(v + 0x20);
    const u64 data = *reinterpret_cast<const u64*>(v + 0x28);
    if (iface && (type & 0x8f)) {
        const u64 set = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x138);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64, u32)>(set)(iface, data, reinterpret_cast<u64>(text), 1);
    }
    if ((type & 0x40) && iface) {
        const u64 release = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x18);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64)>(release)(iface, v + 8, data);
    }
}
void SetFieldText(u64 root, const char* path, const char16_t* text) {
    Wrapper field;
    if (text && field.Fetch(root, path)) SetValueText(field.at(), text);
}
bool ShowPart(u64 root, const char* path, bool visible) {
    Wrapper part;
    if (!part.Fetch(root, path)) return false;
    part.SetVisible(visible);
    return true;
}

void CursorMoveSound() { MenuSound(0, 0, kCursorMoveLabel); }
void DecideSound() { MenuSound(0x64, 1, kDecideLabel); }
void EnterSound() { MenuSound(0x7da, 9, kEnterMenuLabel); }
void CancelSound() { MenuSound(0xc8, 1, kCancelLabel); }
void SwitchSound() { MenuSound(0x3fc, 5, kSwitchLabel); }
void TypeSound() { MenuSound(0x190, 0, kSliderLabel); }

std::u16string Prompt(const char16_t* image, const char16_t* label) {
    return std::u16string(u"<font color='#A5A58C'><img src='") + image + u"' vspace='-8'>\xff1a" + label + u"</font>";
}

bool AddPromptsHook(PromptsHook hook) {
    std::lock_guard<std::mutex> lk(g_hooks_mu);
    if (!g_installed) {
        g_installed = true;
        g_install_ok = coop::ReplacePrologue(kSetHelp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53},
                                             reinterpret_cast<const void*>(&SetHelp),
                                             reinterpret_cast<void**>(&g_set_help_original),
                                             "menu widgets: our help lines") &&
                       coop::ReplacePrologue(kRefreshPrompts, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56},
                                             reinterpret_cast<const void*>(&Refresh),
                                             reinterpret_cast<void**>(&g_refresh_prompts_original),
                                             "menu widgets: our button prompts");
    }
    if (g_install_ok && hook) g_hooks.push_back(hook);
    return g_install_ok;
}

void RefreshPrompts(u64 screen) {
    reinterpret_cast<RefreshPromptsFn>(coop::Guest(kRefreshPrompts))(screen);  // through the hook once installed
}

}  // namespace BbMenuWidgets
