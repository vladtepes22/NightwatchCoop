// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_rate.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace BbEngine::FrameRate {
namespace {
enum class SiteKind {
    Step, ContextStep, TargetStep, HighStep, EffectStep, CountSixth, CountThird, Three, NegThree, EaseDistance,
    EaseChase, FrameModeTime, WaitSleep, LimiterCap
};

struct Site {
    std::uint32_t offset;
    SiteKind kind;
    bool code;
    bool checked;
    std::uint8_t prefix[4];
};

struct ListWrite {
    std::uint8_t list;
    std::uint32_t offset;
    std::uint16_t size;
    std::uint32_t at;
};

constexpr Site kSites[] = {
#include "frame_rate_sites.inc"
};
#include "frame_rate_lists.inc"

enum List : std::uint8_t { Sixty, Ninety, Uncapped, Sprint };

constexpr std::size_t kFrameTime = 0x535a908;
constexpr std::size_t kFrameTimeCave = 0x2018e3d;
constexpr std::uint8_t kFrameTimeCaveCode[] = {0xc5, 0xfa, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00};
constexpr float kSlowest = 30.0f;
constexpr int kSettleFrames = 120;
constexpr int kRiseFrames = 30;
constexpr float kDrift = 0.1f;

std::uint8_t* g_image = nullptr;
std::vector<const Site*> g_data, g_code;
float g_target = 60.0f;
float g_code_rate = 60.0f;
float g_held = 0.0f;
int g_drift_frames = 0;
int g_adjustments = 0;
bool g_uncapped = false;
bool g_follow = true;
bool g_data_writable = false;
std::atomic<float> g_shown{0.0f};

bool Switch(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return v[0] != '0';
}

bool CodeKind(SiteKind kind) {
    return kind == SiteKind::Step || kind == SiteKind::ContextStep || kind == SiteKind::HighStep ||
           kind == SiteKind::EffectStep || kind == SiteKind::CountSixth || kind == SiteKind::CountThird;
}

bool TargetKind(SiteKind kind) {
    return kind == SiteKind::FrameModeTime || kind == SiteKind::WaitSleep || kind == SiteKind::LimiterCap;
}

std::int32_t LimiterSleep(float rate) {
    return rate <= 60.0f ? 16000 : rate <= 90.0f ? 10000 : rate <= 120.0f ? 8000 : 6500;
}

std::uint32_t Value(SiteKind kind, float rate) {
    float f = 0.0f;
    std::int32_t i = 0;
    bool integer = false;
    switch (kind) {
    case SiteKind::Step:
    case SiteKind::ContextStep:
    case SiteKind::TargetStep:
    case SiteKind::FrameModeTime: f = 1.0f / rate; break;
    case SiteKind::HighStep: f = rate > 60.0f ? 1.0f / rate : 1.0f / 30.0f; break;
    case SiteKind::EffectStep: f = 1.125f / rate; break;
    case SiteKind::CountSixth: i = std::max(5, int(rate / 6.0f)); integer = true; break;
    case SiteKind::CountThird: i = std::max(10, int(rate / 3.0f)); integer = true; break;
    case SiteKind::Three: f = 3.0f / rate; break;
    case SiteKind::NegThree: f = -3.0f / rate; break;
    case SiteKind::EaseDistance: f = float(1.0 - std::pow(0.9, 30.0 / rate)); break;
    case SiteKind::EaseChase: f = float(1.0 - std::pow(1.0 - 1.0 / 30.0, 30.0 / rate)); break;
    case SiteKind::WaitSleep: i = std::int32_t(990000.0f / rate); integer = true; break;
    case SiteKind::LimiterCap: i = LimiterSleep(rate); integer = true; break;
    }
    std::uint32_t bits;
    if (integer) std::memcpy(&bits, &i, 4);
    else std::memcpy(&bits, &f, 4);
    return bits;
}

bool Plausible(SiteKind kind, const std::uint8_t* at) {
    float f;
    std::int32_t i;
    std::memcpy(&f, at, 4);
    std::memcpy(&i, at, 4);
    switch (kind) {
    case SiteKind::Step:
    case SiteKind::ContextStep:
    case SiteKind::TargetStep:
    case SiteKind::HighStep:
    case SiteKind::FrameModeTime: return f > 1.0f / 1000.0f && f < 0.034f;
    case SiteKind::EffectStep: return f > 0.0005f && f < 0.04f;
    case SiteKind::CountSixth: return i >= 5 && i <= 200;
    case SiteKind::CountThird: return i >= 10 && i <= 400;
    case SiteKind::Three: return f > 0.002f && f < 0.11f;
    case SiteKind::NegThree: return f < -0.002f && f > -0.11f;
    case SiteKind::EaseDistance: return f > 0.0f && f < 0.2f;
    case SiteKind::EaseChase: return f > 0.0f && f < 0.07f;
    case SiteKind::WaitSleep:
    case SiteKind::LimiterCap: return i >= 500 && i <= 40000;
    }
    return false;
}

float TargetRate(bool& uncapped) {
    const char* fps = std::getenv("BB_FPS");
    const std::string v = fps ? fps : "60";
    uncapped = v == "uncap";
    if (uncapped) {
        const char* limit = std::getenv("BB_FPS_LIMIT");
        const int l = limit ? std::atoi(limit) : 0;
        return float(l >= 30 && l <= 1000 ? l : 240);
    }
    const int f = std::atoi(v.c_str());
    return float(f >= 30 && f <= 1000 ? f : 60);
}

bool Unlock(std::uint8_t* at, std::size_t size) {
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(at) & ~std::uintptr_t(0xfff);
    const std::uintptr_t end = (reinterpret_cast<std::uintptr_t>(at) + size + 0xfff) & ~std::uintptr_t(0xfff);
#ifdef _WIN32
    DWORD before = 0;
    return VirtualProtect(reinterpret_cast<void*>(first), end - first, PAGE_EXECUTE_READWRITE, &before) != 0;
#else
    return mprotect(reinterpret_cast<void*>(first), end - first, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

void ApplyLists(std::uint8_t* image, std::size_t size) {
    bool wanted[4] = {};
    if (g_uncapped) wanted[Uncapped] = wanted[Sprint] = true;
    else if (g_target > 60.0f) wanted[Ninety] = wanted[Sprint] = true;
    else wanted[Sixty] = true;
    int written = 0, already = 0, kept = 0;
    for (const ListWrite& w : kListWrites) {
        if (!wanted[w.list]) continue;
        if (std::size_t(w.offset) + w.size > size) { ++kept; continue; }
        std::uint8_t* at = image + w.offset;
        const std::uint8_t* original = kListBlob + w.at;
        const std::uint8_t* patched = original + w.size;
        if (std::memcmp(at, original, w.size) == 0) {
            if (!Unlock(at, w.size)) { ++kept; continue; }
            std::memcpy(at, patched, w.size);
            ++written;
        } else if (std::memcmp(at, patched, w.size) == 0) {
            ++already;
        } else {
            ++kept;
        }
    }
    host_log("Frame rate: the engine wrote the frame-rate code for %.0f FPS%s (%d writes, %d already in place, %d left "
             "to another patch)",
             double(g_target), g_uncapped ? " uncapped" : "", written, already, kept);
}

bool Store(std::uint8_t* at, std::uint32_t bits) {
    std::uint32_t now;
    std::memcpy(&now, at, 4);
    if (now == bits) return true;
    if (!Unlock(at, 4)) return false;
    std::memcpy(at, &bits, 4);
    return true;
}

void WriteCodeSites(float rate) {
    for (const Site* s : g_code) Store(g_image + s->offset, Value(s->kind, rate));
    g_code_rate = rate;
}

bool MakeDataWritable() {
    std::vector<std::uintptr_t> pages;
    for (const Site* s : g_data) {
        const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(g_image + s->offset) & ~std::uintptr_t(0xfff);
        if (std::find(pages.begin(), pages.end(), page) == pages.end()) pages.push_back(page);
    }
    for (const std::uintptr_t page : pages) {
#ifdef _WIN32
        DWORD before = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_EXECUTE_READWRITE, &before)) return false;
#else
        if (mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
#endif
    }
    return true;
}

float Rounded(float rate) {
    return std::clamp(std::round(rate), kSlowest, g_target);
}

// bbport (visy 0.7.7 CL-6857c0, deadinside28's ClothStiffnessFactor, bloodborne_pc 0fc2921c, their
// issue #58; GPL-2.0-or-later): Havok's stiffness factor of a cloth constraint set for one substep
// (+0x1167a80, replaced). `mode` 2 adapts the constraints' stiffness to how often they are solved,
// (substeps x a x b)^-1.725 (a, b the instance's scales); for a set of type 8 the iteration's share
// instead, for types 5 and 10 only the last substep. The substeps are per frame, so above 30 FPS the
// cloth is solved more often by the same factor and is stiffer by as much (cloaks and hair flutter).
// The factor here is the game's x the frame's length in 30 FPS frames (30 / the rate the game holds):
// the cloth is pulled as far per second as at 30 FPS. BB_CLOTH_STIFFNESS=0: off.
constexpr std::size_t kClothFactor = 0x1167a80;
// push rbp; mov rbp, rsp; push r15; push r14; push rbx; push rax; vmovaps xmm2, xmm0 (14 bytes)
constexpr std::uint8_t kClothFactorCode[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                             0x56, 0x53, 0x50, 0xc5, 0xf8, 0x28, 0xd0};
static_assert(sizeof kClothFactorCode == 14, "the jump below covers exactly these instructions");
constexpr std::size_t kClothExponent = 0x48d7bbc;
constexpr std::size_t kClothBend = 0x7387f4; // Steady cloth's bend-constraint site (scripts/cloth.py BEND)
float g_cloth_exponent = -1.725f;
std::atomic<float> g_cloth_rate{30.0f};
std::atomic<std::uint64_t> g_cloth_calls{0};

__attribute__((sysv_abi)) float ClothStiffnessFactor(const std::uint8_t* set, int mode, int iteration, int substeps,
                                                     float a, float b) {
    std::uint32_t type;
    std::memcpy(&type, set + 0x18, 4);
    const float frames = 30.0f / std::max(g_cloth_rate.load(std::memory_order_relaxed), kSlowest);
    if (g_cloth_calls.fetch_add(1, std::memory_order_relaxed) == 20000)
        host_log("Frame rate: cloth stiffness factor: constraint type %u, %.0f FPS: factor x%.3f", type,
                 double(30.0f / frames), double(frames));
    const bool adapts = mode == 2 && !(substeps == 1 && a == 1.0f && b == 1.0f);
    if (adapts && type == 8) return float(iteration + 1) / float(substeps);
    if (adapts && (type == 5 || type == 10)) return substeps - 1 == iteration ? 1.0f : 0.0f;
    const float game = adapts ? std::pow(float(substeps) * a * b, g_cloth_exponent) : 1.0f;
    if (type == 5 || type == 8 || type == 10) return game;
    return game * frames;
}

bool InstallClothFactor(std::uint8_t* image, std::size_t size) {
    const char* v = std::getenv("BB_CLOTH_STIFFNESS");
    if (v && *v && std::strtof(v, nullptr) == 0.0f) {
        host_log("Frame rate: cloth stiffness factor off (BB_CLOTH_STIFFNESS=0)");
        return false;
    }
    // The start's Steady cloth (scripts/cloth.py, bbport.ini steady_cloth, on by default) already
    // eases the cloth above 30 FPS through its bend constraints (a jmp at +0x7387f4); both together
    // would ease it twice. The factor is used only when Steady cloth is off.
    if (size > kClothBend && image[kClothBend] == 0xe9) {
        host_log("Frame rate: cloth stiffness factor left out: Steady cloth already corrects the cloth for the frame rate");
        return false;
    }
    if (size < kClothExponent + 4 || std::memcmp(image + kClothFactor, kClothFactorCode, sizeof kClothFactorCode) != 0) {
        host_log("Frame rate: +0x%zx is not the expected code: cloth stiffness factor not installed", kClothFactor);
        return false;
    }
    std::memcpy(&g_cloth_exponent, image + kClothExponent, 4);
    if (!(g_cloth_exponent < 0.0f && g_cloth_exponent > -4.0f)) {
        host_log("Frame rate: cloth exponent %.3f unexpected: cloth stiffness factor not installed",
                 double(g_cloth_exponent));
        return false;
    }
    // jmp [rip + 0]; the handler's address (14 bytes, whole instructions; nothing jumps into them).
    std::uint8_t jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(&ClothStiffnessFactor);
    std::memcpy(jump + 6, &target, 8);
    if (!WriteCode(image + kClothFactor, jump, sizeof jump)) {
        host_log("Frame rate: cloth stiffness factor not installed (code not writable)");
        return false;
    }
    host_log("Frame rate: cloth stiffness factor on (BB_CLOTH_STIFFNESS=0 turns it off)");
    return true;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    if (!Switch("LANTERN_FRAMERATE", true)) {
        host_log("Frame rate: native frame rate off (Engine switches); the start's frame-rate patches are used");
        return false;
    }
    g_target = TargetRate(g_uncapped);
    if (!image || size < kFrameTime + 4) return false;
    if (!g_uncapped && g_target <= 30.0f) {
        host_log("Frame rate: the game's own 30 FPS");
        return false;
    }
    if (Switch("LANTERN_FRAMERATE_FROM_ENGINE", false)) ApplyLists(image, size);
    if (std::memcmp(image + kFrameTimeCave, kFrameTimeCaveCode, sizeof kFrameTimeCaveCode) != 0) {
        host_log("Frame rate: the frame-time code is not in place; the game runs at its own 30 FPS");
        return false;
    }
    g_follow = Switch("LANTERN_FRAMERATE_FOLLOW", true);
    int skipped = 0, held = 0, fixed = 0;
    for (const Site& s : kSites) {
        if (s.offset + 4 > size) { ++skipped; continue; }
        std::uint8_t* at = image + s.offset;
        if ((s.checked && std::memcmp(at - 4, s.prefix, 4) != 0) || !Plausible(s.kind, at)) {
            ++skipped;
            continue;
        }
        if (TargetKind(s.kind)) {
            if (g_uncapped) continue;
            if (!Store(at, Value(s.kind, g_target))) { ++skipped; continue; }
            ++fixed;
            continue;
        }
        if (s.kind == SiteKind::TargetStep) {
            if (!Store(at, Value(s.kind, g_target))) { ++skipped; continue; }
            ++held;
            continue;
        }
        (CodeKind(s.kind) && s.code ? g_code : g_data).push_back(&s);
    }
    g_image = image;
    for (const Site* s : g_code) Store(image + s->offset, Value(s->kind, g_target));
    for (const Site* s : g_data) Store(image + s->offset, Value(s->kind, g_target));
    g_code_rate = g_target;
    g_held = g_target;
    g_cloth_rate.store(g_target, std::memory_order_relaxed);
    InstallClothFactor(image, size);
    host_log("Frame rate: native at %.0f FPS%s: %zu code, %zu data, %d target and %d fixed sites (%d left as they were); "
             "%s",
             double(g_target), g_uncapped ? " (uncapped limit)" : "", g_code.size(), g_data.size(), fixed, held, skipped,
             g_follow ? "steps follow the frame rate the game holds" : "steps fixed at the target");
    return true;
}

void Tick() {
    if (!g_image || !g_follow) return;
    float frame;
    std::memcpy(&frame, g_image + kFrameTime, 4);
    if (!(frame > 0.0f) || !std::isfinite(frame)) return;
    const float rate = std::clamp(1.0f / frame, kSlowest, g_target);
    g_held = g_held + (rate - g_held) * 0.05f;
    g_shown.store(g_held, std::memory_order_relaxed);
    g_cloth_rate.store(g_held, std::memory_order_relaxed);
    if (!g_data_writable) {
        if (!MakeDataWritable()) {
            host_log("Frame rate: the data sites cannot be made writable; they stay at %.0f FPS", double(g_target));
            g_data.clear();
        }
        g_data_writable = true;
    }
    for (const Site* s : g_data) {
        const std::uint32_t bits = Value(s->kind, rate);
        __atomic_store_n(reinterpret_cast<std::uint32_t*>(g_image + s->offset), bits, __ATOMIC_RELAXED);
    }
    if (g_code.empty()) return;
    const float wanted = Rounded(g_held);
    const bool up = wanted > g_code_rate;
    const bool stall = g_held < kSlowest + 2.0f;
    if (std::fabs(wanted - g_code_rate) > g_code_rate * kDrift && (up || !stall)) {
        if (++g_drift_frames >= (up ? kRiseFrames : kSettleFrames)) {
            ++g_adjustments;
            if (g_adjustments <= 20 || g_adjustments % 50 == 0)
                host_log("Frame rate: the game holds %.0f FPS (target %.0f): steps set for %.0f (%d changes so far)",
                         double(g_held), double(g_target), double(wanted), g_adjustments);
            WriteCodeSites(wanted);
            g_drift_frames = 0;
        }
    } else {
        g_drift_frames = 0;
    }
}

float HeldRate() {
    return g_shown.load(std::memory_order_relaxed);
}
}
