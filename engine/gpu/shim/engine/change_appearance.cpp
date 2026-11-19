// SPDX-License-Identifier: GPL-3.0-or-later
#include "change_appearance.h"
#include "engine_state.h"
#include "bbport_settings.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace coop::b2 {
void NoteOwnAppearanceChanged();  // party/party_b2.h: re-send the own record, the peers rebuild this hunter
}

namespace BbEngine::ChangeAppearance {
namespace {
constexpr std::uint64_t kListStepType = 7;
constexpr char16_t kListStepName[] = u"ChrMakeCommandList";

constexpr std::size_t kOpenWindow = 0x1ad8150;
constexpr std::size_t kCloseWindow = 0x1ad8e60;
constexpr std::size_t kMenuHeap = 0x5540418;
constexpr std::size_t kHeapAllocate = 0x58, kHeapFree = 0x70;

constexpr std::size_t kDescriptorSize = 0xc0;
constexpr std::size_t kDescriptorVtable = 0x5336b30, kDescriptorVtable2 = 0x5336b70, kDescriptorListVtable = 0x53310d0;
constexpr std::size_t kFactoryVtable = 0x533ee10, kFactory = 0x1b08010;
constexpr std::size_t kWindowName = 0x4998486;
constexpr std::uint64_t kWindowType = 6;
constexpr std::size_t kController = 0x60, kControllerUpdate = 0x18;

// The mirror: the prepared Dream layout's part "ダミーCHR_再キャラメイク" (the game's unused stand-in, made an
// enemy part), entity 2100202, in the Hunter's Dream (block 0x15000000). The game's own calls turn it off and on:
// the chr by entity id (0x13c97a0, as party_balance's ChrByEntity) and the chr's disable (0x18c6390 (chr, on): it
// compares chr +0x18 -> +0x20 bit 0, the disabled bit, first).
constexpr std::uint32_t kDreamBlock = 0x15000000;
constexpr int kMirrorEntity = 2100202;
constexpr std::size_t kChrByEntity = 0x13c97a0, kChrDisable = 0x18c6390;
constexpr std::uint8_t kFrameEntry[] = {0x55, 0x48, 0x89, 0xe5};
constexpr unsigned kMirrorEvery = 15;  // frames between looks while nothing changes

std::uint64_t g_image = 0;
int g_watch = -1;
bool g_editor = false;
std::uint64_t g_step = 0;
std::uint64_t g_title_step = 0;
std::atomic<bool> g_preview{false};
std::uint64_t g_descriptor = 0;
std::uint8_t g_update_flag = 1;
unsigned g_opened = 0;
bool g_mirror_code = false;   // both calls are 1.09's
bool g_mirror_off = false;    // this code turned the mirror off (only then does it turn it on again)
bool g_mirror_want = true;
unsigned g_mirror_ticks = 0;
const bool g_hold_title = [] {
    const char* v = std::getenv("BB_HOLD_TITLE_STEP");
    return v && std::strcmp(v, "1") == 0;
}();

std::uint64_t Image(std::size_t at) { return g_image + at; }
std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return host_read_safe(reinterpret_cast<const void*>(at), &v, sizeof v) ? v : 0;
}
void Put(std::uint64_t at, std::uint64_t v) { std::memcpy(reinterpret_cast<void*>(at), &v, sizeof v); }
std::uint64_t Virtual(std::uint64_t object, std::size_t slot) { return Follow(Follow(object) + slot); }

std::uint64_t Allocate(std::size_t size) {
    const std::uint64_t heap = Follow(Image(kMenuHeap));
    return heap ? std::uint64_t(Call(Virtual(heap, kHeapAllocate), heap, size, 0x10)) : 0;
}
void Free(std::uint64_t p) {
    const std::uint64_t heap = Follow(Image(kMenuHeap));
    if (heap && p) Call(Virtual(heap, kHeapFree), heap, p);
}

bool OpenWindow() {
    const std::uint64_t d = Allocate(kDescriptorSize);
    if (!d) return false;
    std::memset(reinterpret_cast<void*>(d), 0, kDescriptorSize);
    Put(d + 0x00, Image(kDescriptorVtable));
    Put(d + 0x08, Image(kDescriptorVtable2));
    Put(d + 0x18, kWindowType);
    Put(d + 0x20, Image(kWindowName));
    Put(d + 0x30, Image(kFactoryVtable));
    Put(d + 0x38, Image(kFactory));
    Put(d + 0x50, d + 0x30);
    Put(d + 0x68, Image(kDescriptorListVtable));
    Call(Image(kOpenWindow), d);
    if (!Follow(d + kController)) {
        Call(Image(kCloseWindow), d);
        Free(d);
        return false;
    }
    g_descriptor = d;
    return true;
}

void CloseWindow() {
    Call(Image(kCloseWindow), g_descriptor);
    Free(g_descriptor);
    g_descriptor = 0;
}
}

