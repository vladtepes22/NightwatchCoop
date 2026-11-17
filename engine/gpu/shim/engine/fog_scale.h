// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Fog amount (graphics_fog): the game's fog table is copied, the copy scaled, and the upload pointed at the copy
// (graphics.cpp BeforeFogUpload). Scaling the game's own table in place compounded on every upload, so its values
// drifted and the fog flickered. Pure rules here for tests/test_fog_scale.cpp.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef BB_FOG_SCALE_MUTANTS
namespace BbEngine::FogScale {
inline int g_mutant = 0;
}
#define BB_FOG_SCALE_PICK(n, wrong, right) (::BbEngine::FogScale::g_mutant == (n) ? (wrong) : (right))
#else
#define BB_FOG_SCALE_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::FogScale {
inline constexpr int kMutants = 4;
inline constexpr std::size_t kBlockBytes = 0x60;
inline constexpr std::size_t kBlockFloats = kBlockBytes / 4;
inline constexpr std::uint32_t kMaxBlocks = 4096;
inline constexpr std::size_t kMaxBytes = std::size_t(kMaxBlocks) * kBlockBytes;
inline constexpr int kOpacities[] = {0, 8, 12};

// Nothing to do: picture tuning off, or fog at the game's own value.
inline bool Untouched(bool on, float k) { return BB_FOG_SCALE_PICK(4, !on, !on || k == 1.0f); }

inline float Scaled(float a, float k) { return k > 1 ? std::min(a * k, std::max(a, 1.0f)) : a * k; }

// Bytes to copy: every block, and every byte the upload reads (its size rounded up to 4, as the game rounds it);
// 0 when that is over the cap (the upload then keeps the game's table).
inline std::size_t CopyBytes(std::uint32_t count, std::uint32_t upload) {
    const std::size_t want = std::max(std::size_t(count) * kBlockBytes, (std::size_t(upload) + 3) & ~std::size_t(3));
    return BB_FOG_SCALE_PICK(3, true, want <= kMaxBytes) ? want : 0;
}

// Copies `bytes` of the game's table into `scratch` and scales the opacities of its `count` blocks there; the game's
// table is not written. Returns the copy.
inline std::uint8_t* ScaleCopy(const std::uint8_t* table, std::uint32_t count, std::size_t bytes, float k,
                               std::uint8_t* scratch) {
    std::uint8_t* out = BB_FOG_SCALE_PICK(1, const_cast<std::uint8_t*>(table), scratch);
    if (out != table) std::memcpy(out, table, bytes);
    const std::size_t blocks = std::min<std::size_t>(count, bytes / kBlockBytes);
    for (std::size_t i = BB_FOG_SCALE_PICK(2, 1, 0); i < blocks; ++i) {
        for (const int o : kOpacities) {
            std::uint8_t* at = out + i * kBlockBytes + std::size_t(o) * 4;
            float a = 0;
            std::memcpy(&a, at, sizeof a);
            a = Scaled(a, k);
            std::memcpy(at, &a, sizeof a);
        }
    }
    return out;
}

// A fog value right next to 1 (a slider's float steps) is the game's own.
inline float SnapToOne(float v) { return std::fabs(v - 1.0f) < 0.005f ? 1.0f : v; }
}
