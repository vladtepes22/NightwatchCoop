// SPDX-License-Identifier: GPL-2.0-or-later
// Live FPS clock (live_clock.h). Ported from visy's bbport_frame_rate.cpp (0.7.8: 18907808, 13a35bbb),
// itself Mrsuss60/bloodborne_pc_windows_port a7cbfae6, 83a168ee (GPL-2.0-or-later); the detours and
// the stub emitter are deadinside's (Supermedo/bloodborne_pc 72e5c8ce, GPL-2.0-or-later).
//
// The "60 FPS++ (Deltatime)" patch (patches/Bloodborne.xml; chosen by the start, start/patches.cpp)
// leaves the main loop's step block at 0x2018e3d in a known form and makes the cloth, animation and
// platform code read the pacer's measured frame time ([pacer + 0x264]) and a global step (image +
// 0x535a908). LiveClockTick replaces that block: it sets the pacer's frame time target and the measured
// time from the target rate (LiveClockStep) and returns the step. The pacer's mode is held at one
// vblank with the live target (HookPacerMode / HookPacerLate), and its four frame time constants are
// lowered to 1/240 s so 90 / 120 / uncapped can exceed 60. The native frame rate (frame_rate.cpp)
// stands aside while this runs; the party code reads Steps / GameSeconds / LastDt from it as before.
#include "live_clock.h"
#include "live_clock_rules.h"
#include "engine_hooks.h"
#include "bbport_settings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" void* runtime_low_map(size_t size, int prot); // src/runtime_memory.c

namespace BbEngine {
void host_log(const char* format, ...);
}

namespace BbEngine::LiveClock {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

/// A stub's bytes, built in place (deadinside's emitter, visy's bbport_guest_code.h).
struct Emitter {
    std::array<u8, 512> code{};
    std::size_t size = 0;
    bool overflow = false;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            if (size < code.size()) code[size] = b;
            ++size;
        }
        overflow = overflow || size > code.size();
    }
    void U32(std::uint32_t v) {
        u8 b[4];
        std::memcpy(b, &v, 4);
        Bytes({b[0], b[1], b[2], b[3]});
    }
    void U64(u64 v) {
        U32(std::uint32_t(v));
        U32(std::uint32_t(v >> 32));
    }
    /// rax rcx rdx rsi rdi r8-r11 kept, r12 holds the unaligned stack pointer, the stack aligned.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
    /// xmm0..15 below the aligned stack (a sysv function may change any of them).
    void SaveXmm() {
        Bytes({0x48, 0x81, 0xec});
        U32(256);
        for (u8 i = 0; i < 16; ++i) {
            Bytes({0xf3});
            if (i >= 8) Bytes({0x44});
            Bytes({0x0f, 0x7f, u8(0x84 | (i & 7) << 3), 0x24});
            U32(16u * i);
        }
    }
    void RestoreXmm() {
        for (u8 i = 0; i < 16; ++i) {
            Bytes({0xf3});
            if (i >= 8) Bytes({0x44});
            Bytes({0x0f, 0x6f, u8(0x84 | (i & 7) << 3), 0x24});
            U32(16u * i);
        }
        Bytes({0x48, 0x81, 0xc4});
        U32(256);
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8});
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0});
    }
    void JmpAbs(u64 target) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(target);
    }
};

/// The block at 0x2018e3d as the Deltatime patch leaves it (46 bytes up to 0x2018e6b): movss xmm0,
/// [rbx+0x264]; maxss xmm0,[rbx+0x18]; minss xmm0,[1/30]; movss [rbp-0x38],xmm0; movss [0x535a908],xmm0;
/// lea rax; 5 nops. Anything else (another eboot, another patch of the same code): not installed.
constexpr u8 kBlock[46] = {0xf3, 0x0f, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x5f, 0x43, 0x18, 0xf3, 0x0f, 0x5d,
                           0x05, 0x12, 0xd5, 0x90, 0x02, 0xf3, 0x0f, 0x11, 0x45, 0xc8, 0xf3, 0x0f, 0x11, 0x05, 0xa9, 0x1a,
                           0x34, 0x03, 0x48, 0x8d, 0x05, 0xaa, 0x76, 0x52, 0x03, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr u8 kResume[5] = {0x48, 0x8d, 0x75, 0xc0, 0xe8}; // 0x2018e6b: lea rsi,[rbp-0x40]; call
constexpr u64 kBlockSite = 0x2018e3d, kResumeSite = 0x2018e6b, kGlobalDt = 0x535a908;
constexpr std::size_t kStubPage = 4096;

u64 g_base = 0, g_size = 0;
u8* g_stubs = nullptr;
std::size_t g_stub_used = 0;
alignas(16) float g_shortest = 1.0f / LiveCeiling; ///< the pacer's frame time target (HookPacerMode reads it)
LiveClockState g_state;
std::atomic<bool> g_active{false};
std::atomic<int> g_logged_target{-1};
std::atomic<u64> g_steps{0};
std::atomic<double> g_game_seconds{0.0};
std::atomic<float> g_last_dt{0.0f};

/// A page within rel32 reach of the image: the next block of the runtime's low area, as the game
/// menu's stubs do (bbport_game_menu.cpp), else a free spot 16 MiB steps around the image.
u8* MapNear(std::size_t size) {
#ifdef _WIN32
    if (void* p = runtime_low_map(size, 3)) {
        const u64 at = reinterpret_cast<u64>(p);
        if (at >= g_base && at + size <= g_base + (2ull << 30) - (1ull << 20)) return static_cast<u8*>(p);
    }
    for (u64 k = 1; k <= 64; ++k)
        for (const u64 hint : {g_base - k * (16ull << 20), g_base + g_size + k * (16ull << 20)})
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(hint & ~u64(0xffff)), size, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE))
                return static_cast<u8*>(p);