bool Watch(std::uint8_t* image) {
    g_image = reinterpret_cast<std::uint64_t>(image);
    g_watch = menu_steps_watch(kListStepType, kListStepName);
    return g_watch >= 0;
}

void EnableEditor() {
    g_editor = true;
    g_mirror_code = !std::memcmp(reinterpret_cast<const void*>(Image(kChrByEntity)), kFrameEntry, sizeof kFrameEntry) &&
                    !std::memcmp(reinterpret_cast<const void*>(Image(kChrDisable)), kFrameEntry, sizeof kFrameEntry);
    if (!g_mirror_code) host_log("change appearance: the mirror's switch is off (the chr calls are not 1.09's)");
}

void MirrorTick() {
    if (!g_editor || !g_mirror_code) return;
    const bool want = BbSettings::Get().change_appearance;
    const bool changed = want != g_mirror_want;
    g_mirror_want = want;
    if (!changed && g_mirror_ticks++ % kMirrorEvery) return;
    std::uint32_t block = 0;
    if (!world_player_block(&block) || block != kDreamBlock) {
        g_mirror_off = false;  // a load of the Dream makes the mirror anew
        return;
    }
    const std::uint64_t chr = Call(Image(kChrByEntity), std::uint64_t(kMirrorEntity), std::uint64_t(kMirrorEntity), 0);
    if (!chr) return;
    std::uint8_t bits = 0;
    const bool off = host_read_safe(reinterpret_cast<const void*>(Follow(chr + 0x18) + 0x20), &bits, 1) && (bits & 1);
    if (!want && !off) {
        Call(Image(kChrDisable), chr, 1);
        g_mirror_off = true;
        host_log("change appearance: the Dream's mirror turned off (Hunter's Dream Mirror off)");
    } else if (want && off && g_mirror_off) {
        Call(Image(kChrDisable), chr, 0);
        g_mirror_off = false;
        host_log("change appearance: the Dream's mirror turned on again");
    }
}

bool PreviewShown() { return g_preview.load(std::memory_order_relaxed); }

void Tick() {
    if (g_watch < 0) return;
    const bool world = world_player_block(nullptr);
    if (const std::uint64_t step = menu_steps_take(g_watch); menu_step_is(step, kListStepType, kListStepName)) {
        if (!world) {
            if (!g_title_step) {
                if (g_hold_title) menu_step_hold(step);
                g_title_step = step;
            }
        } else if (g_editor && BbSettings::Get().change_appearance && !g_step && !g_descriptor && OpenWindow()) {
            menu_step_hold(step);
            g_step = step;
            if (g_opened++ < 4) host_log("change appearance: the editor's window opened (ChrMake_BG, the hunter's preview)");
        }
    }
    if (g_title_step && g_hold_title && (world || *menu_step_count(g_title_step) == 1)) {
        menu_step_release(g_title_step);
        g_title_step = 0;
    }
    if (g_title_step && !g_hold_title && (world || !menu_step_is(g_title_step, kListStepType, kListStepName)))
        g_title_step = 0;
    if (g_descriptor && !world) {
        CloseWindow();
        host_log("change appearance: the editor's window closed on leaving the world");
    }
    if (g_descriptor) {
        if (const std::uint64_t controller = Follow(g_descriptor + kController))
            Call(Virtual(controller, kControllerUpdate), controller, &g_update_flag);
    }
    if (g_step && (!g_descriptor || *menu_step_count(g_step) == 1)) {
        if (g_descriptor) CloseWindow();
        menu_step_release(g_step);
        g_step = 0;
        // visy 0.8.1 mirror_frame: the editor closed in the world: the party sees the new look at once (look sync's
        // poll would take up to 2 s).
        coop::b2::NoteOwnAppearanceChanged();
    }
    g_preview.store(g_step || g_title_step, std::memory_order_relaxed);
}
}
