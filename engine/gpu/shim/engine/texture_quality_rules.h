// Texture quality (texture_quality in bbport.ini): big block-compressed materials are made
// without their largest mip levels to save video memory. Ported from visy c55cd33d (after
// Mrsuss60/bloodborne_pc_windows_port 8841db5b and deadinside28/bloodborne_pc). Pure rules
// for the native test; the texture cache applies them (texture_cache.cpp, FindImage).
#pragma once

#include <algorithm>
#include <cstdint>

namespace TextureQuality {

enum Quality : int { Auto = 0, Full = 1, Half = 2, Quarter = 3, Count = 4 };

inline const char* Name(int quality) {
    switch (quality) {
    case Full: return "full";
    case Half: return "half";
    case Quarter: return "quarter";
    default: return "auto";
    }
}

/// Levels the setting asks for: 0 full, 1 half, 2 quarter. Auto is half on a card whose own
/// device-local memory is 3 GB or less, else full.
inline uint32_t WantedSkip(int quality, uint64_t device_memory) {
    if (quality == Auto) {
        quality = device_memory != 0 && device_memory <= (3ull << 30) ? Half : Full;
    }
    return quality == Quarter ? 2 : quality == Half ? 1 : 0;
}

/// Levels one texture may lose. `eligible` = a sampled, plain 2D block-compressed colour
/// texture with one layer and one sample that the GPU has not written. A level goes only while
/// the level being dropped has a smaller side of at least 512 and 3 levels are left before it
/// (visy's rule: a 512 texture becomes 256 at half).
inline uint32_t SkipLevels(bool eligible, uint32_t width, uint32_t height, uint32_t levels,
                           uint32_t wanted) {
    if (!eligible || wanted == 0 || levels < 3) {
        return 0;
    }
    uint32_t skip = 0;
    while (skip < wanted && levels - skip >= 3 && (std::min(width, height) >> skip) >= 512) {
        ++skip;
    }
    return skip;
}

} // namespace TextureQuality