#else
    (void)size;
#endif
    return nullptr;
}

void StubsWritable(bool writable) {
#ifdef _WIN32
    DWORD unused = 0;
    VirtualProtect(g_stubs, kStubPage, writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ, &unused);
    if (!writable) FlushInstructionCache(GetCurrentProcess(), g_stubs, kStubPage);
#else
    (void)writable;
#endif
}

/// `e` into the stub page; a rel32 jump to it at `site` over `length` bytes (the rest int3) if the
/// site holds `expected`.
bool Detour(u64 site, const u8* expected, std::size_t length, const Emitter& e) {
    u8* code = reinterpret_cast<u8*>(g_base + site);
    if (length < 5 || length > 64 || std::memcmp(code, expected, length) != 0 || e.overflow ||
        g_stub_used + e.size > kStubPage)
        return false;
    u8* stub = g_stubs + g_stub_used;
    const std::int64_t rel = std::int64_t(reinterpret_cast<u64>(stub)) - std::int64_t(g_base + site + 5);
    if (rel != std::int32_t(rel)) return false;
    std::memcpy(stub, e.code.data(), e.size);
    g_stub_used += (e.size + 15) & ~std::size_t(15);
    const std::int32_t rel32 = std::int32_t(rel);
    u8 jump[64];
    jump[0] = 0xe9;
    std::memcpy(jump + 1, &rel32, 4);
    for (std::size_t i = 5; i < length; ++i) jump[i] = 0xcc;
    return WriteCode(code, jump, length);
}

