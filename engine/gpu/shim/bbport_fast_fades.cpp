// SPDX-License-Identifier: GPL-3.0-or-later
// Faster black fades around a map load (visy 0.8.1 loadux081, the fades half of his bbport_fast_loading.cpp; his
// unpaced loading screen is not taken: ours paces loading screens in engine/loading.cpp).
#include "bbport_fast_fades.h"

#include "bbport_load_policy.h"
#include "bbport_settings.h"
#include "party/coop_hooks.h"
#include "party/game_state.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbFastFades {

namespace {

using u64 = std::uint64_t;

constexpr u64 kFlipperUpdate = 0x2034770, kFlipperCall = 0x2018e22;  // SprjFlipper::Update, the main loop's call of it
constexpr u64 kSprjFadePtr = 0x5540068;  // SprjFade*; the map load's black fades are its objects at +0x20 and +0x38

template <class T>
T Read(u64 address, T fallback) {
    T v{};
    return address && coop::SafeGet(address, &v) ? v : fallback;
}

using FlipperUpdateFn = void(BB_COOP_SYSV*)(u64);

// A fade object: floats at +0xc.. (current RGBA, target RGBA), +0x38 the remaining time, counting down from its 0.5 s by
// the game's own step every frame while the alpha follows it (research/loadux081.md in visy's tree). At the start of
// each frame the countdown is taken down once more by the step the game made last frame, so the fade runs twice as
// fast at any frame rate. Black fades only, never during a cutscene.
float g_prev[2] = {0, 0};  // game thread only

void FadesFrame() {
    const u64 at = coop::Guest(kSprjFadePtr);
    const u64 fade = at ? Read<u64>(at, 0) : 0;
    if (!fade) return;
    const bool remo = coop::CutscenePlaying();
    int i = 0;
    for (const u64 slot : {u64(0x20), u64(0x38)}) {
        float& prev = g_prev[i++];
        const u64 obj = Read<u64>(fade + slot, 0);
        if (!obj) continue;
        float st = Read<float>(obj + 0x38, 0.f);
        if (!(st >= 0.f && st < 100.f)) st = 0.f;
        bool black = true;
        for (const u64 c : {u64(0xc), u64(0x10), u64(0x14), u64(0x1c), u64(0x20), u64(0x24)})
            black = black && Read<float>(obj + c, 1.f) == 0.f;
        if (!remo && black) {
            const float extra = bbport::loadpolicy::FadeExtra(prev, st, 2.0f, 0.25f);
            if (extra > 0.f) {
                st -= extra;
                coop::SafeWrite(obj + 0x38, &st, 4);
            }
        }
        prev = st;
    }
}

BB_COOP_SYSV void FlipperUpdateHook(u64 flipper) {
    FadesFrame();
    reinterpret_cast<FlipperUpdateFn>(coop::Guest(kFlipperUpdate))(flipper);
}

}  // namespace

void Install() {
    const char* e = std::getenv("BB_FAST_FADES");
    const bool on = e && *e ? std::strcmp(e, "0") != 0 : BbSettings::Get().fast_fades.load();
    if (!on) {
        std::printf("Fast fades: off\n");
        return;
    }
    // The call site in the main loop, not Update's entry: the engine's frame pacer hooks that entry already.
    const bool hooked = coop::HookCallSite(kFlipperCall, kFlipperUpdate, reinterpret_cast<const void*>(&FlipperUpdateHook),
                                           "fast fades: SprjFlipper update call");
    std::printf("Fast fades: %s\n", hooked ? "on (map load fades twice as fast)" : "NOT hooked (the game's executable differs): off");
}

}  // namespace BbFastFades