/// The pacer's mode after the game chose it (0x203493d, `lea r15, [r12 + 0x268]`; r12 the pacer): one
/// vblank (+0x10), no flip skipping (+0x14), the live target as the frame time (+0x18), no vblank
/// counts to wait for (+0x268, +0x26c).
bool HookPacerMode() {
    constexpr u8 code[] = {0x4d, 0x8d, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00};
    Emitter e;
    e.Bytes({0x41, 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00}); // mov dword [r12 + 0x10], 1
    e.Bytes({0x41, 0xc6, 0x44, 0x24, 0x14, 0x00});                   // mov byte [r12 + 0x14], 0
    e.Bytes({0x50, 0x48, 0xb8});                                       // push rax; movabs rax, &g_shortest
    e.U64(reinterpret_cast<u64>(&g_shortest));
    e.Bytes({0x8b, 0x00});                                             // mov eax, [rax]
    e.Bytes({0x41, 0x89, 0x44, 0x24, 0x18, 0x58});                     // mov [r12 + 0x18], eax; pop rax
    e.Bytes({0x49, 0xc7, 0x84, 0x24, 0x68, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov qword [r12 + 0x268], 0
    for (const u8 b : code) e.Bytes({b});
    e.JmpAbs(g_base + 0x203493d + sizeof(code));
    return Detour(0x203493d, code, sizeof(code), e);
}

/// The pacer keeps its one vblank when a frame came late (0x2034d96: `test dl, dl; je; mov eax,
/// [r12 + 0x10]`, then eax is stored back: 0 when late).
bool HookPacerLate() {
    constexpr u8 code[] = {0x84, 0xd2, 0x74, 0x05, 0x41, 0x8b, 0x44, 0x24, 0x10};
    // The Deltatime patch already NOPs that je (0x2034d98): the vblank count is always kept.
    constexpr u8 patched[] = {0x84, 0xd2, 0x90, 0x90, 0x41, 0x8b, 0x44, 0x24, 0x10};
    if (std::memcmp(reinterpret_cast<const u8*>(g_base + 0x2034d96), patched, sizeof patched) == 0) return true;
    Emitter e;
    e.Bytes({0x84, 0xd2});                   // test dl, dl
    e.Bytes({0x41, 0x8b, 0x44, 0x24, 0x10}); // mov eax, [r12 + 0x10]
    e.JmpAbs(g_base + 0x2034d9f);
    return Detour(0x2034d96, code, sizeof(code), e);
}

/// The main loop's step (rdi: the pacer; rsi: where the step goes, [rbp - 0x38]).
__attribute__((sysv_abi)) void LiveClockTick(u8* pacer, float* out) {
    float measured;
    std::memcpy(&measured, pacer + 0x264, 4);
    auto& s = BbSettings::Get();
    const int target = s.target_fps.load(std::memory_order_relaxed);
    const int max_dt_hz = s.max_dt_hz.load(std::memory_order_relaxed);
    float interval = Frame30;
    const float dt = LiveClockStep(g_state, measured, target, max_dt_hz, interval);
    std::memcpy(pacer + 0x18, &interval, 4);
    g_shortest = interval;
    // The patch's code caves sample the pacer's measured time and the global step: both get the step.
    std::memcpy(pacer + 0x264, &dt, 4);
    std::memcpy(reinterpret_cast<u8*>(g_base + kGlobalDt), &dt, 4);
    *out = dt;
    g_last_dt.store(dt, std::memory_order_relaxed);
    g_game_seconds.store(g_game_seconds.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
    g_steps.fetch_add(1, std::memory_order_relaxed);
    if (g_logged_target.exchange(target, std::memory_order_relaxed) != target)
        host_log("Live clock: target %d FPS%s, step %.5f s (max 1/%d s)", target > 0 ? target : LiveCeiling,
                 target > 0 ? "" : " (uncapped)", double(dt), max_dt_hz);
}

bool InstallLive() {
    const u8* image = reinterpret_cast<const u8*>(g_base);
    if (g_size <= 0x2035000 || std::memcmp(image + kBlockSite, kBlock, sizeof kBlock) != 0 ||
        std::memcmp(image + kResumeSite, kResume, sizeof kResume) != 0) {
        host_log("Live clock: the \"60 FPS++ (Deltatime)\" step block is not in place: the patch's static 60 FPS");
        return false;
    }
    Emitter e;
    e.Bytes({0x9c}); // pushfq
    e.SaveVolatile();
    e.SaveXmm();
    e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx (the pacer)
    e.Bytes({0x48, 0x8d, 0x75, 0xc8}); // lea rsi, [rbp - 0x38]
    e.CallAbs(reinterpret_cast<const void*>(&LiveClockTick));
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0x9d}); // popfq
    e.JmpAbs(g_base + kResumeSite);
    if (!Detour(kBlockSite, kBlock, sizeof kBlock, e)) {
        host_log("Live clock: the step block could not be replaced: the patch's static 60 FPS");
        return false;
    }
    // The game's own pacer waits for the target (without this, 107 steps a second at a target of 60).
    const bool pacer = HookPacerMode() && HookPacerLate();
    // The pacer's frame time in its four mode setups: mov dword [r12 + 0x18], 1/60 (the patch) -> 1/240.
    constexpr std::uint32_t kWas = 0x3c888889, kNow = 0x3b888889;
    int lowered = 0;
    for (const u64 site : {0x2034845ull, 0x203485full, 0x2034883ull, 0x20348b8ull}) {
        std::uint32_t value;
        std::memcpy(&value, image + site, 4);
        if (value == kNow || (value == kWas && WriteCode(reinterpret_cast<u8*>(g_base + site), &kNow, 4))) ++lowered;
    }
    host_log("Live clock: on (Mrsuss60's clock via visy 0.7.8): step block hooked, pacer mode hook %s, %d of 4 pacer "
             "constants at 1/240 s, target %d FPS (0 = uncapped), step at most 1/%d s",
             pacer ? "on" : "NOT installed (the vblank holds the rate)", lowered, BbSettings::Get().target_fps.load(),
             BbSettings::Get().max_dt_hz.load());
    return true;
}
} // namespace

bool Install(std::uint8_t* image, std::size_t size) {
    const char* active = std::getenv("BB_LIVE_FPS_ACTIVE");
    if (!active || std::strcmp(active, "1") != 0 || !image) return false;
    g_base = reinterpret_cast<u64>(image);
    g_size = size;
    // The launcher's rate is the starting target; the game menu changes it live.
    const char* fps = std::getenv("BB_FPS");
    if (fps && *fps) BbSettings::Get().target_fps = LiveTargetFromRate(TargetRate(fps, std::getenv("BB_FPS_LIMIT")));
    g_stubs = MapNear(kStubPage);
    if (!g_stubs) {
        host_log("Live clock: no memory next to the game's code: the patch's static 60 FPS");
        return false;
    }
    StubsWritable(true);
    const bool ok = InstallLive();
    StubsWritable(false);
    g_active.store(ok, std::memory_order_relaxed);
    return ok;
}

bool Active() { return g_active.load(std::memory_order_relaxed); }
std::uint64_t Steps() { return g_steps.load(std::memory_order_relaxed); }
double GameSeconds() { return g_game_seconds.load(std::memory_order_relaxed); }
float LastDt() { return g_last_dt.load(std::memory_order_relaxed); }
} // namespace BbEngine::LiveClock
